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
include_directories(${PROJECT_SOURCE_DIR}/bussiness/mediaGateway/inc)
include_directories(${PROJECT_SOURCE_DIR}/bussiness/gb28181/inc)
include_directories(${PROJECT_SOURCE_DIR}/bussiness/rtspStreamer/inc)
include_directories(${PROJECT_SOURCE_DIR}/bussiness/rtmpStreamer/inc)
include_directories(${PROJECT_SOURCE_DIR}/thirdparty/mpp/inc)
include_directories(${PROJECT_SOURCE_DIR}/thirdparty/rtspServer/inc)
include_directories(${PROJECT_SOURCE_DIR}/thirdparty/rtmp/inc)
include_directories(${PROJECT_SOURCE_DIR}/thirdparty/osip/inc)

set(THIRDPARTY_MPP_LIB_DIR ${PROJECT_SOURCE_DIR}/thirdparty/mpp/lib)
set(THIRDPARTY_RTMP_ROOT ${PROJECT_SOURCE_DIR}/thirdparty/rtmp)
set(THIRDPARTY_RTMP_INC_DIR ${THIRDPARTY_RTMP_ROOT}/inc)
set(THIRDPARTY_RTMP_LIB_DIR ${THIRDPARTY_RTMP_ROOT}/lib)
set(THIRDPARTY_RTSP_LIB_DIR ${PROJECT_SOURCE_DIR}/thirdparty/rtspServer/lib)
set(THIRDPARTY_EXOSIP_ROOT ${PROJECT_SOURCE_DIR}/thirdparty/exosip)
set(THIRDPARTY_EXOSIP_INC_DIR ${THIRDPARTY_EXOSIP_ROOT}/inc)
set(THIRDPARTY_EXOSIP_LIB_DIR ${THIRDPARTY_EXOSIP_ROOT}/lib)
set(THIRDPARTY_OSIP_ROOT ${PROJECT_SOURCE_DIR}/thirdparty/osip)
set(THIRDPARTY_OSIP_LIB_DIR ${THIRDPARTY_OSIP_ROOT}/lib)
set(THIRDPARTY_OPENSSL_ROOT ${PROJECT_SOURCE_DIR}/thirdparty/openssl)
set(THIRDPARTY_OPENSSL_INC_DIR ${THIRDPARTY_OPENSSL_ROOT}/inc)
set(THIRDPARTY_OPENSSL_LIB_DIR ${THIRDPARTY_OPENSSL_ROOT}/lib)
set(EXOSIP_COMPAT_INCLUDE_DIR ${CMAKE_BINARY_DIR}/generated/exosip_include)
set(OPENSSL_COMPAT_INCLUDE_DIR ${CMAKE_BINARY_DIR}/generated/openssl_include)

file(MAKE_DIRECTORY ${EXOSIP_COMPAT_INCLUDE_DIR}/eXosip2)
file(COPY ${THIRDPARTY_EXOSIP_INC_DIR}/ DESTINATION ${EXOSIP_COMPAT_INCLUDE_DIR}/eXosip2 FILES_MATCHING PATTERN "*.h")
file(MAKE_DIRECTORY ${OPENSSL_COMPAT_INCLUDE_DIR}/openssl)
file(COPY ${THIRDPARTY_OPENSSL_INC_DIR}/ DESTINATION ${OPENSSL_COMPAT_INCLUDE_DIR}/openssl FILES_MATCHING PATTERN "*.h")

include_directories(${EXOSIP_COMPAT_INCLUDE_DIR})
include_directories(${OPENSSL_COMPAT_INCLUDE_DIR})
link_directories(${THIRDPARTY_MPP_LIB_DIR})
link_directories(${THIRDPARTY_RTMP_LIB_DIR})
link_directories(${THIRDPARTY_RTSP_LIB_DIR})
link_directories(${THIRDPARTY_EXOSIP_LIB_DIR})
link_directories(${THIRDPARTY_OSIP_LIB_DIR})
link_directories(${THIRDPARTY_OPENSSL_LIB_DIR})

