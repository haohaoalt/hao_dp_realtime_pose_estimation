// Copyright 2022 Simon Erik Nylund.
// Author: snenyl

#include "PoseEstimation/PoseEstimation.h"

void PoseEstimation::run_pose_estimation() {
  rs2::frameset frames = p.wait_for_frames();
  rs2::video_frame image = frames.get_color_frame();
  rs2::depth_frame depth = frames.get_depth_frame();

  realsense_points_ = realsense_pointcloud_.calculate(depth);
  pcl_points_ = points_to_pcl(realsense_points_);

  detection_output_struct_ = object_detection_object_.get_detection();

  if (std::chrono::system_clock::now() > start_debug_time_ && enable_debug_mode_) {
    std::cout << " X: " << detection_output_struct_.x
              << " Y: " << detection_output_struct_.y
              << " Width: " << detection_output_struct_.width
              << " Height: " << detection_output_struct_.height
              << " Conf: " << detection_output_struct_.confidence << std::endl;
  }

  calculate_3d_crop();
  edit_pointcloud();

  if (std::chrono::system_clock::now() > start_debug_time_ && enable_debug_mode_) {
    std::cout << "cloud_pallet_->size(): " << cloud_pallet_->size() << std::endl;
  }

  if (detection_output_struct_.width > minimum_object_detection_width_pixels_ &&
      detection_output_struct_.height > minimum_object_detection_height_pixels_ &&
      wait_with_ransac_for_ > minimum_iterations_before_ransac_) {
    calculate_ransac();
  }
  if (ransac_model_coefficients_.size() > minimum_ransac_coefficients_) {
    calculate_pose_vector();
  }
  wait_with_ransac_for_++;

  view_pointcloud();

  const int w = image.as<rs2::video_frame>().get_width();
  const int h = image.as<rs2::video_frame>().get_height();

  cv::Mat cv_image(cv::Size(w, h),
                   CV_8UC3,
                   (void *) image.get_data(),  // TODO(simon) Using C-style cast.  Use reinterpret_cast<void *>(...) instead.
                   cv::Mat::AUTO_STEP);
  cv::cvtColor(cv_image, cv_image, cv::COLOR_BGR2RGB);

  image_ = cv_image;

  calculate_aruco();
  object_detection_object_.run_object_detection(image_);
  calculate_pose();

  log_data(image.get_frame_number());
  cv::imshow(opencv_image_window_name_, cv_image);
  cv::waitKey(cv_waitkey_delay_);

  ransac_model_coefficients_.clear();
}

void PoseEstimation::setup_pose_estimation() {
  rosbag_path_ = std::filesystem::current_path().parent_path() / rosbag_relative_path_;

  if (load_from_rosbag) {
    std::cout << "Loaded rosbag: " << rosbag_path_ << std::endl;
    rs2::config cfg;
    cfg.enable_device_from_file(rosbag_path_, !single_run_);
    auto profile = p.start(cfg);
    auto dev = profile.get_device();

    if (auto p = dev.as<rs2::playback>()) {
      p.set_real_time(realsense_skip_frames_);
    }

  } else if (!load_from_rosbag) {
    p.start();
  }

  if (enable_logger_) {
    std::ofstream LoggerFile(std::filesystem::current_path().parent_path() / "log/data_out.csv");
    LoggerFile << "frame,p_x,p_y,p_z,p_r,p_p,p_y,a_x,a_y,a_z,a_r,a_p,a_y" << std::endl;
    LoggerFile.close();
  }

  set_camera_parameters();

  object_detection_object_.set_model_path(object_detection_model_relative_path_);
  object_detection_object_.set_object_detection_settings(object_detection_nms_threshold_,
                                                         object_detection_bbox_conf_threshold_);
  object_detection_object_.setup_object_detection();
  pcl::visualization::PCLVisualizer::Ptr
      viewer(new pcl::visualization::PCLVisualizer(pcl_window_name_));
  viewer_ = viewer;

  std::cout << "Setup" << std::endl;
}

void PoseEstimation::calculate_aruco() {
  cv::aruco::detectMarkers(image_,
                           dictionary_,
                           markerCorners_,
                           markerIds_,
                           parameters_,
                           rejectedCandidates_);

  if (markerCorners_.size() > minimum_marker_corners_) {
    cv::drawMarker(image_,
                   markerCorners_.at(0).at(0),
                   cv::Scalar(0, 0, 255));  // TODO(simon) Magic number.
    cv::drawMarker(image_,
                   markerCorners_.at(0).at(1),
                   cv::Scalar(0, 0, 255));  // TODO(simon) Magic number.
    cv::drawMarker(image_,
                   markerCorners_.at(0).at(2),
                   cv::Scalar(0, 0, 255));  // TODO(simon) Magic number.
    cv::drawMarker(image_,
                   markerCorners_.at(0).at(3),
                   cv::Scalar(0, 0, 255));  // TODO(simon) Magic number.
  }
}

