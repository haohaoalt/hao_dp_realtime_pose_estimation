add_library(pose_estimation
            PoseEstimation/PoseEstimation.h
            PoseEstimation/PoseEstimation.cc
            )

set_target_properties(pose_estimation PROPERTIES LINKER_LANGUAGE CXX)

target_include_directories(pose_estimation PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(pose_estimation
                      ${OpenCV_LIBS}
                      ${PCL_LIBRARIES})