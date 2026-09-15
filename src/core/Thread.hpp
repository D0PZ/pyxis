// ============================================================================
//  Thread.hpp - Prioridad multimedia (MMCSS) y nombrado de hilos
//
//  Sin MMCSS, el planificador de Windows trata al hilo de presentacion igual
//  que a un compilador de fondo: basta con que el antivirus se despierte para
//  perder fotogramas. AvSetMmThreadCharacteristics inscribe el hilo en el
//  Multimedia Class Scheduler Service, que le garantiza una porcion de CPU
//  reservada y lo protege de la inanicion.
//
//  MmcssScope es RAII: el registro se deshace solo al salir del ambito, lo
//  que importa porque dejar un hilo inscrito tras su muerte degrada la
//  planificacion de todo el sistema.
// ============================================================================
#pragma once

#include <string>

namespace pyxis {

// Tareas MMCSS registradas por Windows en
// HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Multimedia\SystemProfile\Tasks
enum class MmcssTask {
    Playback,     // decodificacion y presentacion de video
    ProAudio,     // renderizado de audio de baja latencia
};

class MmcssScope {
public:
    explicit MmcssScope(MmcssTask task) noexcept;
    ~MmcssScope();

    MmcssScope(const MmcssScope&)            = delete;
    MmcssScope& operator=(const MmcssScope&) = delete;
    MmcssScope(MmcssScope&&)                 = delete;
    MmcssScope& operator=(MmcssScope&&)      = delete;

    // Falso si MMCSS rechazo el registro (por politica de grupo, por ejemplo).
    // No es fatal: la reproduccion sigue, solo con menos garantias.
    [[nodiscard]] bool active() const noexcept { return handle_ != nullptr; }

private:
    void* handle_ = nullptr;
};

// Asigna un nombre al hilo actual. Visible en el depurador, en el Explorador
// de procesos y en las trazas ETW; convierte un volcado ilegible en algo que
// se puede leer.
void SetCurrentThreadName(const wchar_t* name) noexcept;

}  // namespace pyxis
