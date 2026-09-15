#include "render/VideoRenderer.hpp"

#include "core/Error.hpp"
#include "core/Log.hpp"

#include "shaders/fullscreen_vs.h"
#include "shaders/video_ps.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pyxis {
namespace {

// ---------------------------------------------------------------------------
//  Coeficientes de las matrices YUV -> RGB
//
//  Cada norma se define por Kr y Kb (el peso del rojo y del azul en la luma);
//  Kg sale de que los tres sumen 1. Derivar la matriz de aqui en lugar de
//  copiar numeros ya multiplicados hace evidente de donde sale cada valor.
// ---------------------------------------------------------------------------
struct LumaWeights {
    double kr;
    double kb;
};

LumaWeights WeightsFor(YuvMatrix matrix) noexcept {
    switch (matrix) {
        case YuvMatrix::BT601:     return {0.299,  0.114};    // ITU-R BT.601-7
        case YuvMatrix::BT709:     return {0.2126, 0.0722};   // ITU-R BT.709-6
        case YuvMatrix::BT2020NCL: return {0.2627, 0.0593};   // ITU-R BT.2020-2
    }
    return {0.2126, 0.0722};
}

// Construye la matriz 4x4 que convierte (y, u, v, 1) en RGB, incluyendo la
// expansion de rango limitado a completo.
//
// Para profundidad n, el rango limitado ocupa:
//     luma  : 16*2^(n-8) .. 235*2^(n-8)   (219*2^(n-8) codigos utiles)
//     croma : 16*2^(n-8) .. 240*2^(n-8)   (224*2^(n-8) codigos utiles)
//
// El muestreador entrega codigo/(2^n - 1), de modo que los factores de escala
// llevan esa normalizacion al rango 0..1 (luma) y -0.5..0.5 (croma).
void BuildColorMatrix(const ColorInfo& color, float out[16]) noexcept {
    const auto [kr, kb] = WeightsFor(color.matrix);
    const double kg = 1.0 - kr - kb;

    // Coeficientes de la reconstruccion, con el croma ya centrado en cero.
    const double rv = 2.0 * (1.0 - kr);
    const double bu = 2.0 * (1.0 - kb);
    const double gu = -2.0 * kb * (1.0 - kb) / kg;
    const double gv = -2.0 * kr * (1.0 - kr) / kg;

    const int    depth = std::clamp(color.bitDepth, 8, 16);
    const double maxCode = static_cast<double>((1 << depth) - 1);
    const double step    = static_cast<double>(1 << (depth - 8));

    double lumaScale   = 1.0;
    double lumaOffset  = 0.0;
    double chromaScale = 1.0;
    double chromaOffset = -static_cast<double>(1 << (depth - 1)) / maxCode;

    if (color.range == ColorRange::Limited) {
        lumaScale    = maxCode / (219.0 * step);
        lumaOffset   = -16.0 / 219.0;
        chromaScale  = maxCode / (224.0 * step);
        chromaOffset = -128.0 / 224.0;
    }

    // Fila i: rgb[i] = lumaScale*y + a*u + b*v + desplazamiento acumulado.
    const double rows[3][3] = {
        {lumaScale, 0.0,              rv * chromaScale},
        {lumaScale, gu * chromaScale, gv * chromaScale},
        {lumaScale, bu * chromaScale, 0.0             },
    };
    const double chromaSum[3] = {rv, gu + gv, bu};

    for (int row = 0; row < 3; ++row) {
        out[row * 4 + 0] = static_cast<float>(rows[row][0]);
        out[row * 4 + 1] = static_cast<float>(rows[row][1]);
        out[row * 4 + 2] = static_cast<float>(rows[row][2]);
        out[row * 4 + 3] =
            static_cast<float>(lumaOffset + chromaSum[row] * chromaOffset);
    }

    out[12] = 0.0f;
    out[13] = 0.0f;
    out[14] = 0.0f;
    out[15] = 1.0f;
}

// P010 guarda 10 bits en los 6 bits ALTOS de cada palabra de 16. El
// muestreador devuelve codigo16/65535, asi que hay que reescalar a
// codigo10/1023 para que la matriz vea el rango que espera.
float BitScaleFor(const ColorInfo& color, DXGI_FORMAT lumaFormat) noexcept {
    if (lumaFormat != DXGI_FORMAT_R16_UNORM) return 1.0f;

    const int depth = std::clamp(color.bitDepth, 8, 16);
    const float storedMax  = 65535.0f;
    const float usefulMax  = static_cast<float>(((1 << depth) - 1) << (16 - depth));
    return storedMax / usefulMax;
}

std::uint32_t TransferCode(TransferFunction transfer) noexcept {
    switch (transfer) {
        case TransferFunction::Sdr: return 0;
        case TransferFunction::Pq:  return 1;
        case TransferFunction::Hlg: return 2;
    }
    return 0;
}

// Formato de la vista sobre cada plano, en funcion del formato de la textura.
struct PlaneFormats {
    DXGI_FORMAT luma;
    DXGI_FORMAT chroma;
};

PlaneFormats PlaneFormatsFor(DXGI_FORMAT textureFormat) noexcept {
    switch (textureFormat) {
        case DXGI_FORMAT_P010:
        case DXGI_FORMAT_P016:
            return {DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16G16_UNORM};
        case DXGI_FORMAT_NV12:
        default:
            return {DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8G8_UNORM};
    }
}

}  // namespace

