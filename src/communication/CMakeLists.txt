# ============================================
# Communication Layer (通讯层)
# ============================================
# 职责:
#   - TCP 网络IO
#   - 机器人 TCP/JSON通讯
#   - 通过 signal 将数据发送到 Controller
#
# 依赖:
#   - 仅依赖 Qt::Core, Qt::Network
#   - 不依赖 Controller / UI / Parser
#   - 使用 signal/slot 与 Controller 通信
# ============================================

set(COMMUNICATION_SOURCES
    ControlProtocol.cpp
    ControlProtocol.h
    ControlSession.cpp
    ControlSession.h
    EmergencyChannel.cpp
    EmergencyChannel.h
    ManagementClient.cpp
    ManagementClient.h
    SharedStructs.h
    HostProtocol.cpp
    HostProtocol.h

    # 机器人 TCP/JSON客户端
    RobotTcpClient.cpp
    RobotTcpClient.h
)

# 构建静态库
add_library(communication_layer STATIC ${COMMUNICATION_SOURCES})

# 链接Qt库 - 支持Qt5/Qt6
if(QT_VERSION EQUAL 6)
    target_link_libraries(communication_layer PUBLIC
        Qt6::Core
        Qt6::Network
        Qt6::Gui
        utils_layer
    )
else()
    target_link_libraries(communication_layer PUBLIC
        Qt5::Core
        Qt5::Network
        Qt5::Gui
    )
endif()

# 包含头文件目录
target_include_directories(communication_layer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
