#include "ui/Controller.hpp"

#include "core/Error.hpp"
#include "core/Log.hpp"
#include "core/Text.hpp"
#include "core/Thread.hpp"

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
constexpr Micros kSeekTiny   = 1 * kMicrosPerSecond;
constexpr Micros kSeekMedium = 10 * kMicrosPerSecond;
constexpr Micros kSeekLarge  = 60 * kMicrosPerSecond;

constexpr float kVolumeStep = 0.05f;

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
        },
        .onFilesDropped   = [this](const std::vector<std::wstring>& paths) {
            OnFilesDropped(paths);
        },
        .onKeyDown        = [this](int key, bool shift, bool control) {
            OnKeyDown(key, shift, control);
        },
        .onMouseMove      = [this](int x, int y) { OnMouseMove(x, y); },
        .onLeftButtonDown = [this](int x, int y) { OnLeftButtonDown(x, y); },
        .onLeftButtonUp   = [this](int x, int y) { OnLeftButtonUp(x, y); },
        .onDoubleClick    = [this] { window_.SetFullscreen(!window_.IsFullscreen()); },
        .onWheel          = [this](int delta) { OnWheel(delta); },
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
}

void Controller::PresentOneFrame() {
    // Esperar ANTES de dibujar es lo que da la latencia minima: al salir de
    // aqui queda justo un intervalo de refresco para producir el fotograma, y
    // el reloj se lee en el ultimo momento posible.
    swapChain_.WaitForNextFrame();

    ApplyPendingResize();

    VideoFrame nextFrame;
    if (player_.SelectFrame(nextFrame) == Player::FrameSelection::Updated) {
        currentFrame_ = std::move(nextFrame);
        UpdateHdrMode(currentFrame_);
    }

    if (currentFrame_.IsValid()) {
        videoRenderer_.Draw(swapChain_, currentFrame_, player_.SampleAspectRatio());
    } else {
        videoRenderer_.Clear(swapChain_);
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
}

void Controller::UpdateHdrMode(const VideoFrame& frame) {
    // El HDR se activa solo cuando AMBAS condiciones se cumplen. Forzarlo con
    // contenido SDR lo deja apagado y grisaceo; forzarlo en una pantalla sin
    // HDR lo deja directamente ilegible.
    const bool contentIsHdr = frame.color.IsHdr();
    const DisplayCapabilities display = swapChain_.QueryDisplayCapabilities();
    const bool wanted = contentIsHdr && display.supportsHdr10;

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

    model.title     = player_.Title();
    model.position  = player_.Position();
    model.duration  = player_.Duration();
    model.paused    = player_.State() != PlayerState::Playing;
    model.muted     = player_.Muted();
    model.volume    = player_.Volume();
    model.rateMilli = player_.RateMilli();
    model.showStats = showStats_.load(std::memory_order_relaxed);

    // Los controles permanecen visibles si esta en pausa: ocultar la barra en
    // pausa obliga a mover el raton para saber donde se quedo la reproduccion.
    model.showControls = model.paused || (now - lastActivity) < kControlsHideDelay;

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
    const DisplayCapabilities display = swapChain_.QueryDisplayCapabilities();

    std::array<wchar_t, 1024> buffer{};
    std::swprintf(
        buffer.data(), buffer.size(),
        L"Contenedor   %hs\n"
        L"Decodificador %hs (%s)\n"
        L"Resolucion   %d x %d\n"
        L"Duracion     %s\n"
        L"\n"
        L"Presentacion %.1f fps\n"
        L"Decodificados %llu\n"
        L"Descartados  %llu\n"
        L"Tardios      %llu\n"
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
void Controller::NoteUserActivity() {
    lastActivity_.store(NowMicros(), std::memory_order_relaxed);
}

void Controller::ShowToast(const std::wstring& text) {
    std::lock_guard<std::mutex> lock(toastMutex_);
    toastText_   = text;
    toastExpiry_ = NowMicros() + kToastDuration;
}

void Controller::OnKeyDown(int virtualKey, bool shift, bool control) {
    NoteUserActivity();

    switch (virtualKey) {
        case VK_SPACE:
        case 'K':
            player_.TogglePause();
            break;

        case VK_LEFT:
            player_.SeekRelative(-(control ? kSeekLarge : shift ? kSeekTiny : kSeekSmall));
            break;
        case VK_RIGHT:
            player_.SeekRelative(control ? kSeekLarge : shift ? kSeekTiny : kSeekSmall);
            break;
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
            if (window_.IsFullscreen()) window_.SetFullscreen(false);
            break;

        case 'I':
            showStats_.store(!showStats_.load(std::memory_order_relaxed),
                             std::memory_order_relaxed);
            break;

        case 'O':
            ShowOpenDialog();
            break;

        case VK_OEM_4: {   // [
            const int rate = std::max(250, player_.RateMilli() - 250);
            player_.SetRateMilli(rate);
            ShowToast(L"Velocidad x" + std::to_wstring(rate / 1000.0).substr(0, 4));
            break;
        }
        case VK_OEM_6: {   // ]
            const int rate = std::min(4000, player_.RateMilli() + 250);
            player_.SetRateMilli(rate);
            ShowToast(L"Velocidad x" + std::to_wstring(rate / 1000.0).substr(0, 4));
            break;
        }
        case VK_BACK:
            player_.SetRateMilli(1000);
            ShowToast(L"Velocidad normal");
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

void Controller::OnMouseMove(int x, int y) {
    NoteUserActivity();

    // Arrastre de la barra de progreso: se busca en vivo para que el usuario
    // vea a donde va, en lugar de saltar solo al soltar.
    if (seeking_.load(std::memory_order_relaxed)) {
        const Micros target = overlay_.HitTestSeekBar(x, y);
        if (target != kNoTimestamp) player_.Seek(target);
    }
}

void Controller::OnLeftButtonDown(int x, int y) {
    NoteUserActivity();

    const Micros target = overlay_.HitTestSeekBar(x, y);
    if (target != kNoTimestamp) {
        seeking_.store(true, std::memory_order_relaxed);
        player_.Seek(target);
        return;
    }

    // Un clic fuera de los controles alterna pausa, como en cualquier
    // reproductor de escritorio.
    if (overlay_.ControlsVisible() || player_.State() == PlayerState::Playing) {
        player_.TogglePause();
    }
}

void Controller::OnLeftButtonUp(int, int) {
    seeking_.store(false, std::memory_order_relaxed);
}

void Controller::OnWheel(int delta) {
    NoteUserActivity();

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
        player_.Close();
        currentFrame_ = VideoFrame{};
        videoRenderer_.InvalidateViewCache();

        player_.Open(path);
        currentPath_ = path;

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
                {L"Archivos de video",
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
