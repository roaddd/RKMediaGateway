cmake_minimum_required(VERSION 3.10)
project(RKMediaGateway VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED BUILD_TARGET)
    set(BUILD_TARGET "all")
endif()

include_directories(${PROJECT_SOURCE_DIR}/bussiness/v4l2Capture/inc)
include_directories(${PROJECT_SOURCE_DIR}/bussiness/mppEncoder/inc)
include_directories(${PROJECT_SOURCE_DIR}/bussiness/rtspStreamer/inc)
include_directories(${PROJECT_SOURCE_DIR}/thirdparty/mpp/inc)
include_directories(${PROJECT_SOURCE_DIR}/thirdparty/rtspServer/inc)

set(THIRDPARTY_MPP_LIB_DIR ${PROJECT_SOURCE_DIR}/thirdparty/mpp/lib)
set(THIRDPARTY_RTSP_LIB_DIR ${PROJECT_SOURCE_DIR}/thirdparty/rtspServer/lib)
link_directories(${THIRDPARTY_MPP_LIB_DIR})
link_directories(${THIRDPARTY_RTSP_LIB_DIR})

file(GLOB V4L2_CAPTURE_SRC ${PROJECT_SOURCE_DIR}/bussiness/v4l2Capture/src/*.c)
file(GLOB MPP_ENCODER_SRC ${PROJECT_SOURCE_DIR}/bussiness/mppEncoder/src/*.c)
file(GLOB RTSP_STREAMER_SRC ${PROJECT_SOURCE_DIR}/bussiness/rtspStreamer/src/*.c)

if(BUILD_TARGET STREQUAL "v4l2_test" OR BUILD_TARGET STREQUAL "all")
    add_executable(v4l2_test
        ${PROJECT_SOURCE_DIR}/main/main_v4l2Capture_test.cpp
        ${V4L2_CAPTURE_SRC}
    )
    target_link_libraries(v4l2_test PRIVATE pthread m)
    set_target_properties(v4l2_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endif()

if(BUILD_TARGET STREQUAL "mpp_test" OR BUILD_TARGET STREQUAL "all")
    add_executable(mpp_test
        ${PROJECT_SOURCE_DIR}/main/main_mpp_h264_test.cpp
        ${V4L2_CAPTURE_SRC}
        ${MPP_ENCODER_SRC}
    )
    target_link_libraries(mpp_test PRIVATE rockchip_mpp pthread m)
    set_target_properties(mpp_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endif()

if(BUILD_TARGET STREQUAL "rtsp_gateway" OR BUILD_TARGET STREQUAL "all")
    add_executable(rtsp_gateway
        ${PROJECT_SOURCE_DIR}/main/main.cpp
        ${V4L2_CAPTURE_SRC}
        ${MPP_ENCODER_SRC}
        ${RTSP_STREAMER_SRC}
    )
    target_link_libraries(rtsp_gateway PRIVATE rockchip_mpp rtsp_server pthread m)
    set_target_properties(rtsp_gateway PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endif()

message(STATUS "BUILD_TARGET=${BUILD_TARGET}")
message(STATUS "CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")
message(STATUS "C compiler=${CMAKE_C_COMPILER}")
