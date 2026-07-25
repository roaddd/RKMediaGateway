if(BUILD_TARGET STREQUAL "enum_frame_interval_test" OR BUILD_TARGET STREQUAL "all")
    add_executable(enum_frame_interval_test
        ${PROJECT_SOURCE_DIR}/test/sensor/enum_frame_interval_test.c
    )
    set_target_properties(enum_frame_interval_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endif()

add_subdirectory(webRTC)
