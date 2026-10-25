if(BUILD_TARGET STREQUAL "media_frame_source_stats_test" OR BUILD_TARGET STREQUAL "all")
    add_executable(media_frame_source_stats_test
        ${CMAKE_CURRENT_SOURCE_DIR}/src/media_frame_source_stats_test.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/testLoggerStub.c
        ${PROJECT_SOURCE_DIR}/bussiness/mediaFrameSource/src/mediaFrameSourceStats.c
    )
    target_include_directories(media_frame_source_stats_test PRIVATE
        ${PROJECT_SOURCE_DIR}/bussiness/common
        ${PROJECT_SOURCE_DIR}/bussiness/logger/inc
        ${PROJECT_SOURCE_DIR}/bussiness/mediaFrameSource/inc
    )
    set_target_properties(media_frame_source_stats_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endif()

if(BUILD_TARGET STREQUAL "gateway_pipeline_integration_test")
    add_executable(gateway_pipeline_integration_test
        ${CMAKE_CURRENT_SOURCE_DIR}/src/gateway_pipeline_integration_test.cpp
        ${V4L2_CAPTURE_SRC}
        ${AUDIO_CAPTURE_SRC}
        ${AUDIO_FRAME_SOURCE_SRC}
        ${AUDIO_DECODER_SRC}
        ${AUDIO_PLAYBACK_SRC}
        ${AUDIO_TALKBACK_SRC}
        ${AUDIO_ENCODER_SRC}
        ${MPP_ENCODER_SRC}
        ${MEDIA_CORE_SRC}
        ${MEDIA_FRAME_SOURCE_SRC}
        ${ISP_CONTROLLER_SRC}
        ${DEF_VALUE_SRC}
        ${UTIL_SRC}
        ${THREAD_MESSAGE_QUEUE_SRC}
        ${debug_command_SRC}
        ${SYSTEM_DEBUG_SRC}
        ${MEDIA_OUTPUT_SRC}
        ${MEDIA_GATEWAY_SRC}
        ${MEDIA_GATEWAY_DEBUG_SRC}
    )
    target_link_libraries(gateway_pipeline_integration_test PRIVATE
        media_capability
        rockchip_mpp
        rtsp_server
        eXosip2
        osip2
        osipparser2
        ssl
        crypto
        logger
        pthread
        m
    )
    target_enable_webrtc_output(gateway_pipeline_integration_test)
    target_enable_opus_codec(gateway_pipeline_integration_test)
    target_link_alsa_audio(gateway_pipeline_integration_test)
    target_enable_fdk_aac_encoder(gateway_pipeline_integration_test)
    if(ENABLE_RKAIQ)
        target_compile_definitions(gateway_pipeline_integration_test PRIVATE ENABLE_RKAIQ=1)
        target_link_libraries(gateway_pipeline_integration_test PRIVATE rkaiq)
    endif()
    if(ENABLE_RTMP)
        target_compile_definitions(gateway_pipeline_integration_test PRIVATE ENABLE_RTMP_OUTPUT=1)
        if(LIBRTMP_INCLUDE_DIR AND LIBRTMP_LIBRARY)
            target_include_directories(gateway_pipeline_integration_test SYSTEM PRIVATE
                ${LIBRTMP_INCLUDE_DIR}
                ${LIBRTMP_COMPAT_INCLUDE_DIR}
            )
            target_link_libraries(gateway_pipeline_integration_test PRIVATE ${LIBRTMP_LIBRARY})
            target_compile_definitions(gateway_pipeline_integration_test PRIVATE ENABLE_RTMP_LIBRTMP=1)
        endif()
    endif()
    set_target_properties(gateway_pipeline_integration_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endif()

if(BUILD_TARGET STREQUAL "video_pipeline_stats_test" OR BUILD_TARGET STREQUAL "all")
    add_executable(video_pipeline_stats_test
        ${CMAKE_CURRENT_SOURCE_DIR}/src/video_pipeline_stats_test.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/testLoggerStub.c
        ${PROJECT_SOURCE_DIR}/bussiness/mediaGateway/src/mediaGatewayVideoPipelineStats.c
    )
    target_include_directories(video_pipeline_stats_test PRIVATE
        ${PROJECT_SOURCE_DIR}/bussiness/common
        ${PROJECT_SOURCE_DIR}/bussiness/logger/inc
        ${PROJECT_SOURCE_DIR}/bussiness/mediaGateway/inc
    )
    set_target_properties(video_pipeline_stats_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endif()
