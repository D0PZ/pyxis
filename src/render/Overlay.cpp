#include "render/Overlay.hpp"

#include "core/Error.hpp"
#include "core/Log.hpp"

#include "shaders/fullscreen_vs.h"
#include "shaders/overlay_ps.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <cwchar>

namespace pyxis {
namespace {

// Metricas de la interfaz, en pixeles logicos.
//
//  La barra se organiza en tres filas, como la de cualquier reproductor de
//  escritorio: titulo arriba, barra de progreso a lo ancho, y debajo los
//  botones con el reloj. Tener los controles en su propia fila -y no montados
//  sobre la barra de progreso- es lo que permite que la barra ocupe todo el
//  ancho y que los botones tengan una zona de clic comoda.
constexpr float kBarHeight     = 108.0f;
constexpr float kMargin        = 24.0f;
constexpr float kSeekBarHeight = 6.0f;
constexpr float kSeekBarY      = 58.0f;   // desde el borde inferior
constexpr float kButtonRowY    = 26.0f;   // centro de la fila de botones

// Botones de transporte.
constexpr float kButtonSize = 30.0f;
constexpr float kButtonGap  = 6.0f;

// Controles a la derecha.
constexpr float kSpeedWidth     = 52.0f;
constexpr float kSnapshotWidth  = 34.0f;
constexpr float kTrimWidth      = 34.0f;
constexpr float kCropWidth      = 34.0f;
constexpr float kFiltersWidth   = 34.0f;

// Panel de ajustes.
constexpr float kPanelWidth      = 330.0f;
constexpr float kPanelPadding    = 16.0f;
constexpr float kPanelRowHeight  = 30.0f;
constexpr float kPanelLabelWidth = 92.0f;
constexpr float kPanelValueWidth = 46.0f;
constexpr float kToneGraphHeight = 92.0f;

// Tirador del encuadre. Grande a proposito: se manipula sobre la imagen, donde
// no hay ninguna otra referencia visual que ayude a apuntar.
constexpr float kCropHandleSize = 16.0f;
constexpr float kCropGrab       = 11.0f;

// Los cinco deslizadores, en el orden en que se dibujan.
constexpr std::array<FilterControl, 5> kSliderControls = {
    FilterControl::Exposure, FilterControl::Brightness, FilterControl::Contrast,
    FilterControl::Saturation, FilterControl::Gamma,
};

constexpr std::array<const wchar_t*, 5> kSliderLabels = {
    L"Exposición", L"Brillo", L"Contraste", L"Saturación", L"Gamma",
};

// Las tres bandas tonales, con su posicion en el grafico.
constexpr std::array<FilterControl, 3> kBandControls = {
    FilterControl::Shadows, FilterControl::Midtones, FilterControl::Highlights,
};
constexpr std::array<float, 3> kBandCenters = {0.15f, 0.50f, 0.85f};

// Misma formula que ApplyToneBands en video.hlsl. Duplicarla es deliberado: el
// grafico tiene que dibujar exactamente la curva que aplica la GPU, y no hay
// forma de compartir codigo entre HLSL y C++. Si una cambia, la otra tambien.
[[nodiscard]] float BandWeight(float luminance, float center) noexcept {
    const float distance = (luminance - center) / 0.25f;
    return std::exp(-distance * distance);
}

[[nodiscard]] float ToneCurve(float luminance, const ImageAdjustments& a) noexcept {
    const float gain = 1.0f + (a.shadows - 1.0f) * BandWeight(luminance, 0.15f) +
                              (a.midtones - 1.0f) * BandWeight(luminance, 0.50f) +
                              (a.highlights - 1.0f) * BandWeight(luminance, 0.85f);
    return std::clamp(luminance * std::max(gain, 0.0f), 0.0f, 1.0f);
}

[[nodiscard]] std::wstring FormatValue(FilterControl control, float value) {
    std::array<wchar_t, 24> buffer{};
    if (control == FilterControl::Exposure) {
        std::swprintf(buffer.data(), buffer.size(), L"%+.1f", value);
    } else {
        std::swprintf(buffer.data(), buffer.size(), L"%.2f", value);
    }
    return buffer.data();
}

// Tirador de los puntos de recorte. Mas ancho que su dibujo: acertar con el
// raton sobre una linea de dos pixeles es imposible.
constexpr float kTrimGrabWidth = 14.0f;
constexpr float kControlGap     = 10.0f;
constexpr float kControlHeight  = 28.0f;
constexpr float kSpeedMenuItemH = 30.0f;

// Texto de cada velocidad. Se construye una vez: formatearlo en cada repintado
// seria reconstruir seis cadenas por nada.
const std::array<std::wstring, kPlaybackRates.size()> kSpeedLabels = {
    L"0.25x", L"0.5x", L"0.75x", L"1x", L"1.5x", L"2x",
};

[[nodiscard]] std::size_t IndexForRate(int rateMilli) noexcept {
    for (std::size_t i = 0; i < kPlaybackRates.size(); ++i) {
        if (kPlaybackRates[i] == rateMilli) return i;
    }
    return 3;   // 1x
}

// Ambar para el recorte. Se elige distinto del azul del progreso a proposito:
// son dos cosas que conviven en la misma barra y confundirlas seria facil.
[[nodiscard]] D2D1_COLOR_F TrimColor(float alpha) noexcept {
    return D2D1::ColorF(1.0f, 0.72f, 0.24f, alpha);
}

[[nodiscard]] bool Contains(const D2D1_RECT_F& rect, float x, float y) noexcept {
    return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
}

// Los iconos se dibujan con geometria y no con caracteres de una fuente: asi
// se ven identicos en cualquier equipo, tenga instaladas las fuentes que tenga,
// y escalan sin depender de que exista el glifo en el tamano adecuado.
void FillTriangle(ID2D1DeviceContext* context, ID2D1Geometry* triangle,
                  ID2D1Brush* brush, D2D1_POINT_2F center, float scale,
                  bool pointsRight) {
    const D2D1::Matrix3x2F flip =
        D2D1::Matrix3x2F::Scale(pointsRight ? scale : -scale, scale);
    context->SetTransform(flip * D2D1::Matrix3x2F::Translation(center.x, center.y));
    context->FillGeometry(triangle, brush);
    context->SetTransform(D2D1::Matrix3x2F::Identity());
}

void FillPauseBars(ID2D1DeviceContext* context, ID2D1Brush* brush,
                   D2D1_POINT_2F center) {
    constexpr float kBarW = 3.6f;
    constexpr float kBarH = 8.0f;
    constexpr float kSeparation = 3.2f;

    context->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(center.x - kSeparation - kBarW, center.y - kBarH,
                                      center.x - kSeparation, center.y + kBarH),
                          1.4f, 1.4f),
        brush);
    context->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(center.x + kSeparation, center.y - kBarH,
                                      center.x + kSeparation + kBarW, center.y + kBarH),
                          1.4f, 1.4f),
        brush);
}

// Blanco de la interfaz en HDR. BT.2408 fija el blanco de grafismos en 203
// nits; subirlo hace que el texto deslumbre en escenas oscuras.
constexpr float kUiWhiteNits = 203.0f;

std::wstring FormatTime(Micros micros) {
    if (micros == kNoTimestamp || micros < 0) return L"--:--";

    const long long totalSeconds = micros / kMicrosPerSecond;
    const long long hours   = totalSeconds / 3600;
    const long long minutes = (totalSeconds % 3600) / 60;
    const long long seconds = totalSeconds % 60;

    std::array<wchar_t, 32> buffer{};
    if (hours > 0) {
        std::swprintf(buffer.data(), buffer.size(), L"%lld:%02lld:%02lld",
                      hours, minutes, seconds);
    } else {
        std::swprintf(buffer.data(), buffer.size(), L"%02lld:%02lld", minutes, seconds);
    }
    return buffer.data();
}

D2D1_COLOR_F Rgba(float r, float g, float b, float a) noexcept {
    return D2D1::ColorF(r, g, b, a);
}

}  // namespace

// ---------------------------------------------------------------------------
//  Creacion
// ---------------------------------------------------------------------------
void Overlay::Create(Device& device) {
    Destroy();
    device_ = &device;
    CreateDeviceResources();
    CreateTextFormats();
}