file(GLOB V4L2_CAPTURE_SRC ${PROJECT_SOURCE_DIR}/bussiness/v4l2Capture/src/*.c)
file(GLOB MPP_ENCODER_SRC ${PROJECT_SOURCE_DIR}/bussiness/mppEncoder/src/*.c)
file(GLOB MEDIA_GATEWAY_SRC ${PROJECT_SOURCE_DIR}/bussiness/mediaGateway/src/*.c)
file(GLOB GB28181_SRC ${PROJECT_SOURCE_DIR}/bussiness/gb28181/src/*.c)
file(GLOB RTSP_STREAMER_SRC ${PROJECT_SOURCE_DIR}/bussiness/rtspStreamer/src/*.c)
file(GLOB RTMP_STREAMER_SRC ${PROJECT_SOURCE_DIR}/bussiness/rtmpStreamer/src/*.c)

option(ENABLE_RTMP "Build RTMP sink skeleton" ON)
set(RTMP_ROOT "${THIRDPARTY_RTMP_ROOT}" CACHE PATH "Root path of librtmp installation")

if(ENABLE_RTMP)
    set(LIBRTMP_COMPAT_INCLUDE_DIR ${CMAKE_BINARY_DIR}/generated/rtmp_include)
    unset(LIBRTMP_INCLUDE_DIR CACHE)
    unset(LIBRTMP_LIBRARY CACHE)

    if(EXISTS ${THIRDPARTY_RTMP_INC_DIR}/rtmp.h)
        set(LIBRTMP_INCLUDE_DIR ${THIRDPARTY_RTMP_INC_DIR})
    endif()

    if(EXISTS ${THIRDPARTY_RTMP_LIB_DIR}/librtmp.so)
        set(LIBRTMP_LIBRARY ${THIRDPARTY_RTMP_LIB_DIR}/librtmp.so)
    elseif(EXISTS ${THIRDPARTY_RTMP_LIB_DIR}/librtmp.a)
        set(LIBRTMP_LIBRARY ${THIRDPARTY_RTMP_LIB_DIR}/librtmp.a)
    else()
        set(_RTMP_INCLUDE_HINTS)
        set(_RTMP_LIBRARY_HINTS)
        if(RTMP_ROOT)
            list(APPEND _RTMP_INCLUDE_HINTS
                ${RTMP_ROOT}/include
                ${RTMP_ROOT}/inc
                ${RTMP_ROOT}
            )
            list(APPEND _RTMP_LIBRARY_HINTS
                ${RTMP_ROOT}/lib
                ${RTMP_ROOT}/lib64
                ${RTMP_ROOT}
            )
        endif()

        find_path(LIBRTMP_INCLUDE_DIR
            NAMES rtmp.h librtmp/rtmp.h
            HINTS ${_RTMP_INCLUDE_HINTS}
        )
        find_library(LIBRTMP_LIBRARY
            NAMES rtmp librtmp
            HINTS ${_RTMP_LIBRARY_HINTS}
        )
    endif()

    if(LIBRTMP_INCLUDE_DIR)
        file(MAKE_DIRECTORY ${LIBRTMP_COMPAT_INCLUDE_DIR}/librtmp)
        file(COPY
            ${LIBRTMP_INCLUDE_DIR}/rtmp.h
            ${LIBRTMP_INCLUDE_DIR}/amf.h
            ${LIBRTMP_INCLUDE_DIR}/http.h
            ${LIBRTMP_INCLUDE_DIR}/log.h
            DESTINATION ${LIBRTMP_COMPAT_INCLUDE_DIR}/librtmp
        )
    endif()
endif()

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
    set(GATEWAY_OUTPUT_SRC
        ${MEDIA_GATEWAY_SRC}
        ${RTSP_STREAMER_SRC}
        ${GB28181_SRC}
    )
    if(ENABLE_RTMP)
        list(APPEND GATEWAY_OUTPUT_SRC ${RTMP_STREAMER_SRC})
    endif()

    add_executable(rtsp_gateway
        ${PROJECT_SOURCE_DIR}/main/main.cpp
        ${V4L2_CAPTURE_SRC}
        ${MPP_ENCODER_SRC}
        ${GATEWAY_OUTPUT_SRC}
    )
    target_link_libraries(rtsp_gateway PRIVATE rockchip_mpp rtsp_server eXosip2 osip2 osipparser2 ssl crypto pthread m)
    if(ENABLE_RTMP)
        target_compile_definitions(rtsp_gateway PRIVATE ENABLE_RTMP_SINK=1)
        if(LIBRTMP_INCLUDE_DIR AND LIBRTMP_LIBRARY)
            target_include_directories(rtsp_gateway PRIVATE ${LIBRTMP_INCLUDE_DIR} ${LIBRTMP_COMPAT_INCLUDE_DIR})
            target_link_libraries(rtsp_gateway PRIVATE ${LIBRTMP_LIBRARY})
            target_compile_definitions(rtsp_gateway PRIVATE ENABLE_RTMP_LIBRTMP=1)
        else()
            message(WARNING "ENABLE_RTMP=ON but librtmp not found; building RTMP skeleton without real transport")
        endif()
    endif()
    set_target_properties(rtsp_gateway PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endif()

if(BUILD_TARGET STREQUAL "dual_output_test" OR BUILD_TARGET STREQUAL "all")
    set(DUAL_OUTPUT_SRC
        ${MEDIA_GATEWAY_SRC}
        ${RTSP_STREAMER_SRC}
        ${GB28181_SRC}
    )
    if(ENABLE_RTMP)
        list(APPEND DUAL_OUTPUT_SRC ${RTMP_STREAMER_SRC})
    endif()

    add_executable(dual_output_test
        ${PROJECT_SOURCE_DIR}/main/main_dual_output_test.cpp
        ${V4L2_CAPTURE_SRC}
        ${MPP_ENCODER_SRC}
        ${DUAL_OUTPUT_SRC}
    )
    target_link_libraries(dual_output_test PRIVATE rockchip_mpp rtsp_server eXosip2 osip2 osipparser2 ssl crypto pthread m)
    if(ENABLE_RTMP)
        target_compile_definitions(dual_output_test PRIVATE ENABLE_RTMP_SINK=1)
        if(LIBRTMP_INCLUDE_DIR AND LIBRTMP_LIBRARY)
            target_include_directories(dual_output_test PRIVATE ${LIBRTMP_INCLUDE_DIR} ${LIBRTMP_COMPAT_INCLUDE_DIR})
            target_link_libraries(dual_output_test PRIVATE ${LIBRTMP_LIBRARY})
            target_compile_definitions(dual_output_test PRIVATE ENABLE_RTMP_LIBRTMP=1)
        else()
            message(WARNING "ENABLE_RTMP=ON but librtmp not found; building dual_output_test without real RTMP transport")
        endif()
    endif()
    set_target_properties(dual_output_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endif()

if(BUILD_TARGET STREQUAL "all_services" OR BUILD_TARGET STREQUAL "gb28181_device" OR BUILD_TARGET STREQUAL "all")
    add_executable(all_services
        ${PROJECT_SOURCE_DIR}/main/main_all_services.cpp
        ${V4L2_CAPTURE_SRC}
        ${MPP_ENCODER_SRC}
        ${MEDIA_GATEWAY_SRC}
        ${RTSP_STREAMER_SRC}
        ${GB28181_SRC}
    )
    target_link_libraries(all_services PRIVATE rockchip_mpp rtsp_server eXosip2 osip2 osipparser2 ssl crypto pthread m)
    target_include_directories(all_services PRIVATE
        ${THIRDPARTY_OSIP_ROOT}/inc
        ${EXOSIP_COMPAT_INCLUDE_DIR}
        ${OPENSSL_COMPAT_INCLUDE_DIR}
    )
    set_target_properties(all_services PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endif()

message(STATUS "BUILD_TARGET=${BUILD_TARGET}")
message(STATUS "ENABLE_RTMP=${ENABLE_RTMP}")
message(STATUS "LIBRTMP_INCLUDE_DIR=${LIBRTMP_INCLUDE_DIR}")
message(STATUS "LIBRTMP_LIBRARY=${LIBRTMP_LIBRARY}")
message(STATUS "EXOSIP_COMPAT_INCLUDE_DIR=${EXOSIP_COMPAT_INCLUDE_DIR}")
message(STATUS "OPENSSL_COMPAT_INCLUDE_DIR=${OPENSSL_COMPAT_INCLUDE_DIR}")
message(STATUS "CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")
message(STATUS "C compiler=${CMAKE_C_COMPILER}")

