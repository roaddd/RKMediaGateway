if(BUILD_TARGET STREQUAL "rkmgw" OR BUILD_TARGET STREQUAL "all")
    add_executable(rkmgw
        ${CMAKE_CURRENT_SOURCE_DIR}/shellTool/rkmgw.c
    )
    target_include_directories(rkmgw PRIVATE
        ${PROJECT_SOURCE_DIR}/bussiness/common/debugCommand/inc
    )
    set_target_properties(rkmgw PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endif()