void Overlay::CreateDeviceResources() {
    ID3D11Device* d3d = device_->Handle();

    // Direct2D en modo multihilo: comparte dispositivo con el decodificador y
    // con el renderizador, que corren en hilos distintos.
    D2D1_FACTORY_OPTIONS options{};
#ifndef NDEBUG
    options.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
    PYXIS_CHECK_HR(::D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED,
                                       __uuidof(ID2D1Factory1), &options,
                                       reinterpret_cast<void**>(d2dFactory_.GetAddressOf())),
                   "no se pudo crear la fabrica de Direct2D");

    ComPtr<IDXGIDevice> dxgiDevice;
    PYXIS_CHECK_HR(d3d->QueryInterface(IID_PPV_ARGS(&dxgiDevice)),
                   "el dispositivo D3D11 no expone IDXGIDevice");

    PYXIS_CHECK_HR(d2dFactory_->CreateDevice(dxgiDevice.Get(), &d2dDevice_),
                   "no se pudo crear el dispositivo Direct2D");

    PYXIS_CHECK_HR(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                                   &d2dContext_),
                   "no se pudo crear el contexto Direct2D");

    PYXIS_CHECK_HR(d2dContext_->CreateSolidColorBrush(Rgba(1, 1, 1, 1), &brush_),
                   "no se pudo crear el pincel");

    // Recursos de composicion en D3D11.
    PYXIS_CHECK_HR(d3d->CreateVertexShader(g_FullscreenVS, sizeof(g_FullscreenVS),
                                           nullptr, &fullscreenShader_),
                   "no se pudo crear el vertex shader de la superposicion");
    PYXIS_CHECK_HR(d3d->CreatePixelShader(g_OverlayPS, sizeof(g_OverlayPS),
                                          nullptr, &compositeShader_),
                   "no se pudo crear el pixel shader de la superposicion");

    // Muestreo puntual: la textura se compone 1:1 con la pantalla, asi que
    // filtrar solo emborronaria el texto.
    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter         = D3D11_FILTER_MIN_MAG_MIP_POINT;
    samplerDesc.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MaxLOD         = D3D11_FLOAT32_MAX;
    PYXIS_CHECK_HR(d3d->CreateSamplerState(&samplerDesc, &pointSampler_),
                   "no se pudo crear el muestreador de la superposicion");

    // Mezcla para alfa PREMULTIPLICADO, que es lo que produce Direct2D:
    // resultado = origen + destino * (1 - alfa). Usar SRC_ALPHA en vez de ONE
    // premultiplicaria dos veces y el texto saldria translucido.
    D3D11_BLEND_DESC blendDesc{};
    blendDesc.RenderTarget[0].BlendEnable           = TRUE;
    blendDesc.RenderTarget[0].SrcBlend              = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    PYXIS_CHECK_HR(d3d->CreateBlendState(&blendDesc, &alphaBlend_),
                   "no se pudo crear el estado de mezcla de la superposicion");

    D3D11_RASTERIZER_DESC rasterizerDesc{};
    rasterizerDesc.FillMode        = D3D11_FILL_SOLID;
    rasterizerDesc.CullMode        = D3D11_CULL_NONE;
    rasterizerDesc.DepthClipEnable = TRUE;
    PYXIS_CHECK_HR(d3d->CreateRasterizerState(&rasterizerDesc, &rasterizer_),
                   "no se pudo crear el rasterizador de la superposicion");

    // Triangulo de reproduccion centrado en el origen y apuntando a la derecha.
    // Se reutiliza para play, los botones de paso y el icono de bienvenida.
    PYXIS_CHECK_HR(d2dFactory_->CreatePathGeometry(&triangleGeometry_),
                   "no se pudo crear la geometria del triangulo");
    {
        ComPtr<ID2D1GeometrySink> sink;
        PYXIS_CHECK_HR(triangleGeometry_->Open(&sink),
                       "no se pudo abrir la geometria del triangulo");
        sink->BeginFigure(D2D1::Point2F(-6.0f, -8.0f), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(D2D1::Point2F(8.0f, 0.0f));
        sink->AddLine(D2D1::Point2F(-6.0f, 8.0f));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        PYXIS_CHECK_HR(sink->Close(), "no se pudo cerrar la geometria del triangulo");
    }

    const D2D1_STROKE_STYLE_PROPERTIES dashed = D2D1::StrokeStyleProperties(
        D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
        D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_DASH, 0.0f);
    PYXIS_CHECK_HR(d2dFactory_->CreateStrokeStyle(dashed, nullptr, 0, &dashedStroke_),
                   "no se pudo crear el trazo discontinuo");

    D3D11_BUFFER_DESC bufferDesc{};
    bufferDesc.ByteWidth      = sizeof(Constants);
    bufferDesc.Usage          = D3D11_USAGE_DYNAMIC;
    bufferDesc.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    PYXIS_CHECK_HR(d3d->CreateBuffer(&bufferDesc, nullptr, &constantBuffer_),
                   "no se pudo crear el constant buffer de la superposicion");
}

void Overlay::CreateTextFormats() {
    PYXIS_CHECK_HR(::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                         __uuidof(IDWriteFactory1),
                                         reinterpret_cast<IUnknown**>(
                                             dwriteFactory_.GetAddressOf())),
                   "no se pudo crear la fabrica de DirectWrite");

    // Segoe UI Variable es la tipografia de sistema de Windows 11. Si faltara,
    // DirectWrite sustituye por la de respaldo sin fallar.
    const auto createFormat = [&](const wchar_t* family, float size,
                                  DWRITE_FONT_WEIGHT weight,
                                  ComPtr<IDWriteTextFormat>& out) {
        PYXIS_CHECK_HR(dwriteFactory_->CreateTextFormat(
                           family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                           DWRITE_FONT_STRETCH_NORMAL, size, L"", &out),
                       "no se pudo crear el formato de texto");
    };

    createFormat(L"Segoe UI Variable Display", 15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, timeFormat_);
    createFormat(L"Segoe UI Variable Display", 17.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, titleFormat_);
    // Monoespaciada para las estadisticas: los numeros no bailan al cambiar.
    createFormat(L"Cascadia Mono", 13.0f, DWRITE_FONT_WEIGHT_NORMAL, statsFormat_);
    createFormat(L"Segoe UI Variable Display", 14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                 controlFormat_);

    createFormat(L"Segoe UI Variable Display", 26.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                 welcomeFormat_);
    createFormat(L"Segoe UI Variable Display", 15.0f, DWRITE_FONT_WEIGHT_NORMAL,
                 welcomeBodyFormat_);
    createFormat(L"Segoe UI Variable Display", 13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                 hintKeyFormat_);
    createFormat(L"Segoe UI Variable Display", 13.0f, DWRITE_FONT_WEIGHT_NORMAL,
                 hintTextFormat_);
    createFormat(L"Segoe UI Variable Display", 13.0f, DWRITE_FONT_WEIGHT_NORMAL,
                 panelFormat_);
    createFormat(L"Cascadia Mono", 12.0f, DWRITE_FONT_WEIGHT_NORMAL, panelValueFormat_);

    panelFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    panelValueFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    panelValueFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);

    controlFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    controlFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    welcomeFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    welcomeBodyFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);

    // Las teclas se alinean a la derecha y su descripcion a la izquierda: las
    // dos columnas quedan a ras sin necesidad de tabulaciones.
    hintKeyFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);

    timeFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    titleFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    // Los titulos largos se recortan en lugar de desbordar la barra.
    const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    titleFormat_->SetTrimming(&trimming, nullptr);
}

// ---------------------------------------------------------------------------
//  Superficie
// ---------------------------------------------------------------------------
void Overlay::EnsureSurface(unsigned width, unsigned height) {
    if (surface_ && width_ == width && height_ == height) return;
    if (width == 0 || height == 0) return;

    // El destino de Direct2D debe soltarse antes que la textura, o la textura
    // no se liberara y la memoria crecera en cada redimensionado.
    d2dContext_->SetTarget(nullptr);
    d2dTarget_.Reset();
    surfaceView_.Reset();
    surface_.Reset();

    D3D11_TEXTURE2D_DESC description{};
    description.Width      = width;
    description.Height     = height;
    description.MipLevels  = 1;
    description.ArraySize  = 1;
    description.Format     = DXGI_FORMAT_B8G8R8A8_UNORM;   // el unico que acepta D2D
    description.SampleDesc = {1, 0};
    description.Usage      = D3D11_USAGE_DEFAULT;
    description.BindFlags  = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    PYXIS_CHECK_HR(device_->Handle()->CreateTexture2D(&description, nullptr, &surface_),
                   "no se pudo crear la textura de la superposicion");

    PYXIS_CHECK_HR(device_->Handle()->CreateShaderResourceView(surface_.Get(), nullptr,
                                                               &surfaceView_),
                   "no se pudo crear la vista de la superposicion");

    ComPtr<IDXGISurface> dxgiSurface;
    PYXIS_CHECK_HR(surface_.As(&dxgiSurface), "la textura no expone IDXGISurface");

    const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

    PYXIS_CHECK_HR(d2dContext_->CreateBitmapFromDxgiSurface(dxgiSurface.Get(), &properties,
                                                            &d2dTarget_),
                   "no se pudo enlazar Direct2D con la textura");

    d2dContext_->SetTarget(d2dTarget_.Get());

    width_  = width;
    height_ = height;
    dirty_  = true;
}

void Overlay::Resize(unsigned width, unsigned height) {
    EnsureSurface(width, height);
}

void Overlay::Update(const OverlayModel& model) {
    if (model == model_) return;
    model_ = model;
    dirty_ = true;
}

