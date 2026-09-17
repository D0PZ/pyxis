# ============================================================================
#  Pyxis - Reproductor de video de alto rendimiento para Windows 11 x64
#
#  El binario resultante es autocontenido: FFmpeg y la CRT se enlazan de forma
#  estatica, y todo lo demas (D3D11, DXGI, Direct2D, WASAPI) forma parte del
#  sistema operativo. No requiere instalador, DLLs ni VC++ Redistributable.
# ============================================================================
cmake_minimum_required(VERSION 3.25)

project(pyxis
    VERSION 0.1.0
    DESCRIPTION "Reproductor de video de alto rendimiento para Windows 11 x64"
    # RC es imprescindible: sin el, CMake no sabe compilar res/pyxis.rc y el
    # manifiesto (con la conciencia de PPP y el UTF-8) no llegaria al binario.
    LANGUAGES CXX RC)

# ---------------------------------------------------------------------------
#  Verificaciones de plataforma. Pyxis es deliberadamente monoplataforma: se
#  apoya en D3D11VA, DXGI flip-model y WASAPI, sin capas de abstraccion.
# ---------------------------------------------------------------------------
if(NOT WIN32)
    message(FATAL_ERROR "Pyxis solo soporta Windows 11 x64.")
endif()
if(NOT MSVC)
    message(FATAL_ERROR "Pyxis requiere el compilador MSVC (Visual Studio Build Tools).")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Pyxis solo soporta compilaciones de 64 bits.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(PYXIS_ENABLE_AVX2 "Compilar el codigo propio con AVX2 (FFmpeg despacha en tiempo de ejecucion de todos modos)" OFF)
option(PYXIS_WARNINGS_AS_ERRORS "Tratar los avisos del compilador como errores" OFF)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/CompileShaders.cmake)

# ---------------------------------------------------------------------------
#  FFmpeg (vcpkg, triplete x64-windows-static)
# ---------------------------------------------------------------------------
find_package(FFMPEG REQUIRED)

