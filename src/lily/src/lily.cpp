/*
 * @Author: windzu windzu1@gmail.com
 * @Date: 2023-06-16 17:34:42
 * @LastEditors: wind windzu1@gmail.com
 * @LastEditTime: 2023-11-24 15:33:50
 * @Description:
 * Copyright (c) 2023 by windzu, All Rights Reserved.
 */

#include "lily/lily.h"

Lily::Lily(ros::NodeHandle nh, ros::NodeHandle pnh) {
  nh_ = nh;
  pnh_ = pnh;

  pnh_.param<std::string>("config_path", config_path_, "");
  pnh_.param<bool>("manual_mode", manual_mode_, "");

  if (!init()) {
    ROS_ERROR("init failed");
    return;
  }

  if (manual_mode_) {
    server_.reset(new dynamic_reconfigure::Server<dynamic_tf_config::dynamicConfig>(mutex_, pnh_));
    server_f_ = boost::bind(&Lily::dynamic_config_callback, this, _1);
    server_->setCallback(server_f_);

    ros::Rate rate(10);
    while (ros::ok()) {
      flash_status_bar();
      trans_and_pub();
      ros::spinOnce();
      rate.sleep();
    }
  } else {
    std::cout << "-------------------------" << std::endl;
    std::cout << "Collection" << std::endl;

    ros::Rate rate(10);
    while (ros::ok() && !cloud_map_full_check()) {
      ros::spinOnce();
      rate.sleep();
    }

    std::cout << "-------------------------" << std::endl;
    std::cout << "Calibration" << std::endl;

    // debug
    // echo tf_matrix_map_
    std::cout << "before calibration:" << std::endl;
    std::cout << "-------------------------" << std::endl;
    for (auto iter = tf_matrix_map_.begin(); iter != tf_matrix_map_.end(); iter++) {
      std::cout << iter->first << std::endl;
      std::cout << iter->second << std::endl;
    }
    std::cout << "-------------------------" << std::endl;

    // calibration
    calibrator_.reset(new Calibrator(num_iter_, num_lpr_, th_seeds_, th_dist_));
    tf_matrix_map_ = calibrator_->process(cloud_map_, main_topic_, points_map_, tf_matrix_map_);

    // debug
    std::cout << "after calibration:" << std::endl;
    std::cout << "-------------------------" << std::endl;
    for (auto iter = tf_matrix_map_.begin(); iter != tf_matrix_map_.end(); iter++) {
      std::cout << iter->first << std::endl;
      std::cout << iter->second << std::endl;
    }
    std::cout << "-------------------------" << std::endl;
  }

  std::cout << "-------------------------" << std::endl;
  std::cout << "Saving" << std::endl;
  // save
  save_config();
  ros::shutdown();
  return;
}