// ---------------------------------------------------------------------------
//  Repintado (solo cuando el contenido cambia)
// ---------------------------------------------------------------------------
void Overlay::Repaint() {
    if (!d2dTarget_) return;

    d2dContext_->BeginDraw();
    d2dContext_->Clear(Rgba(0, 0, 0, 0));   // totalmente transparente

    if (model_.showWelcome) {
        DrawWelcome(width_, height_);
    } else if (model_.showControls) {
        DrawControlBar(width_, height_);
    }
    // El editor de encuadre va DEBAJO de la barra en orden de dibujo -es decir,
    // primero- para que los controles queden accesibles por encima de el.
    if (model_.cropEditing && !model_.showWelcome) {
        DrawCropEditor();
    }

    // El menu se dibuja despues de la barra para quedar por encima de ella.
    if (model_.showControls && !model_.showWelcome && model_.speedMenuOpen) {
        DrawSpeedMenu();
    }
    if (model_.showStats) {
        DrawStats(width_);
    }
    if (!model_.toast.empty()) {
        DrawToast(width_, height_);
    }

    const HRESULT hr = d2dContext_->EndDraw();
    if (FAILED(hr)) {
        PYXIS_WARN("el repintado de la superposicion fallo: {}",
                   DescribeError(ErrorDomain::HResult, hr));
        return;
    }
    dirty_ = false;
}

void Overlay::DrawControlBar(unsigned width, unsigned height) {
    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);

    // Fondo degradado simulado con dos bandas semitransparentes: mas barato que
    // un pincel de degradado real y visualmente equivalente a este tamano.
    brush_->SetColor(Rgba(0, 0, 0, 0.35f));
    d2dContext_->FillRectangle(D2D1::RectF(0, h - kBarHeight, w, h - kBarHeight * 0.5f),
                               brush_.Get());
    brush_->SetColor(Rgba(0, 0, 0, 0.70f));
    d2dContext_->FillRectangle(D2D1::RectF(0, h - kBarHeight * 0.5f, w, h), brush_.Get());

    // --- Titulo ------------------------------------------------------------
    if (!model_.title.empty()) {
        brush_->SetColor(Rgba(1, 1, 1, 0.88f));
        d2dContext_->DrawTextW(model_.title.c_str(),
                               static_cast<UINT32>(model_.title.size()),
                               titleFormat_.Get(),
                               D2D1::RectF(kMargin, h - kBarHeight + 8.0f,
                                           w - kMargin, h - kSeekBarY - 10.0f),
                               brush_.Get());
    }

    // --- Barra de progreso -------------------------------------------------
    // Ocupa el ancho completo: los controles viven en su propia fila.
    const float barLeft  = kMargin;
    const float barRight = w - kMargin;
    const float barY     = h - kSeekBarY;

    seekBarRect_ = D2D1::RectF(barLeft, barY - kSeekBarHeight * 0.5f,
                               barRight, barY + kSeekBarHeight * 0.5f);

    brush_->SetColor(Rgba(1, 1, 1, 0.25f));
    d2dContext_->FillRoundedRectangle(D2D1::RoundedRect(seekBarRect_, 3.0f, 3.0f),
                                      brush_.Get());

    double progress = 0.0;
    if (model_.duration != kNoTimestamp && model_.duration > 0) {
        progress = std::clamp(static_cast<double>(model_.position) /
                                  static_cast<double>(model_.duration),
                              0.0, 1.0);
    }

    if (progress > 0.0) {
        D2D1_RECT_F filled = seekBarRect_;
        filled.right = barLeft + static_cast<float>((barRight - barLeft) * progress);
        brush_->SetColor(Rgba(0.35f, 0.72f, 1.0f, 0.95f));
        d2dContext_->FillRoundedRectangle(D2D1::RoundedRect(filled, 3.0f, 3.0f),
                                          brush_.Get());

        brush_->SetColor(Rgba(1, 1, 1, 1));
        d2dContext_->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(filled.right, barY), 7.0f, 7.0f), brush_.Get());
    }

    DrawTrimRange(barLeft, barRight, barY);

    // --- Fila de botones ---------------------------------------------------
    const float rowY = h - kButtonRowY;

    DrawTransport(kMargin, rowY);
    DrawRightControls(w - kMargin, rowY);

    if (model_.filtersOpen) {
        DrawFiltersPanel(w - kMargin, h - kBarHeight - 12.0f);
    }

    // El reloj arranca despues de los botones de transporte.
    const float clockLeft = stepForwardRect_.right + 16.0f;

    std::wstring clock = FormatTime(model_.position);
    if (model_.duration != kNoTimestamp) {
        clock += L"  /  " + FormatTime(model_.duration);
    }
    if (model_.muted) clock += L"   SIN SONIDO";

    brush_->SetColor(Rgba(1, 1, 1, 0.92f));
    d2dContext_->DrawTextW(clock.c_str(), static_cast<UINT32>(clock.size()),
                           timeFormat_.Get(),
                           D2D1::RectF(clockLeft, rowY - 12.0f,
                                       speedRect_.left - 12.0f, rowY + 12.0f),
                           brush_.Get());
}

// ---------------------------------------------------------------------------
//  Seleccion de recorte sobre la barra
//
//  Los puntos A y B se dibujan como banderines con el rango sombreado entre
//  ellos. Los tiradores sobresalen por encima y por debajo de la barra: si
//  quedaran dentro competirian por el mismo pixel que el cabezal de
//  reproduccion y no habria forma de agarrar el que se quiere.
// ---------------------------------------------------------------------------
void Overlay::DrawTrimRange(float barLeft, float barRight, float barY) {
    trimStartHandle_ = D2D1_RECT_F{};
    trimEndHandle_   = D2D1_RECT_F{};

    if (model_.duration == kNoTimestamp || model_.duration <= 0) return;
    if (model_.trimStart == kNoTimestamp && model_.trimEnd == kNoTimestamp) return;

    const float span = barRight - barLeft;
    const auto positionToX = [&](Micros position) {
        const double ratio = std::clamp(static_cast<double>(position) /
                                            static_cast<double>(model_.duration),
                                        0.0, 1.0);
        return barLeft + static_cast<float>(span * ratio);
    };

    const bool hasStart = model_.trimStart != kNoTimestamp;
    const bool hasEnd   = model_.trimEnd != kNoTimestamp;

    const float startX = hasStart ? positionToX(model_.trimStart) : barLeft;
    const float endX   = hasEnd ? positionToX(model_.trimEnd) : barRight;

    // Sombreado del intervalo, solo cuando estan los dos extremos.
    if (hasStart && hasEnd && endX > startX) {
        brush_->SetColor(TrimColor(0.28f));
        d2dContext_->FillRectangle(
            D2D1::RectF(startX, barY - 11.0f, endX, barY + 11.0f), brush_.Get());
    }

    const auto drawHandle = [&](float x, bool isStart, D2D1_RECT_F& outRect) {
        brush_->SetColor(TrimColor(0.98f));

        // Tallo.
        d2dContext_->FillRectangle(
            D2D1::RectF(x - 1.2f, barY - 13.0f, x + 1.2f, barY + 13.0f), brush_.Get());

        // Banderin, apuntando hacia dentro del intervalo.
        const float flagWidth = isStart ? 9.0f : -9.0f;
        d2dContext_->FillRectangle(
            D2D1::RectF(std::min(x, x + flagWidth), barY - 13.0f,
                        std::max(x, x + flagWidth), barY - 6.0f),
            brush_.Get());

        outRect = D2D1::RectF(x - kTrimGrabWidth * 0.5f, barY - 15.0f,
                              x + kTrimGrabWidth * 0.5f, barY + 15.0f);
    };

    if (hasStart) drawHandle(startX, true, trimStartHandle_);
    if (hasEnd)   drawHandle(endX, false, trimEndHandle_);
}

// ---------------------------------------------------------------------------
//  Botones de transporte
//
//  Reproducir, un fotograma atras y un fotograma adelante. Los atajos de
//  teclado ya existian, pero quien abre el programa por primera vez no los
//  conoce: sin botones visibles no hay forma de descubrir que el avance
//  fotograma a fotograma existe.
// ---------------------------------------------------------------------------
void Overlay::DrawTransport(float left, float centerY) {
    const float half = kButtonSize * 0.5f;

    const auto makeRect = [&](float x) {
        return D2D1::RectF(x, centerY - half, x + kButtonSize, centerY + half);
    };

    playRect_        = makeRect(left);
    stepBackRect_    = makeRect(left + kButtonSize + kButtonGap);
    stepForwardRect_ = makeRect(left + 2.0f * (kButtonSize + kButtonGap));

    const auto centerOf = [](const D2D1_RECT_F& rect) {
        return D2D1::Point2F((rect.left + rect.right) * 0.5f,
                             (rect.top + rect.bottom) * 0.5f);
    };

    brush_->SetColor(Rgba(1, 1, 1, 0.88f));

    // Reproducir o pausar.
    const D2D1_POINT_2F playCenter = centerOf(playRect_);
    if (model_.paused) {
        FillTriangle(d2dContext_.Get(), triangleGeometry_.Get(), brush_.Get(),
                     playCenter, 1.0f, true);
    } else {
        FillPauseBars(d2dContext_.Get(), brush_.Get(), playCenter);
    }

    // Paso atras y adelante: triangulo con un tope, como en cualquier
    // reproductor. El tope indica "solo uno", frente al triangulo suelto del
    // play, que significa "sigue".
    const auto drawStep = [&](const D2D1_RECT_F& rect, bool forward) {
        const D2D1_POINT_2F center = centerOf(rect);
        const float offset = forward ? -2.0f : 2.0f;

        FillTriangle(d2dContext_.Get(), triangleGeometry_.Get(), brush_.Get(),
                     D2D1::Point2F(center.x + offset, center.y), 0.82f, forward);

        const float barX = forward ? center.x + 6.0f : center.x - 8.0f;
        d2dContext_->FillRectangle(
            D2D1::RectF(barX, center.y - 6.6f, barX + 2.2f, center.y + 6.6f),
            brush_.Get());
    };

    drawStep(stepBackRect_, false);
    drawStep(stepForwardRect_, true);
}