# ---------------------------------------------------------------------------
#  Shaders HLSL -> cabeceras C con el bytecode incrustado.
#  Compilarlos en tiempo de construccion evita depender de d3dcompiler_47.dll
#  y elimina el coste de compilar shaders al arrancar.
# ---------------------------------------------------------------------------
set(PYXIS_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders")
set(PYXIS_SHADER_OUT "${CMAKE_CURRENT_BINARY_DIR}/generated/shaders")

pyxis_compile_shader(PYXIS_SHADER_HEADERS
    SOURCE  "${PYXIS_SHADER_DIR}/fullscreen.hlsl"
    ENTRY   VSMain
    PROFILE vs_5_0
    SYMBOL  g_FullscreenVS
    OUTPUT  "${PYXIS_SHADER_OUT}/fullscreen_vs.h")

pyxis_compile_shader(PYXIS_SHADER_HEADERS
    SOURCE  "${PYXIS_SHADER_DIR}/video.hlsl"
    ENTRY   PSMain
    PROFILE ps_5_0
    SYMBOL  g_VideoPS
    OUTPUT  "${PYXIS_SHADER_OUT}/video_ps.h")

pyxis_compile_shader(PYXIS_SHADER_HEADERS
    SOURCE  "${PYXIS_SHADER_DIR}/overlay.hlsl"
    ENTRY   PSMain
    PROFILE ps_5_0
    SYMBOL  g_OverlayPS
    OUTPUT  "${PYXIS_SHADER_OUT}/overlay_ps.h")

add_custom_target(pyxis_shaders DEPENDS ${PYXIS_SHADER_HEADERS})

# ---------------------------------------------------------------------------
#  Fuentes
# ---------------------------------------------------------------------------
set(PYXIS_SOURCES
    src/main.cpp

    src/core/Log.cpp
    src/core/Text.cpp
    src/core/Paths.cpp
    src/core/Error.cpp
    src/core/Clock.cpp
    src/core/Thread.cpp

    src/media/FFmpegUtil.cpp
    src/media/Demuxer.cpp
    src/media/VideoDecoder.cpp
    src/media/AudioDecoder.cpp
    src/media/Player.cpp
    src/media/Trimmer.cpp

    src/render/Device.cpp
    src/render/SwapChain.cpp
    src/render/VideoRenderer.cpp
    src/render/Overlay.cpp
    src/render/Snapshot.cpp
    src/render/ClipExporter.cpp

    src/audio/WasapiRenderer.cpp

    src/ui/Window.cpp
    src/ui/Controller.cpp
)

add_executable(pyxis WIN32 ${PYXIS_SOURCES} res/pyxis.rc)
add_dependencies(pyxis pyxis_shaders)

target_include_directories(pyxis PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
    ${FFMPEG_INCLUDE_DIRS})

target_link_directories(pyxis PRIVATE ${FFMPEG_LIBRARY_DIRS})

target_link_libraries(pyxis PRIVATE
    ${FFMPEG_LIBRARIES}
    d3d11.lib
    dxgi.lib
    dxguid.lib
    d2d1.lib
    dwrite.lib
    windowscodecs.lib   # WIC: codificacion PNG de las capturas
    avrt.lib        # MMCSS: prioridad de hilo multimedia
    winmm.lib
    ole32.lib
    oleaut32.lib
    uuid.lib
    shell32.lib
    shcore.lib      # PerMonitorV2 DPI
    dwmapi.lib      # barra de titulo oscura
    comdlg32.lib
    user32.lib
    gdi32.lib
    advapi32.lib
    propsys.lib     # WASAPI / IPropertyStore
    bcrypt.lib
    secur32.lib
    strmiids.lib
    mfplat.lib
    mfuuid.lib
    ws2_32.lib)

target_compile_definitions(pyxis PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    STRICT
    # No se fija _WIN32_WINNT ni NTDDI_VERSION: sin ellos, sdkddkver.h apunta a
    # la version mas alta que conoce el SDK instalado, que es justo lo que
    # queremos en un proyecto que solo soporta Windows 11. Fijarlos a mano solo
    # sirve para ocultar APIs recientes por accidente.
    __STDC_CONSTANT_MACROS      # requerido por las cabeceras de FFmpeg en C++
    PYXIS_VERSION="${PROJECT_VERSION}")

target_compile_options(pyxis PRIVATE
    /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /EHsc
    /MP                 # compilacion paralela
    /wd4100             # parametro formal sin referenciar (callbacks de Win32)
    $<$<CONFIG:Release,RelWithDebInfo>:/O2 /Oi /Ot /GL /Gy /fp:fast>
    $<$<CONFIG:Debug>:/Od /Zi /RTC1>)

if(PYXIS_ENABLE_AVX2)
    target_compile_options(pyxis PRIVATE /arch:AVX2)
endif()
if(PYXIS_WARNINGS_AS_ERRORS)
    target_compile_options(pyxis PRIVATE /WX)
endif()

target_link_options(pyxis PRIVATE
    /SUBSYSTEM:WINDOWS
    /MANIFEST:NO        # el manifiesto se incrusta desde res/pyxis.rc
    $<$<CONFIG:Release,RelWithDebInfo>:/LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO>
    $<$<CONFIG:RelWithDebInfo>:/DEBUG:FULL>)

set_target_properties(pyxis PROPERTIES
    OUTPUT_NAME "pyxis"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    VS_DPI_AWARE "PerMonitor")

# ---------------------------------------------------------------------------
#  Resumen de configuracion
# ---------------------------------------------------------------------------
message(STATUS "-----------------------------------------------------------")
message(STATUS "  Pyxis ${PROJECT_VERSION}")
message(STATUS "  Compilador   : MSVC ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Triplete     : ${VCPKG_TARGET_TRIPLET}")
message(STATUS "  CRT          : estatica (/MT)")
message(STATUS "  AVX2 propio  : ${PYXIS_ENABLE_AVX2}")
message(STATUS "-----------------------------------------------------------")
