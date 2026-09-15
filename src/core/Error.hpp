// ============================================================================
//  Error.hpp - Manejo de errores de Pyxis
//
//  Convencion del proyecto:
//
//    * Los fallos de INICIALIZACION (crear el dispositivo D3D11, abrir un
//      archivo, negociar el formato de audio) lanzan pyxis::Exception. Son
//      raros, fatales y se manejan en un unico punto: main() los captura y
//      los muestra al usuario. Propagarlos con codigos de retorno solo
//      ensuciaria cada capa con ramas que nunca se toman.
//
//    * Los fallos del BUCLE CALIENTE (un paquete corrupto, un fotograma que
//      llega tarde) devuelven enums o bool. Ocurren miles de veces por
//      minuto y deben ser baratos y locales.
//
//  Toda excepcion lleva el dominio del codigo de error (HRESULT, Win32,
//  FFmpeg) para poder traducirlo a un mensaje legible sin perder el valor
//  original, que es lo unico util al depurar.
// ============================================================================
#pragma once

#include <exception>
#include <cstdint>
#include <string>
#include <string_view>

namespace pyxis {

enum class ErrorDomain : std::uint8_t {
    Logic,    // invariante del programa violada
    Win32,    // GetLastError()
    HResult,  // HRESULT de COM/D3D
    FFmpeg,   // codigo negativo de libav*
};

// ---------------------------------------------------------------------------
//  Excepcion unica del proyecto. Guarda el codigo crudo y el contexto en el
//  que se produjo; el mensaje legible se construye al vuelo.
// ---------------------------------------------------------------------------
class Exception final : public std::exception {
public:
    Exception(ErrorDomain domain, std::int64_t code, std::string context);

    [[nodiscard]] const char*   what() const noexcept override { return message_.c_str(); }
    [[nodiscard]] ErrorDomain   domain() const noexcept { return domain_; }
    [[nodiscard]] std::int64_t  code() const noexcept { return code_; }
    [[nodiscard]] const std::string& context() const noexcept { return context_; }

private:
    ErrorDomain  domain_;
    std::int64_t code_;
    std::string  context_;
    std::string  message_;
};

// Traduce un codigo de error a texto legible segun su dominio.
[[nodiscard]] std::string DescribeError(ErrorDomain domain, std::int64_t code);

[[noreturn]] void ThrowHResult(long hr, std::string_view context);
[[noreturn]] void ThrowWin32(unsigned long err, std::string_view context);
[[noreturn]] void ThrowAv(int averr, std::string_view context);
[[noreturn]] void ThrowLogic(std::string_view context);

}  // namespace pyxis

// ---------------------------------------------------------------------------
//  Macros de comprobacion.
//
//  Evaluan la expresion UNA sola vez y conservan el valor original del codigo
//  de error, que es justo lo que se pierde con un `if (FAILED(x)) return;`.
// ---------------------------------------------------------------------------

// HRESULT: lanza si FAILED(hr).
#define PYXIS_CHECK_HR(expr, context)                                          \
    do {                                                                       \
        const long pyxis_hr_ = (expr);                                         \
        if (pyxis_hr_ < 0) ::pyxis::ThrowHResult(pyxis_hr_, (context));        \
    } while (0)

// Win32 BOOL: lanza con GetLastError() si la llamada devuelve falso.
#define PYXIS_CHECK_WIN32(expr, context)                                       \
    do {                                                                       \
        if (!(expr)) ::pyxis::ThrowWin32(::GetLastError(), (context));         \
    } while (0)

// FFmpeg: lanza si el codigo es negativo.
#define PYXIS_CHECK_AV(expr, context)                                          \
    do {                                                                       \
        const int pyxis_av_ = (expr);                                          \
        if (pyxis_av_ < 0) ::pyxis::ThrowAv(pyxis_av_, (context));             \
    } while (0)

// Invariante interna.
#define PYXIS_REQUIRE(cond, context)                                           \
    do {                                                                       \
        if (!(cond)) ::pyxis::ThrowLogic(context);                             \
    } while (0)
