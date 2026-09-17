#include "ui/Controller.hpp"

#include "core/Error.hpp"
#include "core/Log.hpp"
#include "core/Text.hpp"
#include "core/Thread.hpp"
#include "render/Snapshot.hpp"

#include <shobjidl.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <iterator>

namespace pyxis {
namespace {

// Tiempo sin actividad del raton tras el cual la barra de control se oculta.
constexpr Micros kControlsHideDelay = 3 * kMicrosPerSecond;
constexpr Micros kToastDuration     = 2 * kMicrosPerSecond;

constexpr Micros kSeekSmall  = 5 * kMicrosPerSecond;
constexpr Micros kSeekMedium = 10 * kMicrosPerSecond;
constexpr Micros kSeekLarge  = 60 * kMicrosPerSecond;

constexpr float kVolumeStep = 0.05f;

// Avance por fotogramas. La primera pulsacion da UN paso; si la tecla sigue
// abajo pasado el retardo, se repite a ritmo fijo. Se ignora la
// autorrepeticion del teclado a proposito: su cadencia es un ajuste del
// sistema que varia de un equipo a otro, y aqui interesa que revisar un plano
// fotograma a fotograma se sienta igual en todas partes.
constexpr Micros kStepHoldDelay = 350000;                  // 350 ms
constexpr Micros kStepInterval  = kMicrosPerSecond / 20;   // 20 fotogramas/s

// Zoom. El paso es multiplicativo para que ampliar y reducir el mismo numero
// de muescas devuelva exactamente al punto de partida.
constexpr float kZoomStep = 1.25f;
constexpr float kZoomMin  = 0.05f;
constexpr float kZoomMax  = 32.0f;

// Distancia a partir de la cual un clic pasa a considerarse arrastre.
constexpr int kDragThreshold = 4;

// Intervalo minimo entre saltos al arrastrar la barra de progreso. El raton
// genera mas de cien mensajes por segundo y cada salto vacia las tres colas,
// reinicia el flujo de audio e invalida todo lo decodificado: emitirlos sin
// limite deja la imagen congelada mientras se arrastra.
constexpr Micros kScrubMinInterval = 80000;   // 80 ms

// Cada cuanto se refrescan las capacidades del monitor.
constexpr Micros kDisplayQueryInterval = 500000;   // 500 ms

std::uint64_t PackSize(unsigned width, unsigned height) noexcept {
    return (static_cast<std::uint64_t>(width) << 32) | height;
}

std::wstring FormatDuration(Micros micros) {
    if (micros == kNoTimestamp || micros < 0) return L"desconocida";

    const long long seconds = micros / kMicrosPerSecond;
    std::array<wchar_t, 32> buffer{};
    std::swprintf(buffer.data(), buffer.size(), L"%lld:%02lld:%02lld",
                  seconds / 3600, (seconds % 3600) / 60, seconds % 60);
    return buffer.data();
}

}  // namespace

Controller::~Controller() {
    StopPresentation();
    // El fotograma en pantalla referencia una textura del pool del
    // decodificador: hay que soltarlo antes de cerrar el reproductor.
    currentFrame_ = VideoFrame{};
    player_.Close();
    DestroyGraphics();
}

// ---------------------------------------------------------------------------
//  Arranque
// ---------------------------------------------------------------------------
int Controller::Run(const Options& options, int commandShow) {
    options_ = options;

    window_.Create(L"Pyxis", 1280, 720, Window::Callbacks{
        .onResize = [this](unsigned width, unsigned height) {
            // Solo se publica la intencion: redimensionar la cadena de
            // intercambio desde aqui chocaria con el Present en curso.
            pendingSize_.store(PackSize(width, height), std::memory_order_release);
            clientWidth_.store(width, std::memory_order_relaxed);
            clientHeight_.store(height, std::memory_order_relaxed);
        },
        .onFilesDropped   = [this](const std::vector<std::wstring>& paths) {
            OnFilesDropped(paths);
        },
        .onKeyDown        = [this](int key, bool shift, bool control, bool repeated) {
            OnKeyDown(key, shift, control, repeated);
        },
        .onKeyUp          = [this](int key) { OnKeyUp(key); },
        .onFocusLost      = [this] { OnFocusLost(); },
        .onMouseMove      = [this](int x, int y) { OnMouseMove(x, y); },
        .onLeftButtonDown = [this](int x, int y) { OnLeftButtonDown(x, y); },
        .onLeftButtonUp   = [this](int x, int y) { OnLeftButtonUp(x, y); },
        .onRightButtonDown = [this](int x, int y) { OnRightButtonDown(x, y); },
        .onDoubleClick    = [this] { window_.SetFullscreen(!window_.IsFullscreen()); },
        .onWheel          = [this](int delta, int x, int y, bool control) {
            OnWheel(delta, x, y, control);
        },
        .onClose          = [this] { StopPresentation(); },
    });

    CreateGraphics();

    player_.Initialize(options_.disableHardware ? nullptr : device_.Handle(),
                       device_.Context());
    player_.SetVolume(options_.volume);

    window_.Show(commandShow);
    if (options_.startFullscreen) window_.SetFullscreen(true);

    NoteUserActivity();
    StartPresentation();

    if (!options_.path.empty()) OpenMedia(options_.path);

    // Bucle de mensajes. PeekMessage devuelve de inmediato si no hay nada, asi
    // que se cede el resto del cuanto con MsgWaitForMultipleObjects en lugar de
    // girar en vacio consumiendo un nucleo entero.
    while (window_.PumpMessages()) {
        ::MsgWaitForMultipleObjectsEx(0, nullptr, 16, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }

    StopPresentation();
    player_.Close();
    DestroyGraphics();

    return 0;
}

// ---------------------------------------------------------------------------
//  Graficos
// ---------------------------------------------------------------------------
void Controller::CreateGraphics() {
    Device::Options deviceOptions;
#ifndef NDEBUG
    deviceOptions.enableDebugLayer = true;
#endif
    deviceOptions.preferHighPerformanceAdapter = true;

    device_.Create(deviceOptions);
    swapChain_.Create(device_, window_.Handle());
    clientWidth_.store(swapChain_.Width(), std::memory_order_relaxed);
    clientHeight_.store(swapChain_.Height(), std::memory_order_relaxed);
    videoRenderer_.Create(device_);
    overlay_.Create(device_);
    overlay_.Resize(swapChain_.Width(), swapChain_.Height());
}

void Controller::DestroyGraphics() noexcept {
    overlay_.Destroy();
    videoRenderer_.Destroy();
    swapChain_.Destroy();
    device_.Destroy();
}

void Controller::RecreateGraphicsAfterDeviceLoss() {
    PYXIS_WARN("se perdio el dispositivo grafico; reconstruyendo");

    // Se guarda donde estaba la reproduccion para retomarla en el mismo punto.
    const std::wstring path = currentPath_;
    const Micros position = player_.Position();
    const bool wasPlaying = player_.State() == PlayerState::Playing;

    // El reproductor se cierra primero: sus fotogramas referencian texturas del
    // dispositivo que esta a punto de desaparecer.
    player_.Close();
    currentFrame_ = VideoFrame{};

    DestroyGraphics();
    CreateGraphics();

    player_.Initialize(options_.disableHardware ? nullptr : device_.Handle(),
                       device_.Context());
    player_.SetVolume(options_.volume);

    if (!path.empty()) {
        try {
            player_.Open(path);
            player_.Seek(position);
            if (wasPlaying) player_.Play();
            currentPath_ = path;
        } catch (const Exception& error) {
            PYXIS_ERROR("no se pudo retomar la reproduccion: {}", error.what());
        }
    }
}

// ---------------------------------------------------------------------------
//  Hilo de presentacion
// ---------------------------------------------------------------------------
void Controller::StartPresentation() {
    if (presenting_.exchange(true, std::memory_order_acq_rel)) return;
    presentationThread_ = std::thread([this] { PresentationThread(); });
}

void Controller::StopPresentation() noexcept {
    if (!presenting_.exchange(false, std::memory_order_acq_rel)) return;
    if (presentationThread_.joinable()) presentationThread_.join();
}

void Controller::PresentationThread() {
    SetCurrentThreadName(L"pyxis-present");
    const MmcssScope mmcss(MmcssTask::Playback);

    // WIC se instancia por CoCreateInstance al guardar una captura, y eso exige
    // que el hilo tenga apartamento. MULTITHREADED para encajar con el resto
    // del proceso.
    const bool comInitialized = SUCCEEDED(::CoInitializeEx(nullptr, COINIT_MULTITHREADED));

    while (presenting_.load(std::memory_order_acquire)) {
        try {
            PresentOneFrame();
        } catch (const Exception& error) {
            // Solo la perdida de dispositivo justifica reconstruirlo todo; el
            // resto de fallos se registran y se reintenta el fotograma
            // siguiente, porque abortar dejaria la ventana congelada.
            if (error.domain() == ErrorDomain::HResult &&
                (error.code() == static_cast<std::int64_t>(DXGI_ERROR_DEVICE_REMOVED) ||
                 error.code() == static_cast<std::int64_t>(DXGI_ERROR_DEVICE_RESET))) {
                try {
                    RecreateGraphicsAfterDeviceLoss();
                } catch (const Exception& fatal) {
                    PYXIS_ERROR("la reconstruccion del dispositivo fallo: {}", fatal.what());
                    presenting_.store(false, std::memory_order_release);
                }
            } else {
                PYXIS_ERROR("error al presentar: {}", error.what());
                ::Sleep(16);
            }
        }
    }

    if (comInitialized) ::CoUninitialize();
}

void Controller::PresentOneFrame() {
    // Esperar ANTES de dibujar es lo que da la latencia minima: al salir de
    // aqui queda justo un intervalo de refresco para producir el fotograma, y
    // el reloj se lee en el ultimo momento posible.
    swapChain_.WaitForNextFrame();

    // Cambio de medio. Se atiende aqui, en el hilo dueno de estos recursos: las
    // texturas del pool anterior desaparecen y las vistas cacheadas quedarian
    // apuntando a memoria que el controlador grafico puede reutilizar para el
    // pool nuevo.
    const std::uint32_t epoch = mediaEpoch_.load(std::memory_order_acquire);
    if (epoch != presentedEpoch_) {
        presentedEpoch_ = epoch;
        currentFrame_   = VideoFrame{};
        videoRenderer_.InvalidateViewCache();
    }

    ApplyPendingResize();
    UpdateFrameStepping();

    VideoFrame nextFrame;
    if (player_.SelectFrame(nextFrame) == Player::FrameSelection::Updated) {
        currentFrame_ = std::move(nextFrame);
        UpdateHdrMode(currentFrame_);
    }

    if (currentFrame_.IsValid()) {
        videoRenderer_.SetViewTransform(CurrentView());
        videoRenderer_.Draw(swapChain_, currentFrame_, player_.SampleAspectRatio());
    } else {
        videoRenderer_.Clear(swapChain_);
    }

    // La captura va DESPUES del dibujado y antes de presentar: asi el
    // fotograma ya esta enlazado y las vistas en cache, y el usuario recibe
    // exactamente lo que esta viendo.
    if (snapshotRequested_.exchange(false, std::memory_order_acq_rel)) {
        TakeSnapshot();
    }

    OverlayModel model;
    BuildOverlayModel(model);
    overlay_.Update(model);
    overlay_.Render(swapChain_);

    // Vsync siempre. Con modelo flip y el objeto de latencia, el vsync no anade
    // retardo apreciable y evita el desgarro, que en video es mucho mas visible
    // que en un juego porque la camara se mueve de forma continua.
    swapChain_.Present(PresentMode::Vsync);
}

void Controller::ApplyPendingResize() {
    const std::uint64_t packed = pendingSize_.exchange(0, std::memory_order_acq_rel);
    if (packed == 0) return;

    const auto width  = static_cast<unsigned>(packed >> 32);
    const auto height = static_cast<unsigned>(packed & 0xFFFFFFFFull);

    swapChain_.Resize(width, height);
    overlay_.Resize(swapChain_.Width(), swapChain_.Height());

    // Al cambiar el tamano de la ventana, el desplazamiento puede haber quedado
    // fuera de rango y dejar franjas negras en un video ampliado.
    if (player_.HasVideo()) {
        const RECT fit = CurrentFitRect();
        std::lock_guard<std::mutex> lock(viewMutex_);
        VideoRenderer::ClampPan(fit, swapChain_.Width(), swapChain_.Height(), view_);
    }
}

// ---------------------------------------------------------------------------
//  Avance fotograma a fotograma
// ---------------------------------------------------------------------------
void Controller::UpdateFrameStepping() {
    const int direction = stepDirection_.load(std::memory_order_relaxed);
    if (direction == 0) return;

    const Micros now = NowMicros();
    if (now < nextStepAt_.load(std::memory_order_relaxed)) return;

    // Si el paso anterior aun no ha llegado a pantalla, no se encola otro.
    // Retroceder obliga a rebobinar hasta el fotograma clave y redecodificar, y
    // en material con GOP largo eso tarda mas que el intervalo de repeticion.
    // Esperar hace que el avance se autolimite a lo que la maquina sostiene, en
    // lugar de acumular peticiones que nunca se atienden.
    if (player_.StepPending()) return;

    player_.StepFrame(direction);
    nextStepAt_.store(now + kStepInterval, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
//  Encuadre
// ---------------------------------------------------------------------------
ViewTransform Controller::CurrentView() const {
    std::lock_guard<std::mutex> lock(viewMutex_);
    return view_;
}

RECT Controller::CurrentFitRect() const {
    return VideoRenderer::ComputeFitRect(clientWidth_.load(std::memory_order_relaxed),
                                         clientHeight_.load(std::memory_order_relaxed),
                                         player_.VideoWidth(), player_.VideoHeight(),
                                         player_.SampleAspectRatio());
}

void Controller::ZoomAt(int x, int y, float factor) {
    if (!player_.HasVideo()) return;

    const unsigned width  = clientWidth_.load(std::memory_order_relaxed);
    const unsigned height = clientHeight_.load(std::memory_order_relaxed);
    if (width == 0 || height == 0) return;

    const RECT fit = CurrentFitRect();
    const float fitWidth  = static_cast<float>(fit.right - fit.left);
    const float fitHeight = static_cast<float>(fit.bottom - fit.top);
    if (fitWidth <= 0.0f || fitHeight <= 0.0f) return;

    float zoomAfter = 0.0f;
    {
        std::lock_guard<std::mutex> lock(viewMutex_);

        const ViewTransform previous = view_;
        ViewTransform next = previous;
        next.zoom = std::clamp(previous.zoom * factor, kZoomMin, kZoomMax);
        if (next.zoom == previous.zoom) return;   // ya estaba en el tope

        // Anclaje al cursor: se localiza que punto de la IMAGEN hay bajo el
        // raton (en coordenadas normalizadas) y se recoloca el rectangulo para
        // que ese mismo punto siga ahi despues de ampliar. Sin esto, el zoom
        // tira siempre hacia el centro y revisar una esquina es imposible.
        const RECT before = VideoRenderer::ApplyView(fit, width, height, previous);
        const float beforeWidth  = static_cast<float>(before.right - before.left);
        const float beforeHeight = static_cast<float>(before.bottom - before.top);
        if (beforeWidth <= 0.0f || beforeHeight <= 0.0f) return;

        const float anchorU = (static_cast<float>(x) - before.left) / beforeWidth;
        const float anchorV = (static_cast<float>(y) - before.top) / beforeHeight;

        const float afterWidth  = fitWidth * next.zoom;
        const float afterHeight = fitHeight * next.zoom;

        next.panX = (static_cast<float>(x) - anchorU * afterWidth + afterWidth * 0.5f) -
                    static_cast<float>(width) * 0.5f;
        next.panY = (static_cast<float>(y) - anchorV * afterHeight + afterHeight * 0.5f) -
                    static_cast<float>(height) * 0.5f;

        VideoRenderer::ClampPan(fit, width, height, next);
        view_ = next;
        zoomAfter = next.zoom;
    }

    // El porcentaje se muestra respecto al tamano original, que es lo que el
    // usuario entiende por "zoom", y no respecto al ajuste a la ventana.
    const float originalZoom = VideoRenderer::ZoomForOriginalSize(
        fit, player_.VideoWidth(), player_.SampleAspectRatio());
    const int percent = originalZoom > 0.0f
                            ? static_cast<int>(zoomAfter / originalZoom * 100.0f + 0.5f)
                            : 100;
    ShowToast(L"Zoom " + std::to_wstring(percent) + L"%");
}

void Controller::ApplyFitView() {
    {
        std::lock_guard<std::mutex> lock(viewMutex_);
        view_ = ViewTransform{};
    }
    ShowToast(L"Ajustado a la ventana");
}

void Controller::ApplyOriginalSizeView() {
    if (!player_.HasVideo()) return;

    const RECT fit = CurrentFitRect();
    const float zoom = VideoRenderer::ZoomForOriginalSize(
        fit, player_.VideoWidth(), player_.SampleAspectRatio());

    {
        std::lock_guard<std::mutex> lock(viewMutex_);
        view_ = ViewTransform{std::clamp(zoom, kZoomMin, kZoomMax), 0.0f, 0.0f};
        VideoRenderer::ClampPan(fit, clientWidth_.load(std::memory_order_relaxed),
                                clientHeight_.load(std::memory_order_relaxed), view_);
    }

    ShowToast(L"Tamaño original  " + std::to_wstring(player_.VideoWidth()) + L" x " +
              std::to_wstring(player_.VideoHeight()));
}

void Controller::ResetView() {
    std::lock_guard<std::mutex> lock(viewMutex_);
    view_ = ViewTransform{};
}

void Controller::UpdateHdrMode(const VideoFrame& frame) {
    const bool contentIsHdr = frame.color.IsHdr();
    const Micros now = NowMicros();

    // La consulta al monitor se refresca solo cuando cambia la naturaleza del
    // contenido o cada medio segundo, por si la ventana se arrastro a otra
    // pantalla. Hacerla por fotograma cuesta un recorrido de las salidas de
    // DXGI sesenta veces por segundo.
    if (contentIsHdr != lastContentHdr_ ||
        now - displayQueriedAt_ >= kDisplayQueryInterval) {
        lastContentHdr_   = contentIsHdr;
        displayQueriedAt_ = now;
        cachedDisplay_    = swapChain_.QueryDisplayCapabilities();
    }

    // El HDR se activa solo cuando AMBAS condiciones se cumplen. Forzarlo con
    // contenido SDR lo deja apagado y grisaceo; forzarlo en una pantalla sin
    // HDR lo deja directamente ilegible.
    const bool wanted = contentIsHdr && cachedDisplay_.supportsHdr10;

    if (wanted != swapChain_.IsHdrOutput()) {
        swapChain_.SetHdrOutput(wanted);
        overlay_.Resize(swapChain_.Width(), swapChain_.Height());
    }
}

// ---------------------------------------------------------------------------
//  Modelo de la interfaz
// ---------------------------------------------------------------------------
void Controller::BuildOverlayModel(OverlayModel& model) {
    const Micros now = NowMicros();
    const Micros lastActivity = lastActivity_.load(std::memory_order_relaxed);

    model.title    = player_.Title();
    model.duration = player_.Duration();

    // Mientras se arrastra la barra manda el raton, no el reloj: asi el cabezal
    // no da tirones entre un salto y el siguiente.
    const Micros scrub = scrubPosition_.load(std::memory_order_relaxed);
    const Micros position = scrub != kNoTimestamp ? scrub : player_.Position();

    // La posicion se REDONDEA a decimas de segundo, y no por pereza: el modelo
    // se compara entero para decidir si hay que repintar la superposicion, y en
    // microsegundos cambia siempre, con lo que Direct2D rasterizaria los glifos
    // sesenta veces por segundo sobre una textura del tamano de la ventana. A
    // 4K eso es trabajo suficiente para provocar microcortes. Con decimas, el
    // reloj sigue exacto al segundo y el cabezal se mueve 0,03 px por paso en
    // una pelicula de dos horas.
    model.position = (position / 100000) * 100000;
    model.paused    = player_.State() != PlayerState::Playing;
    model.muted     = player_.Muted();
    model.volume    = player_.Volume();
    model.rateMilli = player_.RateMilli();
    // Sin medio abierto manda la bienvenida: la barra de control no tiene nada
    // que controlar y una ventana negra no explica nada.
    model.showWelcome = player_.State() == PlayerState::Idle;
    model.showStats   = showStats_.load(std::memory_order_relaxed) && !model.showWelcome;
    model.speedMenuOpen      = speedMenuOpen_.load(std::memory_order_relaxed);
    model.speedMenuHighlight = speedMenuHighlight_.load(std::memory_order_relaxed);

    // Los controles permanecen visibles si esta en pausa: ocultar la barra en
    // pausa obliga a mover el raton para saber donde se quedo la reproduccion.
    model.showControls = !model.showWelcome &&
                         (model.paused || model.speedMenuOpen ||
                          (now - lastActivity) < kControlsHideDelay);

    {
        std::lock_guard<std::mutex> lock(toastMutex_);
        if (now < toastExpiry_) {
            model.toast = toastText_;
        } else if (!toastText_.empty()) {
            toastText_.clear();
        }
    }

    if (model.showStats) model.stats = BuildStatsText();

    // El cursor acompana a la barra de control: si la interfaz esta oculta, el
    // puntero sobre el video tambien estorba.
    if (window_.IsFullscreen()) {
        window_.SetCursorVisible(model.showControls);
    }
}

std::wstring Controller::BuildStatsText() const {
    const PlayerStats stats = player_.Stats();
    const DisplayCapabilities& display = cachedDisplay_;

    std::array<wchar_t, 1024> buffer{};
    std::swprintf(
        buffer.data(), buffer.size(),
        L"Contenedor   %hs\n"
        L"Decodificador %hs (%s)\n"
        L"Resolución   %d x %d\n"
        L"Duración     %s\n"
        L"\n"
        L"Presentación %.1f fps\n"
        L"Decodificados %llu\n"
        L"Descartados  %llu\n"
        L"Tardíos      %llu\n"
        L"Cortes audio %llu\n"
        L"\n"
        L"Colas        v-pkt %zu  a-pkt %zu  v-frm %zu\n"
        L"Salida       %s\n"
        L"Pantalla     %s (%.0f nits)\n"
        L"GPU          %s",
        stats.containerName.c_str(),
        stats.decoderName.c_str(),
        stats.hardwareDecoding ? L"D3D11VA" : L"software",
        stats.width, stats.height,
        FormatDuration(player_.Duration()).c_str(),
        stats.measuredFps,
        static_cast<unsigned long long>(stats.framesDecoded),
        static_cast<unsigned long long>(stats.framesDropped),
        static_cast<unsigned long long>(stats.framesLate),
        static_cast<unsigned long long>(stats.audioUnderruns),
        stats.videoPacketQueue, stats.audioPacketQueue, stats.videoFrameQueue,
        swapChain_.IsHdrOutput() ? L"HDR10 PQ BT.2020" : L"SDR sRGB",
        display.supportsHdr10 ? L"HDR10" : L"SDR",
        display.maxLuminanceNits,
        device_.AdapterName().c_str());

    return buffer.data();
}

// ---------------------------------------------------------------------------
//  Entrada
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
//  Velocidad de reproduccion
// ---------------------------------------------------------------------------
void Controller::CycleSpeed(int delta) {
    const int current = player_.RateMilli();

    int index = 3;   // 1x si el valor actual no esta en la lista
    for (std::size_t i = 0; i < kPlaybackRates.size(); ++i) {
        if (kPlaybackRates[i] == current) {
            index = static_cast<int>(i);
            break;
        }
    }

    // Aritmetica modular para que el ciclo de la vuelta en ambos sentidos.
    const auto count = static_cast<int>(kPlaybackRates.size());
    ApplySpeedIndex(((index + delta) % count + count) % count);
}

void Controller::ApplySpeedIndex(int index) {
    if (index < 0 || index >= static_cast<int>(kPlaybackRates.size())) return;

    player_.SetRateMilli(kPlaybackRates[static_cast<std::size_t>(index)]);
    NoteUserActivity();
}

// ---------------------------------------------------------------------------
//  Captura de fotograma
// ---------------------------------------------------------------------------
void Controller::RequestSnapshot() {
    if (!player_.HasVideo()) return;
    snapshotRequested_.store(true, std::memory_order_release);
    NoteUserActivity();
}

void Controller::TakeSnapshot() {
    if (!currentFrame_.IsValid()) {
        ShowToast(L"No hay ningún fotograma que capturar");
        return;
    }

    try {
        const ComPtr<ID3D11Texture2D> texture =
            videoRenderer_.RenderToTexture(currentFrame_, player_.SampleAspectRatio());
        if (!texture) {
            ShowToast(L"No se pudo preparar la captura");
            return;
        }

        const std::wstring path =
            BuildSnapshotPath(player_.Title(), currentFrame_.pts);
        SaveTextureAsPng(device_, texture.Get(), path);

        ShowToast(L"Captura guardada  " +
                  std::filesystem::path(path).filename().wstring());

    } catch (const Exception& error) {
        PYXIS_ERROR("la captura fallo: {}", error.what());
        ShowToast(L"No se pudo guardar la captura");
    }
}

void Controller::NoteUserActivity() {
    lastActivity_.store(NowMicros(), std::memory_order_relaxed);
}

void Controller::ShowToast(const std::wstring& text) {
    std::lock_guard<std::mutex> lock(toastMutex_);
    toastText_   = text;
    toastExpiry_ = NowMicros() + kToastDuration;
}

void Controller::OnKeyDown(int virtualKey, bool shift, bool control, bool repeated) {
    NoteUserActivity();

    // La autorrepeticion del sistema se descarta para todo: el avance por
    // fotogramas lleva su propio ritmo y el resto de acciones no deben
    // dispararse en rafaga por dejar una tecla apoyada.
    if (repeated) return;

    switch (virtualKey) {
        case VK_SPACE:
        case 'K':
            player_.TogglePause();
            break;

        // Las flechas avanzan de UN fotograma. Mantenerlas pulsadas repite el
        // paso a ritmo fijo (ver UpdateFrameStepping). Los saltos por tiempo,
        // que antes vivian aqui, se conservan con modificador.
        case VK_LEFT:
        case VK_RIGHT: {
            const int sign = (virtualKey == VK_RIGHT) ? 1 : -1;

            if (control) {
                player_.SeekRelative(sign * kSeekLarge);
                break;
            }
            if (shift) {
                player_.SeekRelative(sign * kSeekSmall);
                break;
            }

            player_.StepFrame(sign);
            stepDirection_.store(sign, std::memory_order_relaxed);
            nextStepAt_.store(NowMicros() + kStepHoldDelay, std::memory_order_relaxed);
            break;
        }
        case 'J':
            player_.SeekRelative(-kSeekMedium);
            break;
        case 'L':
            player_.SeekRelative(kSeekMedium);
            break;

        case VK_UP: {
            const float volume = std::min(1.0f, player_.Volume() + kVolumeStep);
            player_.SetVolume(volume);
            options_.volume = volume;
            ShowToast(L"Volumen " + std::to_wstring(static_cast<int>(volume * 100 + 0.5f)) + L"%");
            break;
        }
        case VK_DOWN: {
            const float volume = std::max(0.0f, player_.Volume() - kVolumeStep);
            player_.SetVolume(volume);
            options_.volume = volume;
            ShowToast(L"Volumen " + std::to_wstring(static_cast<int>(volume * 100 + 0.5f)) + L"%");
            break;
        }

        case 'M':
            player_.SetMuted(!player_.Muted());
            ShowToast(player_.Muted() ? L"Silencio" : L"Sonido activado");
            break;

        case 'F':
        case VK_F11:
            window_.SetFullscreen(!window_.IsFullscreen());
            break;

        case VK_ESCAPE:
            if (speedMenuOpen_.load(std::memory_order_relaxed)) {
                speedMenuOpen_.store(false, std::memory_order_relaxed);
                break;
            }
            if (window_.IsFullscreen()) window_.SetFullscreen(false);
            break;

        case 'S':
            RequestSnapshot();
            break;

        case 'I':
            showStats_.store(!showStats_.load(std::memory_order_relaxed),
                             std::memory_order_relaxed);
            break;

        case 'Z':
            ApplyFitView();
            break;

        case 'X':
            ApplyOriginalSizeView();
            break;

        case 'O':
            ShowOpenDialog();
            break;

        // Recorren la MISMA lista que el indicador de la barra, para que
        // teclado y raton no ofrezcan juegos de velocidades distintos.
        case VK_OEM_4:   // [
            CycleSpeed(-1);
            break;
        case VK_OEM_6:   // ]
            CycleSpeed(+1);
            break;
        case VK_BACK:
            player_.SetRateMilli(1000);
            break;

        case 'Q':
            ::PostMessageW(window_.Handle(), WM_CLOSE, 0, 0);
            break;

        default:
            // Teclas 0-9: salto porcentual, como en cualquier reproductor.
            if (virtualKey >= '0' && virtualKey <= '9') {
                const Micros duration = player_.Duration();
                if (duration != kNoTimestamp && duration > 0) {
                    player_.Seek(duration * (virtualKey - '0') / 10);
                }
            }
            break;
    }
}

void Controller::OnKeyUp(int virtualKey) {
    if (virtualKey == VK_LEFT || virtualKey == VK_RIGHT) {
        stepDirection_.store(0, std::memory_order_relaxed);
    }
}

void Controller::OnFocusLost() {
    // Sin WM_KEYUP que lo detenga, el avance seguiria corriendo con la ventana
    // en segundo plano.
    stepDirection_.store(0, std::memory_order_relaxed);
    leftButtonDown_ = false;
    seeking_.store(false, std::memory_order_relaxed);
}

void Controller::OnMouseMove(int x, int y) {
    NoteUserActivity();

    if (speedMenuOpen_.load(std::memory_order_relaxed)) {
        speedMenuHighlight_.store(overlay_.HitTestSpeedMenu(x, y),
                                  std::memory_order_relaxed);
        return;
    }

    // Arrastre de la barra de progreso: se busca en vivo para que el usuario
    // vea a donde va, en lugar de saltar solo al soltar.
    if (seeking_.load(std::memory_order_relaxed)) {
        const Micros target = overlay_.HitTestSeekBar(x, y);
        if (target != kNoTimestamp) RequestScrubSeek(target, false);
        return;
    }

    if (!leftButtonDown_) return;

    const int deltaX = x - dragOrigin_.x;
    const int deltaY = y - dragOrigin_.y;

    if (!dragMoved_ &&
        (std::abs(deltaX) > kDragThreshold || std::abs(deltaY) > kDragThreshold)) {
        dragMoved_ = true;
    }
    if (!dragMoved_) return;

    // Desplazar la imagen ampliada. El desplazamiento se calcula desde el punto
    // donde empezo el arrastre, no de forma incremental: asi no acumula error de
    // redondeo si el raton se mueve mas rapido de lo que llegan los mensajes.
    const RECT fit = CurrentFitRect();
    std::lock_guard<std::mutex> lock(viewMutex_);

    view_.panX = dragStartView_.panX + static_cast<float>(deltaX);
    view_.panY = dragStartView_.panY + static_cast<float>(deltaY);
    VideoRenderer::ClampPan(fit, clientWidth_.load(std::memory_order_relaxed),
                            clientHeight_.load(std::memory_order_relaxed), view_);
}

void Controller::OnLeftButtonDown(int x, int y) {
    NoteUserActivity();

    // Sin medio abierto, cualquier punto de la ventana abre el dialogo: obligar
    // a acertar en un boton pequeno seria justo el problema que la pantalla de
    // bienvenida viene a resolver.
    if (overlay_.WelcomeVisible()) {
        ShowOpenDialog();
        return;
    }

    // El menu de velocidades captura el clic antes que nada mas.
    if (speedMenuOpen_.load(std::memory_order_relaxed)) {
        const int index = overlay_.HitTestSpeedMenu(x, y);
        speedMenuOpen_.store(false, std::memory_order_relaxed);
        speedMenuHighlight_.store(-1, std::memory_order_relaxed);
        if (index >= 0) ApplySpeedIndex(index);
        return;   // un clic fuera del menu solo lo cierra
    }

    if (overlay_.HitTestPlayPause(x, y)) {
        player_.TogglePause();
        return;
    }
    if (overlay_.HitTestStepBack(x, y)) {
        player_.StepFrame(-1);
        return;
    }
    if (overlay_.HitTestStepForward(x, y)) {
        player_.StepFrame(+1);
        return;
    }

    if (overlay_.HitTestSpeed(x, y)) {
        CycleSpeed(+1);
        return;
    }
    if (overlay_.HitTestSnapshot(x, y)) {
        RequestSnapshot();
        return;
    }

    const Micros target = overlay_.HitTestSeekBar(x, y);
    if (target != kNoTimestamp) {
        seeking_.store(true, std::memory_order_relaxed);
        RequestScrubSeek(target, false);
        return;
    }

    // Fuera de la barra, el boton izquierdo sirve para dos cosas segun lo que
    // haga despues: si el raton se mueve, desplaza la imagen; si no, es un clic
    // y alterna la pausa. La decision se toma al soltar.
    leftButtonDown_ = true;
    dragMoved_      = false;
    dragOrigin_     = POINT{x, y};
    dragStartView_  = CurrentView();
}

void Controller::OnLeftButtonUp(int, int) {
    if (seeking_.exchange(false, std::memory_order_relaxed)) {
        // Al soltar SIEMPRE se emite el salto definitivo, aunque el limitador
        // de cadencia acabase de descartar uno: si no, la reproduccion se
        // quedaria donde cayo el ultimo salto permitido y no donde el usuario
        // dejo el cabezal.
        const Micros target = scrubPosition_.load(std::memory_order_relaxed);
        if (target != kNoTimestamp) RequestScrubSeek(target, true);
        scrubPosition_.store(kNoTimestamp, std::memory_order_relaxed);
        return;
    }

    const bool wasClick = leftButtonDown_ && !dragMoved_;
    leftButtonDown_ = false;
    dragMoved_      = false;

    if (wasClick) player_.TogglePause();
}

void Controller::RequestScrubSeek(Micros target, bool final) {
    // La posicion del cabezal se publica siempre, para que la barra siga al
    // raton con fluidez independientemente de cuando se emita el salto.
    scrubPosition_.store(final ? kNoTimestamp : target, std::memory_order_relaxed);

    const Micros now = NowMicros();

    // Dos condiciones para emitir: que el salto anterior ya lo haya recogido el
    // demultiplexor y que haya pasado el intervalo minimo. La primera adapta la
    // cadencia a la maquina y al archivo; la segunda evita castigar al pipeline
    // en ficheros locales donde los saltos se atienden en un instante.
    if (!final) {
        if (player_.SeekPending()) return;
        if (now - lastScrubSeekAt_ < kScrubMinInterval) return;
    }

    lastScrubSeekAt_ = now;
    player_.Seek(target);
}

void Controller::OnRightButtonDown(int x, int y) {
    NoteUserActivity();

    // El clic derecho sobre el indicador despliega la lista completa, que es lo
    // que permite saltar de 0.25x a 1.5x sin recorrer el ciclo entero.
    if (overlay_.HitTestSpeed(x, y)) {
        const bool open = !speedMenuOpen_.load(std::memory_order_relaxed);
        speedMenuOpen_.store(open, std::memory_order_relaxed);
        speedMenuHighlight_.store(-1, std::memory_order_relaxed);
        return;
    }

    speedMenuOpen_.store(false, std::memory_order_relaxed);
    speedMenuHighlight_.store(-1, std::memory_order_relaxed);
}

void Controller::OnWheel(int delta, int x, int y, bool control) {
    NoteUserActivity();

    if (control) {
        ZoomAt(x, y, delta > 0 ? kZoomStep : 1.0f / kZoomStep);
        return;
    }

    const float step = (delta > 0 ? 1.0f : -1.0f) * kVolumeStep;
    const float volume = std::clamp(player_.Volume() + step, 0.0f, 1.0f);
    player_.SetVolume(volume);
    options_.volume = volume;
    ShowToast(L"Volumen " + std::to_wstring(static_cast<int>(volume * 100 + 0.5f)) + L"%");
}

void Controller::OnFilesDropped(const std::vector<std::wstring>& paths) {
    if (paths.empty()) return;
    OpenMedia(paths.front());
}

// ---------------------------------------------------------------------------
//  Apertura de medios
// ---------------------------------------------------------------------------
void Controller::OpenMedia(const std::wstring& path) {
    try {
        // El fotograma en pantalla y la cache de vistas pertenecen al hilo de
        // presentacion. Se le avisa con el numero de medio y el los suelta en
        // su siguiente pasada; tocarlos desde aqui seria una carrera con el
        // dibujado en curso.
        mediaEpoch_.fetch_add(1, std::memory_order_acq_rel);
        scrubPosition_.store(kNoTimestamp, std::memory_order_relaxed);
        stepDirection_.store(0, std::memory_order_relaxed);
        speedMenuOpen_.store(false, std::memory_order_relaxed);

        player_.Close();
        player_.Open(path);
        currentPath_ = path;
        ResetView();   // el encuadre del archivo anterior no aplica a este

        window_.SetTitle(std::filesystem::path(path).filename().wstring() + L"  -  Pyxis");
        Window::KeepDisplayAwake(true);

        player_.Play();
        NoteUserActivity();

    } catch (const Exception& error) {
        Window::KeepDisplayAwake(false);
        const std::wstring message =
            L"No se pudo abrir el archivo:\n\n" + ToUtf16(error.what());
        ::MessageBoxW(window_.Handle(), message.c_str(), L"Pyxis", MB_ICONWARNING | MB_OK);
    }
}

void Controller::ShowOpenDialog() {
    // IFileOpenDialog exige un apartamento monohilo, pero el hilo principal de
    // Pyxis es MTA (lo necesita WASAPI para compartir sus objetos COM con el
    // hilo de audio). La solucion estandar es abrir el dialogo en un hilo STA
    // efimero, que ademas evita que el bucle modal del dialogo bloquee la
    // entrada del reproductor.
    std::thread([this] {
        const HRESULT comInit = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(comInit)) return;

        ComPtr<IFileOpenDialog> dialog;
        if (SUCCEEDED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                         IID_PPV_ARGS(&dialog)))) {
            static const COMDLG_FILTERSPEC filters[] = {
                {L"Archivos de vídeo",
                 L"*.mp4;*.mkv;*.mov;*.avi;*.webm;*.m2ts;*.ts;*.mpg;*.mpeg;*.wmv;*.flv;*.m4v"},
                {L"Archivos de audio", L"*.mp3;*.flac;*.aac;*.m4a;*.opus;*.ogg;*.wav;*.wma"},
                {L"Todos los archivos", L"*.*"},
            };
            dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
            dialog->SetTitle(L"Abrir medio");

            if (SUCCEEDED(dialog->Show(window_.Handle()))) {
                ComPtr<IShellItem> item;
                PWSTR rawPath = nullptr;
                if (SUCCEEDED(dialog->GetResult(&item)) &&
                    SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath))) {
                    const std::wstring path = rawPath;
                    ::CoTaskMemFree(rawPath);

                    // La apertura vuelve al hilo principal: el reproductor no
                    // esta pensado para que lo manipulen dos hilos a la vez.
                    auto* heapPath = new std::wstring(path);
                    if (!::PostMessageW(window_.Handle(), Window::kOpenRequestMessage, 0,
                                        reinterpret_cast<LPARAM>(heapPath))) {
                        delete heapPath;   // la ventana ya no existe
                    }
                }
            }
        }
        ::CoUninitialize();
    }).detach();
}

}  // namespace pyxis
