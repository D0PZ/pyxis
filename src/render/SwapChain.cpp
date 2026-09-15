#include "render/SwapChain.hpp"

#include "core/Error.hpp"
#include "core/Log.hpp"

#include <algorithm>

namespace pyxis {
namespace {

// Tres buferes es el punto optimo del modelo flip: dos dejan al controlador sin
// margen cuando un fotograma se pasa de plazo, y cuatro solo anaden latencia.
constexpr UINT kBufferCount = 3;

constexpr DXGI_FORMAT kSdrFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
// R10G10B10A2 en lugar de FP16: la mitad de ancho de banda para el mismo
// resultado con HDR10, que solo necesita 10 bits por componente.
constexpr DXGI_FORMAT kHdrFormat = DXGI_FORMAT_R10G10B10A2_UNORM;

}  // namespace

void SwapChain::Create(Device& device, HWND window) {
    Destroy();
    device_ = &device;
    window_ = window;
    tearingAllowed_ = device.SupportsTearing();

    RECT client{};
    ::GetClientRect(window, &client);
    width_  = static_cast<unsigned>(std::max<LONG>(1, client.right - client.left));
    height_ = static_cast<unsigned>(std::max<LONG>(1, client.bottom - client.top));

    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width       = width_;
    description.Height      = height_;
    description.Format      = kSdrFormat;
    description.SampleDesc  = {1, 0};   // sin multimuestreo: el video ya viene resuelto
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = kBufferCount;
    description.Scaling     = DXGI_SCALING_NONE;   // sin reescalado de DWM
    description.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    description.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;
    description.Flags       = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    if (tearingAllowed_) {
        description.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }

    ComPtr<IDXGISwapChain1> created;
    PYXIS_CHECK_HR(device.Factory()->CreateSwapChainForHwnd(
                       device.Handle(), window, &description, nullptr, nullptr, &created),
                   "no se pudo crear la cadena de intercambio");

    PYXIS_CHECK_HR(created.As(&swapChain_),
                   "la cadena de intercambio no expone IDXGISwapChain3");

    // Se desactiva la captura de Alt+Intro de DXGI: la pantalla completa la
    // gestiona Pyxis con una ventana sin bordes, que en Windows 11 se lleva
    // mejor con el compositor que el modo exclusivo.
    PYXIS_CHECK_HR(device.Factory()->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER),
                   "no se pudo desactivar el Alt+Intro de DXGI");

    // Latencia maxima 1: DXGI deja preparar un unico fotograma por adelantado.
    // Es lo que permite leer el reloj justo antes de dibujar.
    PYXIS_CHECK_HR(swapChain_->SetMaximumFrameLatency(1),
                   "no se pudo fijar la latencia maxima de fotograma");

    frameLatencyWaitable_ = swapChain_->GetFrameLatencyWaitableObject();
    PYXIS_REQUIRE(frameLatencyWaitable_ != nullptr,
                  "la cadena de intercambio no entrego el objeto de espera");

    CreateRenderTarget();
    ApplyColorSpace();

    PYXIS_INFO("Cadena de intercambio {}x{} modo flip, {} buferes, desgarro {}",
               width_, height_, kBufferCount, tearingAllowed_ ? "permitido" : "no soportado");
}

void SwapChain::CreateRenderTarget() {
    ComPtr<ID3D11Texture2D> backBuffer;
    PYXIS_CHECK_HR(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)),
                   "no se pudo obtener el bufer trasero");

    PYXIS_CHECK_HR(device_->Handle()->CreateRenderTargetView(
                       backBuffer.Get(), nullptr, &renderTarget_),
                   "no se pudo crear la vista de destino de render");
}

void SwapChain::ReleaseRenderTarget() noexcept {
    renderTarget_.Reset();
    // ResizeBuffers falla si queda cualquier referencia al bufer trasero,
    // incluidas las que el contexto mantiene por estar enlazadas.
    if (device_ != nullptr && device_->Context() != nullptr) {
        ID3D11RenderTargetView* none[] = {nullptr};
        device_->Context()->OMSetRenderTargets(1, none, nullptr);
        device_->Context()->Flush();
    }
}

void SwapChain::Resize(unsigned width, unsigned height) {
    if (!swapChain_) return;
    // Al minimizar, Windows manda un WM_SIZE de 0x0. Redimensionar a cero falla,
    // y ademas no hay nada que mostrar, asi que se conserva el tamano anterior.
    if (width == 0 || height == 0) return;
    if (width == width_ && height == height_) return;

    ReleaseRenderTarget();

    UINT flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (tearingAllowed_) flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    const HRESULT hr = swapChain_->ResizeBuffers(
        kBufferCount, width, height, DXGI_FORMAT_UNKNOWN, flags);

    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        // La perdida de dispositivo se trata un nivel mas arriba; aqui solo se
        // registra para que el diagnostico no se pierda.
        PYXIS_ERROR("dispositivo perdido al redimensionar: {}",
                    DescribeError(ErrorDomain::HResult, hr));
        ThrowHResult(hr, "el dispositivo grafico se perdio al redimensionar");
    }
    PYXIS_CHECK_HR(hr, "no se pudieron redimensionar los buferes");

    width_  = width;
    height_ = height;
    CreateRenderTarget();
    ApplyColorSpace();
}