void PoseEstimation::calculate_pose() {
  std::vector<cv::Vec3d> rvecs, tvecs, object_points;
  cv::aruco::estimatePoseSingleMarkers(markerCorners_,
                                       april_tag_marker_length_meter_,
                                       example_camera_matrix_,
                                       example_dist_coefficients_,
                                       rvecs,
                                       tvecs,
                                       object_points);  // TODO(simon) Magic number.
  // TODO(simon) Set marker size as parameter. 0.175 0.535

  if (!rvecs.empty() && !tvecs.empty()) {
    double z_axis_data[3] = {0, 0, 1};  // TODO(simon) Magic number.
    cv::Mat Rot(3, 3, CV_64F), Jacob;  // TODO(simon) Magic number.
    cv::Mat z_axis(1, 3, CV_64F, z_axis_data);  // TODO(simon) Magic number.
    cv::Rodrigues(rvecs, Rot, Jacob);

    if (enable_debug_mode_) {
      std::cout << "z_axis: " << z_axis << std::endl;
      std::cout << "Rot: " << Rot << std::endl;
    }

    ground_truth_vector_ = z_axis * Rot;
  }
  if (!rvecs.empty() && !tvecs.empty()) {
    std::stringstream rotation;
    std::stringstream translation;

    rvecs_ = rvecs;
    tvecs_ = tvecs;

    rotation << "[" << rvecs.at(0)[0] << ", " << rvecs.at(0)[1] << ", " << rvecs.at(0)[2]
             << "]";  // TODO(simon) Magic number.
    translation << tvecs.at(0);  // TODO(simon) Magic number.
    cv::putText(image_,
                rotation.str(),
                cv::Point(50, 50),
                cv::FONT_HERSHEY_DUPLEX,
                1,
                cv::Scalar(0, 255, 0),
                2,
                false);  // TODO(simon) Magic number.
    cv::putText(image_,
                translation.str(),
                cv::Point(50, 100),
                cv::FONT_HERSHEY_DUPLEX,
                1,
                cv::Scalar(0, 255, 0),
                2,
                false);  // TODO(simon) Magic number.
    cv::aruco::drawAxis(image_,
                        example_camera_matrix_,
                        example_dist_coefficients_,
                        rvecs,
                        tvecs,
                        april_tag_marker_length_meter_ / 2);  // TODO(simon) Magic number.
  }
}

void PoseEstimation::set_camera_parameters() {  // TODO(simon) Not in use.
  camera_matrix_.resize(9);  // TODO(simon) Magic number.
  dist_coefficients_.resize(5);  // TODO(simon) Magic number.

  std::fill(camera_matrix_.begin(), camera_matrix_.end(), 0);  // TODO(simon) Magic number.
  std::fill(dist_coefficients_.begin(), dist_coefficients_.end(), 0);  // TODO(simon) Magic number.

  camera_matrix_.at(0) = 907.114;  // fx  // TODO(simon) Magic number.
  camera_matrix_.at(2) = 662.66;  // cx  // TODO(simon) Magic number.
  camera_matrix_.at(4) = 907.605;  // fy  // TODO(simon) Magic number.
  camera_matrix_.at(5) = 367.428;  // cy  // TODO(simon) Magic number.
  camera_matrix_.at(8) = 1;  // 1  // TODO(simon) Magic number.

  dist_coefficients_.at(0) = 0.157553;  // TODO(simon) Magic number.
  dist_coefficients_.at(1) = -0.501105;   // TODO(simon) Magic number.
  dist_coefficients_.at(2) = -0.00164696;  // TODO(simon) Magic number.
  dist_coefficients_.at(3) = 0.000623876;  // TODO(simon) Magic number.
  dist_coefficients_.at(4) = 0.466404;  // TODO(simon) Magic number.
}

