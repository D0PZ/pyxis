// ============================================================================
//  SwapChain.hpp - Presentacion DXGI en modo flip
//
//  Tres decisiones, y por que:
//
//  1. MODELO FLIP (DXGI_SWAP_EFFECT_FLIP_DISCARD). El modelo bitblt heredado
//     copia el bufer trasero al frontal en cada presentacion. A 4K son 33 MB
//     copiados 60 veces por segundo que no hacen absolutamente nada. El modelo
//     flip intercambia punteros: coste cero, y ademas habilita el resto de esta
//     lista. No es opcional para un reproductor moderno.
//
//  2. OBJETO DE ESPERA (FRAME_LATENCY_WAITABLE_OBJECT) con latencia maxima 1.
//     Sin el, el hilo de presentacion corre libre hasta que DXGI lo bloquea
//     dentro de Present(), es decir, DESPUES de haber dibujado. Con el, se
//     espera ANTES de dibujar, se lee el reloj lo mas tarde posible y se elige
//     el fotograma correcto para el instante exacto en que se va a mostrar.
//     Esto es lo que quita entre uno y dos fotogramas de latencia.
//
//  3. DESGARRO PERMITIDO (ALLOW_TEARING). En pantallas de frecuencia variable
//     (G-Sync / FreeSync) es lo que deja que el panel siga al contenido en
//     lugar de al reves, y elimina el juddering del material a 23,976 fps en un
//     panel a 60 Hz. En pantallas fijas no se usa: alli el vsync es preferible.
//
//  HDR: la cadena se crea en R10G10B10A2 con espacio de color PQ BT.2020 solo
//  cuando el monitor lo admite Y el contenido lo necesita. Forzar HDR con
//  contenido SDR lo deja apagado y grisaceo, que es el error clasico.
// ============================================================================
#pragma once

#include "render/Device.hpp"

namespace pyxis {

// Como se presenta cada fotograma.
enum class PresentMode {
    Vsync,     // sincronizado con el refresco; lo correcto en pantallas fijas
    Tearing,   // sin sincronizar; para paneles de frecuencia variable
};

// Capacidades HDR del monitor que contiene la ventana.
struct DisplayCapabilities {
    bool  supportsHdr10      = false;
    float maxLuminanceNits   = 80.0f;    // brillo maximo declarado por el panel
    float minLuminanceNits   = 0.0f;
    float maxFullFrameNits   = 80.0f;    // brillo sostenido a pantalla completa
};

class SwapChain {
public:
    void Create(Device& device, HWND window);
    void Destroy() noexcept;

    // Reajusta los buferes al nuevo tamano del cliente. Es barato y hay que
    // llamarlo desde WM_SIZE; ignora los tamanos nulos (ventana minimizada).
    void Resize(unsigned width, unsigned height);

    // Reconfigura la cadena para contenido HDR o SDR. Solo recrea los buferes
    // si el formato cambia de verdad, asi que es seguro llamarlo por fotograma.
    void SetHdrOutput(bool enabled);

    // Bloquea hasta que DXGI acepte un fotograma nuevo. Debe llamarse ANTES de
    // dibujar; ahi esta todo el beneficio de latencia.
    void WaitForNextFrame() noexcept;

    void Present(PresentMode mode);

    [[nodiscard]] ID3D11RenderTargetView* BackBufferView() const noexcept {
        return renderTarget_.Get();
    }
    [[nodiscard]] IDXGISwapChain3* Handle() const noexcept { return swapChain_.Get(); }

    [[nodiscard]] unsigned Width() const noexcept { return width_; }
    [[nodiscard]] unsigned Height() const noexcept { return height_; }
    [[nodiscard]] bool     IsHdrOutput() const noexcept { return hdrOutput_; }

    // Consulta el monitor que contiene la ventana ahora mismo. Cambia al
    // arrastrar la ventana entre pantallas, asi que se relee en cada
    // reconfiguracion en lugar de cachearse.
    [[nodiscard]] DisplayCapabilities QueryDisplayCapabilities() const;

    // Frecuencia de refresco del monitor, en milihercios (60000 = 60 Hz).
    [[nodiscard]] unsigned RefreshRateMilliHz() const;

private:
    void CreateRenderTarget();
    void ReleaseRenderTarget() noexcept;
    void ApplyColorSpace();

    Device* device_ = nullptr;
    HWND    window_ = nullptr;

    ComPtr<IDXGISwapChain3>        swapChain_;
    ComPtr<ID3D11RenderTargetView> renderTarget_;
    HANDLE                         frameLatencyWaitable_ = nullptr;

    unsigned width_  = 0;
    unsigned height_ = 0;
    bool     hdrOutput_       = false;
    bool     tearingAllowed_  = false;
};

}  // namespace pyxis
