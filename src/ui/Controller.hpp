// ============================================================================
//  Controller.hpp - Union de ventana, graficos y reproductor
//
//  Reparto de hilos:
//
//    HILO PRINCIPAL      bucle de mensajes de Windows y entrada del usuario.
//                        Nunca dibuja, asi que un bucle modal (arrastrar la
//                        ventana, un menu abierto) no detiene el video.
//
//    HILO DE PRESENTACION  espera al objeto de latencia de DXGI, elige el
//                          fotograma que toca, dibuja y presenta. Es el unico
//                          que toca la cadena de intercambio.
//
//  La comunicacion entre ambos es minima y en un solo sentido: el hilo
//  principal publica intenciones en variables atomicas y el de presentacion
//  las consume. No hay callbacks cruzados, que es donde suelen aparecer los
//  interbloqueos en este tipo de programa.
// ============================================================================
#pragma once

#include "media/Player.hpp"
#include "render/Device.hpp"
#include "render/Overlay.hpp"
#include "render/SwapChain.hpp"
#include "render/VideoRenderer.hpp"
#include "ui/Window.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pyxis {

struct Options {
    std::wstring path;                // medio a abrir al arrancar
    bool         startFullscreen = false;
    bool         disableHardware = false;
    bool         verbose         = false;
    float        volume          = 1.0f;
};

class Controller {
public:
    Controller() = default;
    ~Controller();

    Controller(const Controller&)            = delete;
    Controller& operator=(const Controller&) = delete;

    // Ejecuta la aplicacion hasta que el usuario la cierra. Devuelve el codigo
    // de salida del proceso.
    [[nodiscard]] int Run(const Options& options, int commandShow);

private:
    void CreateGraphics();
    void DestroyGraphics() noexcept;
    void RecreateGraphicsAfterDeviceLoss();

    void StartPresentation();
    void StopPresentation() noexcept;
    void PresentationThread();

    // Un unico paso del bucle de presentacion. Separado para que la
    // recuperacion ante perdida de dispositivo sea un try/catch limpio.
    void PresentOneFrame();

    void ApplyPendingResize();
    void UpdateHdrMode(const VideoFrame& frame);
    void BuildOverlayModel(OverlayModel& model);
    [[nodiscard]] std::wstring BuildStatsText() const;

    // Entrada
    void OnKeyDown(int virtualKey, bool shift, bool control);
    void OnMouseMove(int x, int y);
    void OnLeftButtonDown(int x, int y);
    void OnLeftButtonUp(int x, int y);
    void OnWheel(int delta);
    void OnFilesDropped(const std::vector<std::wstring>& paths);

    void OpenMedia(const std::wstring& path);
    void ShowOpenDialog();
    void ShowToast(const std::wstring& text);
    void NoteUserActivity();

    Window        window_;
    Device        device_;
    SwapChain     swapChain_;
    VideoRenderer videoRenderer_;
    Overlay       overlay_;
    Player        player_;

    std::thread       presentationThread_;
    std::atomic<bool> presenting_{false};

    // Tamano solicitado por el hilo principal. Se empaqueta en un solo atomico
    // para que ancho y alto no puedan leerse desparejados.
    std::atomic<std::uint64_t> pendingSize_{0};

    // Fotograma en pantalla. Solo lo toca el hilo de presentacion.
    VideoFrame currentFrame_;

    Options options_{};

    // Estado de la interfaz, compartido entre ambos hilos.
    std::atomic<Micros> lastActivity_{0};
    std::atomic<bool>   showStats_{false};
    std::atomic<bool>   seeking_{false};

    mutable std::mutex toastMutex_;
    std::wstring       toastText_;
    Micros             toastExpiry_ = 0;

    // Posicion a la que volver si hay que reconstruir el dispositivo grafico.
    std::wstring currentPath_;
};

}  // namespace pyxis
