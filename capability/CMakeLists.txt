add_library(media_capability STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/mediaCapability.c
)

target_include_directories(media_capability
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/inc
        ${PROJECT_SOURCE_DIR}/bussiness/common
        ${PROJECT_SOURCE_DIR}/bussiness/mediaCore/inc
)