bool Lily::init() {
  // 1. load config
  config_ = YAML::LoadFile(config_path_);

  // 2, iter config_
  for (auto iter = config_.begin(); iter != config_.end(); iter++) {
    std::string topic = iter->first.as<std::string>();

    // translation is a vector of [x, y, z]
    // rotation is a vector of [w, x, y, z]
    std::vector<double> translation =
        iter->second["transform"]["translation"].as<std::vector<double>>();
    std::vector<double> rotation = iter->second["transform"]["rotation"].as<std::vector<double>>();

    std::vector<double> euler_angles_vec = quaternion_to_euler_angles(rotation);
    Eigen::Matrix4d tf_matrix =
        calculate_tf_matrix_from_translation_and_rotation(translation, rotation);

    // set dynamic_config_map_
    dynamic_tf_config::dynamicConfig config;
    config.lidar_topic = topic;
    config.x = translation[0];
    config.y = translation[1];
    config.z = translation[2];
    config.roll = euler_angles_vec[0];
    config.pitch = euler_angles_vec[1];
    config.yaw = euler_angles_vec[2];
    dynamic_config_map_[topic] = config;

    // find main topic
    bool is_main = iter->second["is_main"].as<bool>();
    if (is_main) {
      main_topic_ = topic;
    }

    // check if need load from pcd file
    bool load_from_file = iter->second["load_from_file"].as<bool>();
    if (load_from_file) {
      std::string file_path = iter->second["file_path"].as<std::string>();
      pcl::PointCloud<pcl::PointXYZI>::Ptr cloud(new pcl::PointCloud<pcl::PointXYZI>);
      if (pcl::io::loadPCDFile<pcl::PointXYZI>(file_path, *cloud) == -1) {
        ROS_ERROR("load pcd file %s failed", file_path.c_str());
        return false;
      }
      cloud_map_[topic] = cloud;
    } else {
      cloud_map_[topic] = nullptr;
    }

    // load points
    points_map_[topic] = std::vector<pcl::PointXYZ>();
    if (iter->second["use_points"]) {
      // subscribe /clicked_point topic
      ros::Subscriber sub = nh_.subscribe<geometry_msgs::PointStamped>(
          "/clicked_point", 1, boost::bind(&Lily::clicked_point_callback, this, _1, topic));

      // publish transformed cloud and selected points from cloud
      ros::Publisher pub = nh_.advertise<sensor_msgs::PointCloud2>(topic + "/calibrated", 1);

      pcl::PointCloud<pcl::PointXYZI>::Ptr transformed_cloud(new pcl::PointCloud<pcl::PointXYZI>);
      pcl::transformPointCloud(*(cloud_map_[topic]), *transformed_cloud, tf_matrix);
      sensor_msgs::PointCloud2::Ptr pc_msg(new sensor_msgs::PointCloud2);
      pcl::toROSMsg(*transformed_cloud, *pc_msg);

      pc_msg->header.frame_id = "base_link";

      // publish
      pub.publish(pc_msg);

      // ros spin
      ros::Rate rate(10);
      while (ros::ok() && points_map_[topic].size() < min_points_num_) {
        pub.publish(pc_msg);
        ros::spinOnce();
        rate.sleep();
      }
    }

    ros::Subscriber sub = nh_.subscribe<sensor_msgs::PointCloud2>(
        topic, 1, boost::bind(&Lily::callback, this, _1, topic));
    ros::Publisher pub = nh_.advertise<sensor_msgs::PointCloud2>(topic + "/calibrated", 1);

    subs_.push_back(sub);
    pubs_map_[topic] = pub;
    tf_matrix_map_[topic] = tf_matrix;
  }

  return true;
}

bool Lily::cloud_map_full_check() {
  for (auto iter = cloud_map_.begin(); iter != cloud_map_.end(); iter++) {
    if (iter->second == nullptr) {
      return false;
    }
  }
  return true;
}

