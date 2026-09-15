// ============================================================================
//  Device.hpp - Dispositivo Direct3D 11 compartido
//
//  Hay UN solo ID3D11Device en todo el proceso y lo comparten el decodificador
//  (a traves de D3D11VA), el renderizador de video y la superposicion de
//  Direct2D. Esto no es una simplificacion: es el requisito que hace posible el
//  zero-copy. Una textura creada por un dispositivo no puede muestrearse desde
//  otro sin compartirla de forma explicita, y compartirla implica una copia por
//  fotograma.
//
//  La contrapartida es que el contexto inmediato lo tocan varios hilos, asi que
//  el dispositivo se marca como protegido para multihilo (ID3D10Multithread).
//  Sin eso, FFmpeg y el renderizador corromperian el estado del dispositivo.
// ============================================================================
#pragma once

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <string>

namespace pyxis {

template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

class Device {
public:
    struct Options {
        // Activa la capa de depuracion de D3D11. Es lentisima, pero convierte
        // un cuelgue silencioso del controlador en un mensaje concreto.
        bool enableDebugLayer = false;
        // Prefiere la GPU dedicada. En portatiles con grafica hibrida, dejarlo
        // en falso hace que la integrada gane y el rendimiento se desplome.
        bool preferHighPerformanceAdapter = true;
    };

    void Create(const Options& options);
    void Destroy() noexcept;

    [[nodiscard]] ID3D11Device*        Handle() const noexcept { return device_.Get(); }
    [[nodiscard]] ID3D11DeviceContext* Context() const noexcept { return context_.Get(); }
    [[nodiscard]] IDXGIFactory2*       Factory() const noexcept { return factory_.Get(); }
    [[nodiscard]] IDXGIAdapter1*       Adapter() const noexcept { return adapter_.Get(); }

    // Cierto si el adaptador y el sistema admiten presentacion con desgarro
    // permitido, que es lo que habilita las pantallas de frecuencia variable.
    [[nodiscard]] bool SupportsTearing() const noexcept { return tearingSupported_; }

    [[nodiscard]] const std::wstring& AdapterName() const noexcept { return adapterName_; }
    [[nodiscard]] std::size_t DedicatedVideoMemory() const noexcept { return videoMemory_; }

private:
    void CreateFactory(const Options& options);
    void SelectAdapter(const Options& options);
    void CreateDevice(const Options& options);
    void EnableMultithreadProtection();
    void DetectTearingSupport();

    ComPtr<IDXGIFactory2>       factory_;
    ComPtr<IDXGIAdapter1>       adapter_;
    ComPtr<ID3D11Device>        device_;
    ComPtr<ID3D11DeviceContext> context_;

    std::wstring adapterName_;
    std::size_t  videoMemory_      = 0;
    bool         tearingSupported_ = false;
};

}  // namespace pyxis
