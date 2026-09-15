// ============================================================================
//  Clock.hpp - Tiempo monotono y reloj maestro de reproduccion
//
//  Todo el proyecto mide el tiempo en microsegundos enteros (Micros). Los
//  flotantes se descartaron a proposito: a 8K y varias horas de metraje, un
//  double acumula error de redondeo suficiente para desincronizar el audio,
//  y los enteros de 64 bits cubren ~292.000 anos sin perder precision.
//
//  MediaClock es el reloj MAESTRO. El audio es quien lo gobierna, porque el
//  oido detecta un salto de audio de 10 ms mientras que el ojo tolera que un
//  fotograma se repita. El renderizador de video lo consulta para decidir
//  que fotograma mostrar; nunca al reves.
// ============================================================================
#pragma once

#include <atomic>
#include <cstdint>

namespace pyxis {

using Micros = std::int64_t;

inline constexpr Micros kMicrosPerSecond = 1'000'000;
inline constexpr Micros kNoTimestamp     = INT64_MIN;

// Reloj monotono de alta resolucion (QueryPerformanceCounter). Inmune a los
// cambios de hora del sistema y al ajuste NTP, a diferencia del reloj de pared.
[[nodiscard]] Micros NowMicros() noexcept;

// ---------------------------------------------------------------------------
//  MediaClock
//
//  Traduce "tiempo real transcurrido" a "posicion en el medio", teniendo en
//  cuenta las pausas y la velocidad de reproduccion.
//
//  Seguro para multiples hilos: un solo escritor (el renderizador de audio, o
//  el hilo de reproduccion cuando no hay pista de audio) y varios lectores.
//  El estado cabe en dos enteros de 64 bits que se publican con release y se
//  leen con acquire, asi que no hace falta cerrojo alguno.
// ---------------------------------------------------------------------------
class MediaClock {
public:
    // Ancla el reloj: en el instante real `atRealTime` el medio iba por
    // `mediaPosition`. Llamado por el renderizador de audio en cada bloque.
    void Anchor(Micros mediaPosition, Micros atRealTime) noexcept {
        // El orden importa: publicamos primero la posicion y despues el
        // instante, y en la lectura invertimos el orden. Asi un lector nunca
        // combina una posicion nueva con un instante viejo.
        anchorMedia_.store(mediaPosition, std::memory_order_relaxed);
        anchorReal_.store(atRealTime, std::memory_order_release);
    }

    // Posicion estimada del medio en este instante.
    [[nodiscard]] Micros Position() const noexcept {
        const Micros real  = anchorReal_.load(std::memory_order_acquire);
        const Micros media = anchorMedia_.load(std::memory_order_relaxed);
        if (real == kNoTimestamp) return media;
        if (paused_.load(std::memory_order_relaxed)) return media;

        const Micros elapsed = NowMicros() - real;
        return media + ScaleByRate(elapsed);
    }

    void SetPaused(bool paused) noexcept {
        if (paused == paused_.load(std::memory_order_relaxed)) return;
        // Al pausar congelamos la posicion actual; al reanudar re-anclamos en
        // el ahora para no "recuperar" el tiempo que estuvo en pausa.
        const Micros position = Position();
        paused_.store(paused, std::memory_order_relaxed);
        Anchor(position, NowMicros());
    }

    [[nodiscard]] bool IsPaused() const noexcept {
        return paused_.load(std::memory_order_relaxed);
    }

    // Velocidad de reproduccion en milesimas (1000 = x1.0). Entero para que el
    // avance del reloj sea exactamente reproducible.
    void SetRateMilli(int rateMilli) noexcept {
        const Micros position = Position();
        rateMilli_.store(rateMilli, std::memory_order_relaxed);
        Anchor(position, NowMicros());
    }

    [[nodiscard]] int RateMilli() const noexcept {
        return rateMilli_.load(std::memory_order_relaxed);
    }

    // Reinicia el reloj tras un salto de posicion.
    void Reset(Micros mediaPosition) noexcept {
        Anchor(mediaPosition, NowMicros());
    }

private:
    [[nodiscard]] Micros ScaleByRate(Micros realElapsed) const noexcept {
        const std::int64_t rate = rateMilli_.load(std::memory_order_relaxed);
        return (realElapsed * rate) / 1000;
    }

    std::atomic<Micros> anchorMedia_{0};
    std::atomic<Micros> anchorReal_{kNoTimestamp};
    std::atomic<int>    rateMilli_{1000};
    std::atomic<bool>   paused_{true};
};

}  // namespace pyxis
