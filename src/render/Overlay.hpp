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
#include "render/VideoRenderer.hpp"

#include <d2d1_1.h>
#include <dwrite_1.h>

#include <array>
#include <string>

namespace pyxis {

// Velocidades de reproduccion disponibles, en milesimas. El orden es el del
// ciclo al pulsar el indicador, y tambien el del menu contextual.
inline constexpr std::array<int, 6> kPlaybackRates = {250, 500, 750, 1000, 1500, 2000};

// Rectangulo en pixeles de cliente. No se usa RECT de Win32 porque el modelo de
// la superposicion se compara entero con `operator==` para decidir si repintar,
// y RECT no es comparable.
struct ScreenRect {
    float left   = 0.0f;
    float top    = 0.0f;
    float right  = 0.0f;
    float bottom = 0.0f;

    [[nodiscard]] float Width() const noexcept { return right - left; }
    [[nodiscard]] float Height() const noexcept { return bottom - top; }
    [[nodiscard]] bool  Valid() const noexcept { return right > left && bottom > top; }

    [[nodiscard]] bool operator==(const ScreenRect&) const = default;
};

// Que parte del rectangulo de encuadre esta bajo el puntero.
enum class CropHandle {
    None,
    Move,
    TopLeft, Top, TopRight,
    Left, Right,
    BottomLeft, Bottom, BottomRight,
};

// Controles del panel de ajustes.
enum class FilterControl {
    None,
    Exposure, Brightness, Contrast, Saturation, Gamma,
    Shadows, Midtones, Highlights,
    Reset,
};

struct FilterRange {
    float minimum;
    float maximum;
};

// Rango de cada control. Vive aqui, y no repartido entre el dibujo y la
// entrada, porque si los dos no coinciden el tirador se queda donde no debe.
[[nodiscard]] constexpr FilterRange FilterRangeFor(FilterControl control) noexcept {
    switch (control) {
        case FilterControl::Exposure:   return {-3.0f, 3.0f};    // pasos de diafragma
        case FilterControl::Brightness: return {-0.5f, 0.5f};
        case FilterControl::Contrast:   return {0.0f, 2.0f};
        case FilterControl::Saturation: return {0.0f, 2.0f};
        case FilterControl::Gamma:      return {0.4f, 2.5f};
        case FilterControl::Shadows:
        case FilterControl::Midtones:
        case FilterControl::Highlights: return {0.0f, 2.0f};
        default:                        return {0.0f, 1.0f};
    }
}

[[nodiscard]] constexpr float FilterValueOf(const ImageAdjustments& adjustments,
                                            FilterControl control) noexcept {
    switch (control) {
        case FilterControl::Exposure:   return adjustments.exposure;
        case FilterControl::Brightness: return adjustments.brightness;
        case FilterControl::Contrast:   return adjustments.contrast;
        case FilterControl::Saturation: return adjustments.saturation;
        case FilterControl::Gamma:      return adjustments.gamma;
        case FilterControl::Shadows:    return adjustments.shadows;
        case FilterControl::Midtones:   return adjustments.midtones;
        case FilterControl::Highlights: return adjustments.highlights;
        default:                        return 0.0f;
    }
}

constexpr void SetFilterValue(ImageAdjustments& adjustments, FilterControl control,
                              float value) noexcept {
    switch (control) {
        case FilterControl::Exposure:   adjustments.exposure   = value; break;
        case FilterControl::Brightness: adjustments.brightness = value; break;
        case FilterControl::Contrast:   adjustments.contrast   = value; break;
        case FilterControl::Saturation: adjustments.saturation = value; break;
        case FilterControl::Gamma:      adjustments.gamma      = value; break;
        case FilterControl::Shadows:    adjustments.shadows    = value; break;
        case FilterControl::Midtones:   adjustments.midtones   = value; break;
        case FilterControl::Highlights: adjustments.highlights = value; break;
        default: break;
    }
}

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

    // Pantalla de bienvenida: sin medio abierto, una ventana negra no dice como
    // cargar nada. Sustituye a la barra de control mientras no haya video.
    bool showWelcome = false;

    // Recorte: puntos A y B sobre la barra. kNoTimestamp = sin marcar.
    Micros trimStart = kNoTimestamp;
    Micros trimEnd   = kNoTimestamp;
    bool   trimBusy     = false;   // exportacion en curso
    int    trimProgress = -1;      // 0 a 100, o -1 si no aplica

    // Encuadre. Mientras `cropEditing` esta activo el video se muestra COMPLETO
    // y el rectangulo se dibuja encima con el exterior atenuado: recortar en
    // vivo impediria ver lo que se esta dejando fuera, que es justo lo que hay
    // que juzgar al encuadrar.
    bool       cropEditing = false;
    CropRect   crop{};
    ScreenRect videoRect{};   // donde se esta dibujando el fotograma

    // Panel de ajustes de imagen.
    bool             filtersOpen = false;
    ImageAdjustments adjustments{};

    // Menu de velocidades desplegado (clic derecho sobre el indicador).
    bool speedMenuOpen = false;

