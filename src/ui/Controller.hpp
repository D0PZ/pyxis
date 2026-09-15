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

    // Emite un paso mientras la flecha siga pulsada. Lo llama el hilo de
    // presentacion, que ya despierta a cada refresco: montar un temporizador
    // aparte solo anadiria otro hilo para el mismo resultado.
    void UpdateFrameStepping();

    // ---- Encuadre ---------------------------------------------------------
    [[nodiscard]] ViewTransform CurrentView() const;
    [[nodiscard]] RECT          CurrentFitRect() const;

    void ZoomAt(int x, int y, float factor);
    void ApplyFitView();
    void ApplyOriginalSizeView();
    void ResetView();
    void BuildOverlayModel(OverlayModel& model);
    [[nodiscard]] std::wstring BuildStatsText() const;

    // Entrada
    void OnKeyDown(int virtualKey, bool shift, bool control, bool repeated);
    void OnKeyUp(int virtualKey);
    void OnFocusLost();
    void OnMouseMove(int x, int y);
    void OnLeftButtonDown(int x, int y);
    void OnLeftButtonUp(int x, int y);
    void OnWheel(int delta, int x, int y, bool control);
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

    // Avance fotograma a fotograma mientras se mantiene una flecha.
    std::atomic<int>    stepDirection_{0};   // -1, 0, +1
    std::atomic<Micros> nextStepAt_{0};

    // Encuadre. Lo escribe el hilo de interfaz y lo lee el de presentacion en
    // cada fotograma, de ahi el cerrojo. Es una estructura de doce bytes que se
    // toca unas pocas veces por segundo: el coste es irrelevante y el codigo
    // queda mas claro que con tres atomicos sueltos que podrian leerse
    // desparejados a mitad de un zoom.
    mutable std::mutex viewMutex_;
    ViewTransform      view_{};

    // Tamano del area de cliente, publicado por el hilo de interfaz.
    std::atomic<unsigned> clientWidth_{0};
    std::atomic<unsigned> clientHeight_{0};

    // Arrastre con el boton izquierdo. Solo lo toca el hilo de interfaz.
    bool          leftButtonDown_ = false;
    bool          dragMoved_      = false;
    POINT         dragOrigin_{};
    ViewTransform dragStartView_{};

    mutable std::mutex toastMutex_;
    std::wstring       toastText_;
    Micros             toastExpiry_ = 0;

    // Posicion a la que volver si hay que reconstruir el dispositivo grafico.
    std::wstring currentPath_;
};

}  // namespace pyxis
