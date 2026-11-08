cmake_minimum_required(VERSION 3.16)
project(host_video_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Network Test)
set(HOST_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
add_executable(rtsp_decoder_probe rtsp_decoder_probe.cpp
    ${HOST_ROOT}/src/controller/FfmpegRtspDecoder.cpp)
target_include_directories(rtsp_decoder_probe PRIVATE ${HOST_ROOT}/src/controller)
target_link_libraries(rtsp_decoder_probe PRIVATE Qt6::Core Qt6::Gui Qt6::Network Qt6::Test)
enable_testing()
add_test(NAME video_decoder_lifecycle COMMAND rtsp_decoder_probe invalidDimensionsAndCancelledCallbacks)
set_tests_properties(video_decoder_lifecycle PROPERTIES TIMEOUT 15)
# The real RTSP test is explicitly launched by rtsp_loopback_integration.py.
