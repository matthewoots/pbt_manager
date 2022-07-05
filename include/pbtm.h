/*
* pbtm.h
*
* ---------------------------------------------------------------------
* Created by Matthew (matthewoots@gmail.com) in 2022
*
*  This program is free software; you can redistribute it and/or
*  modify it under the terms of the GNU General Public License
*  as published by the Free Software Foundation; either version 2
*  of the License, or (at your option) any later version.
*
*  This program is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*  GNU General Public License for more details.
* ---------------------------------------------------------------------
*/
#ifndef PBTM_H
#define PBTM_H

#include <string>
#include <mutex>
#include <iostream>
#include <chrono>
#include <ctime>
#include <math.h>
#include <random>
#include <Eigen/Dense>
#include <ros/ros.h>
#include <random>

#include <nav_msgs/Path.h>

#include <geometry_msgs/Point.h>
#include <geometry_msgs/Quaternion.h>
#include <geometry_msgs/PoseStamped.h>
#include <geometry_msgs/PointStamped.h>

#include <mavros_msgs/CommandBool.h>
#include <mavros_msgs/SetMode.h>
#include <mavros_msgs/PositionTarget.h>
#include <mavros_msgs/State.h>

#include <trajectory_msgs/JointTrajectory.h>
#include <trajectory_msgs/JointTrajectoryPoint.h>

#include <visualization_msgs/Marker.h>

#include <tf/tf.h>

#include "bspline_utils.hpp"

#define KNRM  "\033[0m"
#define KRED  "\033[31m"
#define KGRN  "\033[32m"
#define KYEL  "\033[33m"
#define KBLU  "\033[34m"
#define KMAG  "\033[35m"
#define KCYN  "\033[36m"
#define KWHT  "\033[37m"

using namespace Eigen;
using namespace std;
using namespace trajectory;

class pbtm_class
{
    private:

        enum VehicleTask
        {
            kIdle,
            kTakeOff,
            kHover,
            kMission,
            kHome,
            kLand
        };

        const std::string TaskToString(int v)
        {
            switch (v)
            {
                case kIdle:   return "IDLE";
                case kTakeOff:   return "TAKEOFF";
                case kHover: return "HOVER";
                case kMission:   return "MISSION";
                case kHome:   return "HOME";
                case kLand: return "LAND";
                default:      return "[Unknown Task]";
            }
        }

        struct state_command
        {
            Eigen::Vector3d pos;
            Eigen::Vector3d vel;
            Eigen::Vector3d acc;
            Eigen::Quaterniond q;
            double t;
        };

        pbtm_class::state_command cmd_nwu;

        bspline_trajectory bsu;
        common_trajectory_tool ctt;

        ros::ServiceClient arming_client; 
        ros::ServiceClient set_mode_client; 
        mavros_msgs::CommandBool arm_cmd;
        mavros_msgs::State uav_current_state;

        ros::NodeHandle _nh;
        ros::Subscriber _pos_sub, _waypoint_sub, _state_sub;
        ros::Publisher _pose_nwu_pub, _local_pos_raw_pub, _log_path_pub;

        ros::Timer _drone_timer;

        ros::Time _last_pose_time;
        ros::Time _prev_command_time;

        // Offboard enabled means that offboard control is active and also uav is armed
        bool _offboard_enabled = false;
        bool _setup = false, _state_check = false;
        double takeoff_land_velocity = 0.3;
        int uav_id, uav_task;
        std::string _id;
        double _send_command_interval, _send_command_rate;
        double _timeout, _nwu_yaw_offset, last_yaw, _takeoff_height;
        Eigen::Vector3d _start_global_nwu;

        /** @brief Bspline parameters **/
        int _knot_division, _knot_size;
        double _order, _max_velocity;
        double _knot_interval, _duration;
        time_point<std::chrono::system_clock> stime; // start time for bspline server in time_t
        vector<double> timespan;

        std::mutex send_command_mutex;
        std::mutex pose_mutex;
        std::mutex waypoint_command_mutex;

        Eigen::Affine3d current_transform_enu, global_nwu_pose, home_transformation;
        Eigen::Affine3d global_to_local_t, local_to_global_t;

        nav_msgs::Path path;

        vector<Eigen::Vector3d> wp_pos_vector;
        vector<Eigen::Vector3d> control_points;
        vector<double> height_list;

        // w, x, y, z
        Quaterniond enu_to_nwu() {return Quaterniond(0.7073883, 0, 0, 0.7068252);}
    
    public:

