# ============================================
# State Layer (运行态存储层)
# ============================================

set(STATE_SOURCES
    RobotStateStore.cpp
    RobotStateStore.h
)

add_library(state_layer STATIC ${STATE_SOURCES})

target_link_libraries(state_layer PUBLIC
    Qt6::Core
    communication_layer
)

target_include_directories(state_layer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
