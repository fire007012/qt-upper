cmake_minimum_required(VERSION 3.16)
project(host_communication_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Network Test)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Network Test)
enable_testing()
set(HOST_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(HOST_CONTROL_SOURCES
    ${HOST_ROOT}/src/communication/ControlProtocol.cpp
    ${HOST_ROOT}/src/communication/ControlSession.cpp
    ${HOST_ROOT}/src/communication/EmergencyChannel.cpp)
add_executable(tcp_client_test
    ${HOST_CONTROL_SOURCES}
    tcp_client_test.cpp
    ${HOST_ROOT}/src/communication/RobotTcpClient.cpp
    ${HOST_ROOT}/src/communication/RobotTcpClient.h
    ${HOST_ROOT}/src/communication/HostProtocol.cpp
    ${HOST_ROOT}/src/utils/Logger.cpp
    ${HOST_ROOT}/src/utils/ErrorHandler.cpp
    ${HOST_ROOT}/src/utils/ErrorHandler.h
)
target_include_directories(tcp_client_test PRIVATE
    ${HOST_ROOT}/src/communication ${HOST_ROOT}/src/utils)
target_link_libraries(tcp_client_test PRIVATE
    Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Network Qt${QT_VERSION_MAJOR}::Test)
add_test(NAME tcp_client_recovery COMMAND tcp_client_test)
set_tests_properties(tcp_client_recovery PROPERTIES TIMEOUT 120)

add_executable(management_client_test
    management_client_test.cpp
    ${HOST_ROOT}/src/communication/ManagementClient.cpp
    ${HOST_ROOT}/src/communication/ManagementClient.h
    ${HOST_ROOT}/src/management/RuntimeService.cpp
    ${HOST_ROOT}/src/management/RuntimeService.h
)
target_include_directories(management_client_test PRIVATE
    ${HOST_ROOT}/src/communication ${HOST_ROOT}/src/management)
target_link_libraries(management_client_test PRIVATE
    Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Network Qt${QT_VERSION_MAJOR}::Test)
add_test(NAME management_client_recovery COMMAND management_client_test)
set_tests_properties(management_client_recovery PROPERTIES TIMEOUT 120)

add_executable(configuration_client_test
    configuration_client_test.cpp
    ${HOST_ROOT}/src/communication/ManagementClient.cpp
    ${HOST_ROOT}/src/management/ConfigurationService.cpp)
target_include_directories(configuration_client_test PRIVATE
    ${HOST_ROOT}/src/communication ${HOST_ROOT}/src/management)
target_link_libraries(configuration_client_test PRIVATE
    Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Network Qt${QT_VERSION_MAJOR}::Test)
add_test(NAME configuration_transactions COMMAND configuration_client_test)
set_tests_properties(configuration_transactions PROPERTIES TIMEOUT 60)

# Explicitly launched by configuration_reference_integration.py with an injected backend.
add_executable(configuration_reference_probe
    configuration_reference_probe.cpp
    ${HOST_ROOT}/src/communication/ManagementClient.cpp
    ${HOST_ROOT}/src/management/ConfigurationService.cpp)
target_include_directories(configuration_reference_probe PRIVATE
    ${HOST_ROOT}/src/communication ${HOST_ROOT}/src/management)
target_link_libraries(configuration_reference_probe PRIVATE
    Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Network Qt${QT_VERSION_MAJOR}::Test)

option(HOST_BUILD_WIDGET_TESTS "Build the optional offscreen management UI regression" OFF)
if(HOST_BUILD_WIDGET_TESTS)
    find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)
    add_executable(management_widget_test
        management_widget_test.cpp
        ${HOST_ROOT}/src/communication/ManagementClient.cpp
        ${HOST_ROOT}/src/communication/ManagementClient.h
        ${HOST_ROOT}/src/management/RuntimeService.cpp
        ${HOST_ROOT}/src/management/RuntimeService.h
        ${HOST_ROOT}/src/management/RuntimeManagementWidget.cpp
        ${HOST_ROOT}/src/management/RuntimeManagementWidget.h
        ${HOST_ROOT}/src/management/ConfigurationService.cpp
        ${HOST_ROOT}/src/management/ConfigurationService.h
        ${HOST_ROOT}/src/management/ConfigurationWidget.cpp
        ${HOST_ROOT}/src/management/ConfigurationWidget.h)
    target_include_directories(management_widget_test PRIVATE
        ${HOST_ROOT}/src/communication ${HOST_ROOT}/src/management)
    target_link_libraries(management_widget_test PRIVATE
        Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Network
        Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Test)
    add_test(NAME management_widget_flow COMMAND management_widget_test)
    set_tests_properties(management_widget_flow PROPERTIES
        TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(keyboard_safety_test keyboard_safety_test.cpp
        ${HOST_ROOT}/src/controller/KeyboardController.cpp)
    target_include_directories(keyboard_safety_test PRIVATE ${HOST_ROOT}/src/controller)
    target_link_libraries(keyboard_safety_test PRIVATE Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Test)
    add_test(NAME keyboard_physical_neutral COMMAND keyboard_safety_test)
    set_tests_properties(keyboard_physical_neutral PROPERTIES TIMEOUT 10)
endif()

# Optional read-only contract runner; never starts a robot service by itself.
add_executable(manager_reference_probe
    manager_reference_probe.cpp
    ${HOST_ROOT}/src/communication/ManagementClient.cpp
    ${HOST_ROOT}/src/communication/ManagementClient.h
    ${HOST_ROOT}/src/management/RuntimeService.cpp
    ${HOST_ROOT}/src/management/RuntimeService.h)
target_include_directories(manager_reference_probe PRIVATE
    ${HOST_ROOT}/src/communication ${HOST_ROOT}/src/management)
target_link_libraries(manager_reference_probe PRIVATE
    Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Network)

add_executable(control_session_test
    control_session_test.cpp
    ${HOST_CONTROL_SOURCES}
    ${HOST_ROOT}/src/communication/HostProtocol.cpp
    ${HOST_ROOT}/src/state/RobotStateStore.cpp)
target_include_directories(control_session_test PRIVATE ${HOST_ROOT}/src/communication)
target_link_libraries(control_session_test PRIVATE Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Network Qt${QT_VERSION_MAJOR}::Test)
add_test(NAME control_v2_session COMMAND control_session_test)
set_tests_properties(control_v2_session PROPERTIES TIMEOUT 30)

add_executable(control_transport_test
    control_transport_test.cpp ${HOST_CONTROL_SOURCES}
    ${HOST_ROOT}/src/communication/RobotTcpClient.cpp
    ${HOST_ROOT}/src/communication/HostProtocol.cpp
    ${HOST_ROOT}/src/utils/Logger.cpp
    ${HOST_ROOT}/src/utils/ErrorHandler.cpp)
target_include_directories(control_transport_test PRIVATE ${HOST_ROOT}/src/communication ${HOST_ROOT}/src/utils)
target_link_libraries(control_transport_test PRIVATE Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Network Qt${QT_VERSION_MAJOR}::Test)
add_test(NAME control_v2_transport COMMAND control_transport_test)
set_tests_properties(control_v2_transport PROPERTIES TIMEOUT 30)

add_executable(control_reference_probe
    control_reference_probe.cpp ${HOST_CONTROL_SOURCES}
    ${HOST_ROOT}/src/communication/RobotTcpClient.cpp
    ${HOST_ROOT}/src/communication/HostProtocol.cpp
    ${HOST_ROOT}/src/utils/Logger.cpp
    ${HOST_ROOT}/src/utils/ErrorHandler.cpp)
target_include_directories(control_reference_probe PRIVATE ${HOST_ROOT}/src/communication ${HOST_ROOT}/src/utils)
target_link_libraries(control_reference_probe PRIVATE Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Network Qt${QT_VERSION_MAJOR}::Test)

add_executable(motion_presets_test motion_presets_test.cpp ${HOST_CONTROL_SOURCES}
    ${HOST_ROOT}/src/communication/HostProtocol.cpp)
target_include_directories(motion_presets_test PRIVATE ${HOST_ROOT}/src/communication)
target_link_libraries(motion_presets_test PRIVATE Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Network Qt${QT_VERSION_MAJOR}::Test)
add_test(NAME motion_presets_protocol COMMAND motion_presets_test)
set_tests_properties(motion_presets_protocol PROPERTIES TIMEOUT 30)