// ---------------------------------------------------------------------------
//  Creacion
// ---------------------------------------------------------------------------
void VideoRenderer::Create(Device& device) {
    Destroy();
    device_ = &device;
    CreateShaders();
    CreateSamplerAndBuffer();
}

void VideoRenderer::CreateShaders() {
    ID3D11Device* d3d = device_->Handle();

    PYXIS_CHECK_HR(d3d->CreateVertexShader(g_FullscreenVS, sizeof(g_FullscreenVS),
                                           nullptr, &vertexShader_),
                   "no se pudo crear el vertex shader");

    PYXIS_CHECK_HR(d3d->CreatePixelShader(g_VideoPS, sizeof(g_VideoPS),
                                          nullptr, &pixelShader_),
                   "no se pudo crear el pixel shader de video");
}

void VideoRenderer::CreateSamplerAndBuffer() {
    ID3D11Device* d3d = device_->Handle();

    // Filtrado bilineal con sujecion en los bordes. CLAMP importa: sin el, al
    // escalar, los pixeles del borde muestrearian el lado opuesto de la imagen
    // y apareceria una linea de color equivocado alrededor del video.
    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MinLOD         = 0.0f;
    samplerDesc.MaxLOD         = D3D11_FLOAT32_MAX;

    PYXIS_CHECK_HR(d3d->CreateSamplerState(&samplerDesc, &sampler_),
                   "no se pudo crear el muestreador");

    D3D11_BUFFER_DESC bufferDesc{};
    bufferDesc.ByteWidth      = sizeof(Constants);
    bufferDesc.Usage          = D3D11_USAGE_DYNAMIC;
    bufferDesc.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    PYXIS_CHECK_HR(d3d->CreateBuffer(&bufferDesc, nullptr, &constantBuffer_),
                   "no se pudo crear el constant buffer de video");

    // Sin mezcla: el video es opaco y cubre por completo su rectangulo.
    D3D11_BLEND_DESC blendDesc{};
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    PYXIS_CHECK_HR(d3d->CreateBlendState(&blendDesc, &opaqueBlend_),
                   "no se pudo crear el estado de mezcla");

    // Sin descarte de caras y sin recorte por tijera: el triangulo a pantalla
    // completa ya se limita con el viewport.
    D3D11_RASTERIZER_DESC rasterizerDesc{};
    rasterizerDesc.FillMode        = D3D11_FILL_SOLID;
    rasterizerDesc.CullMode        = D3D11_CULL_NONE;
    rasterizerDesc.DepthClipEnable = TRUE;
    PYXIS_CHECK_HR(d3d->CreateRasterizerState(&rasterizerDesc, &rasterizer_),
                   "no se pudo crear el estado de rasterizacion");
}