void SwapChain::SetHdrOutput(bool enabled) {
    if (!swapChain_) return;
    if (enabled == hdrOutput_) return;

    // Cambiar entre 8 y 10 bits por componente obliga a recrear los buferes.
    ReleaseRenderTarget();

    UINT flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (tearingAllowed_) flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    const DXGI_FORMAT format = enabled ? kHdrFormat : kSdrFormat;
    const HRESULT hr = swapChain_->ResizeBuffers(kBufferCount, width_, height_, format, flags);

    if (FAILED(hr)) {
        // Si el formato de 10 bits no se puede crear, se sigue en SDR. Perder
        // el HDR es aceptable; quedarse sin imagen no lo es.
        PYXIS_WARN("no se pudo cambiar el formato de salida a {}: {}",
                   enabled ? "HDR10" : "SDR", DescribeError(ErrorDomain::HResult, hr));
        PYXIS_CHECK_HR(swapChain_->ResizeBuffers(kBufferCount, width_, height_,
                                                 kSdrFormat, flags),
                       "no se pudo restaurar el formato SDR de la cadena");
        hdrOutput_ = false;
    } else {
        hdrOutput_ = enabled;
    }

    CreateRenderTarget();
    ApplyColorSpace();

    PYXIS_INFO("Salida configurada en {}", hdrOutput_ ? "HDR10 (PQ BT.2020)" : "SDR (sRGB)");
}

void SwapChain::ApplyColorSpace() {
    const DXGI_COLOR_SPACE_TYPE colorSpace =
        hdrOutput_ ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                   : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;

    UINT support = 0;
    if (FAILED(swapChain_->CheckColorSpaceSupport(colorSpace, &support)) ||
        (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) == 0) {
        PYXIS_WARN("el espacio de color solicitado no esta soportado; se deja el actual");
        return;
    }

    PYXIS_CHECK_HR(swapChain_->SetColorSpace1(colorSpace),
                   "no se pudo fijar el espacio de color de la cadena");
}

void SwapChain::WaitForNextFrame() noexcept {
    if (frameLatencyWaitable_ == nullptr) return;
    // 1000 ms de tope: si DXGI no responde (pantalla desconectada, controlador
    // reiniciandose) es preferible seguir y volver a intentarlo que colgar el
    // hilo de presentacion para siempre.
    ::WaitForSingleObjectEx(frameLatencyWaitable_, 1000, TRUE);
}

void SwapChain::Present(PresentMode mode) {
    if (!swapChain_) return;

    UINT syncInterval = 1;
    UINT flags        = 0;

    if (mode == PresentMode::Tearing && tearingAllowed_) {
        // ALLOW_TEARING exige syncInterval 0; DXGI rechaza cualquier otra
        // combinacion con un error dificil de diagnosticar.
        syncInterval = 0;
        flags        = DXGI_PRESENT_ALLOW_TEARING;
    }

    const HRESULT hr = swapChain_->Present(syncInterval, flags);

    if (hr == DXGI_STATUS_OCCLUDED) {
        // La ventana esta tapada o minimizada: DXGI deja de presentar. No es un
        // error, pero conviene no quemar GPU dibujando lo que nadie ve.
        return;
    }
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        ThrowHResult(hr, "el dispositivo grafico se perdio al presentar");
    }
    PYXIS_CHECK_HR(hr, "fallo la presentacion del fotograma");
}

DisplayCapabilities SwapChain::QueryDisplayCapabilities() const {
    DisplayCapabilities capabilities;
    if (!swapChain_) return capabilities;

    ComPtr<IDXGIOutput> output;
    if (FAILED(swapChain_->GetContainingOutput(&output))) return capabilities;

    ComPtr<IDXGIOutput6> output6;
    if (FAILED(output.As(&output6))) return capabilities;

    DXGI_OUTPUT_DESC1 description{};
    if (FAILED(output6->GetDesc1(&description))) return capabilities;

    capabilities.supportsHdr10 =
        description.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    capabilities.maxLuminanceNits = description.MaxLuminance;
    capabilities.minLuminanceNits = description.MinLuminance;
    capabilities.maxFullFrameNits = description.MaxFullFrameLuminance;

    return capabilities;
}

unsigned SwapChain::RefreshRateMilliHz() const {
    if (window_ == nullptr) return 60000;

    // EnumDisplaySettings da la frecuencia entera (60, 144), que redondea 59,94
    // a 60. Para pacing es suficiente; el reloj maestro es el audio, no esto.
    HMONITOR monitor = ::MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!::GetMonitorInfoW(monitor, &info)) return 60000;

    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!::EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode)) return 60000;
    if (mode.dmDisplayFrequency <= 1) return 60000;

    return static_cast<unsigned>(mode.dmDisplayFrequency) * 1000;
}

void SwapChain::Destroy() noexcept {
    ReleaseRenderTarget();
    // El objeto de espera lo cierra la propia cadena al destruirse; cerrarlo
    // aqui seria un doble cierre.
    frameLatencyWaitable_ = nullptr;
    swapChain_.Reset();
    device_    = nullptr;
    window_    = nullptr;
    width_     = 0;
    height_    = 0;
    hdrOutput_ = false;
}

}  // namespace pyxis
