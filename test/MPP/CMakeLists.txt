if(BUILD_TARGET STREQUAL "mpp_encoder_capacity_test" OR BUILD_TARGET STREQUAL "all")
    add_executable(mpp_encoder_capacity_test
        ${CMAKE_CURRENT_SOURCE_DIR}/mpp_encoder_capacity_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/latencyStatistics.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/systemMetricsMonitor.cpp
        ${MPP_ENCODER_SRC}
    )
    target_link_libraries(mpp_encoder_capacity_test PRIVATE rockchip_mpp logger pthread m)
    set_target_properties(mpp_encoder_capacity_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endif()

if(BUILD_TARGET STREQUAL "mpp_encoder_dmabuf_test" OR BUILD_TARGET STREQUAL "all")
    add_executable(mpp_encoder_dmabuf_test
        ${CMAKE_CURRENT_SOURCE_DIR}/mpp_encoder_dmabuf_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/latencyStatistics.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/systemMetricsMonitor.cpp
        ${V4L2_CAPTURE_SRC}
        ${MPP_ENCODER_SRC}
    )
    target_link_libraries(mpp_encoder_dmabuf_test PRIVATE rockchip_mpp logger pthread m)
    set_target_properties(mpp_encoder_dmabuf_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endif()

if(BUILD_TARGET STREQUAL "mpp_latency_statistics_test" OR BUILD_TARGET STREQUAL "all")
    add_executable(mpp_latency_statistics_test
        ${CMAKE_CURRENT_SOURCE_DIR}/latency_statistics_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/latencyStatistics.cpp
    )
    set_target_properties(mpp_latency_statistics_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endif()
