cmake_minimum_required(VERSION 3.16)

project(utils_layer VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 查找Qt6包
find_package(Qt6 REQUIRED COMPONENTS Core)

# 设置自动MOC
set(CMAKE_AUTOMOC ON)

# 源文件
set(SOURCES
    Logger.cpp
    ErrorHandler.cpp
)

# 头文件
set(HEADERS
    Logger.h
    ErrorHandler.h
)

# 创建静态库
add_library(utils_layer STATIC ${SOURCES} ${HEADERS})


# 链接Qt6库
target_link_libraries(utils_layer PUBLIC Qt6::Core)

# 设置包含目录
target_include_directories(utils_layer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
