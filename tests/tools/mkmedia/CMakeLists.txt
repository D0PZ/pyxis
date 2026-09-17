# ---------------------------------------------------------------------------
#  mkmedia - genera el material que necesita la suite de pruebas
#
#  Se enlaza contra el mismo FFmpeg que el reproductor, asi que no anade ninguna
#  dependencia. Sale a bin/tools y no a bin/ para no ensuciar el directorio que
#  se publica como artefacto.
# ---------------------------------------------------------------------------
add_executable(mkmedia main.cpp)

target_include_directories(mkmedia PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(mkmedia PRIVATE ${FFMPEG_LIBRARY_DIRS})

target_link_libraries(mkmedia PRIVATE
    ${FFMPEG_LIBRARIES}
    # h264_mf vive en Media Foundation; el resto son dependencias que arrastra
    # la propia libavcodec al enlazarse de forma estatica.
    mfplat.lib
    mfuuid.lib
    strmiids.lib
    ole32.lib
    oleaut32.lib
    uuid.lib
    bcrypt.lib
    secur32.lib
    ws2_32.lib
    advapi32.lib
    user32.lib)

target_compile_definitions(mkmedia PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    __STDC_CONSTANT_MACROS)

target_compile_options(mkmedia PRIVATE /W4 /permissive- /utf-8 /EHsc)

set_target_properties(mkmedia PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/tools")
