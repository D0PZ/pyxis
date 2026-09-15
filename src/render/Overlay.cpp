#include "render/Overlay.hpp"

#include "core/Error.hpp"
#include "core/Log.hpp"

#include "shaders/fullscreen_vs.h"
#include "shaders/overlay_ps.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace pyxis {
namespace {

// Metricas de la interfaz, en pixeles logicos.
constexpr float kBarHeight     = 92.0f;
constexpr float kMargin        = 24.0f;
constexpr float kSeekBarHeight = 6.0f;
constexpr float kSeekBarY      = 34.0f;   // desde el borde inferior

// Controles a la derecha de la barra de progreso.
constexpr float kSpeedWidth     = 52.0f;
constexpr float kSnapshotWidth  = 34.0f;
constexpr float kControlGap     = 10.0f;
constexpr float kControlHeight  = 28.0f;
constexpr float kControlsWidth  = kSpeedWidth + kControlGap + kSnapshotWidth;
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

[[nodiscard]] bool Contains(const D2D1_RECT_F& rect, float x, float y) noexcept {
    return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
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

    controlFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    controlFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

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

    if (model_.showControls) {
        DrawControlBar(width_, height_);
    }
    // El menu se dibuja despues de la barra para quedar por encima de ella.
    if (model_.showControls && model_.speedMenuOpen) {
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
    d2dContext_->FillRectangle(D2D1::RectF(0, h - kBarHeight, w, h - kBarHeight * 0.5f), brush_.Get());
    brush_->SetColor(Rgba(0, 0, 0, 0.65f));
    d2dContext_->FillRectangle(D2D1::RectF(0, h - kBarHeight * 0.5f, w, h), brush_.Get());

    // --- Barra de progreso -------------------------------------------------
    // Se acorta para dejar sitio a los indicadores de la derecha.
    const float barLeft  = kMargin;
    const float barRight = w - kMargin - kControlsWidth - kControlGap;
    const float barY     = h - kSeekBarY;

    seekBarRect_ = D2D1::RectF(barLeft, barY - kSeekBarHeight * 0.5f,
                               barRight, barY + kSeekBarHeight * 0.5f);

    const D2D1_ROUNDED_RECT track = D2D1::RoundedRect(seekBarRect_, 3.0f, 3.0f);
    brush_->SetColor(Rgba(1, 1, 1, 0.25f));
    d2dContext_->FillRoundedRectangle(track, brush_.Get());

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
        d2dContext_->FillRoundedRectangle(D2D1::RoundedRect(filled, 3.0f, 3.0f), brush_.Get());

        // Cabezal
        brush_->SetColor(Rgba(1, 1, 1, 1));
        d2dContext_->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(filled.right, barY), 7.0f, 7.0f), brush_.Get());
    }

    DrawSpeedControl(w - kMargin, barY);

    // --- Reloj -------------------------------------------------------------
    std::wstring clock = FormatTime(model_.position);
    if (model_.duration != kNoTimestamp) {
        clock += L"  /  " + FormatTime(model_.duration);
    }
    if (model_.paused) clock += L"   PAUSA";
    if (model_.muted)  clock += L"   SIN SONIDO";

    brush_->SetColor(Rgba(1, 1, 1, 0.92f));
    d2dContext_->DrawTextW(clock.c_str(), static_cast<UINT32>(clock.size()),
                           timeFormat_.Get(),
                           D2D1::RectF(kMargin, h - 26.0f, w - kMargin, h - 4.0f),
                           brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);

    // --- Titulo ------------------------------------------------------------
    if (!model_.title.empty()) {
        brush_->SetColor(Rgba(1, 1, 1, 0.88f));
        d2dContext_->DrawTextW(model_.title.c_str(),
                               static_cast<UINT32>(model_.title.size()),
                               titleFormat_.Get(),
                               D2D1::RectF(kMargin, h - kBarHeight + 10.0f,
                                           w - kMargin, h - kSeekBarY - 12.0f),
                               brush_.Get());
    }
}

// ---------------------------------------------------------------------------
//  Indicadores de la derecha
//
//  La velocidad se dibuja como TEXTO, sin marco ni relleno: la intencion es que
//  no parezca un boton. Solo se insinua con un fondo muy tenue cuando el menu
//  esta abierto, para que se entienda de donde sale el desplegable.
// ---------------------------------------------------------------------------
void Overlay::DrawSpeedControl(float right, float centerY) {
    const float snapshotLeft = right - kSnapshotWidth;
    const float speedLeft    = snapshotLeft - kControlGap - kSpeedWidth;

    speedRect_ = D2D1::RectF(speedLeft, centerY - kControlHeight * 0.5f,
                             speedLeft + kSpeedWidth, centerY + kControlHeight * 0.5f);
    snapshotRect_ = D2D1::RectF(snapshotLeft, centerY - kControlHeight * 0.5f,
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

    const bool hasContent =
        model_.showControls || model_.showStats || !model_.toast.empty();
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

bool Overlay::HitTestSpeed(int x, int y) const noexcept {
    if (!model_.showControls) return false;
    return Contains(speedRect_, static_cast<float>(x), static_cast<float>(y));
}

bool Overlay::HitTestSnapshot(int x, int y) const noexcept {
    if (!model_.showControls) return false;
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
    if (!model_.showControls) return kNoTimestamp;
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
