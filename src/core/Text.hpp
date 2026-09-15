// ============================================================================
//  Text.hpp - Conversion entre UTF-8 y UTF-16
//
//  Pyxis tiene dos mundos de texto y una unica frontera entre ellos:
//
//    * Win32 y Direct2D hablan UTF-16 (wchar_t). Es lo que entra por la linea
//      de comandos, el dialogo de archivo y el arrastrar-y-soltar.
//    * FFmpeg y el registro interno hablan UTF-8.
//
//  Toda conversion pasa por aqui. Dispersar llamadas a MultiByteToWideChar por
//  el codigo es como se acaba con rutas rotas en cuanto aparece un acento o un
//  caracter CJK en el nombre del archivo.
// ============================================================================
#pragma once

#include <string>
#include <string_view>

namespace pyxis {

[[nodiscard]] std::string  ToUtf8(std::wstring_view wide);
[[nodiscard]] std::wstring ToUtf16(std::string_view utf8);

}  // namespace pyxis
