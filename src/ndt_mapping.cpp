#include "ndt_mapping.h"

ndt_mapping::ndt_mapping() 
{

  points_sub_ = nh_.subscribe("points_raw", 100000, &ndt_mapping::points_callback,this);
  ndt_map_pub_ = nh_.advertise<sensor_msgs::PointCloud2>("/ndt_map", 1000);
  current_pose_pub_ = nh_.advertise<geometry_msgs::PoseStamped>("/current_pose", 1000);

  // Default values
  nh_.param("max_iter", max_iter_, 30);
  nh_.param("step_size", step_size_, 0.1);
  nh_.param("ndt_res", ndt_res_, 5.0);
  nh_.param("trans_eps", trans_eps_, 0.01);
  nh_.param("voxel_leaf_size", voxel_leaf_size_, 2.0);

  initial_scan_loaded = 0;
  min_add_scan_shift_ = 1.0;

  _tf_x=0.0, _tf_y=0.0, _tf_z=0.0, _tf_roll=0.0, _tf_pitch=0.0, _tf_yaw=0.0;

  std::cout << "incremental_voxel_update: " << _incremental_voxel_update << std::endl;
  std::cout << "(tf_x,tf_y,tf_z,tf_roll,tf_pitch,tf_yaw): (" << _tf_x << ", " << _tf_y << ", " << _tf_z << ", "
            << _tf_roll << ", " << _tf_pitch << ", " << _tf_yaw << ")" << std::endl;

  Eigen::Translation3f tl_btol(_tf_x, _tf_y, _tf_z);                 // tl: translation
  Eigen::AngleAxisf rot_x_btol(_tf_roll, Eigen::Vector3f::UnitX());  // rot: rotation
  Eigen::AngleAxisf rot_y_btol(_tf_pitch, Eigen::Vector3f::UnitY());
  Eigen::AngleAxisf rot_z_btol(_tf_yaw, Eigen::Vector3f::UnitZ());
  tf_btol_ = (tl_btol * rot_z_btol * rot_y_btol * rot_x_btol).matrix();
  tf_ltob_ = tf_btol_.inverse();

  map_.header.frame_id = "map";

  current_pose_.x = current_pose_.y = current_pose_.z = 0.0;current_pose_.roll = current_pose_.pitch = current_pose_.yaw = 0.0;
  previous_pose_.x = previous_pose_.y = previous_pose_.z = 0.0;previous_pose_.roll = previous_pose_.pitch = previous_pose_.yaw = 0.0;

  voxel_grid_filter_.setLeafSize(voxel_leaf_size_, voxel_leaf_size_, voxel_leaf_size_);  

  ndt.setTransformationEpsilon(trans_eps_);
  ndt.setStepSize(step_size_);
  ndt.setResolution(ndt_res_);
  ndt.setMaximumIterations(max_iter_);

  is_first_map_ = true;

}; 

ndt_mapping::~ndt_mapping(){}; 