void Lily::save_config() {
  if (manual_mode_) {
    for (auto iter = dynamic_config_map_.begin(); iter != dynamic_config_map_.end(); iter++) {
      std::string topic = iter->first;
      dynamic_tf_config::dynamicConfig dynamic_config = iter->second;

      Eigen::Matrix4f tf_matrix = Eigen::Matrix4f::Identity();
      Eigen::Translation3f tl(dynamic_config.x, dynamic_config.y, dynamic_config.z);
      Eigen::AngleAxisf rot_x(dynamic_config.roll, Eigen::Vector3f::UnitX());
      Eigen::AngleAxisf rot_y(dynamic_config.pitch, Eigen::Vector3f::UnitY());
      Eigen::AngleAxisf rot_z(dynamic_config.yaw, Eigen::Vector3f::UnitZ());
      tf_matrix = (tl * rot_z * rot_y * rot_x).matrix();

      // convert transform matrix to translation and rotation
      std::vector<double> translation_vec =
          transform_matrix_to_translation(tf_matrix.cast<double>());
      std::vector<double> quaternion_vec = transform_matrix_to_quaternion(tf_matrix.cast<double>());
      std::vector<double> euler_angles_vec =
          transform_matrix_to_euler_angles(tf_matrix.cast<double>());

      // save to config_
      config_[topic]["transform"]["translation"] = translation_vec;
      config_[topic]["transform"]["rotation"] = quaternion_vec;
      config_[topic]["transform"]["rotation_euler"] = euler_angles_vec;
    }

  } else {
    for (auto iter = tf_matrix_map_.begin(); iter != tf_matrix_map_.end(); iter++) {
      std::string topic = iter->first;
      Eigen::Matrix4d tf_matrix = iter->second;

      // convert transform matrix to translation and rotation
      std::vector<double> translation_vec =
          transform_matrix_to_translation(tf_matrix.cast<double>());
      std::vector<double> quaternion_vec = transform_matrix_to_quaternion(tf_matrix.cast<double>());
      std::vector<double> euler_angles_vec =
          transform_matrix_to_euler_angles(tf_matrix.cast<double>());

      // save to config_
      config_[topic]["transform"]["translation"] = translation_vec;
      config_[topic]["transform"]["rotation"] = quaternion_vec;
      config_[topic]["transform"]["rotation_euler"] = euler_angles_vec;
    }
  }

  //   // iter config_
  //   for (auto iter = config_.begin(); iter != config_.end(); iter++) {
  //     std::string topic = iter->first.as<std::string>();
  //     double x = iter->second["tf_x"].as<double>();
  //     double y = iter->second["tf_y"].as<double>();
  //     double z = iter->second["tf_z"].as<double>();
  //     double roll = iter->second["tf_roll"].as<double>();
  //     double pitch = iter->second["tf_pitch"].as<double>();
  //     double yaw = iter->second["tf_yaw"].as<double>();
  //
  //     x = std::round(x * 1000.0) / 1000.0;
  //     y = std::round(y * 1000.0) / 1000.0;
  //     z = std::round(z * 1000.0) / 1000.0;
  //     roll = std::round(roll * 1000.0) / 1000.0;
  //     pitch = std::round(pitch * 1000.0) / 1000.0;
  //     yaw = std::round(yaw * 1000.0) / 1000.0;
  //
  //     config_[topic]["tf_x"] = x;
  //     config_[topic]["tf_y"] = y;
  //     config_[topic]["tf_z"] = z;
  //     config_[topic]["tf_roll"] = roll;
  //     config_[topic]["tf_pitch"] = pitch;
  //     config_[topic]["tf_yaw"] = yaw;
  //   }

  // save config
  std::string save_path = config_path_ + "_" + current_date_time();
  std::ofstream fout(save_path);
  fout << config_;
  fout.close();

  ROS_INFO("save config success");
  return;
}

void Lily::callback(const sensor_msgs::PointCloud2::ConstPtr& msg, const std::string& topic_name) {
  pcl::PointCloud<pcl::PointXYZI>::Ptr cloud(new pcl::PointCloud<pcl::PointXYZI>);
  pcl::fromROSMsg(*msg, *cloud);
  cloud_map_[topic_name] = cloud;
  return;
}

void Lily::trans_and_pub() {
  for (auto iter = cloud_map_.begin(); iter != cloud_map_.end(); iter++) {
    if (iter->second != nullptr) {
      std::string topic = iter->first;
      pcl::PointCloud<pcl::PointXYZI>::Ptr cloud(new pcl::PointCloud<pcl::PointXYZI>);
      pcl::transformPointCloud(*(iter->second), *cloud, tf_matrix_map_[topic]);
      sensor_msgs::PointCloud2::Ptr pc_msg(new sensor_msgs::PointCloud2);
      pcl::toROSMsg(*cloud, *pc_msg);
      pc_msg->header.frame_id = "base_link";
      pubs_map_[topic].publish(pc_msg);
    }
  }
  return;
}

