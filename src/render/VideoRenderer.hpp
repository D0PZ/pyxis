// ============================================================================
//  VideoRenderer.hpp - Dibujado de un fotograma en el bufer trasero
//
//  CACHE DE VISTAS
//  ---------------
//  El pool de D3D11VA es un ARRAY de texturas: todos los fotogramas en vuelo
//  comparten el mismo ID3D11Texture2D y se distinguen por el indice de capa.
//  Para muestrear una capa hace falta una ID3D11ShaderResourceView por capa y
//  por plano.
//
//  Crear esas vistas por fotograma seria una reserva y una validacion del
//  controlador 60 veces por segundo, por dos planos. Como el pool es fijo y
//  pequeno (una docena de capas), las vistas se crean una vez y se cachean.
//  A partir del segundo pase por el pool, dibujar un fotograma no reserva
//  absolutamente nada.
//
//  CAJA NEGRA
//  ----------
//  La relacion de aspecto se respeta ajustando el VIEWPORT, no con matematicas
//  en el shader. Asi las bandas negras las pinta el clear (que la GPU resuelve
//  con una operacion de relleno rapida) y el shader solo se ejecuta sobre los
//  pixeles que realmente contienen imagen.
// ============================================================================
#pragma once

#include "media/Frame.hpp"
#include "render/Device.hpp"
#include "render/SwapChain.hpp"

#include <cstdint>
#include <unordered_map>

namespace pyxis {

// ---------------------------------------------------------------------------
//  Encuadre: zoom y desplazamiento
//
//  El zoom NO se hace en el shader. Se hace agrandando el VIEWPORT, que puede
//  salirse de los limites del destino: el rasterizador recorta lo que sobra y
//  el pixel shader solo se ejecuta sobre lo que queda visible. Ampliar 8x un
//  video 4K no cuesta 16 veces mas trabajo de fragmento, cuesta exactamente el
//  mismo que llenar la ventana, y el muestreo bilineal del escalado sale gratis
//  porque ya estaba ahi.
//
//  zoom = 1.0 significa "ajustado a la ventana", no "tamano original": el
//  tamano original depende de la resolucion del video y del de la pantalla, y
//  se calcula con ZoomForOriginalSize().
// ---------------------------------------------------------------------------
struct ViewTransform {
    float zoom = 1.0f;
    float panX = 0.0f;   // desplazamiento en pixeles de cliente
    float panY = 0.0f;

    [[nodiscard]] bool operator==(const ViewTransform&) const = default;
};

// Ajustes de imagen del usuario. Valores neutros = sin efecto.
struct ImageAdjustments {
    float brightness = 0.0f;   // -1 .. +1
    float contrast   = 1.0f;   //  0 .. +2
    float saturation = 1.0f;   //  0 .. +2
};

class VideoRenderer {
public:
    void Create(Device& device);
    void Destroy() noexcept;

    // Dibuja `frame` sobre el bufer trasero de `swapChain`, con bandas negras
    // si las proporciones no coinciden. `sampleAspect` es la relacion de
    // aspecto del pixel del contenedor (anamorfico).
    void Draw(SwapChain& swapChain, const VideoFrame& frame, AVRational sampleAspect);

    // Limpia el bufer trasero sin dibujar nada (sin medio abierto, o en pausa
    // antes del primer fotograma).
    void Clear(SwapChain& swapChain);

    // Redibuja el fotograma a su RESOLUCION NATIVA en una textura propia y la
    // devuelve. Lo usa la captura de pantalla.
    //
    // No sirve copiar el bufer trasero: eso daria el tamano de la ventana, con
    // las bandas negras, el zoom aplicado y la interfaz encima. Aqui se dibuja
    // limpio, sin encuadre y siempre en SDR, que es lo que espera un PNG.
    [[nodiscard]] ComPtr<ID3D11Texture2D> RenderToTexture(const VideoFrame& frame,
                                                          AVRational sampleAspect);

    void SetAdjustments(const ImageAdjustments& adjustments) noexcept {
        adjustments_ = adjustments;
    }
    [[nodiscard]] const ImageAdjustments& Adjustments() const noexcept { return adjustments_; }

    void SetViewTransform(const ViewTransform& view) noexcept { view_ = view; }
    [[nodiscard]] const ViewTransform& View() const noexcept { return view_; }

    // ---- Geometria del encuadre ------------------------------------------
    //
    //  Publica y estatica a proposito: la interfaz necesita EXACTAMENTE los
    //  mismos calculos para anclar el zoom bajo el raton y para limitar el
    //  desplazamiento. Duplicar esa aritmetica en dos sitios es como se acaba
    //  con un zoom que se desplaza medio pixel por rueda.

    // Rectangulo del video ajustado a la ventana, conservando proporciones.
    [[nodiscard]] static RECT ComputeFitRect(unsigned targetWidth, unsigned targetHeight,
                                             int videoWidth, int videoHeight,
                                             AVRational sampleAspect) noexcept;

    // Aplica zoom y desplazamiento al rectangulo ajustado. Puede devolver un
    // rectangulo mayor que la ventana: es lo que se espera al ampliar.
    [[nodiscard]] static RECT ApplyView(const RECT& fitRect,
                                        unsigned targetWidth, unsigned targetHeight,
                                        const ViewTransform& view) noexcept;

    // Limita el desplazamiento para que no aparezcan franjas vacias cuando la
    // imagen es mayor que la ventana, y lo anula en el eje donde sea menor.
    static void ClampPan(const RECT& fitRect, unsigned targetWidth, unsigned targetHeight,
                         ViewTransform& view) noexcept;

