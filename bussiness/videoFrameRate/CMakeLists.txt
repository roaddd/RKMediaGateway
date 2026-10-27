add_library(video_frame_rate STATIC
    src/videoFrameRate.c
)

target_include_directories(video_frame_rate
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/inc
    PRIVATE
        ${PROJECT_SOURCE_DIR}/bussiness/common
        ${PROJECT_SOURCE_DIR}/bussiness/logger/inc
)