// ---------------------------------------------------------------------------
//  Vistas de recurso
// ---------------------------------------------------------------------------
ID3D11ShaderResourceView* VideoRenderer::AcquireView(ID3D11Texture2D* texture,
                                                     std::uint32_t slice,
                                                     std::uint32_t plane,
                                                     DXGI_FORMAT format) {
    const ViewKey key{texture, slice, plane};

    if (const auto found = viewCache_.find(key); found != viewCache_.end()) {
        return found->second.Get();
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC description{};
    description.Format                         = format;
    description.ViewDimension                  = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    description.Texture2DArray.MostDetailedMip = 0;
    description.Texture2DArray.MipLevels       = 1;
    description.Texture2DArray.FirstArraySlice = slice;
    description.Texture2DArray.ArraySize       = 1;

    ComPtr<ID3D11ShaderResourceView> view;
    const HRESULT hr = device_->Handle()->CreateShaderResourceView(texture, &description, &view);
    if (FAILED(hr)) {
        PYXIS_ERROR("no se pudo crear la vista del plano {} (capa {}): {}",
                    plane, slice, DescribeError(ErrorDomain::HResult, hr));
        return nullptr;
    }

    // La cache crece como mucho hasta (capas del pool x 2 planos); no hace
    // falta desalojo, solo invalidarla al cerrar el medio.
    return viewCache_.emplace(key, std::move(view)).first->second.Get();
}

void VideoRenderer::InvalidateViewCache() noexcept {
    viewCache_.clear();
    softwareLumaView_.Reset();
    softwareChromaView_.Reset();
    softwareLuma_.Reset();
    softwareChroma_.Reset();
    softwareWidth_  = 0;
    softwareHeight_ = 0;
    softwareFormat_ = AV_PIX_FMT_NONE;
}

// ---------------------------------------------------------------------------
//  Repliegue por software: subida a texturas dinamicas
// ---------------------------------------------------------------------------
bool VideoRenderer::UploadSoftwareFrame(const VideoFrame& frame) {
    const AVFrame* source = frame.frame.get();
    const auto format = static_cast<AVPixelFormat>(source->format);
    const bool tenBit = (format == AV_PIX_FMT_P010LE || format == AV_PIX_FMT_P016LE);

    const DXGI_FORMAT lumaFormat   = tenBit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
    const DXGI_FORMAT chromaFormat = tenBit ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
    const UINT bytesPerLumaTexel   = tenBit ? 2u : 1u;

    // Las texturas se recrean solo cuando cambia la geometria o el formato, no
    // en cada fotograma.
    if (softwareWidth_ != source->width || softwareHeight_ != source->height ||
        softwareFormat_ != format) {
        softwareLuma_.Reset();
        softwareChroma_.Reset();
        softwareLumaView_.Reset();
        softwareChromaView_.Reset();

        D3D11_TEXTURE2D_DESC description{};
        description.Width          = static_cast<UINT>(source->width);
        description.Height         = static_cast<UINT>(source->height);
        description.MipLevels      = 1;
        description.ArraySize      = 1;
        description.Format         = lumaFormat;
        description.SampleDesc     = {1, 0};
        description.Usage          = D3D11_USAGE_DYNAMIC;
        description.BindFlags      = D3D11_BIND_SHADER_RESOURCE;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        HRESULT hr = device_->Handle()->CreateTexture2D(&description, nullptr, &softwareLuma_);
        if (FAILED(hr)) {
            PYXIS_ERROR("no se pudo crear la textura de luma: {}",
                        DescribeError(ErrorDomain::HResult, hr));
            return false;
        }

        // El croma de 4:2:0 va a media resolucion en ambos ejes.
        description.Width  = static_cast<UINT>((source->width + 1) / 2);
        description.Height = static_cast<UINT>((source->height + 1) / 2);
        description.Format = chromaFormat;

        hr = device_->Handle()->CreateTexture2D(&description, nullptr, &softwareChroma_);
        if (FAILED(hr)) {
            PYXIS_ERROR("no se pudo crear la textura de croma: {}",
                        DescribeError(ErrorDomain::HResult, hr));
            return false;
        }

        PYXIS_CHECK_HR(device_->Handle()->CreateShaderResourceView(
                           softwareLuma_.Get(), nullptr, &softwareLumaView_),
                       "no se pudo crear la vista de luma");
        PYXIS_CHECK_HR(device_->Handle()->CreateShaderResourceView(
                           softwareChroma_.Get(), nullptr, &softwareChromaView_),
                       "no se pudo crear la vista de croma");

        softwareWidth_  = source->width;
        softwareHeight_ = source->height;
        softwareFormat_ = format;
    }

    ID3D11DeviceContext* context = device_->Context();

    // WRITE_DISCARD deja que el controlador entregue un bufer nuevo en lugar de
    // esperar a que la GPU termine con el anterior. Sin el, cada Map
    // sincronizaria CPU y GPU y el rendimiento se desplomaria.
    const auto copyPlane = [&](ID3D11Texture2D* texture, const std::uint8_t* data,
                               int stride, int rows, std::size_t rowBytes) -> bool {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT hr = context->Map(texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) {
            PYXIS_ERROR("no se pudo mapear la textura: {}",
                        DescribeError(ErrorDomain::HResult, hr));
            return false;
        }

        auto* destination = static_cast<std::uint8_t*>(mapped.pData);
        if (mapped.RowPitch == static_cast<UINT>(stride) &&
            static_cast<std::size_t>(stride) == rowBytes) {
            // Caso feliz: las zancadas coinciden y se copia todo de una vez.
            std::memcpy(destination, data, rowBytes * static_cast<std::size_t>(rows));
        } else {
            for (int row = 0; row < rows; ++row) {
                std::memcpy(destination + static_cast<std::size_t>(row) * mapped.RowPitch,
                            data + static_cast<std::size_t>(row) * stride, rowBytes);
            }
        }
        context->Unmap(texture, 0);
        return true;
    };

    const std::size_t lumaRowBytes =
        static_cast<std::size_t>(source->width) * bytesPerLumaTexel;
    const std::size_t chromaRowBytes =
        static_cast<std::size_t>((source->width + 1) / 2) * bytesPerLumaTexel * 2;

    if (!copyPlane(softwareLuma_.Get(), source->data[0], source->linesize[0],
                   source->height, lumaRowBytes)) {
        return false;
    }
    if (!copyPlane(softwareChroma_.Get(), source->data[1], source->linesize[1],
                   (source->height + 1) / 2, chromaRowBytes)) {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
//  Geometria
// ---------------------------------------------------------------------------
RECT VideoRenderer::ComputeFitRect(unsigned targetWidth, unsigned targetHeight,
                                   int videoWidth, int videoHeight,
                                   AVRational sampleAspect) noexcept {
    RECT result{0, 0, static_cast<LONG>(targetWidth), static_cast<LONG>(targetHeight)};
    if (videoWidth <= 0 || videoHeight <= 0 || targetWidth == 0 || targetHeight == 0) {
        return result;
    }

    // El aspecto de pantalla combina las dimensiones en pixeles con el aspecto
    // del pixel. Ignorarlo hace que el material anamorfico (DVD, algunos
    // rodajes) salga estirado o achatado.
    double aspect = static_cast<double>(videoWidth) / static_cast<double>(videoHeight);
    if (sampleAspect.num > 0 && sampleAspect.den > 0) {
        aspect *= static_cast<double>(sampleAspect.num) / static_cast<double>(sampleAspect.den);
    }

    const double targetAspect =
        static_cast<double>(targetWidth) / static_cast<double>(targetHeight);

    double width  = targetWidth;
    double height = targetHeight;

    if (aspect > targetAspect) {
        // El video es mas ancho: bandas arriba y abajo.
        height = targetWidth / aspect;
    } else {
        // El video es mas alto: bandas a los lados.
        width = targetHeight * aspect;
    }

    const auto left = static_cast<LONG>(std::lround((targetWidth - width) * 0.5));
    const auto top  = static_cast<LONG>(std::lround((targetHeight - height) * 0.5));

    result.left   = left;
    result.top    = top;
    result.right  = left + static_cast<LONG>(std::lround(width));
    result.bottom = top + static_cast<LONG>(std::lround(height));
    return result;
}

RECT VideoRenderer::ApplyView(const RECT& fitRect,
                              unsigned targetWidth, unsigned targetHeight,
                              const ViewTransform& view) noexcept {
    const float fitWidth  = static_cast<float>(fitRect.right - fitRect.left);
    const float fitHeight = static_cast<float>(fitRect.bottom - fitRect.top);

    const float width  = fitWidth * view.zoom;
    const float height = fitHeight * view.zoom;

    // El desplazamiento se mide desde el centro de la ventana, no desde la
    // esquina: asi el zoom sin desplazamiento siempre queda centrado, sea cual
    // sea la proporcion del video.
    const float centerX = static_cast<float>(targetWidth) * 0.5f + view.panX;
    const float centerY = static_cast<float>(targetHeight) * 0.5f + view.panY;

    RECT result;
    result.left   = static_cast<LONG>(std::lround(centerX - width * 0.5f));
    result.top    = static_cast<LONG>(std::lround(centerY - height * 0.5f));
    result.right  = result.left + static_cast<LONG>(std::lround(width));
    result.bottom = result.top + static_cast<LONG>(std::lround(height));
    return result;
}

void VideoRenderer::ClampPan(const RECT& fitRect,
                             unsigned targetWidth, unsigned targetHeight,
                             ViewTransform& view) noexcept {
    const float width  = static_cast<float>(fitRect.right - fitRect.left) * view.zoom;
    const float height = static_cast<float>(fitRect.bottom - fitRect.top) * view.zoom;

    // En el eje donde la imagen no llena la ventana, el desplazamiento se anula:
    // dejar que el usuario arrastre una imagen pequena por la pantalla es
    // desconcertante y no sirve para nada.
    const float slackX = (width - static_cast<float>(targetWidth)) * 0.5f;
    const float slackY = (height - static_cast<float>(targetHeight)) * 0.5f;

    view.panX = slackX > 0.0f ? std::clamp(view.panX, -slackX, slackX) : 0.0f;
    view.panY = slackY > 0.0f ? std::clamp(view.panY, -slackY, slackY) : 0.0f;
}

float VideoRenderer::ZoomForOriginalSize(const RECT& fitRect, int videoWidth,
                                         AVRational sampleAspect) noexcept {
    const float fitWidth = static_cast<float>(fitRect.right - fitRect.left);
    if (fitWidth <= 0.0f || videoWidth <= 0) return 1.0f;

    // El tamano "original" es el de PRESENTACION, no el de almacenamiento: en
    // material anamorfico (DVD, cine rodado con lentes anamorficas) los pixeles
    // no son cuadrados y mostrar la anchura almacenada saldria achatado.
    float displayWidth = static_cast<float>(videoWidth);
    if (sampleAspect.num > 0 && sampleAspect.den > 0) {
        displayWidth *= static_cast<float>(sampleAspect.num) /
                        static_cast<float>(sampleAspect.den);
    }

    return displayWidth / fitWidth;
}

void VideoRenderer::FillConstants(Constants& out, const VideoFrame& frame,
                                  unsigned textureWidth, unsigned textureHeight,
                                  bool hdrOutput) const {
    BuildColorMatrix(frame.color, out.yuvToRgb);

    // Porcion util frente al tamano real de la textura del pool.
    out.uvScale[0] = textureWidth > 0
                         ? static_cast<float>(frame.width) / static_cast<float>(textureWidth)
                         : 1.0f;
    out.uvScale[1] = textureHeight > 0
                         ? static_cast<float>(frame.height) / static_cast<float>(textureHeight)
                         : 1.0f;

    out.texelSize[0] = textureWidth > 0 ? 1.0f / static_cast<float>(textureWidth) : 0.0f;
    out.texelSize[1] = textureHeight > 0 ? 1.0f / static_cast<float>(textureHeight) : 0.0f;

    out.inputTransfer = TransferCode(frame.color.transfer);
    out.outputHdr     = hdrOutput ? 1u : 0u;
    out.sdrWhiteNits  = sdrWhiteNits_;

    // Prioridad de los metadatos: MaxCLL describe el fotograma mas brillante
    // real; la luminancia de masterizacion solo la capacidad del monitor de
    // masterizacion. Si no hay ninguno, 1000 nits es el valor por defecto de
    // facto de HDR10.
    float peak = frame.color.maxContentLightLevel;
    if (peak <= 0.0f) peak = frame.color.maxMasteringLuminance;
    if (peak <= 0.0f) peak = 1000.0f;
    out.srcPeakNits = peak;

    out.brightness = adjustments_.brightness;
    out.contrast   = adjustments_.contrast;
    out.saturation = adjustments_.saturation;
}

// ---------------------------------------------------------------------------
//  Dibujado
// ---------------------------------------------------------------------------
void VideoRenderer::Clear(SwapChain& swapChain) {
    ID3D11DeviceContext* context = device_->Context();
    ID3D11RenderTargetView* target = swapChain.BackBufferView();
    if (target == nullptr) return;

    static constexpr float kBlack[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    context->ClearRenderTargetView(target, kBlack);
    lastVideoRect_ = RECT{};
}

void VideoRenderer::Draw(SwapChain& swapChain, const VideoFrame& frame,
                         AVRational sampleAspect) {
    ID3D11DeviceContext* context = device_->Context();
    ID3D11RenderTargetView* target = swapChain.BackBufferView();
    if (target == nullptr || !frame.IsValid()) return;

    // Las bandas negras se pintan siempre: si no, al cambiar de un video 16:9 a
    // uno 4:3 quedarian restos del anterior en los bordes.
    static constexpr float kBlack[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    context->ClearRenderTargetView(target, kBlack);

    ID3D11ShaderResourceView* lumaView   = nullptr;
    ID3D11ShaderResourceView* chromaView = nullptr;
    unsigned textureWidth  = static_cast<unsigned>(frame.width);
    unsigned textureHeight = static_cast<unsigned>(frame.height);
    DXGI_FORMAT lumaFormat = DXGI_FORMAT_R8_UNORM;

    if (frame.IsHardware()) {
        D3D11_TEXTURE2D_DESC description{};
        frame.texture->GetDesc(&description);
        textureWidth  = description.Width;
        textureHeight = description.Height;

        const PlaneFormats formats = PlaneFormatsFor(description.Format);
        lumaFormat = formats.luma;

        lumaView   = AcquireView(frame.texture, frame.arraySlice, 0, formats.luma);
        chromaView = AcquireView(frame.texture, frame.arraySlice, 1, formats.chroma);
    } else {
        if (!UploadSoftwareFrame(frame)) return;
        lumaView   = softwareLumaView_.Get();
        chromaView = softwareChromaView_.Get();
        // Las texturas por software se crean con el tamano exacto, asi que no
        // hay relleno que compensar.
        textureWidth  = static_cast<unsigned>(softwareWidth_);
        textureHeight = static_cast<unsigned>(softwareHeight_);
        lumaFormat = (softwareFormat_ == AV_PIX_FMT_P010LE ||
                      softwareFormat_ == AV_PIX_FMT_P016LE)
                         ? DXGI_FORMAT_R16_UNORM
                         : DXGI_FORMAT_R8_UNORM;
    }

    if (lumaView == nullptr || chromaView == nullptr) return;

    // Constantes
    Constants constants{};
    FillConstants(constants, frame, textureWidth, textureHeight, swapChain.IsHdrOutput());
    constants.bitScale = BitScaleFor(frame.color, lumaFormat);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    PYXIS_CHECK_HR(context->Map(constantBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
                   "no se pudo mapear el constant buffer de video");
    std::memcpy(mapped.pData, &constants, sizeof(constants));
    context->Unmap(constantBuffer_.Get(), 0);

    // Viewport = rectangulo util. Al ampliar puede desbordar la ventana; el
    // rasterizador recorta y el shader solo corre sobre lo visible.
    const RECT fitRect = ComputeFitRect(swapChain.Width(), swapChain.Height(),
                                        frame.width, frame.height, sampleAspect);
    const RECT destination = ApplyView(fitRect, swapChain.Width(), swapChain.Height(), view_);
    lastVideoRect_ = destination;

    D3D11_VIEWPORT viewport{};
    viewport.TopLeftX = static_cast<float>(destination.left);
    viewport.TopLeftY = static_cast<float>(destination.top);
    viewport.Width    = static_cast<float>(destination.right - destination.left);
    viewport.Height   = static_cast<float>(destination.bottom - destination.top);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    ID3D11ShaderResourceView* views[2] = {lumaView, chromaView};
    ID3D11Buffer*             buffers[1] = {constantBuffer_.Get()};
    ID3D11SamplerState*       samplers[1] = {sampler_.Get()};

    context->OMSetRenderTargets(1, &target, nullptr);
    context->OMSetBlendState(opaqueBlend_.Get(), nullptr, 0xFFFFFFFF);
    context->RSSetState(rasterizer_.Get());
    context->RSSetViewports(1, &viewport);

    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetInputLayout(nullptr);   // la geometria sale de SV_VertexID

    context->VSSetShader(vertexShader_.Get(), nullptr, 0);
    context->PSSetShader(pixelShader_.Get(), nullptr, 0);
    context->PSSetShaderResources(0, 2, views);
    context->PSSetSamplers(0, 1, samplers);
    context->PSSetConstantBuffers(0, 1, buffers);

    context->Draw(3, 0);

    // Desenlazar las vistas evita que D3D11 avise al reutilizar la textura como
    // destino de decodificacion en el siguiente fotograma.
    ID3D11ShaderResourceView* none[2] = {nullptr, nullptr};
    context->PSSetShaderResources(0, 2, none);
}

void VideoRenderer::Destroy() noexcept {
    InvalidateViewCache();
    rasterizer_.Reset();
    opaqueBlend_.Reset();
    constantBuffer_.Reset();
    sampler_.Reset();
    pixelShader_.Reset();
    vertexShader_.Reset();
    device_ = nullptr;
}

}  // namespace pyxis
