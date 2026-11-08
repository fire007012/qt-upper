# ============================================
# Controller层 - 业务逻辑层
# ============================================
# 职责：
#   - 协调Model、Parser、Communication三层
#   - 处理用户交互逻辑
#   - 管理电机控制状态
#
# 依赖：
#   - 依赖其他所有层级 (Model, Parser, Communication)
#   - 依赖 Qt::Core
# ============================================

set(CONTROLLER_SOURCES
    ../state/RobotStateStore.cpp
    ../state/RobotStateStore.h
    ../diagnostics/DiagnosticsWidget.cpp
    ../diagnostics/DiagnosticsWidget.h
    ../management/ConfigurationService.cpp
    ../management/ConfigurationService.h
    ../management/ConfigurationWidget.cpp
    ../management/ConfigurationWidget.h
    ../management/RuntimeService.cpp
    ../management/RuntimeService.h
    ../management/RuntimeManagementWidget.cpp
    ../management/RuntimeManagementWidget.h
    controller.cpp
    controller.h
    CameraGridWidget.cpp
    CameraGridWidget.h
    CO2DisplayWidget.cpp
    CO2DisplayWidget.h
    ControlPanelWidget.cpp
    ControlPanelWidget.h
    GamepadDisplayWidget.cpp
    GamepadDisplayWidget.h
    DisplayLayoutManager.cpp
    DisplayLayoutManager.h
    FfmpegRtspDecoder.cpp
    FfmpegRtspDecoder.h
    KeyboardController.cpp
    KeyboardController.h
    MotorRuntimeCarouselWidget.cpp
    MotorRuntimeCarouselWidget.h
    handlekey.cpp
    handlekey.h
    RtspPlayerWidget.cpp
    RtspPlayerWidget.h
    VideoFrameWidget.cpp
    VideoFrameWidget.h
    RobotAttitudeWidget.cpp
    RobotAttitudeWidget.h
    RobotViewModel.cpp
    RobotViewModel.h
    TelemetryPanelWidget.cpp
    TelemetryPanelWidget.h
)

# 创建静态库
add_library(controller_layer STATIC ${CONTROLLER_SOURCES})
target_compile_definitions(controller_layer PRIVATE HOST_REFERENCE_ROOT="${CMAKE_SOURCE_DIR}/ros2_ws_car")

# 绕过 GCC 13.1 汇编器内部错误 (Segmentation fault)
set_source_files_properties(handlekey.cpp PROPERTIES COMPILE_FLAGS "-O0")

# 链接依赖
target_link_libraries(controller_layer PUBLIC
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickWidgets

    # 层级依赖
    communication_layer
    utils_layer

    # OpenCV 依赖（如果需要）
    # ${OpenCV_LIBS}  # 暂时注释，避免依赖问题
)

# 优先启用vendored SDL3作为跨平台手柄后端（Linux/Windows均可支持DS4/DS5/Xbox等）
if(TARGET SDL3::SDL3)
    target_link_libraries(controller_layer PUBLIC SDL3::SDL3)
    target_compile_definitions(controller_layer PUBLIC USE_SDL3_GAMEPAD)
    message(STATUS "SDL3 gamepad backend enabled")
else()
    message(STATUS "SDL3 gamepad backend not found; using platform fallback only")
endif()

# Windows平台链接XInput库（手柄驱动）
if(WIN32)
    target_link_libraries(controller_layer PUBLIC xinput)
endif()

# 设置包含目录
target_include_directories(controller_layer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/communication
    ${CMAKE_SOURCE_DIR}/src/utils
)
