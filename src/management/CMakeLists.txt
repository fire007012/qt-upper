# ============================================
# Management Layer (管理与配置服务层)
# ============================================
# management_core: 纯非 GUI 服务（RuntimeService / ConfigurationService / RuntimeJournal）
# management_ui:   Qt Widgets 管理与配置界面
# ============================================

set(MANAGEMENT_CORE_SOURCES
    ConfigurationService.cpp
    ConfigurationService.h
    RuntimeService.cpp
    RuntimeService.h
    RuntimeJournal.cpp
    RuntimeJournal.h
)

add_library(management_core STATIC ${MANAGEMENT_CORE_SOURCES})

target_link_libraries(management_core PUBLIC
    Qt6::Core
    Qt6::Network
    communication_layer
    utils_layer
)

target_include_directories(management_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

set(MANAGEMENT_UI_SOURCES
    ConfigurationWidget.cpp
    ConfigurationWidget.h
    RuntimeManagementWidget.cpp
    RuntimeManagementWidget.h
)

add_library(management_ui STATIC ${MANAGEMENT_UI_SOURCES})

target_link_libraries(management_ui PUBLIC
    Qt6::Widgets
    management_core
    communication_layer
    utils_layer
)

target_include_directories(management_ui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