pcl::PointCloud<pcl::PointXYZ>::Ptr PoseEstimation::points_to_pcl(const rs2::points &points) {
  pcl::PointCloud<pcl::PointXYZ>::Ptr cloud(new pcl::PointCloud<pcl::PointXYZ>);

  auto sp = points.get_profile().as<rs2::video_stream_profile>();
  cloud->width = sp.width();
  cloud->height = sp.height();
  cloud->is_dense = false;
  cloud->points.resize(points.size());
  auto ptr = points.get_vertices();
  for (auto &p : cloud->points) {
    p.x = ptr->x;
    p.y = ptr->y;
    p.z = ptr->z;
    ptr++;
  }
  return cloud;
}
void PoseEstimation::edit_pointcloud() {
  pcl::FrustumCulling<pcl::PointXYZ> frustum_filter;

  pcl::PointCloud<pcl::PointXYZ>::Ptr local_cloud(new pcl::PointCloud<pcl::PointXYZ>);
  pcl::PointCloud<pcl::PointXYZ>::Ptr local_pallet(new pcl::PointCloud<pcl::PointXYZ>);

  local_cloud = pcl_points_;
  frustum_filter_inliers_.clear();

  Eigen::Matrix4f camera_pose;
  Eigen::Matrix4f rotation_red_pos_ccw;
  Eigen::Matrix4f rotation_green_pos_cw;

  camera_pose << 0, 0, -1, 0,  // TODO(simon) Magic number.
      0, 1, 0, 0,  // TODO(simon) Magic number.
      1, 0, 0, 0,  // TODO(simon) Magic number.
      0, 0, 0, 1;  // TODO(simon) Magic number.

  Eigen::Vector2d z_inverse;
  z_inverse << 1, 0;  // TODO(simon) Magic number.
  Eigen::Vector2d center_frustum_zy;
  Eigen::Vector2d center_frustum_zx;
  center_frustum_zy << center_frustum_.z, center_frustum_.y;
  center_frustum_zx << center_frustum_.z, center_frustum_.x;
  double angle_zy =
      std::acos((z_inverse.dot(center_frustum_zy)) / (z_inverse.norm() * center_frustum_zy.norm()));
  double angle_zx =
      std::acos((z_inverse.dot(center_frustum_zx)) / (z_inverse.norm() * center_frustum_zx.norm()));

  if (std::chrono::system_clock::now() > start_debug_time_ && enable_debug_mode_) {
    std::cout << "\n ROTATION: \n " << std::endl;
    std::cout << "angle_zy: " << angle_zy << std::endl;
    std::cout << "angle_zx: " << angle_zx << std::endl;
    std::cout << "center_frustum_zy: " << center_frustum_zy.x() << " " << center_frustum_zy.y()
              << std::endl;
    std::cout << "center_frustum_zx: " << center_frustum_zx.x() << " " << center_frustum_zx.y()
              << std::endl;

    std::cout << center_frustum_ << std::endl;

    std::cout << "z_inverse.dot(center_frustum_zy): " << z_inverse.dot(center_frustum_zy)
              << std::endl;
    std::cout << "z_inverse: " << z_inverse << std::endl;
    std::cout << "center_frustum_zy: " << center_frustum_zy << std::endl;
    std::cout << "angle_zy: " << angle_zy << std::endl;
    std::cout << "angle_zx: " << angle_zx << std::endl;
  }

  if (center_frustum_zx.y()
      > 0) {  // TODO(simon) This is a hack. Solve the problem not the symptom.  // TODO(simon) Magic number.
    angle_zx *= -1;  // TODO(simon) Magic number.
  }
  if (center_frustum_zy.y()
      < 0) {  // TODO(simon) This is a hack. Solve the problem not the symptom.  // TODO(simon) Magic number.
    angle_zy *= -1;  // TODO(simon) Magic number.
  }

  float rot_red_rad = angle_zy;
  float rot_green_rad = angle_zx;

  rotation_red_pos_ccw
      << std::cos(rot_red_rad), -std::sin(rot_red_rad), 0, 0,  // TODO(simon) Magic number.
      std::sin(rot_red_rad), std::cos(rot_red_rad), 0, 0,  // TODO(simon) Magic number.
      0, 0, 1, 0,  // TODO(simon) Magic number.
      0, 0, 0, 1;  // TODO(simon) Magic number.

  rotation_green_pos_cw
      << std::cos(rot_green_rad), 0, -std::sin(rot_green_rad), 0,  // TODO(simon) Magic number.
      0, 1, 0, 0,  // TODO(simon) Magic number.
      std::sin(rot_green_rad), 0, std::cos(rot_green_rad), 0,  // TODO(simon) Magic number.
      0, 0, 0, 1;  // TODO(simon) Magic number.

  frustum_filter.setInputCloud(local_cloud);
  frustum_filter.setCameraPose(camera_pose * rotation_red_pos_ccw
                                   * rotation_green_pos_cw);  // frustum_filter.setCameraPose(camera_pose*cam2robot*rotation_red_pos_ccw*rotation_green_pos_cw);
  frustum_filter.setNearPlaneDistance(pcl_frustum_filter_near_plane_distance_meter_);
  frustum_filter.setFarPlaneDistance(pcl_frustum_filter_far_plane_distance_meter_);
  frustum_filter.setVerticalFOV(fov_v_rad_ * rad_to_deg_);
  frustum_filter.setHorizontalFOV(fov_h_rad_ * rad_to_deg_);
  frustum_filter.filter(*local_pallet);
  frustum_filter.filter(frustum_filter_inliers_);

  cloud_pallet_ = local_pallet;

  if (std::chrono::system_clock::now() > start_debug_time_ && enable_debug_mode_) {
    std::cout << "local_pallet->size() " << local_pallet->size() << std::endl;
    std::cout << "cloud_pallet_->size() " << cloud_pallet_->size() << std::endl;
    std::cout << "local_cloud->size() " << local_cloud->size() << std::endl;
  }
}