// ---------------------------------------------------------------------------
//  Indicadores de la derecha
//
//  La velocidad se dibuja como TEXTO, sin marco ni relleno: la intencion es que
//  no parezca un boton. Solo se insinua con un fondo muy tenue cuando el menu
//  esta abierto, para que se entienda de donde sale el desplegable.
// ---------------------------------------------------------------------------
void Overlay::DrawRightControls(float right, float centerY) {
    const float filtersLeft  = right - kFiltersWidth;
    const float cropLeft     = filtersLeft - kControlGap - kCropWidth;
    const float trimLeft     = cropLeft - kControlGap - kTrimWidth;
    const float snapshotLeft = trimLeft - kControlGap - kSnapshotWidth;
    const float speedLeft    = snapshotLeft - kControlGap - kSpeedWidth;

    speedRect_ = D2D1::RectF(speedLeft, centerY - kControlHeight * 0.5f,
                             speedLeft + kSpeedWidth, centerY + kControlHeight * 0.5f);
    snapshotRect_ = D2D1::RectF(snapshotLeft, centerY - kControlHeight * 0.5f,
                                snapshotLeft + kSnapshotWidth,
                                centerY + kControlHeight * 0.5f);
    trimRect_ = D2D1::RectF(trimLeft, centerY - kControlHeight * 0.5f,
                            trimLeft + kTrimWidth, centerY + kControlHeight * 0.5f);
    cropButtonRect_ = D2D1::RectF(cropLeft, centerY - kControlHeight * 0.5f,
                                  cropLeft + kCropWidth, centerY + kControlHeight * 0.5f);
    filtersButtonRect_ = D2D1::RectF(filtersLeft, centerY - kControlHeight * 0.5f,
                                     right, centerY + kControlHeight * 0.5f);

    // --- Velocidad ---------------------------------------------------------
    if (model_.speedMenuOpen) {
        brush_->SetColor(Rgba(1, 1, 1, 0.12f));
        d2dContext_->FillRoundedRectangle(D2D1::RoundedRect(speedRect_, 6.0f, 6.0f),
                                          brush_.Get());
    }

    const std::wstring& label = kSpeedLabels[IndexForRate(model_.rateMilli)];

    // Se resalta en azul cuando no esta a velocidad normal: asi se nota de un
    // vistazo que la reproduccion va alterada, sin tener que leer el numero.
    brush_->SetColor(model_.rateMilli == 1000 ? Rgba(1, 1, 1, 0.80f)
                                              : Rgba(0.45f, 0.80f, 1.0f, 0.98f));
    d2dContext_->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()),
                           controlFormat_.Get(), speedRect_, brush_.Get());

    // --- Captura -----------------------------------------------------------
    // Un circulo dentro de un anillo: la silueta de un obturador. Se dibuja con
    // geometria y no con un emoji de camara porque asi se ve identico en
    // cualquier equipo, tenga las fuentes que tenga.
    const D2D1_POINT_2F center =
        D2D1::Point2F((snapshotRect_.left + snapshotRect_.right) * 0.5f, centerY);

    brush_->SetColor(Rgba(1, 1, 1, 0.80f));
    d2dContext_->DrawEllipse(D2D1::Ellipse(center, 9.0f, 9.0f), brush_.Get(), 1.6f);
    d2dContext_->FillEllipse(D2D1::Ellipse(center, 5.0f, 5.0f), brush_.Get());

    // --- Recorte -----------------------------------------------------------
    // Unas tijeras: dos aros y dos hojas cruzadas. Se apaga mientras no haya un
    // intervalo valido, que es como se comunica que el boton aun no hace nada.
    const bool ready = model_.trimStart != kNoTimestamp &&
                       model_.trimEnd != kNoTimestamp &&
                       model_.trimEnd > model_.trimStart && !model_.trimBusy;

    const D2D1_POINT_2F trimCenter =
        D2D1::Point2F((trimRect_.left + trimRect_.right) * 0.5f, centerY);

    brush_->SetColor(model_.trimBusy ? TrimColor(0.45f)
                                     : (ready ? TrimColor(0.98f) : Rgba(1, 1, 1, 0.32f)));

    const float bladeTop = trimCenter.y - 8.0f;
    const float pivotY   = trimCenter.y + 1.0f;
    d2dContext_->DrawLine(D2D1::Point2F(trimCenter.x - 5.0f, bladeTop),
                          D2D1::Point2F(trimCenter.x + 3.0f, pivotY), brush_.Get(), 1.5f);
    d2dContext_->DrawLine(D2D1::Point2F(trimCenter.x + 5.0f, bladeTop),
                          D2D1::Point2F(trimCenter.x - 3.0f, pivotY), brush_.Get(), 1.5f);
    d2dContext_->DrawEllipse(
        D2D1::Ellipse(D2D1::Point2F(trimCenter.x - 4.0f, trimCenter.y + 6.0f), 3.0f, 3.0f),
        brush_.Get(), 1.4f);
    d2dContext_->DrawEllipse(
        D2D1::Ellipse(D2D1::Point2F(trimCenter.x + 4.0f, trimCenter.y + 6.0f), 3.0f, 3.0f),
        brush_.Get(), 1.4f);

    // --- Encuadre ----------------------------------------------------------
    // Las dos escuadras cruzadas del simbolo de recorte fotografico. Se enciende
    // cuando hay un encuadre activo o se esta editando.
    const D2D1_POINT_2F cropCenter =
        D2D1::Point2F((cropButtonRect_.left + cropButtonRect_.right) * 0.5f, centerY);

    const bool cropActive = model_.cropEditing || !model_.crop.IsFull();
    brush_->SetColor(model_.cropEditing ? Rgba(0.45f, 0.80f, 1.0f, 0.98f)
                                        : Rgba(1, 1, 1, cropActive ? 0.92f : 0.72f));

    d2dContext_->DrawLine(D2D1::Point2F(cropCenter.x - 4.0f, cropCenter.y - 9.0f),
                          D2D1::Point2F(cropCenter.x - 4.0f, cropCenter.y + 5.0f),
                          brush_.Get(), 1.5f);
    d2dContext_->DrawLine(D2D1::Point2F(cropCenter.x - 9.0f, cropCenter.y + 4.0f),
                          D2D1::Point2F(cropCenter.x + 5.0f, cropCenter.y + 4.0f),
                          brush_.Get(), 1.5f);
    d2dContext_->DrawLine(D2D1::Point2F(cropCenter.x + 4.0f, cropCenter.y - 5.0f),
                          D2D1::Point2F(cropCenter.x + 4.0f, cropCenter.y + 9.0f),
                          brush_.Get(), 1.5f);
    d2dContext_->DrawLine(D2D1::Point2F(cropCenter.x - 5.0f, cropCenter.y - 4.0f),
                          D2D1::Point2F(cropCenter.x + 9.0f, cropCenter.y - 4.0f),
                          brush_.Get(), 1.5f);

    // --- Ajustes -----------------------------------------------------------
    // Tres deslizadores con el mando a distinta altura, el icono universal de
    // "parametros".
    const D2D1_POINT_2F filtersCenter =
        D2D1::Point2F((filtersButtonRect_.left + filtersButtonRect_.right) * 0.5f, centerY);

    brush_->SetColor(model_.filtersOpen ? Rgba(0.45f, 0.80f, 1.0f, 0.98f)
                                        : Rgba(1, 1, 1,
                                               model_.adjustments.IsNeutral() ? 0.72f : 0.92f));

    static constexpr std::array<float, 3> kRows  = {-5.5f, 0.0f, 5.5f};
    static constexpr std::array<float, 3> kKnobs = {-2.5f, 3.5f, -0.5f};

    for (std::size_t i = 0; i < kRows.size(); ++i) {
        const float y = filtersCenter.y + kRows[i];
        d2dContext_->DrawLine(D2D1::Point2F(filtersCenter.x - 8.0f, y),
                              D2D1::Point2F(filtersCenter.x + 8.0f, y), brush_.Get(), 1.3f);
        d2dContext_->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(filtersCenter.x + kKnobs[i], y), 2.4f, 2.4f),
            brush_.Get());
    }
}