void Lily::clicked_point_callback(const geometry_msgs::PointStamped::ConstPtr& msg,
                                  const std::string& topic_name) {
  if (points_map_.find(topic_name) == points_map_.end()) {
    ROS_ERROR("topic %s not in points_map_", topic_name.c_str());
    return;
  }

  pcl::PointXYZ point;
  point.x = msg->point.x;
  point.y = msg->point.y;
  point.z = msg->point.z;
  points_map_[topic_name].push_back(point);

  // ros info
  ROS_INFO("topic %s, point: (%f, %f, %f)", topic_name.c_str(), point.x, point.y, point.z);

  if (points_map_[topic_name].size() == min_points_num_) {
    //
  }

  return;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// auto mode
///////////////////////////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////////////////////////
// mannual mode
///////////////////////////////////////////////////////////////////////////////////////////////////
void Lily::dynamic_config_callback(dynamic_tf_config::dynamicConfig config) {
  std::string topic = config.lidar_topic;

  // check if need save config
  if (topic == "save") {
    save_config();
    return;
  }

  // check topic if in dynamic_config_map_
  if (dynamic_config_map_.find(topic) == dynamic_config_map_.end()) {
    ROS_ERROR("topic %s not in dynamic_config_map_", topic.c_str());
    return;
  }

  // update tf_matrix_map_
  // 切换topic时，是用对应lidar的参数更新状态栏
  if (topic != last_topic_name_) {
    ROS_WARN("change lidar_topic, will update tf_matrix_map_");

    last_topic_name_ = topic;
    flash_status_bar_flag_ = true;
    temp_config_ = dynamic_config_map_[topic];
    return;
  }

  // update dynamic_config_map_
  dynamic_config_map_[topic] = config;

  if (cloud_map_[topic] != nullptr) {
    Eigen::Matrix4d tf_matrix = Eigen::Matrix4d::Identity();
    Eigen::Translation3d tl(config.x, config.y, config.z);
    Eigen::AngleAxisd rot_x(config.roll, Eigen::Vector3d::UnitX());
    Eigen::AngleAxisd rot_y(config.pitch, Eigen::Vector3d::UnitY());
    Eigen::AngleAxisd rot_z(config.yaw, Eigen::Vector3d::UnitZ());
    tf_matrix = (tl * rot_z * rot_y * rot_x).matrix();
    tf_matrix_map_[topic] = tf_matrix;
  }

  return;
}

void Lily::flash_status_bar() {
  if (flash_status_bar_flag_) {
    server_->updateConfig(temp_config_);
    flash_status_bar_flag_ = false;
  }
  return;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// utils
///////////////////////////////////////////////////////////////////////////////////////////////////
Eigen::Vector3d Lily::rotation_matrix_to_euler_angles(const Eigen::Matrix3d& R) {
  // assert(isRotationMatrix(R));
  double sy = sqrt(R(0, 0) * R(0, 0) + R(1, 0) * R(1, 0));

  double singular = sy < 1e-6;  // If

  double x, y, z;
  if (!singular) {
    x = atan2(R(2, 1), R(2, 2));
    y = atan2(-R(2, 0), sy);
    z = atan2(R(1, 0), R(0, 0));
  } else {
    x = atan2(-R(1, 2), R(1, 1));
    y = atan2(-R(2, 0), sy);
    z = 0;
  }
  return Eigen::Vector3d(x, y, z);
}

std::vector<double> Lily::transform_matrix_to_euler_angles(const Eigen::Matrix4d& T) {
  Eigen::Matrix3d R = T.block<3, 3>(0, 0);
  Eigen::Vector3d euler_angles = rotation_matrix_to_euler_angles(R);
  std::vector<double> euler_angles_vec(euler_angles.data(),
                                       euler_angles.data() + euler_angles.size());
  return euler_angles_vec;
}

std::vector<double> Lily::transform_matrix_to_quaternion(const Eigen::Matrix4d& T) {
  Eigen::Matrix3d R = T.block<3, 3>(0, 0);
  Eigen::Quaterniond quat(R);
  quat.normalize();  // 正规化四元数以确保其表示有效的旋转
  Eigen::Vector4d quaternion = Eigen::Vector4d(quat.w(), quat.x(), quat.y(), quat.z());
  std::vector<double> quaternion_vec(quaternion.data(), quaternion.data() + quaternion.size());
  return quaternion_vec;
}

std::vector<double> Lily::transform_matrix_to_translation(const Eigen::Matrix4d& T) {
  Eigen::Vector3d translation = T.block<3, 1>(0, 3);
  std::vector<double> translation_vec(translation.data(), translation.data() + translation.size());
  return translation_vec;
}

std::vector<double> Lily::quaternion_to_euler_angles(const std::vector<double>& q) {
  Eigen::Quaterniond quat(q[0], q[1], q[2], q[3]);
  quat.normalize();  // 正规化四元数以确保其表示有效的旋转
  Eigen::Vector3d euler_angles = quat.toRotationMatrix().eulerAngles(0, 1, 2);
  std::vector<double> euler_angles_vec(euler_angles.data(),
                                       euler_angles.data() + euler_angles.size());
  return euler_angles_vec;
}

Eigen::Matrix4d Lily::calculate_tf_matrix_from_translation_and_rotation(
    const std::vector<double>& translation, const std::vector<double>& rotation) {
  Eigen::Matrix4d tf_matrix = Eigen::Matrix4d::Identity();

  // calculate tf_matrix_map_ from translation and rotation(quat)
  // - translation is a vector of [x, y, z]
  // - rotation is a vector of [w, x, y, z]
  // - tf_matrix is a 4x4 matrix

  Eigen::Translation3d trans(translation[0], translation[1], translation[2]);
  Eigen::Quaterniond quat(rotation[0], rotation[1], rotation[2], rotation[3]);
  quat.normalize();  // 正规化四元数以确保其表示有效的旋转

  Eigen::Affine3d transform =
      Eigen::Translation3d(translation[0], translation[1], translation[2]) * quat;
  tf_matrix = transform.matrix();

  return tf_matrix;
}

Eigen::Matrix4d Lily::calculate_tf_matrix_by_points(const std::string topic,
                                                    const std::vector<double>& rotation) {
  Eigen::Matrix4d tf_matrix = Eigen::Matrix4d::Identity();

  // subscribe /clicked_point topic
  ros::Subscriber sub = nh_.subscribe<geometry_msgs::PointStamped>(
      "/clicked_point", 1, boost::bind(&Lily::clicked_point_callback, this, _1, topic));

  // publish transformed cloud and selected points from cloud
  ros::Publisher pub = nh_.advertise<sensor_msgs::PointCloud2>(topic + "/calibrated", 1);

  pcl::PointCloud<pcl::PointXYZI>::Ptr transformed_cloud(new pcl::PointCloud<pcl::PointXYZI>);
  pcl::transformPointCloud(*(cloud_map_[topic]), *transformed_cloud, tf_matrix);
  sensor_msgs::PointCloud2::Ptr pc_msg(new sensor_msgs::PointCloud2);
  pcl::toROSMsg(*transformed_cloud, *pc_msg);

  pc_msg->header.frame_id = "base_link";

  // publish
  pub.publish(pc_msg);

  // ros spin
  ros::Rate rate(10);
  while (ros::ok() && points_map_[topic].size() < min_points_num_) {
    pub.publish(pc_msg);
    ros::spinOnce();
    rate.sleep();
  }
}

std::string Lily::current_date_time() {
  auto now = std::chrono::system_clock::now();
  auto in_time_t = std::chrono::system_clock::to_time_t(now);

  std::stringstream ss;
  ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d %X");
  return ss.str();
}