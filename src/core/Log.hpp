// ============================================================================
//  Log.hpp - Registro de diagnostico
//
//  Un reproductor pasa la mayor parte del tiempo en bucles que se ejecutan
//  decenas de veces por segundo, asi que registrar no puede costar nada
//  cuando el nivel esta desactivado. El filtro por nivel es una lectura
//  atomica relajada, y los argumentos solo se formatean si el mensaje va a
//  emitirse de verdad (de ahi que las macros envuelvan la llamada).
//
//  La salida va a OutputDebugStringW (visible en el depurador y en DebugView)
//  y, opcionalmente, a un archivo.
// ============================================================================
#pragma once

#include <atomic>
#include <format>
#include <string>
#include <string_view>

namespace pyxis::log {

enum class Level : int {
    Trace = 0,
    Debug = 1,
    Info  = 2,
    Warn  = 3,
    Error = 4,
    Off   = 5,
};

namespace detail {
inline std::atomic<int> g_level{static_cast<int>(Level::Info)};
void Emit(Level level, std::string_view message) noexcept;
}  // namespace detail

void SetLevel(Level level) noexcept;
[[nodiscard]] Level GetLevel() noexcept;

// Duplica la salida en un archivo. Si la ruta esta vacia, cierra el archivo.
void SetLogFile(const std::wstring& path) noexcept;
void Shutdown() noexcept;

[[nodiscard]] inline bool Enabled(Level level) noexcept {
    return static_cast<int>(level) >= detail::g_level.load(std::memory_order_relaxed);
}

// Formatea y emite. Nunca lanza: un fallo al registrar no debe tumbar la
// reproduccion, asi que los errores de formato se degradan a texto crudo.
template <typename... Args>
void Write(Level level, std::format_string<Args...> fmt, Args&&... args) noexcept {
    try {
        detail::Emit(level, std::format(fmt, std::forward<Args>(args)...));
    } catch (...) {
        detail::Emit(Level::Error, "fallo al formatear un mensaje de registro");
    }
}

}  // namespace pyxis::log

// La comprobacion de nivel va fuera de la llamada para que los argumentos ni
// siquiera se evaluen cuando el nivel esta desactivado.
#define PYXIS_LOG(level, ...)                                                  \
    do {                                                                       \
        if (::pyxis::log::Enabled(level)) {                                    \
            ::pyxis::log::Write(level, __VA_ARGS__);                           \
        }                                                                      \
    } while (0)

#define PYXIS_TRACE(...) PYXIS_LOG(::pyxis::log::Level::Trace, __VA_ARGS__)
#define PYXIS_DEBUG(...) PYXIS_LOG(::pyxis::log::Level::Debug, __VA_ARGS__)
#define PYXIS_INFO(...)  PYXIS_LOG(::pyxis::log::Level::Info,  __VA_ARGS__)
#define PYXIS_WARN(...)  PYXIS_LOG(::pyxis::log::Level::Warn,  __VA_ARGS__)
#define PYXIS_ERROR(...) PYXIS_LOG(::pyxis::log::Level::Error, __VA_ARGS__)
