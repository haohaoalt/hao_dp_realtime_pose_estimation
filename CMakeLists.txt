cmake_minimum_required(VERSION 3.21)
project(realtime_pose_estimation)

set(CMAKE_CXX_STANDARD 17)
set(ENV{OpenCV_DIR} "/usr/include/opencv4") # Select OpenCV with contrib
set(OpenCV_DIR "/usr/include/opencv4") # Select OpenCV with contrib
# /usr/include/opencv4
# /usr/include/opencv4/opencv2
# /usr/local/include/opencv2
# /opt/intel/openvino_2021.4.752/opencv/include
# /usr/lib/x86_64-linux-gnu/

find_package(realsense2 REQUIRED)
find_package(OpenCV REQUIRED)
find_package(InferenceEngine REQUIRED)
find_package(ngraph REQUIRED)
find_package(PCL REQUIRED )


add_subdirectory(include/PoseEstimation)
add_subdirectory(include/ObjectDetection)
add_subdirectory(include/Logger)

include_directories(
            ${OpenCV_INCLUDE_DIRS}
            ${PCL_INCLUDE_DIRS}
)

add_executable(realtime_pose_estimation src/main.cc)

target_link_libraries(realtime_pose_estimation
                      pose_estimation
                      object_detection
                      ${realsense2_LIBRARY}
                      ${OpenCV_LIBS}
                      ${PCL_LIBRARIES}

)

target_link_libraries(pose_estimation
                      object_detection)

