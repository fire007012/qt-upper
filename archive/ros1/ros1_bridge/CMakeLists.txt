cmake_minimum_required(VERSION 3.0.2)
project(ros1_bridge)

add_compile_options(-std=c++11)

find_package(catkin REQUIRED COMPONENTS
  roscpp
  rospy
  geometry_msgs
  sensor_msgs
  std_msgs
  std_srvs
)

catkin_package(
  CATKIN_DEPENDS
    roscpp
    rospy
    geometry_msgs
    sensor_msgs
    std_msgs
    std_srvs
)

include_directories(
  ${catkin_INCLUDE_DIRS}
)

add_executable(ros_image_rtsp_node
  src/ros_image_rtsp_node.cpp
)

target_link_libraries(ros_image_rtsp_node
  ${catkin_LIBRARIES}
)

catkin_install_python(PROGRAMS
  bridge_control_signal_monitor.py
  bridge_control_signal_monitor_gui.py
  host_bridge_node.py
  mediamtx_launcher.sh
  video_manager_node.py
  DESTINATION ${CATKIN_PACKAGE_BIN_DESTINATION}
)

install(TARGETS ros_image_rtsp_node
  RUNTIME DESTINATION ${CATKIN_PACKAGE_BIN_DESTINATION}
)

install(FILES
  bridge_control.yaml
  ros_image_rtsp.example.yaml
  DESTINATION ${CATKIN_PACKAGE_SHARE_DESTINATION}
)

install(DIRECTORY launch
  DESTINATION ${CATKIN_PACKAGE_SHARE_DESTINATION}
)
