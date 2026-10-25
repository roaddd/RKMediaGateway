if(BUILD_TARGET STREQUAL "media_capability_test" OR BUILD_TARGET STREQUAL "all")
    add_executable(media_capability_test
        ${CMAKE_CURRENT_SOURCE_DIR}/media_capability_test.c
    )
    target_link_libraries(media_capability_test PRIVATE media_capability)
    set_target_properties(media_capability_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endif()
