// ============================================================================
//  Paths.hpp - Rutas de salida de Pyxis
//
//  Las capturas van a Imagenes\Pyxis y los recortes a Videos\Pyxis. Ambos
//  necesitan lo mismo: resolver una carpeta conocida del usuario, crear el
//  subdirectorio y limpiar el nombre del archivo. Tenerlo en un solo sitio
//  evita que las dos salidas acaben nombrando distinto o fallando distinto.
// ============================================================================
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace pyxis {

enum class UserFolder {
    Pictures,
    Videos,
};

// Devuelve <carpeta del usuario>\Pyxis, creandola si hace falta. Si no se puede
// (perfil movil sin permisos, por ejemplo), cae al directorio actual antes que
// renunciar a guardar.
[[nodiscard]] std::filesystem::path PyxisOutputFolder(UserFolder folder);

// Sustituye por guion bajo lo que Windows no admite en un nombre de archivo.
// El titulo suele venir de un nombre de archivo -y ya seria valido-, pero
// tambien puede venir de una URL.
[[nodiscard]] std::wstring SanitizeFileName(std::wstring_view name);

}  // namespace pyxis
