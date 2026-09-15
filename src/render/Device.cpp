#include "render/Device.hpp"

#include "core/Error.hpp"
#include "core/Log.hpp"
#include "core/Text.hpp"

#include <array>

namespace pyxis {

void Device::Create(const Options& options) {
    CreateFactory(options);
    SelectAdapter(options);
    CreateDevice(options);
    EnableMultithreadProtection();
    DetectTearingSupport();

    PYXIS_INFO("GPU: {} ({} MiB dedicados, desgarro permitido: {})",
               ToUtf8(adapterName_), videoMemory_ / (1024 * 1024),
               tearingSupported_ ? "si" : "no");
}

void Device::CreateFactory(const Options& options) {
    UINT flags = 0;
    if (options.enableDebugLayer) flags |= DXGI_CREATE_FACTORY_DEBUG;

    PYXIS_CHECK_HR(::CreateDXGIFactory2(flags, IID_PPV_ARGS(&factory_)),
                   "no se pudo crear la fabrica DXGI");
}

void Device::SelectAdapter(const Options& options) {
    // IDXGIFactory6 permite pedir el adaptador por preferencia de rendimiento.
    // Es la unica forma fiable de no acabar en la grafica integrada de un
    // portatil hibrido: enumerar por indice devuelve la integrada primero.
    ComPtr<IDXGIFactory6> factory6;
    if (options.preferHighPerformanceAdapter &&
        SUCCEEDED(factory_.As(&factory6))) {
        const HRESULT hr = factory6->EnumAdapterByGpuPreference(
            0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter_));
        if (FAILED(hr)) {
            PYXIS_WARN("no se pudo seleccionar la GPU de alto rendimiento ({}); "
                       "se usara el adaptador por defecto",
                       DescribeError(ErrorDomain::HResult, hr));
            adapter_.Reset();
        }
    }

    if (!adapter_) {
        // Repliegue: el primer adaptador enumerado. Dejar adapter_ nulo y pasar
        // nullptr a D3D11CreateDevice tambien funcionaria, pero entonces no se
        // podrian leer el nombre ni la memoria para el diagnostico.
        PYXIS_CHECK_HR(factory_->EnumAdapters1(0, &adapter_),
                       "no se encontro ningun adaptador grafico");
    }

    DXGI_ADAPTER_DESC1 description{};
    if (SUCCEEDED(adapter_->GetDesc1(&description))) {
        adapterName_ = description.Description;
        videoMemory_ = description.DedicatedVideoMemory;
    }
}

void Device::CreateDevice(const Options& options) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT |   // requerido por Direct2D
                 D3D11_CREATE_DEVICE_VIDEO_SUPPORT;   // requerido por D3D11VA

    if (options.enableDebugLayer) flags |= D3D11_CREATE_DEVICE_DEBUG;

    // 11_1 aporta los recursos que usa el shader de video; 11_0 se acepta como
    // minimo para GPUs antiguas que aun cumplen los requisitos de Windows 11.
    static constexpr std::array<D3D_FEATURE_LEVEL, 2> kLevels = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    D3D_FEATURE_LEVEL achieved{};
    HRESULT hr = ::D3D11CreateDevice(
        adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
        kLevels.data(), static_cast<UINT>(kLevels.size()), D3D11_SDK_VERSION,
        &device_, &achieved, &context_);

    // La capa de depuracion solo existe si estan instaladas las "Graphics
    // Tools" de Windows. Si falta, se reintenta sin ella en lugar de abortar.
    if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG) != 0) {
        PYXIS_WARN("la capa de depuracion de D3D11 no esta disponible; se omite");
        flags &= ~static_cast<UINT>(D3D11_CREATE_DEVICE_DEBUG);
        hr = ::D3D11CreateDevice(
            adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
            kLevels.data(), static_cast<UINT>(kLevels.size()), D3D11_SDK_VERSION,
            &device_, &achieved, &context_);
    }

    PYXIS_CHECK_HR(hr, "no se pudo crear el dispositivo Direct3D 11");

    PYXIS_INFO("Direct3D 11 creado con nivel de caracteristicas {}.{}",
               (achieved >> 12) & 0xF, (achieved >> 8) & 0xF);
}

void Device::EnableMultithreadProtection() {
    // CRITICO. El contexto inmediato lo usan el hilo de decodificacion (via
    // FFmpeg), el hilo de presentacion y Direct2D. Sin esta proteccion, D3D11
    // no serializa los accesos y el resultado va desde fotogramas corruptos
    // hasta la perdida del dispositivo.
    //
    // El coste es un cerrojo por llamada al contexto, que es exactamente el
    // precio de compartir un dispositivo y la razon por la que se agrupa el
    // trabajo de GPU en lotes grandes.
    ComPtr<ID3D10Multithread> multithread;
    PYXIS_CHECK_HR(device_.As(&multithread),
                   "el dispositivo no expone ID3D10Multithread");

    multithread->SetMultithreadProtected(TRUE);
}

void Device::DetectTearingSupport() {
    ComPtr<IDXGIFactory5> factory5;
    if (FAILED(factory_.As(&factory5))) return;

    BOOL allowed = FALSE;
    if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                                &allowed, sizeof(allowed)))) {
        tearingSupported_ = allowed != FALSE;
    }
}

void Device::Destroy() noexcept {
    if (context_) {
        // Vaciar antes de soltar evita que el controlador arrastre trabajo
        // pendiente sobre recursos que estan a punto de desaparecer.
        context_->ClearState();
        context_->Flush();
    }
    context_.Reset();
    device_.Reset();
    adapter_.Reset();
    factory_.Reset();
    adapterName_.clear();
    videoMemory_      = 0;
    tearingSupported_ = false;
}

}  // namespace pyxis