// ---------------------------------------------------------------------------
//  Editor de encuadre
//
//  El video se sigue viendo ENTERO y lo que queda fuera del rectangulo se
//  atenua. Recortar en vivo mientras se ajusta seria contraproducente: para
//  decidir un encuadre hay que ver lo que se esta dejando fuera.
// ---------------------------------------------------------------------------
void Overlay::DrawCropEditor() {
    if (!model_.videoRect.Valid()) return;

    const ScreenRect area = CropScreenRect();
    if (!area.Valid()) return;

    const ScreenRect& video = model_.videoRect;

    // Atenuado de las cuatro franjas exteriores. Cuatro rectangulos en vez de
    // uno con agujero: no hace falta geometria recortada para esto.
    brush_->SetColor(Rgba(0, 0, 0, 0.58f));
    d2dContext_->FillRectangle(D2D1::RectF(video.left, video.top, video.right, area.top),
                               brush_.Get());
    d2dContext_->FillRectangle(
        D2D1::RectF(video.left, area.bottom, video.right, video.bottom), brush_.Get());
    d2dContext_->FillRectangle(D2D1::RectF(video.left, area.top, area.left, area.bottom),
                               brush_.Get());
    d2dContext_->FillRectangle(D2D1::RectF(area.right, area.top, video.right, area.bottom),
                               brush_.Get());

    // Regla de los tercios dentro del encuadre: es la referencia con la que se
    // compone un plano, y tenerla delante mientras se ajusta es media ayuda.
    brush_->SetColor(Rgba(1, 1, 1, 0.20f));
    for (int i = 1; i <= 2; ++i) {
        const float fraction = static_cast<float>(i) / 3.0f;
        const float x = area.left + area.Width() * fraction;
        const float y = area.top + area.Height() * fraction;
        d2dContext_->DrawLine(D2D1::Point2F(x, area.top), D2D1::Point2F(x, area.bottom),
                              brush_.Get(), 1.0f);
        d2dContext_->DrawLine(D2D1::Point2F(area.left, y), D2D1::Point2F(area.right, y),
                              brush_.Get(), 1.0f);
    }

    // Marco.
    brush_->SetColor(Rgba(1, 1, 1, 0.95f));
    d2dContext_->DrawRectangle(
        D2D1::RectF(area.left, area.top, area.right, area.bottom), brush_.Get(), 1.6f);

    // Tiradores: ocho cuadrados, cuatro en las esquinas y cuatro en los puntos
    // medios de cada lado.
    const float half = kCropHandleSize * 0.5f;
    const std::array<D2D1_POINT_2F, 8> handles = {
        D2D1::Point2F(area.left, area.top),
        D2D1::Point2F((area.left + area.right) * 0.5f, area.top),
        D2D1::Point2F(area.right, area.top),
        D2D1::Point2F(area.left, (area.top + area.bottom) * 0.5f),
        D2D1::Point2F(area.right, (area.top + area.bottom) * 0.5f),
        D2D1::Point2F(area.left, area.bottom),
        D2D1::Point2F((area.left + area.right) * 0.5f, area.bottom),
        D2D1::Point2F(area.right, area.bottom),
    };

    for (const D2D1_POINT_2F& handle : handles) {
        const D2D1_RECT_F box = D2D1::RectF(handle.x - half, handle.y - half,
                                            handle.x + half, handle.y + half);
        brush_->SetColor(Rgba(0.05f, 0.07f, 0.10f, 0.85f));
        d2dContext_->FillRoundedRectangle(D2D1::RoundedRect(box, 3.0f, 3.0f), brush_.Get());
        brush_->SetColor(Rgba(0.45f, 0.80f, 1.0f, 0.98f));
        d2dContext_->DrawRoundedRectangle(D2D1::RoundedRect(box, 3.0f, 3.0f),
                                          brush_.Get(), 1.8f);
    }

    // Dimensiones resultantes, para que el encuadre se pueda ajustar a un
    // tamano concreto y no solo a ojo.
    std::array<wchar_t, 64> caption{};
    std::swprintf(caption.data(), caption.size(), L"%.0f %%  x  %.0f %%",
                  model_.crop.Width() * 100.0f, model_.crop.Height() * 100.0f);

    // La etiqueta va encima del marco, salvo que no quepa: entonces baja a su
    // interior. Con un encuadre pegado al borde superior, fuera quedaria
    // recortada por la propia ventana.
    const bool fitsAbove = area.top - 30.0f >= video.top;
    const float badgeTop = fitsAbove ? area.top - 30.0f : area.top + 6.0f;

    brush_->SetColor(Rgba(0, 0, 0, 0.65f));
    const D2D1_RECT_F badge =
        D2D1::RectF(area.left + (fitsAbove ? 0.0f : 6.0f), badgeTop,
                    area.left + (fitsAbove ? 128.0f : 134.0f), badgeTop + 24.0f);
    d2dContext_->FillRoundedRectangle(D2D1::RoundedRect(badge, 5.0f, 5.0f), brush_.Get());

    brush_->SetColor(Rgba(1, 1, 1, 0.92f));
    d2dContext_->DrawTextW(caption.data(),
                           static_cast<UINT32>(std::wcslen(caption.data())),
                           controlFormat_.Get(), badge, brush_.Get());
}

ScreenRect Overlay::CropScreenRect() const noexcept {
    const ScreenRect& video = model_.videoRect;
    if (!video.Valid()) return {};

    return ScreenRect{video.left + video.Width() * model_.crop.left,
                      video.top + video.Height() * model_.crop.top,
                      video.left + video.Width() * model_.crop.right,
                      video.top + video.Height() * model_.crop.bottom};
}