    // Indice de kPlaybackRates bajo el puntero dentro del menu, o -1.
    int speedMenuHighlight = -1;

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

    // Botones de transporte, a la izquierda de la barra.
    [[nodiscard]] bool HitTestPlayPause(int x, int y) const noexcept;
    [[nodiscard]] bool HitTestStepBack(int x, int y) const noexcept;
    [[nodiscard]] bool HitTestStepForward(int x, int y) const noexcept;

    // Controles a la derecha de la barra.
    [[nodiscard]] bool HitTestSpeed(int x, int y) const noexcept;
    [[nodiscard]] bool HitTestSnapshot(int x, int y) const noexcept;
    [[nodiscard]] bool HitTestTrim(int x, int y) const noexcept;
    [[nodiscard]] bool HitTestCropButton(int x, int y) const noexcept;
    [[nodiscard]] bool HitTestFiltersButton(int x, int y) const noexcept;

    // Encuadre: que tirador hay bajo el punto.
    [[nodiscard]] CropHandle HitTestCrop(int x, int y) const noexcept;

    // Panel de ajustes: que control y con que valor normalizado (0..1).
    [[nodiscard]] FilterControl HitTestFilters(int x, int y) const noexcept;
    [[nodiscard]] float FilterValueAt(FilterControl control, int x, int y) const noexcept;

    // Rectangulo del recorte en pixeles de cliente, para que la interfaz sepa
    // donde esta sin repetir la aritmetica.
    [[nodiscard]] ScreenRect CropScreenRect() const noexcept;

    // Tiradores de los puntos A y B sobre la barra de progreso.
    [[nodiscard]] bool HitTestTrimStart(int x, int y) const noexcept;
    [[nodiscard]] bool HitTestTrimEnd(int x, int y) const noexcept;

    // La bienvenida ocupa toda la ventana: cualquier clic abre el dialogo.
    [[nodiscard]] bool WelcomeVisible() const noexcept { return model_.showWelcome; }

    // Indice dentro de kPlaybackRates del elemento del menu bajo el punto, o
    // -1 si el menu esta cerrado o el punto cae fuera.
    [[nodiscard]] int HitTestSpeedMenu(int x, int y) const noexcept;

    [[nodiscard]] bool ControlsVisible() const noexcept { return model_.showControls; }

private:
    void CreateDeviceResources();
    void CreateTextFormats();
    void EnsureSurface(unsigned width, unsigned height);
    void Repaint();

    void DrawControlBar(unsigned width, unsigned height);
    void DrawTransport(float left, float centerY);
    void DrawTrimRange(float barLeft, float barRight, float barY);
    void DrawCropEditor();
    void DrawFiltersPanel(float right, float bottom);
    void DrawRightControls(float right, float centerY);
    void DrawSpeedMenu();
    void DrawWelcome(unsigned width, unsigned height);
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
    ComPtr<IDWriteTextFormat> controlFormat_;   // indicadores de la derecha
    ComPtr<IDWriteTextFormat> welcomeFormat_;   // titulo de la bienvenida
    ComPtr<IDWriteTextFormat> welcomeBodyFormat_;
    ComPtr<IDWriteTextFormat> hintKeyFormat_;   // columna de teclas, a la derecha
    ComPtr<IDWriteTextFormat> hintTextFormat_;
    ComPtr<IDWriteTextFormat> panelFormat_;      // etiquetas del panel
    ComPtr<IDWriteTextFormat> panelValueFormat_; // valores, alineados a la derecha

    // El triangulo de reproduccion se reutiliza para el boton de play, los de
    // paso y el icono de la bienvenida. Crearlo una vez y moverlo con una
    // transformacion evita reconstruir geometria en cada repintado.
    ComPtr<ID2D1PathGeometry> triangleGeometry_;

    // Trazo discontinuo del recuadro de la bienvenida: comunica "suelta aqui"
    // sin necesidad de explicarlo.
    ComPtr<ID2D1StrokeStyle> dashedStroke_;

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

    // Geometria del ultimo repintado, para el posicionamiento con el raton.
    D2D1_RECT_F seekBarRect_{};
    D2D1_RECT_F playRect_{};
    D2D1_RECT_F stepBackRect_{};
    D2D1_RECT_F stepForwardRect_{};
    D2D1_RECT_F speedRect_{};
    D2D1_RECT_F snapshotRect_{};
    D2D1_RECT_F trimRect_{};
    D2D1_RECT_F cropButtonRect_{};
    D2D1_RECT_F filtersButtonRect_{};
    D2D1_RECT_F filtersPanelRect_{};
    D2D1_RECT_F toneGraphRect_{};

    // Pista de cada deslizador del panel, en el orden de FilterControl.
    std::array<D2D1_RECT_F, 5> sliderTracks_{};
    D2D1_RECT_F resetRect_{};
    D2D1_RECT_F trimStartHandle_{};
    D2D1_RECT_F trimEndHandle_{};
    D2D1_RECT_F speedMenuRect_{};
    float       speedMenuItemHeight_ = 0.0f;
};

}  // namespace pyxis
