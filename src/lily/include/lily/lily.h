/*
 * @Author: windzu windzu1@gmail.com
 * @Date: 2023-07-05 11:38:22
 * @LastEditors: windzu windzu1@gmail.com
 * @LastEditTime: 2023-10-19 01:55:53
 * @Description:
 * Copyright (c) 2023 by windzu, All Rights Reserved.
 */

#pragma once
#include <dynamic_reconfigure/server.h>
#include <lily/dynamicConfig.h>

#include <pcl_conversions/pcl_conversions.h>
#include <pcl_ros/transforms.h>
#include <ros/ros.h>
#include <sensor_msgs/PointCloud2.h>

#include <boost/thread.hpp>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <memory>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <string>
#include <tf/tf.h>
#include <unordered_map>
#include <vector>
#include <yaml-cpp/yaml.h>

class Lily {
public:
  Lily(ros::NodeHandle nh, ros::NodeHandle pnh);

private:
  bool init();
  void callback(const sensor_msgs::PointCloud2::ConstPtr &msg,
                const std::string &topic_name);
  void trans_and_pub();
  bool cloud_map_full_check();
  void dynamic_config_callback(dynamic_tf_config::dynamicConfig config);
  void flash_status_bar();
  void save_config();
  std::string current_date_time();

private:
  // ros
  ros::NodeHandle nh_;
  ros::NodeHandle pnh_;
  std::string config_path_;
  bool manual_mode_ = false;
  std::string main_topic_name_ = "";

  YAML::Node config_;

  // variables
  std::vector<ros::Subscriber> subs_;
  std::unordered_map<std::string, ros::Publisher> pubs_map_;

  std::unordered_map<std::string, pcl::PointCloud<pcl::PointXYZI>::Ptr>
      cloud_map_;
  std::unordered_map<std::string, Eigen::Matrix4d> tf_matrix_map_;

  // dynamic reconfigure
  boost::recursive_mutex mutex_;
  std::shared_ptr<dynamic_reconfigure::Server<dynamic_tf_config::dynamicConfig>>
      server_;
  dynamic_reconfigure::Server<dynamic_tf_config::dynamicConfig>::CallbackType
      server_f_;
  std::string last_topic_name_ = "";
  std::unordered_map<std::string, dynamic_tf_config::dynamicConfig>
      dynamic_config_map_;
  bool flash_status_bar_flag_ = false;
  dynamic_tf_config::dynamicConfig temp_config_;
};