    // Zoom necesario para que un pixel del video ocupe un pixel de pantalla.
    [[nodiscard]] static float ZoomForOriginalSize(const RECT& fitRect,
                                                   int videoWidth,
                                                   AVRational sampleAspect) noexcept;

    // Nits del blanco de referencia SDR. BT.2408 recomienda 203; subirlo hace
    // que el contenido HDR mapeado a SDR salga mas brillante.
    void SetSdrWhiteNits(float nits) noexcept { sdrWhiteNits_ = nits; }

    // Invalida la cache de vistas. Obligatorio al cerrar un medio: las texturas
    // del pool desaparecen y las vistas quedarian colgando.
    void InvalidateViewCache() noexcept;

    // Rectangulo donde se dibujo el ultimo fotograma, en pixeles de cliente.
    // Lo usa la interfaz para situar la superposicion.
    [[nodiscard]] RECT LastVideoRect() const noexcept { return lastVideoRect_; }

private:
    // Disposicion del constant buffer. Debe coincidir EXACTAMENTE con el
    // cbuffer de video.hlsl, incluido el relleno implicito de HLSL.
    struct alignas(16) Constants {
        float         yuvToRgb[16];     // b0, registros 0-3
        float         uvScale[2];       // registro 4 .xy
        float         texelSize[2];     // registro 4 .zw
        std::uint32_t inputTransfer;    // registro 5 .x
        std::uint32_t outputHdr;        // registro 5 .y
        float         bitScale;         // registro 5 .z
        float         sdrWhiteNits;     // registro 5 .w
        float         srcPeakNits;      // registro 6 .x
        float         brightness;       // registro 6 .y
        float         contrast;         // registro 6 .z
        float         saturation;       // registro 6 .w
    };
    static_assert(sizeof(Constants) == 112, "el constant buffer debe casar con video.hlsl");

    struct ViewKey {
        ID3D11Texture2D* texture;
        std::uint32_t    slice;
        std::uint32_t    plane;   // 0 = luma, 1 = croma

        [[nodiscard]] bool operator==(const ViewKey&) const = default;
    };

    struct ViewKeyHash {
        [[nodiscard]] std::size_t operator()(const ViewKey& key) const noexcept {
            // Mezcla estilo splitmix: el puntero ya esta bien distribuido en sus
            // bits altos, y slice/plane son valores muy pequenos.
            std::size_t hash = reinterpret_cast<std::uintptr_t>(key.texture);
            hash ^= static_cast<std::size_t>(key.slice) * 0x9E3779B97F4A7C15ULL;
            hash ^= static_cast<std::size_t>(key.plane) * 0xBF58476D1CE4E5B9ULL;
            return hash;
        }
    };

    // Vistas y geometria de un fotograma, listas para dibujar. Las comparten
    // el dibujado en pantalla y la captura.
    struct BoundFrame {
        ID3D11ShaderResourceView* luma          = nullptr;
        ID3D11ShaderResourceView* chroma        = nullptr;
        unsigned                  textureWidth  = 0;
        unsigned                  textureHeight = 0;
        DXGI_FORMAT               lumaFormat    = DXGI_FORMAT_R8_UNORM;

        [[nodiscard]] bool Valid() const noexcept {
            return luma != nullptr && chroma != nullptr;
        }
    };

    [[nodiscard]] BoundFrame BindFrame(const VideoFrame& frame);
    void IssueDraw(ID3D11RenderTargetView* target, const D3D11_VIEWPORT& viewport,
                   const BoundFrame& bound, const Constants& constants);

    void CreateShaders();
    void CreateSamplerAndBuffer();

    // Vistas para la ruta por hardware (cacheadas).
    [[nodiscard]] ID3D11ShaderResourceView* AcquireView(ID3D11Texture2D* texture,
                                                        std::uint32_t slice,
                                                        std::uint32_t plane,
                                                        DXGI_FORMAT format);

    // Sube un fotograma de software a las texturas dinamicas y devuelve sus
    // vistas. Solo se usa cuando la aceleracion no esta disponible.
    [[nodiscard]] bool UploadSoftwareFrame(const VideoFrame& frame);

    void FillConstants(Constants& out, const VideoFrame& frame,
                       unsigned textureWidth, unsigned textureHeight,
                       bool hdrOutput) const;

    Device* device_ = nullptr;

    ComPtr<ID3D11VertexShader> vertexShader_;
    ComPtr<ID3D11PixelShader>  pixelShader_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11Buffer>       constantBuffer_;
    ComPtr<ID3D11BlendState>   opaqueBlend_;
    ComPtr<ID3D11RasterizerState> rasterizer_;

    // Texturas dinamicas del repliegue por software.
    ComPtr<ID3D11Texture2D>          softwareLuma_;
    ComPtr<ID3D11Texture2D>          softwareChroma_;
    ComPtr<ID3D11ShaderResourceView> softwareLumaView_;
    ComPtr<ID3D11ShaderResourceView> softwareChromaView_;
    int          softwareWidth_  = 0;
    int          softwareHeight_ = 0;
    AVPixelFormat softwareFormat_ = AV_PIX_FMT_NONE;

    std::unordered_map<ViewKey, ComPtr<ID3D11ShaderResourceView>, ViewKeyHash> viewCache_;

    ImageAdjustments adjustments_{};
    ViewTransform    view_{};
    float            sdrWhiteNits_ = 203.0f;   // BT.2408
    RECT             lastVideoRect_{};
};

}  // namespace pyxis
