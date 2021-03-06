#include "swiftnav_ros/swiftnav_ros_driver.h"
#include <libsbp/system.h>
#include <libsbp/navigation.h>

#include <sensor_msgs/NavSatFix.h>
#include <sensor_msgs/TimeReference.h>

#include "kitty_common/GPSBaseline.h"
#include "kitty_common/GPSVelocity.h"

namespace swiftnav_ros {
    PIKSI::PIKSI(const ros::NodeHandle &_nh,
                 const ros::NodeHandle &_nh_priv,
                 const std::string _port) :
            nh(_nh),
            nh_priv(_nh_priv),
            port(_port),
            frame_id("gps"),
            piksid(-1),

            heartbeat_diag(nh, nh_priv, "ppiksi_time_diag"),
            llh_diag(nh, nh_priv, "ppiksi_llh_diag"),
            rtk_diag(nh, nh_priv, "ppiksi_rtk_diag"),

            min_llh_rate(0.5),
            max_llh_rate(50.0),
            min_rtk_rate(0.5),
            max_rtk_rate(50.0),
            min_heartbeat_rate(0.5),
            max_heartbeat_rate(50.0),

            llh_pub_freq(diagnostic_updater::FrequencyStatusParam(
                    &min_llh_rate, &max_llh_rate, 0.1, 50)),
            rtk_pub_freq(diagnostic_updater::FrequencyStatusParam(
                    &min_rtk_rate, &max_rtk_rate, 0.1, 50)),
            heartbeat_pub_freq(diagnostic_updater::FrequencyStatusParam(
                    &min_rtk_rate, &max_rtk_rate, 0.1, 50)),

            io_failure_count(0),
            last_io_failure_count(0),
            open_failure_count(0),
            last_open_failure_count(0),
            heartbeat_flags(0),

            num_llh_satellites(0),
            llh_status(0),
            llh_lat(0.0),
            llh_lon(0.0),
            llh_height(0.0),
            llh_h_accuracy(0.0),
            hdop(1.0),

            rtk_status(0),
            num_rtk_satellites(0),
            rtk_north(0.0),
            rtk_east(0.0),
            rtk_height(0.0),
            rtk_h_accuracy(0.04),     // 4cm

            spin_rate(2000),      // call sbp_process this fast to avoid dropped msgs
            spin_thread(&PIKSI::spin, this) {
        cmd_lock.unlock();
        heartbeat_diag.setHardwareID("piksi heartbeat");
        heartbeat_diag.add(heartbeat_pub_freq);

        llh_diag.setHardwareID("piksi lat/lon");
        llh_diag.add(llh_pub_freq);

        rtk_diag.setHardwareID("piksi rtk");
        rtk_diag.add("Piksi Status", this, &PIKSI::DiagCB);
        rtk_diag.add(rtk_pub_freq);

        nh_priv.param("frame_id", frame_id, (std::string) "gps");
    }

    PIKSI::~PIKSI() {
        spin_thread.interrupt();
        PIKSIClose();
    }

    bool PIKSI::PIKSIOpen() {
        boost::mutex::scoped_lock lock(cmd_lock);
        return PIKSIOpenNoLock();
    }

    bool PIKSI::PIKSIOpenNoLock() {
        if (piksid >= 0)
            return true;

        piksid = piksi_open(port.c_str(), 115200);

        if (piksid < 0) {
            open_failure_count++;
            return false;
        }

        sbp_state_init(&state);
        sbp_state_set_io_context(&state, &piksid);

        sbp_register_callback(&state, SBP_MSG_HEARTBEAT, &heartbeat_callback, (void *) this, &heartbeat_callback_node);
        sbp_register_callback(&state, SBP_MSG_GPS_TIME, &time_callback, (void *) this, &time_callback_node);
        sbp_register_callback(&state, SBP_MSG_POS_LLH, &pos_llh_callback, (void *) this, &pos_llh_callback_node);
        sbp_register_callback(&state, SBP_MSG_BASELINE_NED, &baseline_ned_callback, (void *) this,
                              &baseline_ned_callback_node);
        sbp_register_callback(&state, SBP_MSG_VEL_NED, &vel_ned_callback, (void *) this, &vel_ned_callback_node);

        llh_pub = nh.advertise<sensor_msgs::NavSatFix>("gps/fix", 1);
        time_pub = nh.advertise<sensor_msgs::TimeReference>("gps/time", 1);
        baseline_pub = nh.advertise<kitty_common::GPSBaseline>("gps/baseline", 1);
        vel_pub = nh.advertise<kitty_common::GPSVelocity>("gps/velocity", 1);

        return true;
    }

    void PIKSI::PIKSIClose() {
        boost::mutex::scoped_lock lock(cmd_lock);
        PIKSICloseNoLock();
    }

    void PIKSI::PIKSICloseNoLock() {
        int8_t old_piksid = piksid;
        if (piksid < 0) {
            return;
        }
        piksid = -1;
        piksi_close(old_piksid);
        if (llh_pub)
            llh_pub.shutdown();
        if (time_pub)
            time_pub.shutdown();
    }

