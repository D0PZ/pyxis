#pragma once
// ============================================================================
//  Fault - Inyeccion de fallos para las pruebas
//
//  Hay caminos del programa que solo se recorren cuando algo va mal: la perdida
//  del dispositivo grafico y el pool de texturas que no cabe en memoria. Son
//  precisamente los que mas facil es romper sin enterarse, porque nadie los
//  ejecuta en el uso normal.
//
//  Provocarlos de verdad exigiria actualizar el controlador grafico a mitad de
//  reproduccion o agotar la VRAM, cosa que ninguna prueba automatica puede
//  hacer de forma fiable. Esta valvula los dispara con un argumento de linea de
//  comandos (`--fault <nombre>`), recorriendo el MISMO codigo que en el caso
//  real: no simula la recuperacion, simula la averia.
//
//  Vive en un global a proposito. Es diagnostico puro, y pasarlo por las firmas
//  de Player, VideoDecoder, SwapChain y Controller ensuciaria cuatro interfaces
//  con un parametro que en produccion siempre vale None.
// ============================================================================

#include "core/Clock.hpp"

#include <atomic>
#include <string_view>

namespace pyxis {

enum class Fault {
    None,
    DeviceLoss,   // DXGI_ERROR_DEVICE_REMOVED al presentar, una sola vez
    PoolFull,     // da por fallido el primer pool D3D11VA, para forzar el reintento
};

namespace detail {

inline std::atomic<int>     g_faultKind{static_cast<int>(Fault::None)};
inline std::atomic<Micros>  g_faultArmedAt{0};
inline std::atomic<bool>    g_faultSpent{false};

// Margen antes de disparar. Un fallo en el primer fotograma llegaria antes de
// que haya algo que perder; con unos segundos de reproduccion real por delante,
// la recuperacion tiene que restaurar posicion, estado y decodificador, que es
// justo lo que interesa comprobar.
constexpr Micros kFaultDelay = 3 * kMicrosPerSecond;

}  // namespace detail

inline void SetFault(Fault kind) noexcept {
    detail::g_faultKind.store(static_cast<int>(kind), std::memory_order_release);
    detail::g_faultArmedAt.store(NowMicros(), std::memory_order_release);
    detail::g_faultSpent.store(false, std::memory_order_release);
}

// Para fallos que deben mantenerse mientras dure el proceso (la configuracion
// del pool se consulta cada vez que se abre un medio).
[[nodiscard]] inline bool FaultActive(Fault kind) noexcept {
    return detail::g_faultKind.load(std::memory_order_acquire) == static_cast<int>(kind);
}

// Para fallos de un solo uso. Devuelve true como mucho una vez, y solo despues
// del margen: sin desarmarlo, la perdida de dispositivo se repetiria en cada
// fotograma y el programa no saldria nunca del bucle de reconstruccion.
[[nodiscard]] inline bool FaultFires(Fault kind) noexcept {
    if (!FaultActive(kind)) return false;
    if (detail::g_faultSpent.load(std::memory_order_acquire)) return false;

    const Micros armedAt = detail::g_faultArmedAt.load(std::memory_order_acquire);
    if (NowMicros() - armedAt < detail::kFaultDelay) return false;

    return !detail::g_faultSpent.exchange(true, std::memory_order_acq_rel);
}

[[nodiscard]] inline Fault ParseFault(std::wstring_view name) noexcept {
    if (name == L"device-loss") return Fault::DeviceLoss;
    if (name == L"pool-full")   return Fault::PoolFull;
    return Fault::None;
}

}  // namespace pyxis
