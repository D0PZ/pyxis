#include "core/Clock.hpp"

#include <windows.h>

namespace pyxis {
namespace {

// La frecuencia de QPC es fija desde el arranque del sistema, asi que se
// consulta una unica vez. Se guarda como reciproco escalado para convertir con
// una multiplicacion en lugar de una division en cada llamada.
struct QpcScale {
    std::int64_t frequency;

    QpcScale() noexcept {
        LARGE_INTEGER value{};
        ::QueryPerformanceFrequency(&value);   // nunca falla en Windows XP+
        frequency = value.QuadPart;
    }
};

const QpcScale g_qpc;

}  // namespace

Micros NowMicros() noexcept {
    LARGE_INTEGER counter{};
    ::QueryPerformanceCounter(&counter);

    // Se separa en segundos y resto antes de escalar. La forma ingenua
    // (counter * 1'000'000 / frequency) desborda un int64 a las ~2,5 horas de
    // actividad del sistema con una frecuencia QPC de 10 MHz.
    const std::int64_t seconds   = counter.QuadPart / g_qpc.frequency;
    const std::int64_t remainder = counter.QuadPart % g_qpc.frequency;
    return seconds * kMicrosPerSecond + (remainder * kMicrosPerSecond) / g_qpc.frequency;
}

}  // namespace pyxis