void PoseEstimation::view_pointcloud() {
  if (first_run_) {
    viewer_->setBackgroundColor(pcl_background_color_rgb_[red_color_id_],
                                pcl_background_color_rgb_[green_color_id_],
                                pcl_background_color_rgb_[blue_color_id_]);
    viewer_->addCoordinateSystem(pcl_coordinate_system_size_);
    viewer_->initCameraParameters();
    viewer_->setCameraPosition(pcl_initial_camera_position_pos_xyz_view_xyz_up_xyz_[x_position_id_],
                               pcl_initial_camera_position_pos_xyz_view_xyz_up_xyz_[y_position_id_],
                               pcl_initial_camera_position_pos_xyz_view_xyz_up_xyz_[z_position_id_],
                               pcl_initial_camera_position_pos_xyz_view_xyz_up_xyz_[3],  // TODO(simon) Magic numbers.
                               pcl_initial_camera_position_pos_xyz_view_xyz_up_xyz_[4],
                               pcl_initial_camera_position_pos_xyz_view_xyz_up_xyz_[5],
                               pcl_initial_camera_position_pos_xyz_view_xyz_up_xyz_[6],
                               pcl_initial_camera_position_pos_xyz_view_xyz_up_xyz_[7],
                               pcl_initial_camera_position_pos_xyz_view_xyz_up_xyz_[8]);
    first_run_ = false;
  }

  pcl::PointCloud<pcl::PointXYZRGB>::Ptr final_cloud_view(new pcl::PointCloud<pcl::PointXYZRGB>);

  final_cloud_view->clear();
  viewer_->removeAllShapes();
  viewer_->removeAllPointClouds();
  viewer_->removeCoordinateSystem(apriltag_coordinate_system_reference_name_,
                                  pcl_viewport_id_);  // TODO(simon) Magic number.

  pcl::copyPointCloud(*pcl_points_, *final_cloud_view);

  for (int i = iterations_start_at_; i < final_cloud_view->points.size(); ++i) {
    final_cloud_view->points[i].r = 255;  // TODO(simon) Magic number.
    final_cloud_view->points[i].g = 255;  // TODO(simon) Magic number.
    final_cloud_view->points[i].b = 255;  // TODO(simon) Magic number.
  }

  if (frustum_filter_inliers_.size() > 10) {  // TODO(simon) Magic number.
    for (int i = iterations_start_at_; i < frustum_filter_inliers_.size(); ++i) {
      final_cloud_view->points[frustum_filter_inliers_.at(i)].b = 0;
    }
  }

//! Adding and removing Pointclouds
  viewer_->addPointCloud(final_cloud_view,
                         "final_cloud",
                         pcl_viewport_id_);  //! Everything with color  // TODO(simon) Magic number.

  if (!rvecs_.empty() && !tvecs_.empty()) {
    Eigen::Affine3f rotation;

    pcl::PointXYZ startpoint = pcl::PointXYZ(tvecs_.at(first_)[x_position_id_],
                                             tvecs_.at(first_)[y_position_id_],
                                             tvecs_.at(first_)[z_position_id_]);
    std::vector<double> ground_truth_vector_converted
        (ground_truth_vector_.begin<double>(), ground_truth_vector_.end<double>());

    pcl::PointXYZ endpoint = pcl::PointXYZ(tvecs_.at(first_)[x_position_id_]
                                               - ground_truth_vector_converted.at(x_position_id_),  // TODO(simon) Testing remove "*2" Justering er ikke linjær
                                           tvecs_.at(first_)[y_position_id_]
                                               + ground_truth_vector_converted.at(y_position_id_),
                                           tvecs_.at(first_)[z_position_id_]
                                               - ground_truth_vector_converted.at(z_position_id_));

    converted_ground_truth_vector_.at(x_position_id_) = tvecs_.at(first_)[x_position_id_];
    converted_ground_truth_vector_.at(y_position_id_) = tvecs_.at(first_)[y_position_id_];
    converted_ground_truth_vector_.at(z_position_id_) = tvecs_.at(first_)[z_position_id_];
    converted_ground_truth_vector_.at(end_x_position_id_) =
        -ground_truth_vector_converted.at(x_position_id_);
    converted_ground_truth_vector_.at(end_y_position_id_) =
        ground_truth_vector_converted.at(y_position_id_);
    converted_ground_truth_vector_.at(end_z_position_id_) =
        -ground_truth_vector_converted.at(z_position_id_);

    if (enable_debug_mode_) {
      for (int i = iterations_start_at_; i < converted_ground_truth_vector_.size();
           ++i) {  // TODO(simon) Magic number.
        std::cout << "converted_ground_truth_vector_.at(" << i << ")"
                  << converted_ground_truth_vector_.at(i) << std::endl;
      }

      std::cout << "ENDPOINT: " << endpoint << std::endl;
    }

    viewer_->addLine(startpoint,
                     endpoint,
                     ground_truth_vector_color_rgb_[red_color_id_],
                     ground_truth_vector_color_rgb_[green_color_id_],
                     ground_truth_vector_color_rgb_[blue_color_id_],  // TODO(simon) Magic number.
                     ground_truth_vector_name_,
                     pcl_viewport_id_);
  }

  if (std::chrono::system_clock::now() > start_debug_time_ && enable_debug_mode_) {
    std::cout << "square_frustum_detection_points_.at(0)" << square_frustum_detection_points_.at(0)
              << std::endl;  // TODO(simon) Magic number.
    std::cout << "square_frustum_detection_points_.at(1)" << square_frustum_detection_points_.at(1)
              << std::endl;  // TODO(simon) Magic number.
    std::cout << "square_frustum_detection_points_.at(2)" << square_frustum_detection_points_.at(2)
              << std::endl;  // TODO(simon) Magic number.
    std::cout << "square_frustum_detection_points_.at(3)" << square_frustum_detection_points_.at(3)
              << std::endl;  // TODO(simon) Magic number.
    start_debug_time_ = std::chrono::system_clock::now();
    start_debug_time_ += std::chrono::seconds(debug_print_after_seconds_);
  }

  if (!square_frustum_detection_points_.empty()) {
    viewer_->addLine(pcl_point_origin_xyz_,
                     square_frustum_detection_points_.at(0),
                     selected_point_color_rgb_[red_color_id_],
                     selected_point_color_rgb_[green_color_id_],
                     selected_point_color_rgb_[blue_color_id_],
                     top_right_detection_corner_vector_name_,
                     pcl_viewport_id_);
    viewer_->addLine(pcl_point_origin_xyz_,
                     square_frustum_detection_points_.at(1),
                     selected_point_color_rgb_[red_color_id_],
                     selected_point_color_rgb_[green_color_id_],
                     selected_point_color_rgb_[blue_color_id_],
                     top_left_detection_corner_vector_name_,
                     pcl_viewport_id_);
    viewer_->addLine(pcl_point_origin_xyz_,
                     square_frustum_detection_points_.at(2),
                     selected_point_color_rgb_[red_color_id_],
                     selected_point_color_rgb_[green_color_id_],
                     selected_point_color_rgb_[blue_color_id_],
                     bottom_right_detection_corner_vector_name_,
                     pcl_viewport_id_);
    viewer_->addLine(pcl_point_origin_xyz_,
                     square_frustum_detection_points_.at(3),
                     selected_point_color_rgb_[red_color_id_],
                     selected_point_color_rgb_[green_color_id_],
                     selected_point_color_rgb_[blue_color_id_],
                     bottom_left_detection_corner_vector_name_,
                     pcl_viewport_id_);
    viewer_->addLine(pcl_point_origin_xyz_,
                     center_frustum_,
                     center_frustum_vector_color_rgb_[red_color_id_],
                     center_frustum_vector_color_rgb_[green_color_id_],
                     center_frustum_vector_color_rgb_[blue_color_id_],
                     center_detection_vector_name_,
                     pcl_viewport_id_);
  }

  if (ransac_model_coefficients_.size() > 2) {  // TODO(simon) Magic number.
    pcl::ModelCoefficients coff;
    coff.values = ransac_model_coefficients_;
    viewer_->addPlane(coff, 0.0, 0.0, 0.0,
                      ground_plane_reference_name_,
                      pcl_viewport_id_);  // TODO(simon) Magic number.
  }

  if (second_ransac_model_coefficients_.size() > 2) {  // TODO(simon) Magic number.
    pcl::ModelCoefficients coff;
    coff.values = second_ransac_model_coefficients_;
    viewer_->addPlane(coff, 0.0, 0.0, 0.0,
                      pallet_plane_reference_name_,
                      pcl_viewport_id_);  // TODO(simon) Magic number.
  }

  if (ransac_model_coefficients_.size() > 2) {  // TODO(simon) Magic number.
    viewer_->addLine(plane_frustum_vector_intersect_,
                     pose_vector_end_point_,
                     pose_vector_color_rgb_[red_color_id_],
                     pose_vector_color_rgb_[green_color_id_],
                     pose_vector_color_rgb_[blue_color_id_],
                     pose_vector_reference_name_,
                     pcl_viewport_id_);
  }
  viewer_->spinOnce(pcl_spin_time_);
}
void PoseEstimation::calculate_3d_crop() {
  std::vector<Eigen::Vector2d> detection_point_vec(4);

  Eigen::Vector2d image_center
      (zed_k_matrix_[2],  // TODO(simon) Get K matrix from intel realsense  // TODO(simon) Magic number.
       zed_k_matrix_[3]);  // TODO(simon) Get K matrix from intel realsense  // TODO(simon) Magic number.

  detection_point_vec.at(0).x() = detection_output_struct_.x;  // TODO(simon) Magic number.
  detection_point_vec.at(0).y() = detection_output_struct_.y;  // TODO(simon) Magic number.

  detection_point_vec.at(1).x() =
      detection_output_struct_.x + detection_output_struct_.width;  // TODO(simon) Magic number.
  detection_point_vec.at(1).y() = detection_output_struct_.y;  // TODO(simon) Magic number.

  detection_point_vec.at(2).x() = detection_output_struct_.x;  // TODO(simon) Magic number.
  detection_point_vec.at(2).y() =
      detection_output_struct_.y + detection_output_struct_.height;  // TODO(simon) Magic number.

  detection_point_vec.at(3).x() =
      detection_output_struct_.x + detection_output_struct_.width;  // TODO(simon) Magic number.
  detection_point_vec.at(3).y() =
      detection_output_struct_.y + detection_output_struct_.height;  // TODO(simon) Magic number.

  detection_from_image_center_.clear();
  square_frustum_detection_points_.clear();
  for (int i = iterations_start_at_; i < number_of_object_detection_corner_vectors_; ++i) {
    detection_from_image_center_.emplace_back((detection_point_vec.at(i) - image_center)
                                                  / zed_k_matrix_[0]);  // TODO(simon) Get K matrix from intel realsense; The difference bwetween this is too spall  // TODO(simon) Magic number.
    square_frustum_detection_points_.emplace_back(
        detection_from_image_center_.at(i).x() * detection_vector_scale_,
        detection_from_image_center_.at(i).y() * detection_vector_scale_,
        detection_vector_scale_);
  }

  if (std::chrono::system_clock::now() > start_debug_time_ && enable_debug_mode_) {
    for (int i = iterations_start_at_; i < number_of_object_detection_corner_vectors_; ++i) {
      std::cout << "image_center X: " << image_center << std::endl;
      std::cout << "detection_point_vec.at(i): " << detection_point_vec.at(i) << std::endl;
      std::cout << "zed_k_matrix_[0] " << zed_k_matrix_[0]
                << std::endl;  // TODO(simon) Magic number.
      std::cout << "detection_from_image_center_: " << detection_from_image_center_.at(i)
                << std::endl;
    }
  }

  Eigen::Vector3f vector_0(square_frustum_detection_points_.at(first_).x,
                           square_frustum_detection_points_.at(first_).y,
                           square_frustum_detection_points_.at(first_).z);

  Eigen::Vector3f vector_1(square_frustum_detection_points_.at(second_).x,
                           square_frustum_detection_points_.at(second_).y,
                           square_frustum_detection_points_.at(second_).z);

  Eigen::Vector3f vector_2(square_frustum_detection_points_.at(third_).x,
                           square_frustum_detection_points_.at(third_).y,
                           square_frustum_detection_points_.at(third_).z);

  Eigen::Vector3f vector_3(square_frustum_detection_points_.at(fourth_).x,
                           square_frustum_detection_points_.at(fourth_).y,
                           square_frustum_detection_points_.at(fourth_).z);

  for (int i = iterations_start_at_; i < number_of_object_detection_corner_vectors_; ++i) {
    center_frustum_.x += square_frustum_detection_points_.at(i).x;
    center_frustum_.y += square_frustum_detection_points_.at(i).y;
    center_frustum_.z += square_frustum_detection_points_.at(i).z;
  }
  center_frustum_.x /= number_of_object_detection_corner_vectors_;
  center_frustum_.y /= number_of_object_detection_corner_vectors_;
  center_frustum_.z /= number_of_object_detection_corner_vectors_;

  Eigen::Vector3f vector_center(center_frustum_.x,
                                center_frustum_.y,
                                center_frustum_.z);

  Eigen::Vector3f vector_camera_front(1, 0, 0);  // TODO(simon) Magic number.

  float dot_01 = vector_0.dot(vector_1);
  float dot_02 = vector_0.dot(vector_2);

  float len_sq_0 = vector_0.norm();
  float len_sq_1 = vector_1.norm();
  float len_sq_2 = vector_2.norm();

  float angle_01 = std::acos(dot_01 / (len_sq_0 * len_sq_1));
  float angle_02 = std::acos(dot_02 / (len_sq_0 * len_sq_2));

  fov_h_rad_ = angle_01;
  fov_v_rad_ = angle_02;

  if (std::chrono::system_clock::now() > start_debug_time_ && enable_debug_mode_) {
    std::cout << "detection_point_vec_0: " << detection_point_vec.at(0).x() << " "
              << detection_point_vec.at(0).y() << std::endl;  // TODO(simon) Magic number.
    std::cout << "detection_point_vec_1: " << detection_point_vec.at(1).x() << " "
              << detection_point_vec.at(1).y() << std::endl;  // TODO(simon) Magic number.
    std::cout << "detection_point_vec_2: " << detection_point_vec.at(2).x() << " "
              << detection_point_vec.at(2).y() << std::endl;  // TODO(simon) Magic number.
    std::cout << "detection_point_vec_3: " << detection_point_vec.at(3).x() << " "
              << detection_point_vec.at(3).y() << std::endl;  // TODO(simon) Magic number.
  }
}

