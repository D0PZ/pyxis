// ============================================================================
//  Overlay.hpp - Interfaz en pantalla (barra de progreso, reloj, avisos)
//
//  Direct2D dibuja sobre una textura BGRA8 propia y un shader la compone
//  encima del video. Dos razones:
//
//    1. Direct2D no admite R10G10B10A2, que es el formato del bufer trasero en
//       modo HDR10. Sin la textura intermedia no habria interfaz en HDR.
//
//    2. La interfaz cambia como mucho una vez por segundo (el reloj), mientras
//       que el video va a 60 fps. Con la textura intermedia, repintar texto
//       -que es la parte cara, por el rasterizado de glifos- ocurre solo
//       cuando el contenido cambia de verdad. Componer la textura ya dibujada
//       cuesta un triangulo.
//
//  De ahi el campo `dirty_`: es lo que convierte la interfaz en gratis.
// ============================================================================
#pragma once

#include "core/Clock.hpp"
#include "render/Device.hpp"
#include "render/SwapChain.hpp"

#include <d2d1_1.h>
#include <dwrite_1.h>

#include <string>

namespace pyxis {

// Estado que la interfaz muestra. Se compara completo para decidir si hay que
// repintar, de ahi el operador de igualdad.
struct OverlayModel {
    std::wstring title;
    Micros       position = 0;
    Micros       duration = kNoTimestamp;

    bool  paused    = false;
    bool  muted     = false;
    float volume    = 1.0f;
    int   rateMilli = 1000;

    // Aviso transitorio ("Volumen 70%", "Silencio"). Se desvanece solo.
    std::wstring toast;

    // Panel de estadisticas (tecla I).
    bool         showStats = false;
    std::wstring stats;

    // Barra de control visible. Se oculta sola tras unos segundos sin actividad.
    bool showControls = true;

    [[nodiscard]] bool operator==(const OverlayModel&) const = default;
};

class Overlay {
public:
    void Create(Device& device);
    void Destroy() noexcept;

    void Resize(unsigned width, unsigned height);

    // Actualiza el contenido. Solo marca para repintar si algo cambio.
    void Update(const OverlayModel& model);

    // Compone la interfaz sobre el bufer trasero ya dibujado.
    void Render(SwapChain& swapChain);

    // Traduce una posicion X de la barra de progreso a tiempo del medio.
    // Devuelve kNoTimestamp si el punto queda fuera de la barra.
    [[nodiscard]] Micros HitTestSeekBar(int x, int y) const noexcept;

    [[nodiscard]] bool ControlsVisible() const noexcept { return model_.showControls; }

private:
    void CreateDeviceResources();
    void CreateTextFormats();
    void EnsureSurface(unsigned width, unsigned height);
    void Repaint();

    void DrawControlBar(unsigned width, unsigned height);
    void DrawStats(unsigned width);
    void DrawToast(unsigned width, unsigned height);

    Device* device_ = nullptr;

    // Direct2D
    ComPtr<ID2D1Factory1>       d2dFactory_;
    ComPtr<ID2D1Device>         d2dDevice_;
    ComPtr<ID2D1DeviceContext>  d2dContext_;
    ComPtr<ID2D1Bitmap1>        d2dTarget_;
    ComPtr<ID2D1SolidColorBrush> brush_;

    // DirectWrite
    ComPtr<IDWriteFactory1>  dwriteFactory_;
    ComPtr<IDWriteTextFormat> timeFormat_;
    ComPtr<IDWriteTextFormat> titleFormat_;
    ComPtr<IDWriteTextFormat> statsFormat_;

    // Textura intermedia y su composicion
    ComPtr<ID3D11Texture2D>          surface_;
    ComPtr<ID3D11ShaderResourceView> surfaceView_;
    ComPtr<ID3D11PixelShader>        compositeShader_;
    ComPtr<ID3D11VertexShader>       fullscreenShader_;
    ComPtr<ID3D11SamplerState>       pointSampler_;
    ComPtr<ID3D11BlendState>         alphaBlend_;
    ComPtr<ID3D11Buffer>             constantBuffer_;
    ComPtr<ID3D11RasterizerState>    rasterizer_;

    struct alignas(16) Constants {
        std::uint32_t outputHdr;
        float         uiWhiteNits;
        float         opacity;
        float         padding;
    };
    static_assert(sizeof(Constants) == 16, "el constant buffer debe casar con overlay.hlsl");

    OverlayModel model_{};
    unsigned     width_  = 0;
    unsigned     height_ = 0;
    bool         dirty_  = true;

    // Geometria de la barra de progreso del ultimo repintado, para el
    // posicionamiento con el raton.
    D2D1_RECT_F seekBarRect_{};
};

}  // namespace pyxis
