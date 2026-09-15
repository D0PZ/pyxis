// ============================================================================
//  overlay.hlsl - Composicion de la interfaz sobre el video
//
//  Direct2D dibuja la interfaz en una textura BGRA8 aparte y este shader la
//  mezcla encima del fotograma. El rodeo es necesario porque Direct2D no
//  admite R10G10B10A2, que es justo el formato del bufer trasero cuando la
//  salida esta en HDR10. Dibujar en una textura intermedia y componer aqui
//  resuelve el conflicto y, de paso, permite repintar la interfaz solo cuando
//  cambia en lugar de en cada fotograma.
//
//  ALFA PREMULTIPLICADO: es lo que produce Direct2D. Las conversiones de color
//  no son lineales, asi que hay que deshacer la premultiplicacion antes de
//  convertir y rehacerla despues; aplicarlas sobre el color premultiplicado
//  oscurece los bordes suavizados del texto.
// ============================================================================

#include "color.hlsli"

Texture2D<float4> OverlayTexture : register(t0);
SamplerState      PointSampler   : register(s0);

cbuffer OverlayConstants : register(b0) {
    uint  c_outputHdr;      // 0 = salida SDR, 1 = salida HDR10 PQ
    float c_uiWhiteNits;    // brillo del blanco de la interfaz en HDR
    float c_opacity;        // atenuacion global, para el desvanecido
    float c_padding;
};

struct PixelInput {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

float4 PSMain(PixelInput input) : SV_Target {
    float4 source = OverlayTexture.Sample(PointSampler, input.uv);

    source *= c_opacity;

    // Los pixeles totalmente transparentes se descartan antes de cualquier
    // calculo: en la practica son la gran mayoria de la pantalla.
    if (source.a <= 0.0001f) discard;

    if (c_outputHdr != 0u) {
        const float3 straight = source.rgb / source.a;   // deshacer premultiplicado
        const float3 encoded  = SrgbToHdr10(straight, c_uiWhiteNits);
        source.rgb = encoded * source.a;                 // rehacerlo
    }

    return source;
}