void PoseEstimation::calculate_ransac() {
  pcl::ModelCoefficients::Ptr first_coefficients(new pcl::ModelCoefficients);
  pcl::PointIndices::Ptr first_inliers(new pcl::PointIndices);

  pcl::SACSegmentation<pcl::PointXYZ> seg;
  seg.setOptimizeCoefficients(true);

  seg.setModelType(pcl::SACMODEL_PLANE);
  seg.setEpsAngle(ransac_eps_angle_radians_);
  seg.setMethodType(pcl::SAC_RANSAC);
  seg.setMaxIterations(ransac_max_iterations_);
  seg.setDistanceThreshold(first_ransac_distance_threshold_meter_);

  if (enable_debug_mode_) {
    std::cout << "First RANSAC" << std::endl;
    std::cout << "cloud_pallet_ size: " << cloud_pallet_->size() << std::endl;
  }

  if (cloud_pallet_->size()
      > minimum_points_for_ransac_) {
    seg.setInputCloud(cloud_pallet_);
    seg.segment(*first_inliers, *first_coefficients);

    first_ransac_model_coefficients_.clear();
    for (int i = iterations_start_at_; i < first_coefficients->values.size(); ++i) {
      first_ransac_model_coefficients_.emplace_back(first_coefficients->values.at(i));
    }

    pcl::PointCloud<pcl::PointXYZ>::Ptr final(new pcl::PointCloud<pcl::PointXYZ>);
    std::vector<int> test_inliers;
    pcl::SampleConsensusModelPlane<pcl::PointXYZ>::Ptr
        model_p(new pcl::SampleConsensusModelPlane<pcl::PointXYZ>(cloud_pallet_));

    pcl::RandomSampleConsensus<pcl::PointXYZ> ransac(model_p);
    ransac.setDistanceThreshold(second_ransac_distance_threshold_meter_);
    ransac.computeModel();
    ransac.getInliers(test_inliers);

    pcl::copyPointCloud(*cloud_pallet_, test_inliers, *final);
    final_ = final;
  }

  pcl::PointIndices::Ptr inliers(new pcl::PointIndices);

  pcl::PointCloud<pcl::PointNormal>::Ptr
      input_cloud_with_normals(new pcl::PointCloud<pcl::PointNormal>);
  pcl::PointCloud<pcl::PointNormal>::Ptr
      output_cloud_with_normals(new pcl::PointCloud<pcl::PointNormal>);
  pcl::PointCloud<pcl::PointNormal>::Ptr final_with_normals(new pcl::PointCloud<pcl::PointNormal>);

  input_cloud_with_normals->clear();
  output_cloud_with_normals->clear();
  final_with_normals->clear();

  input_cloud_with_normals->resize(cloud_pallet_->size());
  for (int i = iterations_start_at_; i < cloud_pallet_->points.size();
       ++i) {
    input_cloud_with_normals->points.at(i).x = cloud_pallet_->at(i).x;
    input_cloud_with_normals->points.at(i).y = cloud_pallet_->at(i).y;
    input_cloud_with_normals->points.at(i).z = cloud_pallet_->at(i).z;
  }

  if (enable_debug_mode_) {
    std::cout << "Sampling surface normals" << std::endl;
    std::cout << "input_cloud_with_normals size: " << input_cloud_with_normals->size() << std::endl;
  }


//!  Sampling surface normals
  if (input_cloud_with_normals->size()
      > minimum_points_for_sampling_surface_normals_) {
    pcl::SamplingSurfaceNormal<pcl::PointNormal> sample_surface_normal;
    sample_surface_normal.setInputCloud(input_cloud_with_normals);
    sample_surface_normal.setSample(sample_surface_normal_sample_size_);
    sample_surface_normal.setRatio(sample_surface_normal_ratio_);  // TODO(simon) Setting that is required to be a parameter.  // TODO(simon) Magic number.
    sample_surface_normal.filter(*output_cloud_with_normals);

    output_cloud_with_normals_ = output_cloud_with_normals;
    final_with_normals_ = final_with_normals;

    std::vector<int> temp_index;
    pcl::removeNaNNormalsFromPointCloud(*output_cloud_with_normals_,
                                        *output_cloud_with_normals_,
                                        temp_index);

    int counter = 0;  // TODO(simon) Magic number.

    for (int i = iterations_start_at_; i < output_cloud_with_normals_->size();
         ++i) {  // TODO(simon) Magic number.
      if (abs(output_cloud_with_normals_->at(i).normal_y) < 0.45
          &&  //! Default 0.45 or 0.10  // TODO(simon) Magic number.
              abs(output_cloud_with_normals_->at(i).x) > 0.2
          &&  //! Remove vector at origo.  // TODO(simon) Magic number.
              abs(output_cloud_with_normals_->at(i).y) > 0.2
          &&  //! Remove vector at origo.  // TODO(simon) Magic number.
              abs(output_cloud_with_normals_->at(i).z)
                  > 0.2)  //! Remove vector at origo.  // TODO(simon) Magic number.
      {
        final_with_normals_->emplace_back(output_cloud_with_normals_->at(i));
        counter++;
      }
    }
  }

  //! RANSAC
  if (input_cloud_with_normals->size()
      > minimum_points_for_ransac_) {  // TODO(simon) 10 should be set as input parameter.  // TODO(simon) Magic number.
    pcl::SACSegmentationFromNormals<pcl::PointNormal, pcl::PointNormal> segmentation;
    pcl::ModelCoefficients::Ptr coefficients(new pcl::ModelCoefficients);

    segmentation.setOptimizeCoefficients(true);
    segmentation.setModelType(pcl::SACMODEL_NORMAL_PLANE);  // TODO(simon) Test with different models SACMODEL_PLANE | SACMODEL_NORMAL_PLANE | SACMODEL_PERPENDICULAR_PLANE
    segmentation.setMethodType(pcl::SAC_RANSAC);
    segmentation.setMaxIterations(maximum_iterations_for_segmentation_);
    segmentation.setDistanceThreshold(segmentation_distance_threshold_meter_);

    if (enable_debug_mode_) {
      std::cout << "Inbetween " << std::endl;
      std::cout << "input_cloud_with_normals size: " << input_cloud_with_normals->size()
                << std::endl;
    }

    segmentation.setEpsAngle(segmentation_eps_angle_radians_);
    segmentation.setInputCloud(output_cloud_with_normals_);
    segmentation.setInputNormals(output_cloud_with_normals_);

    segmentation.segment(*inliers, *coefficients);

    ransac_model_coefficients_.clear();
    for (int i = iterations_start_at_; i < coefficients->values.size();
         ++i) {
      ransac_model_coefficients_.emplace_back(coefficients->values.at(i));
    }
  }

  //! Extract filter

  if (output_cloud_with_normals_->size()
      > minimum_points_for_ransac_) {
    pcl::PointCloud<pcl::PointNormal>::Ptr
        extracted_cloud_with_normals(new pcl::PointCloud<pcl::PointNormal>);
    // Extract all points
    pcl::ExtractIndices<pcl::PointNormal> extract_filter;
    extract_filter.setInputCloud(output_cloud_with_normals_);
    extract_filter.setNegative(true);
    extract_filter.setIndices(inliers);
    extract_filter.filter(*extracted_cloud_with_normals);

    extracted_cloud_with_normals_ = extracted_cloud_with_normals;
  }

  //! RANSAC 2
  if (extracted_cloud_with_normals_->size()
      > minimum_points_for_ransac_) {
    pcl::SACSegmentationFromNormals<pcl::PointNormal, pcl::PointNormal> second_segmentation;
    pcl::ModelCoefficients::Ptr second_coefficients(new pcl::ModelCoefficients);
    pcl::PointIndices::Ptr second_inliers(new pcl::PointIndices);

    second_segmentation.setOptimizeCoefficients(true);
    second_segmentation.setModelType(pcl::SACMODEL_NORMAL_PLANE);  // TODO(simon) Test with different models SACMODEL_PLANE | SACMODEL_NORMAL_PLANE | SACMODEL_PERPENDICULAR_PLANE
    second_segmentation.setMethodType(pcl::SAC_RANSAC);
    second_segmentation.setMaxIterations(maximum_iterations_for_segmentation_);
    second_segmentation.setDistanceThreshold(segmentation_distance_threshold_meter_);

    if (enable_debug_mode_) {
      std::cout << "second_segmentation " << std::endl;
      std::cout << "extracted_cloud_with_normals_ size: " << extracted_cloud_with_normals_->size()
                << std::endl;
    }

    second_segmentation.setEpsAngle(segmentation_eps_angle_radians_);
    second_segmentation.setInputCloud(extracted_cloud_with_normals_);
    second_segmentation.setInputNormals(extracted_cloud_with_normals_);

    second_segmentation.segment(*second_inliers, *second_coefficients);

    second_ransac_model_coefficients_.clear();
    for (int i = iterations_start_at_; i < second_coefficients->values.size();
         ++i) {  // TODO(simon) Magic number.
      second_ransac_model_coefficients_.emplace_back(second_coefficients->values.at(i));
    }
  }
  inliers_ = inliers;
}