void ndt_mapping::points_callback(const sensor_msgs::PointCloud2::ConstPtr& input)
{
  pcl::PointCloud<pcl::PointXYZI> scan;
  pcl::PointCloud<pcl::PointXYZI>::Ptr filtered_scan_ptr(new pcl::PointCloud<pcl::PointXYZI>());
  pcl::PointCloud<pcl::PointXYZI>::Ptr transformed_scan_ptr(new pcl::PointCloud<pcl::PointXYZI>());
  tf::Quaternion q;

  Eigen::Matrix4f t_localizer(Eigen::Matrix4f::Identity());
  Eigen::Matrix4f t_base_link(Eigen::Matrix4f::Identity());
  //static tf::TransformBroadcaster br;
  tf::Transform transform;

  pcl::fromROSMsg(*input, scan);
  pcl::PointCloud<pcl::PointXYZI>::Ptr scan_ptr(new pcl::PointCloud<pcl::PointXYZI>(scan));

  // Add initial point cloud to velodyne_map
  if (initial_scan_loaded == 0)
  {
    pcl::transformPointCloud(*scan_ptr, *transformed_scan_ptr, tf_btol_);
    map_ += *transformed_scan_ptr;
    initial_scan_loaded = 1;
  }

  voxel_grid_filter_.setInputCloud(scan_ptr);
  voxel_grid_filter_.filter(*filtered_scan_ptr);
  ndt.setInputSource(filtered_scan_ptr);

  pcl::PointCloud<pcl::PointXYZI>::Ptr map_ptr(new pcl::PointCloud<pcl::PointXYZI>(map_));
  if (is_first_map_ == true){
    ndt.setInputTarget(map_ptr);
    is_first_map_ = false;
  }

  Eigen::Translation3f init_translation(current_pose_.x, current_pose_.y, current_pose_.z);
  Eigen::AngleAxisf init_rotation_x(current_pose_.roll, Eigen::Vector3f::UnitX());
  Eigen::AngleAxisf init_rotation_y(current_pose_.pitch, Eigen::Vector3f::UnitY());
  Eigen::AngleAxisf init_rotation_z(current_pose_.yaw, Eigen::Vector3f::UnitZ());

  Eigen::Matrix4f init_guess =
      (init_translation * init_rotation_z * init_rotation_y * init_rotation_x).matrix() * tf_btol_;

  pcl::PointCloud<pcl::PointXYZI>::Ptr output_cloud(new pcl::PointCloud<pcl::PointXYZI>);
 
  ndt.align(*output_cloud, init_guess);
  double fitness_score = ndt.getFitnessScore();
  t_localizer = ndt.getFinalTransformation();
  bool has_converged = ndt.hasConverged();
  int final_num_iteration = ndt.getFinalNumIteration();
  double transformation_probability = ndt.getTransformationProbability();

  t_base_link = t_localizer * tf_ltob_;

  pcl::transformPointCloud(*scan_ptr, *transformed_scan_ptr, t_localizer);

  tf::Matrix3x3 mat_b;
  mat_b.setValue(static_cast<double>(t_base_link(0, 0)), static_cast<double>(t_base_link(0, 1)),
                 static_cast<double>(t_base_link(0, 2)), static_cast<double>(t_base_link(1, 0)),
                 static_cast<double>(t_base_link(1, 1)), static_cast<double>(t_base_link(1, 2)),
                 static_cast<double>(t_base_link(2, 0)), static_cast<double>(t_base_link(2, 1)),
                 static_cast<double>(t_base_link(2, 2)));

  // Update current_pose_.
  current_pose_.x = t_base_link(0, 3);current_pose_.y = t_base_link(1, 3);current_pose_.z = t_base_link(2, 3);
  mat_b.getRPY(current_pose_.roll, current_pose_.pitch, current_pose_.yaw, 1);//mat2rpy

  transform.setOrigin(tf::Vector3(current_pose_.x, current_pose_.y, current_pose_.z));
  q.setRPY(current_pose_.roll, current_pose_.pitch, current_pose_.yaw); //q from rpy
  transform.setRotation(q);//trans from q

  br_.sendTransform(tf::StampedTransform(transform, input->header.stamp, "map", "base_link"));

  double shift = sqrt(pow(current_pose_.x - previous_pose_.x, 2.0) + pow(current_pose_.y - previous_pose_.y, 2.0));
  if (shift >= min_add_scan_shift_)
  {
    map_ += *transformed_scan_ptr;
    previous_pose_.x = current_pose_.x;previous_pose_.y = current_pose_.y;previous_pose_.z = current_pose_.z;
    previous_pose_.roll = current_pose_.roll;previous_pose_.pitch = current_pose_.pitch;previous_pose_.yaw = current_pose_.yaw;
    ndt.setInputTarget(map_ptr);

    sensor_msgs::PointCloud2::Ptr map_msg_ptr(new sensor_msgs::PointCloud2);
    pcl::toROSMsg(*map_ptr, *map_msg_ptr);
    ndt_map_pub_.publish(*map_msg_ptr);
  }

  //sensor_msgs::PointCloud2::Ptr map_msg_ptr(new sensor_msgs::PointCloud2);
  //pcl::toROSMsg(*map_ptr, *map_msg_ptr);
  //ndt_map_pub_.publish(*map_msg_ptr);// it makes rviz very slow.

  current_pose_msg_.header.frame_id = "map";
  current_pose_msg_.header.stamp = input->header.stamp;
  current_pose_msg_.pose.position.x = current_pose_.x;current_pose_msg_.pose.position.y = current_pose_.y;current_pose_msg_.pose.position.z = current_pose_.z;
  current_pose_msg_.pose.orientation.x = q.x();current_pose_msg_.pose.orientation.y = q.y();current_pose_msg_.pose.orientation.z = q.z();current_pose_msg_.pose.orientation.w = q.w();

  current_pose_pub_.publish(current_pose_msg_);

  std::cout << "-----------------------------------------------------------------" << std::endl;
  std::cout << "Sequence number: " << input->header.seq << std::endl;
  std::cout << "Number of scan points: " << scan_ptr->size() << " points." << std::endl;
  std::cout << "Number of filtered scan points: " << filtered_scan_ptr->size() << " points." << std::endl;
  std::cout << "transformed_scan_ptr: " << transformed_scan_ptr->points.size() << " points." << std::endl;
  std::cout << "map: " << map_.points.size() << " points." << std::endl;
  std::cout << "NDT has converged: " << ndt.hasConverged() << std::endl;
  std::cout << "Fitness score: " << fitness_score << std::endl;
  std::cout << "Number of iteration: " << ndt.getFinalNumIteration() << std::endl;
  std::cout << "(x,y,z,roll,pitch,yaw):" << std::endl;
  std::cout << "(" << current_pose_.x << ", " << current_pose_.y << ", " << current_pose_.z << ", " << current_pose_.roll
            << ", " << current_pose_.pitch << ", " << current_pose_.yaw << ")" << std::endl;
  std::cout << "Transformation Matrix:" << std::endl;
  std::cout << t_localizer << std::endl;
  std::cout << "shift: " << shift << std::endl;
  std::cout << "-----------------------------------------------------------------" << std::endl;
}


