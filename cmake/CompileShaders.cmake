# ============================================================================
#  pyxis_compile_shader
#
#  Compila un shader HLSL a una cabecera C con el bytecode incrustado usando
#  fxc.exe del Windows SDK. Incrustar el bytecode significa que el ejecutable
#  no necesita d3dcompiler_47.dll y no paga coste de compilacion al arrancar.
#
#  Uso:
#     pyxis_compile_shader(<lista_salida>
#         SOURCE  <ruta.hlsl>
#         ENTRY   <punto_de_entrada>
#         PROFILE <vs_5_0|ps_5_0|...>
#         SYMBOL  <nombre_del_array_C>
#         OUTPUT  <ruta.h>)
# ============================================================================

if(NOT DEFINED PYXIS_FXC_EXECUTABLE)
    # El generador Ninja no expone WINDOWS_SDK_BIN, asi que hay que encontrar
    # fxc.exe por cuenta propia.
    #
    # La pista mas fiable es rc.exe: CMake ya lo ha resuelto para compilar los
    # recursos, vive en el MISMO directorio del SDK que fxc.exe y garantiza que
    # ambos pertenecen a la misma version del kit. Leer el registro seria mas
    # directo, pero la sintaxis [HKEY_...] de get_filename_component desaparecio
    # en CMake 4.
    set(_pyxis_sdk_hints "")
    if(CMAKE_RC_COMPILER)
        get_filename_component(_pyxis_rc_dir "${CMAKE_RC_COMPILER}" DIRECTORY)
        list(APPEND _pyxis_sdk_hints "${_pyxis_rc_dir}")
    endif()

    # Respaldo: cualquier version del SDK instalada en las rutas canonicas, de
    # la mas reciente a la mas antigua.
    file(GLOB _pyxis_sdk_bins
        "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/*/x64"
        "$ENV{ProgramFiles}/Windows Kits/10/bin/*/x64")
    list(SORT _pyxis_sdk_bins)
    list(REVERSE _pyxis_sdk_bins)
    list(APPEND _pyxis_sdk_hints ${_pyxis_sdk_bins})

    find_program(PYXIS_FXC_EXECUTABLE
        NAMES fxc fxc.exe
        HINTS ${_pyxis_sdk_hints}
        DOC "Compilador de shaders HLSL (fxc.exe) del Windows SDK")

    if(NOT PYXIS_FXC_EXECUTABLE)
        message(FATAL_ERROR
            "No se encontro fxc.exe. Instala el componente 'Windows 11 SDK' "
            "desde el Visual Studio Installer.")
    endif()
    message(STATUS "Compilador HLSL: ${PYXIS_FXC_EXECUTABLE}")
endif()

function(pyxis_compile_shader OUT_VAR)
    cmake_parse_arguments(ARG "" "SOURCE;ENTRY;PROFILE;SYMBOL;OUTPUT" "" ${ARGN})

    foreach(_required SOURCE ENTRY PROFILE SYMBOL OUTPUT)
        if(NOT ARG_${_required})
            message(FATAL_ERROR "pyxis_compile_shader: falta el argumento ${_required}")
        endif()
    endforeach()

    get_filename_component(_out_dir "${ARG_OUTPUT}" DIRECTORY)
    get_filename_component(_name "${ARG_SOURCE}" NAME)
    get_filename_component(_src_dir "${ARG_SOURCE}" DIRECTORY)

    # fxc resuelve los #include respecto al directorio del .hlsl, pero CMake no
    # lo sabe: hay que declarar las cabeceras como dependencias a mano o un
    # cambio en color.hlsli no dispararia la recompilacion.
    file(GLOB _shader_headers "${_src_dir}/*.hlsli")

    # /O3 optimizacion maxima; /Qstrip_* recorta metadatos que no usamos.
    set(_flags /nologo /O3 /Qstrip_reflect /Qstrip_debug /Ges)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(_flags /nologo /Od /Zi /Ges)
    endif()

    add_custom_command(
        OUTPUT  "${ARG_OUTPUT}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_out_dir}"
        COMMAND "${PYXIS_FXC_EXECUTABLE}" ${_flags}
                /T ${ARG_PROFILE}
                /E ${ARG_ENTRY}
                /Vn ${ARG_SYMBOL}
                /Fh "${ARG_OUTPUT}"
                "${ARG_SOURCE}"
        MAIN_DEPENDENCY "${ARG_SOURCE}"
        DEPENDS "${ARG_SOURCE}" ${_shader_headers}
        COMMENT "HLSL ${_name} [${ARG_PROFILE}:${ARG_ENTRY}] -> ${ARG_SYMBOL}"
        VERBATIM)

    set(${OUT_VAR} ${${OUT_VAR}} "${ARG_OUTPUT}" PARENT_SCOPE)
endfunction()