Eigen::Affine3f PoseEstimation::create_rotation_matrix(float ax, float ay, float az) {
  Eigen::Affine3f rx =
      Eigen::Affine3f(Eigen::AngleAxisf(ax,
                                        Eigen::Vector3f(1, 0, 0)));  // TODO(simon) Magic number.
  Eigen::Affine3f ry =
      Eigen::Affine3f(Eigen::AngleAxisf(ay,
                                        Eigen::Vector3f(0, 1, 0)));  // TODO(simon) Magic number.
  Eigen::Affine3f rz =
      Eigen::Affine3f(Eigen::AngleAxisf(az,
                                        Eigen::Vector3f(0, 0, 1)));  // TODO(simon) Magic number.
  return rz * ry * rx;
}

void PoseEstimation::calculate_pose_vector() {
  intersect_point_.values.resize(4);  // TODO(simon) Magic number.
  float distance_scalar = 0;
  Eigen::Vector3f center_frustum_vector;
  Eigen::Vector3f plane_vector_intersect;
  Eigen::Vector3f plane_orgin;
  Eigen::Vector3f plane_normal_vector;

  plane_vector_intersect_.x = -ransac_model_coefficients_.at(plane_normal_x_id_)
      * ransac_model_coefficients_.at(plane_hessian_component_id_);
  plane_vector_intersect_.y = -ransac_model_coefficients_.at(plane_normal_y_id_)
      * ransac_model_coefficients_.at(plane_hessian_component_id_);
  plane_vector_intersect_.z = -ransac_model_coefficients_.at(plane_normal_z_id_)
      * ransac_model_coefficients_.at(plane_hessian_component_id_);

  plane_orgin.x() = plane_vector_intersect_.x;
  plane_orgin.y() = plane_vector_intersect_.y;
  plane_orgin.z() = plane_vector_intersect_.z;

  plane_normal_vector.x() = ransac_model_coefficients_.at(x_position_id_);
  plane_normal_vector.y() = ransac_model_coefficients_.at(y_position_id_);
  plane_normal_vector.z() = ransac_model_coefficients_.at(z_position_id_);

  center_frustum_vector.x() = center_frustum_.x;
  center_frustum_vector.y() = center_frustum_.y;
  center_frustum_vector.z() = center_frustum_.z;

  distance_scalar = (plane_orgin.dot(plane_normal_vector)) /
      (center_frustum_vector.dot(plane_normal_vector));

  if (enable_debug_mode_) {
    std::cout << "distance_scalar: " << distance_scalar << std::endl;
  }

  plane_vector_intersect = center_frustum_vector * distance_scalar;

  if (enable_debug_mode_) {
    std::cout << "plane_vector_intersect: " << plane_vector_intersect << std::endl;
  }

  intersect_point_.values[0] =
      plane_vector_intersect.x();  // TODO(simon) Magic number.
  intersect_point_.values[1] = plane_vector_intersect.y();  // TODO(simon) Magic number.
  intersect_point_.values[2] = plane_vector_intersect.z();  // TODO(simon) Magic number.
  intersect_point_.values[3] = 0.1;  // TODO(simon) Magic number.

  plane_frustum_vector_intersect_.x = plane_vector_intersect.x();
  plane_frustum_vector_intersect_.y = plane_vector_intersect.y();
  plane_frustum_vector_intersect_.z = plane_vector_intersect.z();

  if (second_ransac_model_coefficients_.at(plane_normal_z_id_)
      > 0) {  // TODO(simon) required.  // TODO(simon) Magic number.
    pose_vector_end_point_.x = plane_vector_intersect.x()
        + second_ransac_model_coefficients_.at(plane_normal_x_id_);
    pose_vector_end_point_.y = plane_vector_intersect.y()
        + (-1 * first_ransac_model_coefficients_.at(plane_normal_z_id_));
    pose_vector_end_point_.z = plane_vector_intersect.z()
        + second_ransac_model_coefficients_.at(plane_normal_z_id_);
  } else if (ransac_model_coefficients_.at(plane_normal_z_id_) < 0) {  // TODO(simon) Magic number.
  }
}