// ---------------------------------------------------------------------------
//  Panel de ajustes de imagen
// ---------------------------------------------------------------------------
void Overlay::DrawFiltersPanel(float right, float bottom) {
    const float height = kPanelPadding * 2.0f +
                         kPanelRowHeight * static_cast<float>(kSliderControls.size()) +
                         kToneGraphHeight + 58.0f;

    const float left = right - kPanelWidth;
    const float top  = bottom - height;

    filtersPanelRect_ = D2D1::RectF(left, top, right, bottom);

    brush_->SetColor(Rgba(0.05f, 0.07f, 0.10f, 0.94f));
    d2dContext_->FillRoundedRectangle(D2D1::RoundedRect(filtersPanelRect_, 10.0f, 10.0f),
                                      brush_.Get());
    brush_->SetColor(Rgba(1, 1, 1, 0.14f));
    d2dContext_->DrawRoundedRectangle(D2D1::RoundedRect(filtersPanelRect_, 10.0f, 10.0f),
                                      brush_.Get(), 1.0f);

    // --- Deslizadores ------------------------------------------------------
    const float trackLeft  = left + kPanelPadding + kPanelLabelWidth;
    const float trackRight = right - kPanelPadding - kPanelValueWidth;

    for (std::size_t i = 0; i < kSliderControls.size(); ++i) {
        const FilterControl control = kSliderControls[i];
        const FilterRange   range   = FilterRangeFor(control);
        const float value = FilterValueOf(model_.adjustments, control);

        const float rowTop = top + kPanelPadding + kPanelRowHeight * static_cast<float>(i);
        const float rowY   = rowTop + kPanelRowHeight * 0.5f;

        sliderTracks_[i] = D2D1::RectF(trackLeft, rowY - 9.0f, trackRight, rowY + 9.0f);

        brush_->SetColor(Rgba(1, 1, 1, 0.70f));
        d2dContext_->DrawTextW(kSliderLabels[i],
                               static_cast<UINT32>(std::wcslen(kSliderLabels[i])),
                               panelFormat_.Get(),
                               D2D1::RectF(left + kPanelPadding, rowTop,
                                           trackLeft - 8.0f, rowTop + kPanelRowHeight),
                               brush_.Get());

        // Pista.
        brush_->SetColor(Rgba(1, 1, 1, 0.16f));
        d2dContext_->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(trackLeft, rowY - 2.0f, trackRight, rowY + 2.0f),
                              2.0f, 2.0f),
            brush_.Get());

        const float fraction =
            std::clamp((value - range.minimum) / (range.maximum - range.minimum), 0.0f, 1.0f);
        const float knobX = trackLeft + (trackRight - trackLeft) * fraction;

        // Marca del valor neutro: sin ella no hay forma de volver al centro a
        // ojo, y es justo el punto al que se quiere volver.
        const float neutralValue = control == FilterControl::Exposure ||
                                           control == FilterControl::Brightness
                                       ? 0.0f
                                       : 1.0f;
        const float neutralFraction = std::clamp(
            (neutralValue - range.minimum) / (range.maximum - range.minimum), 0.0f, 1.0f);
        const float neutralX = trackLeft + (trackRight - trackLeft) * neutralFraction;

        brush_->SetColor(Rgba(1, 1, 1, 0.30f));
        d2dContext_->FillRectangle(
            D2D1::RectF(neutralX - 0.6f, rowY - 6.0f, neutralX + 0.6f, rowY + 6.0f),
            brush_.Get());

        // Tramo recorrido desde el neutro, para leer de un vistazo hacia donde
        // se ha movido.
        brush_->SetColor(Rgba(0.35f, 0.72f, 1.0f, 0.85f));
        d2dContext_->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(std::min(neutralX, knobX), rowY - 2.0f,
                                          std::max(neutralX, knobX), rowY + 2.0f),
                              2.0f, 2.0f),
            brush_.Get());

        brush_->SetColor(Rgba(1, 1, 1, 0.96f));
        d2dContext_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, rowY), 6.0f, 6.0f),
                                 brush_.Get());

        brush_->SetColor(Rgba(1, 1, 1, 0.80f));
        const std::wstring text = FormatValue(control, value);
        d2dContext_->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()),
                               panelValueFormat_.Get(),
                               D2D1::RectF(trackRight + 8.0f, rowTop, right - kPanelPadding,
                                           rowTop + kPanelRowHeight),
                               brush_.Get());
    }

    // --- Grafico de bandas tonales ------------------------------------------
    const float graphTop = top + kPanelPadding +
                           kPanelRowHeight * static_cast<float>(kSliderControls.size()) + 8.0f;
    toneGraphRect_ = D2D1::RectF(left + kPanelPadding, graphTop,
                                 right - kPanelPadding, graphTop + kToneGraphHeight);

    brush_->SetColor(Rgba(1, 1, 1, 0.07f));
    d2dContext_->FillRoundedRectangle(D2D1::RoundedRect(toneGraphRect_, 6.0f, 6.0f),
                                      brush_.Get());
    brush_->SetColor(Rgba(1, 1, 1, 0.12f));
    d2dContext_->DrawRoundedRectangle(D2D1::RoundedRect(toneGraphRect_, 6.0f, 6.0f),
                                      brush_.Get(), 1.0f);

    // Diagonal de referencia: es la curva sin tocar, y sirve para ver de un
    // golpe cuanto se ha desviado el ajuste.
    brush_->SetColor(Rgba(1, 1, 1, 0.18f));
    d2dContext_->DrawLine(D2D1::Point2F(toneGraphRect_.left, toneGraphRect_.bottom),
                          D2D1::Point2F(toneGraphRect_.right, toneGraphRect_.top),
                          brush_.Get(), 1.0f);

    // Curva resultante, muestreada.
    const float graphWidth  = toneGraphRect_.right - toneGraphRect_.left;
    const float graphHeight = toneGraphRect_.bottom - toneGraphRect_.top;

    brush_->SetColor(Rgba(0.45f, 0.80f, 1.0f, 0.95f));
    constexpr int kSamples = 48;
    D2D1_POINT_2F previous{};
    for (int i = 0; i <= kSamples; ++i) {
        const float input = static_cast<float>(i) / kSamples;
        const float output = ToneCurve(input, model_.adjustments);

        const D2D1_POINT_2F point =
            D2D1::Point2F(toneGraphRect_.left + graphWidth * input,
                          toneGraphRect_.bottom - graphHeight * output);
        if (i > 0) d2dContext_->DrawLine(previous, point, brush_.Get(), 1.8f);
        previous = point;
    }

    // Los tres puntos de control se dibujan SOBRE la curva, en la altura que
    // esa banda produce. Colocarlos a una altura independiente los dejaria
    // flotando al margen del trazo que representan, que es justo lo que un
    // editor de curvas no debe hacer: el punto ES la curva en ese tono.
    for (std::size_t i = 0; i < kBandControls.size(); ++i) {
        const float center = kBandCenters[i];
        const float output = ToneCurve(center, model_.adjustments);

        const D2D1_POINT_2F point =
            D2D1::Point2F(toneGraphRect_.left + graphWidth * center,
                          toneGraphRect_.bottom - graphHeight * output);

        brush_->SetColor(Rgba(0.05f, 0.07f, 0.10f, 0.9f));
        d2dContext_->FillEllipse(D2D1::Ellipse(point, 7.0f, 7.0f), brush_.Get());
        brush_->SetColor(Rgba(1.0f, 0.72f, 0.24f, 0.98f));
        d2dContext_->FillEllipse(D2D1::Ellipse(point, 4.5f, 4.5f), brush_.Get());
    }

    static constexpr std::wstring_view kBandCaption = L"sombras        medios        altas luces";
    brush_->SetColor(Rgba(1, 1, 1, 0.45f));
    d2dContext_->DrawTextW(kBandCaption.data(), static_cast<UINT32>(kBandCaption.size()),
                           hintTextFormat_.Get(),
                           D2D1::RectF(toneGraphRect_.left, toneGraphRect_.bottom + 2.0f,
                                       toneGraphRect_.right, toneGraphRect_.bottom + 22.0f),
                           brush_.Get());

    // --- Restablecer --------------------------------------------------------
    resetRect_ = D2D1::RectF(left + kPanelPadding, bottom - kPanelPadding - 26.0f,
                             right - kPanelPadding, bottom - kPanelPadding);

    const bool neutral = model_.adjustments.IsNeutral();
    brush_->SetColor(Rgba(1, 1, 1, neutral ? 0.06f : 0.14f));
    d2dContext_->FillRoundedRectangle(D2D1::RoundedRect(resetRect_, 6.0f, 6.0f),
                                      brush_.Get());

    static constexpr std::wstring_view kReset = L"Restablecer";
    brush_->SetColor(Rgba(1, 1, 1, neutral ? 0.35f : 0.88f));
    d2dContext_->DrawTextW(kReset.data(), static_cast<UINT32>(kReset.size()),
                           controlFormat_.Get(), resetRect_, brush_.Get());
}

void Overlay::DrawSpeedMenu() {
    const auto count = static_cast<float>(kPlaybackRates.size());
    const float menuHeight = count * kSpeedMenuItemH + 8.0f;
    const float menuWidth  = kSpeedWidth + 26.0f;

    // El menu crece HACIA ARRIBA desde el indicador: hacia abajo se saldria de
    // la ventana, porque la barra ya esta pegada al borde inferior.
    const float left = speedRect_.right - menuWidth;
    const float top  = speedRect_.top - menuHeight - 8.0f;

    speedMenuRect_       = D2D1::RectF(left, top, left + menuWidth, top + menuHeight);
    speedMenuItemHeight_ = kSpeedMenuItemH;

    brush_->SetColor(Rgba(0.05f, 0.07f, 0.10f, 0.94f));
    d2dContext_->FillRoundedRectangle(D2D1::RoundedRect(speedMenuRect_, 8.0f, 8.0f),
                                      brush_.Get());
    brush_->SetColor(Rgba(1, 1, 1, 0.14f));
    d2dContext_->DrawRoundedRectangle(D2D1::RoundedRect(speedMenuRect_, 8.0f, 8.0f),
                                      brush_.Get(), 1.0f);

    const std::size_t current = IndexForRate(model_.rateMilli);

    for (std::size_t i = 0; i < kPlaybackRates.size(); ++i) {
        const float itemTop = top + 4.0f + static_cast<float>(i) * kSpeedMenuItemH;
        const D2D1_RECT_F item = D2D1::RectF(left + 4.0f, itemTop,
                                             left + menuWidth - 4.0f,
                                             itemTop + kSpeedMenuItemH);

        if (static_cast<int>(i) == model_.speedMenuHighlight) {
            brush_->SetColor(Rgba(1, 1, 1, 0.10f));
            d2dContext_->FillRoundedRectangle(D2D1::RoundedRect(item, 5.0f, 5.0f),
                                              brush_.Get());
        }

        brush_->SetColor(i == current ? Rgba(0.45f, 0.80f, 1.0f, 0.98f)
                                      : Rgba(1, 1, 1, 0.82f));
        d2dContext_->DrawTextW(kSpeedLabels[i].c_str(),
                               static_cast<UINT32>(kSpeedLabels[i].size()),
                               controlFormat_.Get(), item, brush_.Get());
    }
}

