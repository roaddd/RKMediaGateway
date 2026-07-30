set(THIRDPARTY_DATACHANNEL_ROOT ${PROJECT_SOURCE_DIR}/thirdparty/datachannel)
set(THIRDPARTY_DATACHANNEL_INC_DIR ${THIRDPARTY_DATACHANNEL_ROOT}/inc)
set(THIRDPARTY_DATACHANNEL_LIB ${THIRDPARTY_DATACHANNEL_ROOT}/lib/libdatachannel.so.0.24.5)

if(BUILD_TARGET STREQUAL "webrtc_peerconnection_test" OR BUILD_TARGET STREQUAL "all")
    if(NOT EXISTS ${THIRDPARTY_DATACHANNEL_LIB})
        message(FATAL_ERROR "libdatachannel not found: ${THIRDPARTY_DATACHANNEL_LIB}")
    endif()

    add_executable(webrtc_peerconnection_test
        ${CMAKE_CURRENT_SOURCE_DIR}/webrtc_peerconnection_test.cpp
    )

    target_include_directories(webrtc_peerconnection_test PRIVATE
        ${THIRDPARTY_DATACHANNEL_INC_DIR}
    )
    target_link_libraries(webrtc_peerconnection_test PRIVATE
        ${THIRDPARTY_DATACHANNEL_LIB}
        pthread
    )
    target_compile_definitions(webrtc_peerconnection_test PRIVATE
        RTC_ENABLE_MEDIA=0
        RTC_ENABLE_WEBSOCKET=0
    )
    target_compile_features(webrtc_peerconnection_test PRIVATE cxx_std_17)
    set_target_properties(webrtc_peerconnection_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
        BUILD_RPATH ${THIRDPARTY_DATACHANNEL_ROOT}/lib
        INSTALL_RPATH ${THIRDPARTY_DATACHANNEL_ROOT}/lib
    )
endif()

if(BUILD_TARGET STREQUAL "webrtc_ws_server_test" OR BUILD_TARGET STREQUAL "all")
    if(NOT EXISTS ${THIRDPARTY_DATACHANNEL_LIB})
        message(FATAL_ERROR "libdatachannel not found: ${THIRDPARTY_DATACHANNEL_LIB}")
    endif()

    add_executable(webrtc_ws_server_test
        ${CMAKE_CURRENT_SOURCE_DIR}/webrtc_ws_server_test.cpp
    )

    target_include_directories(webrtc_ws_server_test PRIVATE
        ${THIRDPARTY_DATACHANNEL_INC_DIR}
    )
    target_link_libraries(webrtc_ws_server_test PRIVATE
        ${THIRDPARTY_DATACHANNEL_LIB}
        pthread
    )
    target_compile_definitions(webrtc_ws_server_test PRIVATE
        RTC_ENABLE_MEDIA=0
        RTC_ENABLE_WEBSOCKET=1
    )
    target_compile_features(webrtc_ws_server_test PRIVATE cxx_std_17)
    set_target_properties(webrtc_ws_server_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
        BUILD_RPATH ${THIRDPARTY_DATACHANNEL_ROOT}/lib
        INSTALL_RPATH ${THIRDPARTY_DATACHANNEL_ROOT}/lib
    )
endif()
