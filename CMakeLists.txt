# CMakeLists.txt（项目根目录）
cmake_minimum_required(VERSION 3.10)
project(RKMediaGateway VERSION 1.0 LANGUAGES C CXX)

# 设置C/C++标准
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 接收build.sh传入的编译目标参数（默认all）
if(NOT DEFINED BUILD_TARGET)
    set(BUILD_TARGET "all")
endif()

# 1. 包含头文件目录
include_directories(${PROJECT_SOURCE_DIR}/bussiness/v4l2Capture/inc)

# 2. 收集V4L2采集模块源码
file(GLOB V4L2_CAPTURE_SRC ${PROJECT_SOURCE_DIR}/bussiness/v4l2Capture/src/*.c)

# 3. 编译目标控制
# 目标1：v4l2_test（编译main_v4l2Capture_test.cpp）
if(BUILD_TARGET STREQUAL "v4l2_test" OR BUILD_TARGET STREQUAL "all")
    add_executable(v4l2_test 
        ${PROJECT_SOURCE_DIR}/main/main_v4l2Capture_test.cpp
        ${V4L2_CAPTURE_SRC}
    )
    # 链接V4L2库（RK3568需要）
    target_link_libraries(v4l2_test PRIVATE pthread m)
    # 设置输出路径
    set_target_properties(v4l2_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endif()

# 目标2：main（编译main.cpp，后续扩展）
# if(BUILD_TARGET STREQUAL "all")
#     add_executable(main 
#         ${PROJECT_SOURCE_DIR}/main/main.cpp
#         ${V4L2_CAPTURE_SRC}
#     )
#     target_link_libraries(main PRIVATE pthread m)
#     set_target_properties(main PROPERTIES
#         RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
#     )
# endif()

# 打印编译信息（方便调试）
message(STATUS "编译目标：${BUILD_TARGET}")
message(STATUS "编译模式：${CMAKE_BUILD_TYPE}")
message(STATUS "编译器：${CMAKE_C_COMPILER}")