    void heartbeat_callback(u16 sender_id, u8 len, u8 msg[], void *context) {
        if (context == NULL) {
            ROS_ERROR_STREAM("Critical Error: Pisk SBP driver heartbeat context void.");
            return;
        }

        msg_heartbeat_t hb = *(msg_heartbeat_t *) msg;

        class PIKSI *driver = (class PIKSI *) context;
        driver->heartbeat_pub_freq.tick();
        driver->heartbeat_flags |= (hb.flags & 0x7);    // accumulate errors for diags
        driver->sbp_protocol_version = (hb.flags & 0xFF0000) >> 16;
        if (driver->sbp_protocol_version < 2) {
            ROS_ERROR_STREAM("SBP Major protocol version mismatch. "
                                     "Driver compatible with 2.0 and later. Version "
                                     << driver->sbp_protocol_version << driver->heartbeat_flags << " detected.");
            return;
        }
    }

    void time_callback(u16 sender_id, u8 len, u8 msg[], void *context) {
        if (context == NULL) {
            ROS_ERROR_STREAM("Critical Error: Pisk SBP driver time context void.");
            return;
        }

        class PIKSI *driver = (class PIKSI *) context;

        msg_gps_time_t time = *(msg_gps_time_t *) msg;
        if ((time.flags & 0x7) != 0) {

            ROS_DEBUG_STREAM("Got a Piksi Multi time message");

            sensor_msgs::TimeReferencePtr time_msg(new sensor_msgs::TimeReference);

            time_msg->header.frame_id = driver->frame_id;
            time_msg->header.stamp = ros::Time::now();

            time_msg->time_ref.sec = time.tow;
            time_msg->source = "gps";

            driver->time_pub.publish(time_msg);
        } else {
            ROS_DEBUG_STREAM("Problem: Got a Piksi Multi time message with a bad time flag");
        }



        return;
    }

    void pos_llh_callback(u16 sender_id, u8 len, u8 msg[], void *context) {
        if (context == NULL) {
            ROS_ERROR_STREAM("Critical Error: Pisk SBP driver pos_llh context void.");
            return;
        }

        class PIKSI *driver = (class PIKSI *) context;

        msg_pos_llh_t llh = *(msg_pos_llh_t *) msg;

        // populate diagnostic data
        driver->llh_pub_freq.tick();
        driver->llh_status |= llh.flags;

        if ((llh.flags & 0x7) != 0) {

            ROS_DEBUG_STREAM("Got a Piksi Multi llh message");

            sensor_msgs::NavSatFixPtr llh_msg(new sensor_msgs::NavSatFix);

            llh_msg->header.frame_id = driver->frame_id;
            llh_msg->header.stamp = ros::Time::now();

            llh_msg->status.status = 0;
            llh_msg->status.service = 1;

            llh_msg->latitude = llh.lat;
            llh_msg->longitude = llh.lon;
            llh_msg->altitude = llh.height;

            // populate the covariance matrix
            double h_covariance = llh.h_accuracy * llh.h_accuracy * 1.0e-6;
            double v_covariance = llh.v_accuracy * llh.v_accuracy * 1.0e-6;
            llh_msg->position_covariance[0] = h_covariance;   // x = 0, 0
            llh_msg->position_covariance[4] = h_covariance;   // y = 1, 1
            llh_msg->position_covariance[8] = v_covariance;   // z = 2, 2

            driver->llh_pub.publish(llh_msg);

            driver->num_llh_satellites = llh.n_sats;
            driver->llh_lat = llh.lat;
            driver->llh_lon = llh.lon;
            driver->llh_height = llh.height;
            driver->llh_h_accuracy = llh.h_accuracy / 1000.0;
            driver->llh_v_accuracy = llh.v_accuracy / 1000.0;
        } else {
            ROS_DEBUG_STREAM("Problem: Got a Piksi Multi llh message with a bad time flag");
        }
        return;
    }

    void baseline_ned_callback(u16 sender_id, u8 len, u8 msg[], void *context) {
        if (context == NULL) {
            ROS_ERROR_STREAM("Critical Error: Pisk SBP driver baseline context void.");
            return;
        }

        class PIKSI *driver = (class PIKSI *) context;

        msg_baseline_ned_t sbp_ned = *(msg_baseline_ned_t *) msg;

        // save diagnostic data
        driver->rtk_pub_freq.tick();
        driver->rtk_status = sbp_ned.flags;

        if ((sbp_ned.flags & 0x7) != 0) {

            ROS_DEBUG_STREAM("Got a Piksi Multi ned message");

            kitty_common::GPSBaseline gps_baseline;

            gps_baseline.header.frame_id = driver->frame_id;
            gps_baseline.header.stamp = ros::Time::now();

            gps_baseline.time_of_week = sbp_ned.tow;
            gps_baseline.n = sbp_ned.n / 1000.0;
            gps_baseline.e = sbp_ned.e / 1000.0;
            gps_baseline.d = sbp_ned.d / 1000.0;
            gps_baseline.h_accuracy = sbp_ned.h_accuracy / 1000.0;
            gps_baseline.v_accuracy = sbp_ned.v_accuracy / 1000.0;
            gps_baseline.n_sats = sbp_ned.n_sats;

            // pull off bits 0, 1, and 2 for the flags
            gps_baseline.fix_mode = sbp_ned.flags & 0b111;

            driver->baseline_pub.publish(gps_baseline);
        } else {
            ROS_DEBUG_STREAM("Problem: Got a Piksi Multi ned message with a bad time flag");
        }
        return;
    }