// ---------------------------------------------------------------------------
//  Pantalla de bienvenida
//
//  Sin medio abierto la ventana era negra y muda. Quien abre el programa por
//  primera vez no tiene forma de saber que acepta archivos arrastrados, ni que
//  existe un dialogo, ni que hay atajos. Este recuadro resuelve las tres cosas
//  de una vez, y ademas es clicable entero: no hace falta acertar en un boton.
// ---------------------------------------------------------------------------
void Overlay::DrawWelcome(unsigned width, unsigned height) {
    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);

    // Fondo: un velo oscuro sobre el negro para que el recuadro no flote en el
    // vacio y se lea como una superficie.
    brush_->SetColor(Rgba(0.04f, 0.05f, 0.07f, 1.0f));
    d2dContext_->FillRectangle(D2D1::RectF(0, 0, w, h), brush_.Get());

    const float boxWidth  = std::min(560.0f, w - 64.0f);
    const float boxHeight = std::min(330.0f, h - 64.0f);
    const float left      = (w - boxWidth) * 0.5f;
    const float top       = (h - boxHeight) * 0.5f;
    const float centerX   = w * 0.5f;

    const D2D1_ROUNDED_RECT box = D2D1::RoundedRect(
        D2D1::RectF(left, top, left + boxWidth, top + boxHeight), 14.0f, 14.0f);

    brush_->SetColor(Rgba(1, 1, 1, 0.035f));
    d2dContext_->FillRoundedRectangle(box, brush_.Get());
    brush_->SetColor(Rgba(1, 1, 1, 0.22f));
    d2dContext_->DrawRoundedRectangle(box, brush_.Get(), 1.6f, dashedStroke_.Get());

    // --- Icono -------------------------------------------------------------
    const D2D1_POINT_2F iconCenter = D2D1::Point2F(centerX, top + 62.0f);

    brush_->SetColor(Rgba(0.35f, 0.72f, 1.0f, 0.22f));
    d2dContext_->FillEllipse(D2D1::Ellipse(iconCenter, 30.0f, 30.0f), brush_.Get());
    brush_->SetColor(Rgba(0.45f, 0.80f, 1.0f, 0.95f));
    FillTriangle(d2dContext_.Get(), triangleGeometry_.Get(), brush_.Get(),
                 D2D1::Point2F(iconCenter.x + 2.0f, iconCenter.y), 1.5f, true);

    // --- Textos ------------------------------------------------------------
    static constexpr std::wstring_view kTitle = L"Arrastra un vídeo aquí";
    brush_->SetColor(Rgba(1, 1, 1, 0.94f));
    d2dContext_->DrawTextW(kTitle.data(), static_cast<UINT32>(kTitle.size()),
                           welcomeFormat_.Get(),
                           D2D1::RectF(left, top + 108.0f, left + boxWidth, top + 148.0f),
                           brush_.Get());

    static constexpr std::wstring_view kSubtitle =
        L"o haz clic en cualquier punto para abrir un archivo";
    brush_->SetColor(Rgba(1, 1, 1, 0.55f));
    d2dContext_->DrawTextW(kSubtitle.data(), static_cast<UINT32>(kSubtitle.size()),
                           welcomeBodyFormat_.Get(),
                           D2D1::RectF(left, top + 150.0f, left + boxWidth, top + 178.0f),
                           brush_.Get());

    // --- Separador y atajos -------------------------------------------------
    const float separatorY = top + 196.0f;
    brush_->SetColor(Rgba(1, 1, 1, 0.12f));
    d2dContext_->DrawLine(D2D1::Point2F(left + 40.0f, separatorY),
                          D2D1::Point2F(left + boxWidth - 40.0f, separatorY),
                          brush_.Get(), 1.0f);

    // Dos columnas: las teclas alineadas a la derecha y su descripcion a la
    // izquierda, de modo que queden a ras sin tabulaciones.
    static constexpr std::wstring_view kKeysLeft  = L"Espacio\nIzq / Der\nF";
    static constexpr std::wstring_view kTextLeft  =
        L"reproducir o pausar\nfotograma a fotograma\npantalla completa";
    static constexpr std::wstring_view kKeysRight = L"Ctrl + rueda\nS\nA / B";
    static constexpr std::wstring_view kTextRight =
        L"ampliar\nguardar el fotograma\nmarcar un recorte";

    const float hintTop    = separatorY + 16.0f;
    const float hintBottom = top + boxHeight - 12.0f;
    const float columnWide = (boxWidth - 80.0f) * 0.5f;
    const float keyWidth   = 76.0f;

    const auto drawColumn = [&](float columnLeft, std::wstring_view keys,
                                std::wstring_view text) {
        brush_->SetColor(Rgba(0.45f, 0.80f, 1.0f, 0.85f));
        d2dContext_->DrawTextW(keys.data(), static_cast<UINT32>(keys.size()),
                               hintKeyFormat_.Get(),
                               D2D1::RectF(columnLeft, hintTop,
                                           columnLeft + keyWidth, hintBottom),
                               brush_.Get());

        brush_->SetColor(Rgba(1, 1, 1, 0.62f));
        d2dContext_->DrawTextW(text.data(), static_cast<UINT32>(text.size()),
                               hintTextFormat_.Get(),
                               D2D1::RectF(columnLeft + keyWidth + 10.0f, hintTop,
                                           columnLeft + columnWide, hintBottom),
                               brush_.Get());
    };

    drawColumn(left + 40.0f, kKeysLeft, kTextLeft);
    drawColumn(left + 40.0f + columnWide, kKeysRight, kTextRight);
}

void Overlay::DrawStats(unsigned width) {
    if (model_.stats.empty()) return;

    const float panelWidth = 460.0f;
    const float left = static_cast<float>(width) - panelWidth - kMargin;

    // Se mide el texto para ajustar el panel en lugar de fijar una altura:
    // el contenido cambia segun el codec y la fuente.
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwriteFactory_->CreateTextLayout(
            model_.stats.c_str(), static_cast<UINT32>(model_.stats.size()),
            statsFormat_.Get(), panelWidth - 24.0f, 1200.0f, &layout))) {
        return;
    }

    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);

    const D2D1_RECT_F panel =
        D2D1::RectF(left, kMargin, left + panelWidth, kMargin + metrics.height + 24.0f);

    brush_->SetColor(Rgba(0, 0, 0, 0.72f));
    d2dContext_->FillRoundedRectangle(D2D1::RoundedRect(panel, 8.0f, 8.0f), brush_.Get());

    brush_->SetColor(Rgba(0.82f, 0.95f, 1.0f, 0.95f));
    d2dContext_->DrawTextLayout(D2D1::Point2F(left + 12.0f, kMargin + 12.0f),
                                layout.Get(), brush_.Get());
}

void Overlay::DrawToast(unsigned width, unsigned height) {
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwriteFactory_->CreateTextLayout(
            model_.toast.c_str(), static_cast<UINT32>(model_.toast.size()),
            titleFormat_.Get(), static_cast<float>(width) - 2 * kMargin, 200.0f, &layout))) {
        return;
    }

    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);

    const float boxWidth  = metrics.widthIncludingTrailingWhitespace + 32.0f;
    const float boxHeight = metrics.height + 20.0f;
    const float left      = (static_cast<float>(width) - boxWidth) * 0.5f;
    const float top       = static_cast<float>(height) * 0.12f;

    brush_->SetColor(Rgba(0, 0, 0, 0.7f));
    d2dContext_->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(left, top, left + boxWidth, top + boxHeight),
                          10.0f, 10.0f),
        brush_.Get());

    brush_->SetColor(Rgba(1, 1, 1, 0.95f));
    d2dContext_->DrawTextLayout(D2D1::Point2F(left + 16.0f, top + 10.0f),
                                layout.Get(), brush_.Get());
}

// ---------------------------------------------------------------------------
//  Composicion
// ---------------------------------------------------------------------------
void Overlay::Render(SwapChain& swapChain) {
    EnsureSurface(swapChain.Width(), swapChain.Height());
    if (!surfaceView_) return;

    const bool hasContent = model_.showWelcome || model_.showControls ||
                            model_.cropEditing || model_.showStats ||
                            !model_.toast.empty();
    if (!hasContent) return;

    if (dirty_) Repaint();

    ID3D11DeviceContext* context = device_->Context();
    ID3D11RenderTargetView* target = swapChain.BackBufferView();
    if (target == nullptr) return;

    Constants constants{};
    constants.outputHdr   = swapChain.IsHdrOutput() ? 1u : 0u;
    constants.uiWhiteNits = kUiWhiteNits;
    constants.opacity     = 1.0f;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(constantBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return;
    }
    std::memcpy(mapped.pData, &constants, sizeof(constants));
    context->Unmap(constantBuffer_.Get(), 0);

    D3D11_VIEWPORT viewport{};
    viewport.Width    = static_cast<float>(swapChain.Width());
    viewport.Height   = static_cast<float>(swapChain.Height());
    viewport.MaxDepth = 1.0f;

    ID3D11ShaderResourceView* views[1]    = {surfaceView_.Get()};
    ID3D11SamplerState*       samplers[1] = {pointSampler_.Get()};
    ID3D11Buffer*             buffers[1]  = {constantBuffer_.Get()};

    context->OMSetRenderTargets(1, &target, nullptr);
    context->OMSetBlendState(alphaBlend_.Get(), nullptr, 0xFFFFFFFF);
    context->RSSetState(rasterizer_.Get());
    context->RSSetViewports(1, &viewport);

    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetInputLayout(nullptr);

    context->VSSetShader(fullscreenShader_.Get(), nullptr, 0);
    context->PSSetShader(compositeShader_.Get(), nullptr, 0);
    context->PSSetShaderResources(0, 1, views);
    context->PSSetSamplers(0, 1, samplers);
    context->PSSetConstantBuffers(0, 1, buffers);

    context->Draw(3, 0);

    ID3D11ShaderResourceView* none[1] = {nullptr};
    context->PSSetShaderResources(0, 1, none);
}

bool Overlay::HitTestPlayPause(int x, int y) const noexcept {
    if (!model_.showControls || model_.showWelcome) return false;
    return Contains(playRect_, static_cast<float>(x), static_cast<float>(y));
}

bool Overlay::HitTestStepBack(int x, int y) const noexcept {
    if (!model_.showControls || model_.showWelcome) return false;
    return Contains(stepBackRect_, static_cast<float>(x), static_cast<float>(y));
}

bool Overlay::HitTestStepForward(int x, int y) const noexcept {
    if (!model_.showControls || model_.showWelcome) return false;
    return Contains(stepForwardRect_, static_cast<float>(x), static_cast<float>(y));
}

