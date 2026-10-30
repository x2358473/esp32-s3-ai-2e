# ES7210 + ES8311 local audio example: no UI, no network stack in app.

set(SOURCES
    "audio/audio_codec.c"
    "audio/audio_service.c"
    "audio/codecs/box_audio_codec.c"
    "audio/codecs/es8311_audio_codec.c"
    "audio/processors/audio_debugger.c"
    "audio/processors/afe_audio_processor.c"
    "audio/processors/no_audio_processor.c"
    "audio_application.c"
    "button.c"
    "example_es7210_es8311.c"
)

set(INCLUDE_DIRS
    "."
    "audio"
    "audio/codecs"
    "audio/processors"
)

# Use project root: CMAKE_CURRENT_SOURCE_DIR is unreliable during component_get_requirements (may be under build/).
set(EXAMPLE_TEST_PCM "${CMAKE_SOURCE_DIR}/main/assets/canon.pcm") 
set(EXAMPLE_EMBED_PCM_ARGS)
if(EXISTS "${EXAMPLE_TEST_PCM}")
    list(APPEND EXAMPLE_EMBED_PCM_ARGS EMBED_FILES "${EXAMPLE_TEST_PCM}")
    message(STATUS "Embedded test PCM: ${EXAMPLE_TEST_PCM}")
else()
    message(WARNING "canon.pcm not found at ${EXAMPLE_TEST_PCM} — copy your test PCM here; auto-play will skip until then")
endif()

idf_component_register(
    SRCS ${SOURCES}
    ${EXAMPLE_EMBED_PCM_ARGS}
    INCLUDE_DIRS ${INCLUDE_DIRS}
    WHOLE_ARCHIVE
)