    void vel_ned_callback(u16 sender_id, u8 len, u8 msg[], void *context) {
        if (context == NULL) {
            ROS_ERROR_STREAM("Critical Error: Pisk SBP driver vel_ned context void.");
            return;
        }

        ROS_DEBUG_STREAM("Got a Piksi Multi vel_ned message");

        class PIKSI *driver = (class PIKSI *) context;

        msg_vel_ned_t sbp_vel = *(msg_vel_ned_t *) msg;

        kitty_common::GPSVelocity gps_velocity;

        gps_velocity.header.frame_id = driver->frame_id;
        gps_velocity.header.stamp = ros::Time::now();

        gps_velocity.time_of_week = sbp_vel.tow;
        gps_velocity.n = sbp_vel.n / 1000.0;
        gps_velocity.e = sbp_vel.e / 1000.0;
        gps_velocity.d = sbp_vel.d / 1000.0;
        gps_velocity.h_accuracy = sbp_vel.h_accuracy / 1000.0;
        gps_velocity.v_accuracy = sbp_vel.v_accuracy / 1000.0;
        gps_velocity.n_sats = sbp_vel.n_sats;

        // pull off bits 0, 1, and 2 for the flags
        gps_velocity.compute_mode = sbp_vel.flags & 0b111;

        driver->vel_pub.publish(gps_velocity);

        return;
    }

    void PIKSI::spin() {
        while (ros::ok()) {
            boost::this_thread::interruption_point();
            PIKSI::spinOnce();
            heartbeat_diag.update();
            llh_diag.update();
            rtk_diag.update();
            spin_rate.sleep();
        }
    }

    void PIKSI::spinOnce() {
        int ret;

        cmd_lock.lock();
        if (piksid < 0 && !PIKSIOpenNoLock()) {
            cmd_lock.unlock();
            return;
        }

        ret = sbp_process(&state, &read_data);
        cmd_lock.unlock();
    }

    void PIKSI::DiagCB(diagnostic_updater::DiagnosticStatusWrapper &stat) {
        stat.summary(diagnostic_msgs::DiagnosticStatus::OK, "PIKSI status OK");
        boost::mutex::scoped_lock lock(cmd_lock);

        if (piksid < 0 && !PIKSIOpenNoLock()) {
            stat.summary(diagnostic_msgs::DiagnosticStatus::ERROR, "Disconnected");
            return;
        } else if (open_failure_count > last_open_failure_count) {
            stat.summary(diagnostic_msgs::DiagnosticStatus::ERROR,
                         "Open Failure Count Increase");
        } else if (io_failure_count > last_io_failure_count) {
            stat.summary(diagnostic_msgs::DiagnosticStatus::WARN,
                         "I/O Failure Count Increase");
        } else if (0 != heartbeat_flags & 0x7) {
            stat.summary(diagnostic_msgs::DiagnosticStatus::ERROR,
                         "Piksi Error indicated by heartbeat flags");
        }

        stat.add("io_failure_count", io_failure_count);
        last_io_failure_count = io_failure_count;

        stat.add("open_failure_count", open_failure_count);
        last_open_failure_count = open_failure_count;

        stat.add("Heartbeat status (0 = good)", heartbeat_flags);
        stat.add("Number of satellites used in GPS RTK solution", num_rtk_satellites);
        stat.add("GPS RTK solution status (1 = good)", rtk_status);
        stat.add("GPS RTK meters north", rtk_north);
        stat.add("GPS RTK meters east", rtk_east);
        stat.add("GPS RTK height difference (m)", rtk_height);
        stat.add("GPS RTK horizontal accuracy (m)", rtk_h_accuracy);
        stat.add("GPS RTK velocity north", rtk_vel_north);
        stat.add("GPS RTK velocity east", rtk_vel_east);
        stat.add("GPS RTK velocity up", rtk_vel_up);
        stat.add("Number of satellites used for lat/lon", num_llh_satellites);
        stat.add("GPS lat/lon solution status", llh_status);
        stat.add("GPS latitude", llh_lat);
        stat.add("GPS longitude", llh_lon);
        stat.add("GPS altitude", llh_height);
        stat.add("GPS lat/lon horizontal accuracy (m)", llh_h_accuracy);
    }

}
