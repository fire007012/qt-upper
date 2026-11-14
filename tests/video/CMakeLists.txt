cmake_minimum_required(VERSION 3.16)
project(host_video_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Network Test Widgets)
set(HOST_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
add_executable(rtsp_decoder_probe rtsp_decoder_probe.cpp
    ${HOST_ROOT}/src/controller/FfmpegRtspDecoder.cpp)
target_include_directories(rtsp_decoder_probe PRIVATE ${HOST_ROOT}/src/controller)
target_link_libraries(rtsp_decoder_probe PRIVATE Qt6::Core Qt6::Gui Qt6::Network Qt6::Test)
enable_testing()
add_test(NAME video_decoder_lifecycle COMMAND rtsp_decoder_probe invalidDimensionsAndCancelledCallbacks)
set_tests_properties(video_decoder_lifecycle PROPERTIES TIMEOUT 15)
# The real RTSP test is explicitly launched by rtsp_loopback_integration.py.
add_executable(rtsp_widget_test rtsp_widget_test.cpp
    ${HOST_ROOT}/src/controller/RtspPlayerWidget.cpp
    ${HOST_ROOT}/src/controller/VideoFrameWidget.cpp
    ${HOST_ROOT}/src/controller/FfmpegRtspDecoder.cpp)
target_include_directories(rtsp_widget_test PRIVATE ${HOST_ROOT}/src/controller)
target_link_libraries(rtsp_widget_test PRIVATE Qt6::Widgets Qt6::Network Qt6::Test)
add_test(NAME video_widget_lifecycle COMMAND rtsp_widget_test)
set_tests_properties(video_widget_lifecycle PROPERTIES
    TIMEOUT 15 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