void PoseEstimation::log_data(uint32_t frame) {
  if (enable_logger_ && ransac_model_coefficients_.size() > 1 && tvecs_.size() >= 1
      && rvecs_.size() >= 1) {  // TODO(simon) Magic number.
    std::ofstream LoggerFile(std::filesystem::current_path().parent_path() /
      logger_file_save_relative_path_, std::ios_base::app | std::ios_base::out);

    LoggerFile << frame << ","
               << plane_frustum_vector_intersect_.x << ","
               << plane_frustum_vector_intersect_.y << ","
               << plane_frustum_vector_intersect_.z << ","
               << second_ransac_model_coefficients_.at(plane_normal_x_id_) << ","
               << (-1 * first_ransac_model_coefficients_.at(plane_normal_z_id_)) << ","
               << second_ransac_model_coefficients_.at(plane_normal_z_id_) << ","
               << converted_ground_truth_vector_.at(0) << ","  // TODO(simon) Magic number.
               << converted_ground_truth_vector_.at(1) << ","  // TODO(simon) Magic number.
               << converted_ground_truth_vector_.at(2) << ","  // TODO(simon) Magic number.
               << converted_ground_truth_vector_.at(3) << ","  // TODO(simon) Magic number.
               << converted_ground_truth_vector_.at(4) << ","  // TODO(simon) Magic number.
               << converted_ground_truth_vector_.at(5)  // TODO(simon) Magic number.
               << std::endl;
    LoggerFile.close();
  } else {
    std::ofstream LoggerFile(std::filesystem::current_path().parent_path() /
      logger_file_save_relative_path_, std::ios_base::app | std::ios_base::out);

    LoggerFile << frame << std::endl;
    LoggerFile.close();
  }
}