bool Overlay::HitTestCropButton(int x, int y) const noexcept {
    if (!model_.showControls || model_.showWelcome) return false;
    return Contains(cropButtonRect_, static_cast<float>(x), static_cast<float>(y));
}

bool Overlay::HitTestFiltersButton(int x, int y) const noexcept {
    if (!model_.showControls || model_.showWelcome) return false;
    return Contains(filtersButtonRect_, static_cast<float>(x), static_cast<float>(y));
}

CropHandle Overlay::HitTestCrop(int x, int y) const noexcept {
    if (!model_.cropEditing || model_.showWelcome) return CropHandle::None;

    const ScreenRect area = CropScreenRect();
    if (!area.Valid()) return CropHandle::None;

    const auto px = static_cast<float>(x);
    const auto py = static_cast<float>(y);

    const bool nearLeft   = std::abs(px - area.left) <= kCropGrab;
    const bool nearRight  = std::abs(px - area.right) <= kCropGrab;
    const bool nearTop    = std::abs(py - area.top) <= kCropGrab;
    const bool nearBottom = std::abs(py - area.bottom) <= kCropGrab;

    const bool insideX = px >= area.left - kCropGrab && px <= area.right + kCropGrab;
    const bool insideY = py >= area.top - kCropGrab && py <= area.bottom + kCropGrab;
    if (!insideX || !insideY) return CropHandle::None;

    // Las esquinas ganan a los lados: cuando el puntero esta en una zona donde
    // ambos aplican, redimensionar en dos ejes es lo que se espera.
    if (nearLeft && nearTop)     return CropHandle::TopLeft;
    if (nearRight && nearTop)    return CropHandle::TopRight;
    if (nearLeft && nearBottom)  return CropHandle::BottomLeft;
    if (nearRight && nearBottom) return CropHandle::BottomRight;

    if (nearLeft)   return CropHandle::Left;
    if (nearRight)  return CropHandle::Right;
    if (nearTop)    return CropHandle::Top;
    if (nearBottom) return CropHandle::Bottom;

    if (px > area.left && px < area.right && py > area.top && py < area.bottom) {
        return CropHandle::Move;
    }
    return CropHandle::None;
}

FilterControl Overlay::HitTestFilters(int x, int y) const noexcept {
    if (!model_.filtersOpen || !model_.showControls || model_.showWelcome) {
        return FilterControl::None;
    }

    const auto px = static_cast<float>(x);
    const auto py = static_cast<float>(y);
    if (!Contains(filtersPanelRect_, px, py)) return FilterControl::None;

    if (Contains(resetRect_, px, py)) return FilterControl::Reset;

    for (std::size_t i = 0; i < kSliderControls.size(); ++i) {
        if (Contains(sliderTracks_[i], px, py)) return kSliderControls[i];
    }

    // Dentro del grafico manda el punto de control mas cercano en horizontal.
    if (Contains(toneGraphRect_, px, py)) {
        const float width = toneGraphRect_.right - toneGraphRect_.left;
        if (width <= 0.0f) return FilterControl::None;

        const float fraction = (px - toneGraphRect_.left) / width;

        std::size_t best = 0;
        float bestDistance = 2.0f;
        for (std::size_t i = 0; i < kBandCenters.size(); ++i) {
            const float distance = std::abs(fraction - kBandCenters[i]);
            if (distance < bestDistance) { bestDistance = distance; best = i; }
        }
        return kBandControls[best];
    }

    // El clic ha caido en el panel pero no sobre un control: se consume igual,
    // para que no se propague al video que hay debajo.
    return FilterControl::None;
}

float Overlay::FilterValueAt(FilterControl control, int x, int y) const noexcept {
    const FilterRange range = FilterRangeFor(control);

    // Las bandas se manipulan en VERTICAL sobre el grafico; los deslizadores en
    // horizontal. Es la diferencia entre "subir las luces" y "mover un mando".
    //
    // El valor no es la altura sin mas, sino la altura DIVIDIDA por el tono de
    // la banda: arrastrar el punto de las sombras hasta el doble de su altura
    // significa ganancia dos, igual que hacerlo con el de las altas luces. Asi
    // el punto sigue al cursor en lugar de desplazarse una fraccion.
    for (std::size_t i = 0; i < kBandControls.size(); ++i) {
        if (kBandControls[i] != control) continue;

        const float height = toneGraphRect_.bottom - toneGraphRect_.top;
        if (height <= 0.0f) return range.minimum;

        const float target =
            std::clamp((toneGraphRect_.bottom - static_cast<float>(y)) / height, 0.0f, 1.0f);

        return std::clamp(target / kBandCenters[i], range.minimum, range.maximum);
    }

    for (std::size_t i = 0; i < kSliderControls.size(); ++i) {
        if (kSliderControls[i] != control) continue;

        const D2D1_RECT_F& track = sliderTracks_[i];
        const float width = track.right - track.left;
        if (width <= 0.0f) return range.minimum;

        const float fraction =
            std::clamp((static_cast<float>(x) - track.left) / width, 0.0f, 1.0f);
        return range.minimum + (range.maximum - range.minimum) * fraction;
    }
    return range.minimum;
}

bool Overlay::HitTestTrim(int x, int y) const noexcept {
    if (!model_.showControls || model_.showWelcome) return false;
    return Contains(trimRect_, static_cast<float>(x), static_cast<float>(y));
}

bool Overlay::HitTestTrimStart(int x, int y) const noexcept {
    if (!model_.showControls || model_.showWelcome) return false;
    return Contains(trimStartHandle_, static_cast<float>(x), static_cast<float>(y));
}

bool Overlay::HitTestTrimEnd(int x, int y) const noexcept {
    if (!model_.showControls || model_.showWelcome) return false;
    return Contains(trimEndHandle_, static_cast<float>(x), static_cast<float>(y));
}

bool Overlay::HitTestSpeed(int x, int y) const noexcept {
    if (!model_.showControls || model_.showWelcome) return false;
    return Contains(speedRect_, static_cast<float>(x), static_cast<float>(y));
}

bool Overlay::HitTestSnapshot(int x, int y) const noexcept {
    if (!model_.showControls || model_.showWelcome) return false;
    return Contains(snapshotRect_, static_cast<float>(x), static_cast<float>(y));
}

int Overlay::HitTestSpeedMenu(int x, int y) const noexcept {
    if (!model_.showControls || !model_.speedMenuOpen) return -1;
    if (speedMenuItemHeight_ <= 0.0f) return -1;

    const auto px = static_cast<float>(x);
    const auto py = static_cast<float>(y);
    if (!Contains(speedMenuRect_, px, py)) return -1;

    const float offset = py - (speedMenuRect_.top + 4.0f);
    if (offset < 0.0f) return -1;

    const auto index = static_cast<int>(offset / speedMenuItemHeight_);
    if (index < 0 || index >= static_cast<int>(kPlaybackRates.size())) return -1;
    return index;
}

Micros Overlay::HitTestSeekBar(int x, int y) const noexcept {
    if (!model_.showControls || model_.showWelcome) return kNoTimestamp;
    if (model_.duration == kNoTimestamp || model_.duration <= 0) return kNoTimestamp;

    // Margen vertical generoso: acertar una barra de 6 px con el raton es
    // frustrante, asi que la zona sensible es mucho mas alta de lo que se ve.
    constexpr float kGrabMargin = 14.0f;
    const auto px = static_cast<float>(x);
    const auto py = static_cast<float>(y);

    if (py < seekBarRect_.top - kGrabMargin || py > seekBarRect_.bottom + kGrabMargin) {
        return kNoTimestamp;
    }
    if (seekBarRect_.right <= seekBarRect_.left) return kNoTimestamp;

    const float clamped = std::clamp(px, seekBarRect_.left, seekBarRect_.right);
    const double ratio = (clamped - seekBarRect_.left) /
                         (seekBarRect_.right - seekBarRect_.left);

    return static_cast<Micros>(ratio * static_cast<double>(model_.duration));
}

void Overlay::Destroy() noexcept {
    if (d2dContext_) d2dContext_->SetTarget(nullptr);
    d2dTarget_.Reset();
    brush_.Reset();
    d2dContext_.Reset();
    d2dDevice_.Reset();
    d2dFactory_.Reset();

    dashedStroke_.Reset();
    triangleGeometry_.Reset();
    panelValueFormat_.Reset();
    panelFormat_.Reset();
    hintTextFormat_.Reset();
    hintKeyFormat_.Reset();
    welcomeBodyFormat_.Reset();
    welcomeFormat_.Reset();
    controlFormat_.Reset();
    statsFormat_.Reset();
    titleFormat_.Reset();
    timeFormat_.Reset();
    dwriteFactory_.Reset();

    constantBuffer_.Reset();
    rasterizer_.Reset();
    alphaBlend_.Reset();
    pointSampler_.Reset();
    compositeShader_.Reset();
    fullscreenShader_.Reset();
    surfaceView_.Reset();
    surface_.Reset();

    device_ = nullptr;
    width_  = 0;
    height_ = 0;
    dirty_  = true;
}

}  // namespace pyxis