        pbtm_class(ros::NodeHandle &nodeHandle) : _nh(nodeHandle)
        {
            _nh.param<std::string>("agent_id", _id, "drone0");
            _nh.param<double>("send_command_rate", _send_command_rate, 1.0);
            _nh.param<double>("timeout", _timeout, 0.5);
            _nh.param<double>("takeoff_height", _takeoff_height, 1.0);

            std::vector<double> position_list;
            _nh.getParam("global_start_position", position_list);
            _start_global_nwu.x() = position_list[0];
            _start_global_nwu.y() = position_list[1];
            _start_global_nwu.z() = position_list[2];

            // height min followed by height max
            _nh.getParam("height_range", height_list);
            _nh.param<double>("yaw_offset_rad", _nwu_yaw_offset, 0.0);

            /** @brief Bspline parameters **/
            _nh.param<double>("order", _order, 1.0);
            _nh.param<double>("max_velocity", _max_velocity, 1.0);
            _nh.param<int>("knot_division", _knot_division, 1);

            _send_command_interval = 1 / _send_command_rate;

            // Reset uav task to idle
            uav_task = 0;
            // Reset commanded state
            cmd_nwu.pos = Eigen::Vector3d::Zero();
            cmd_nwu.vel = Eigen::Vector3d::Zero();
            cmd_nwu.acc = Eigen::Vector3d::Zero();
            cmd_nwu.q = Eigen::Quaterniond(1.0, 0.0, 0.0, 0.0); 

            // For inclusion of yaw offset
            Eigen::Quaterniond q_corrected;
            q_corrected = AngleAxisd(0, Eigen::Vector3d::UnitX())
                * AngleAxisd(0, Eigen::Vector3d::UnitY())
                * AngleAxisd(_nwu_yaw_offset, Eigen::Vector3d::UnitZ());
            // Setup local_to_global transform
            local_to_global_t = Affine3d::Identity(); 
            local_to_global_t.translate(_start_global_nwu);
            local_to_global_t.rotate(q_corrected.inverse());
            local_to_global_t.rotate(enu_to_nwu().inverse());

            // Setup global_to_local transform
            global_to_local_t = Affine3d::Identity(); 
            global_to_local_t.rotate(enu_to_nwu());
            global_to_local_t.rotate(q_corrected);
            global_to_local_t.translate(-_start_global_nwu);

            /** @brief Get the uav id in int **/
            std::string copy_id = _id; 
            std::string uav_id_char = copy_id.erase(0,5); // removes first 5 character
            uav_id = stoi(uav_id_char);

            /* ------------ Subscribers ------------ */
            /** @brief Get Mavros State of PX4 */
            _state_sub = _nh.subscribe<mavros_msgs::State>(
                "/" + _id + "/mavros/state", 10, boost::bind(&pbtm_class::uavStateCallBack, this, _1));
            /** @brief Subscriber that receives local position via mavros */
            _pos_sub = _nh.subscribe<geometry_msgs::PoseStamped>(
                "/" + _id + "/mavros/local_position/pose", 20, &pbtm_class::pose_callback, this);
            /** @brief Subscriber that receives waypoint information from user */
            _waypoint_sub = _nh.subscribe<trajectory_msgs::JointTrajectory>(
                "/trajectory/points", 20, &pbtm_class::waypoint_command_callback, this);
            

            /* ------------ Publishers ------------ */
            /** @brief Publisher that publishes control raw setpoints via mavros */
            _local_pos_raw_pub = _nh.advertise<mavros_msgs::PositionTarget>(
                "/" + _id + "/mavros/setpoint_raw/local", 20);
            /** @brief Publisher that publishes global_pose in nwu frame */
            _pose_nwu_pub = _nh.advertise<geometry_msgs::PoseStamped>(
                "/" + _id + "/uav/nwu", 20);
            _log_path_pub = _nh.advertise<nav_msgs::Path>(
                "/" + _id + "/uav/log_path", 10, true);


            /* ------------ Timers ------------ */
            /** @brief Timer that handles drone state at each time frame */
            _drone_timer = _nh.createTimer(ros::Duration(
                _send_command_interval), &pbtm_class::drone_timer, this, false, false);


            /* ------------ Service Clients ------------ */
            /** @brief Service Client that handles arming in Mavros */
            arming_client = _nh.serviceClient<mavros_msgs::CommandBool>(
                "/" + _id + "/mavros/cmd/arming");
            /** @brief Service Client that handles mode switching in Mavros */
            set_mode_client = _nh.serviceClient<mavros_msgs::SetMode>(
                "/" + _id + "/mavros/set_mode");

            printf("[%sdrone%d%s pbtm.h] global_start_pose [%s%.2lf %.2lf %.2lf%s]! \n", 
                KGRN, uav_id, KNRM,
                KBLU, _start_global_nwu(0), _start_global_nwu(1), _start_global_nwu(2), KNRM);
            printf("[%sdrone%d%s pbtm.h] height_range [%s%.2lf %.2lf%s]! \n", 
                KGRN, uav_id, KNRM,
                KBLU, height_list[0], height_list[1], KNRM);
            printf("[%sdrone%d%s pbtm.h] yaw_offset_rad [%s%.2lf%s]! \n", 
                KGRN, uav_id, KNRM, KBLU, _nwu_yaw_offset, KNRM);

            printf("%s[drone%d%s pbtm.h] constructed! \n", KGRN, uav_id, KNRM);
            _drone_timer.start();
        }

        ~pbtm_class()
        {
            _drone_timer.stop();
        }

        bool check_last_time(double tolerance, ros::Time time);

        void drone_timer(const ros::TimerEvent &);

        void send_command();

        void initialize_bspline_server(double desired_velocity);

        bool update_get_command_by_time();

        void check_mavros_state();

        void set_offboard();

        void stop_and_hover();

        void visualize_log_path();

        /** @brief callbacks */
        void pose_callback(const geometry_msgs::PoseStampedConstPtr& msg);
        void waypoint_command_callback(const trajectory_msgs::JointTrajectory::ConstPtr &msg);
        void uavStateCallBack(const mavros_msgs::State::ConstPtr &msg);

        /** @brief common utility functions */
        int joint_trajectory_to_waypoint(trajectory_msgs::JointTrajectory jt);

};


#endif