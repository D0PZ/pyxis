// ============================================================================
//  video.hlsl - Conversion YUV -> RGB, funciones de transferencia y HDR
//
//  Este shader recibe los dos planos que produce el decodificador (luma y
//  croma) y entrega pixeles listos para la cadena de intercambio. Todo el
//  trabajo de color ocurre aqui, en la GPU, porque es masivamente paralelo:
//  a 8K son 33 millones de pixeles por fotograma.
//
//  LOS CUATRO CAMINOS
//  ------------------
//      SDR -> SDR    directo. No se linealiza: ir a lineal y volver no aporta
//                    nada y cuesta dos pow() por componente.
//      HDR -> SDR    linealizar, mapear tonos, BT.2020 -> BT.709, recodificar.
//      HDR -> HDR    PQ se pasa tal cual; HLG se reconvierte a PQ.
//      SDR -> HDR    linealizar, BT.709 -> BT.2020, anclar al blanco de
//                    referencia y codificar en PQ.
//
//  La ramificacion es UNIFORME (todos los pixeles del fotograma toman la misma
//  rama porque depende de constantes), asi que la GPU no paga divergencia.
// ============================================================================

#include "color.hlsli"

// ---------------------------------------------------------------------------
//  Entradas
// ---------------------------------------------------------------------------
//  NV12: luma R8_UNORM,  croma R8G8_UNORM
//  P010: luma R16_UNORM, croma R16G16_UNORM
Texture2D<float>  LumaPlane   : register(t0);
Texture2D<float2> ChromaPlane : register(t1);

SamplerState LinearSampler : register(s0);

cbuffer VideoConstants : register(b0) {
    // Matriz combinada: expansion de rango + conversion YUV -> RGB. Se calcula
    // en la CPU una sola vez por cambio de formato y se aplica a (y, u, v, 1),
    // de modo que el desplazamiento de rango limitado sale gratis en el mismo
    // producto matriz-vector.
    //
    // row_major es obligatorio: HLSL empaqueta las matrices de un cbuffer en
    // orden de columnas por defecto, y la CPU la rellena por filas.
    row_major float4x4 c_yuvToRgb;

    // Region de la textura que se muestrea. Cubre dos cosas a la vez:
    //
    //   * El relleno del pool de hardware, que alinea las texturas a multiplos
    //     del macrobloque: una imagen de 1920x1080 suele vivir dentro de una de
    //     1920x1088, y sin corregirlo se muestrearian las 8 filas sobrantes.
    //   * El recorte de encuadre que haya definido el usuario.
    //
    // Se combinan en la CPU, de modo que aqui el recorte no cuesta ni una
    // instruccion: es el mismo producto que ya habia que hacer.
    float2 c_uvScale;
    float2 c_uvOffset;

    uint  c_inputTransfer;   // 0 = SDR, 1 = PQ, 2 = HLG
    uint  c_outputHdr;       // 0 = salida SDR sRGB, 1 = salida HDR10 PQ
    float c_bitScale;        // correccion de P010 (datos en los bits altos)
    float c_sdrWhiteNits;    // nits del blanco de referencia (203 por BT.2408)

    float c_srcPeakNits;     // pico del contenido, de los metadatos de masterizacion
    float c_brightness;      // -1 .. +1   desplazamiento
    float c_exposure;        // -3 .. +3   pasos de diafragma
    float c_contrast;        //  0 .. +2   1 = sin cambio

    float c_saturation;      //  0 .. +2   1 = sin cambio
    float c_gamma;           // 0.3 .. 3   1 = sin cambio
    float c_shadows;         //  0 .. +2   ganancia de las sombras
    float c_midtones;        //  0 .. +2   ganancia de los medios

    float c_highlights;      //  0 .. +2   ganancia de las altas luces
    float3 c_adjustPadding;
};

struct PixelInput {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

// ---------------------------------------------------------------------------
//  Mapeo de tonos HDR -> SDR
//
//  Reinhard extendido sobre la LUMINANCIA, no sobre cada canal por separado.
//  Aplicarlo por canal comprime los componentes de forma desigual y desatura
//  los reflejos hasta dejarlos blancos; operar sobre la luminancia y reescalar
//  el color conserva el tono.
//
//  La entrada esta normalizada de forma que 1.0 es el blanco difuso, y `peak`
//  es el pico del contenido en esas mismas unidades.
// ---------------------------------------------------------------------------
float3 TonemapReinhard(float3 color, float peak) {
    const float luminance = max(dot(color, kLumaBt2020), 1e-6f);

    const float peakSquared = max(peak * peak, 1.0f);
    const float mapped = luminance * (1.0f + luminance / peakSquared) / (1.0f + luminance);

    return color * (mapped / luminance);
}

// ---------------------------------------------------------------------------
//  Bandas tonales
//
//  Tres ganancias -sombras, medios y altas luces- ponderadas por campanas que
//  se solapan. La alternativa habitual seria una curva con puntos de control
//  arbitrarios, pero esto da el mismo resultado practico con tres numeros y sin
//  discontinuidades: las campanas suman aproximadamente uno en todo el rango,
//  asi que subir las tres por igual equivale a subir el brillo, y mover solo
//  una no crea escalones en las zonas vecinas.
// ---------------------------------------------------------------------------
float BandWeight(float luminance, float center, float width) {
    const float distance = (luminance - center) / width;
    return exp(-distance * distance);
}

float3 ApplyToneBands(float3 color) {
    // Si las tres estan a uno no hay nada que hacer. Es el caso normal y la
    // rama es uniforme, asi que la GPU no paga divergencia por comprobarlo.
    if (c_shadows == 1.0f && c_midtones == 1.0f && c_highlights == 1.0f) {
        return color;
    }

    const float luminance = dot(color, kLumaBt709);

    const float weightShadow = BandWeight(luminance, 0.15f, 0.25f);
    const float weightMid    = BandWeight(luminance, 0.50f, 0.25f);
    const float weightHigh   = BandWeight(luminance, 0.85f, 0.25f);

    const float gain = 1.0f + (c_shadows - 1.0f) * weightShadow +
                              (c_midtones - 1.0f) * weightMid +
                              (c_highlights - 1.0f) * weightHigh;

    return color * max(gain, 0.0f);
}

// ---------------------------------------------------------------------------
//  Ajustes de imagen del usuario
//
//  El ORDEN importa y no es arbitrario:
//
//    1. Exposicion, en luz LINEAL. Un paso de diafragma es una duplicacion de
//       la luz que entra, y eso solo es cierto antes de la curva de gamma.
//       Aplicarla sobre el valor codificado aclararia las sombras mucho mas de
//       lo que haria una camara real.
//    2. El resto en dominio de display, que es donde el ojo espera que actuen
//       y donde lo hacen los controles de cualquier televisor.
// ---------------------------------------------------------------------------
float3 ApplyImageAdjustments(float3 color) {
    if (c_exposure != 0.0f) {
        float3 linearColor = SrgbToLinear(color) * exp2(c_exposure);
        color = LinearToSrgb(linearColor);
    }

    color = ApplyToneBands(color);

    color += c_brightness;
    color = (color - 0.5f) * c_contrast + 0.5f;

    const float gray = dot(color, kLumaBt709);
    color = lerp(gray.xxx, color, c_saturation);

    if (c_gamma != 1.0f) {
        color = pow(max(color, 0.0f), 1.0f / c_gamma);
    }

    return color;
}

// ---------------------------------------------------------------------------
//  Shader principal
// ---------------------------------------------------------------------------
float4 PSMain(PixelInput input) : SV_Target {
    const float2 uv = input.uv * c_uvScale + c_uvOffset;

    // El croma de 4:2:0 vive en un plano de media resolucion. El muestreo
    // bilineal con las mismas coordenadas interpola entre las muestras
    // vecinas, que es la aproximacion habitual y visualmente indistinguible de
    // una reconstruccion con posicionamiento exacto en material real.
    const float  luma   = LumaPlane.Sample(LinearSampler, uv).r * c_bitScale;
    const float2 chroma = ChromaPlane.Sample(LinearSampler, uv).rg * c_bitScale;

    // La matriz absorbe el desplazamiento del croma (0.5) y la expansion de
    // rango limitado a completo, de ahi el 1.0 final.
    float3 rgb = mul(c_yuvToRgb, float4(luma, chroma.x, chroma.y, 1.0f)).rgb;

    const bool inputIsHdr  = (c_inputTransfer != 0u);
    const bool outputIsHdr = (c_outputHdr != 0u);

    if (!inputIsHdr && !outputIsHdr) {
        // --- SDR -> SDR: el camino mas transitado y el mas barato. ---------
        rgb = ApplyImageAdjustments(rgb);
        return float4(saturate(rgb), 1.0f);
    }

    // A partir de aqui se trabaja en luz lineal.
    float3 linearColor;
    if (c_inputTransfer == 1u) {
        linearColor = PqToLinear(rgb) * kPqMaxNits;        // nits absolutos
    } else if (c_inputTransfer == 2u) {
        // HLG es relativa a la pantalla. Se ancla a 1000 nits, que es el
        // sistema de referencia que asume BT.2100 para emision.
        linearColor = HlgToLinear(rgb) * 1000.0f;
    } else {
        linearColor = SrgbToLinear(rgb) * c_sdrWhiteNits;  // SDR -> nits
    }

    if (outputIsHdr) {
        // --- Salida HDR10: BT.2020 + PQ ------------------------------------
        if (!inputIsHdr) {
            // El contenido SDR se coloca en el contenedor BT.2020 sin
            // estirarlo. Subir su brillo para "aprovechar" el HDR es
            // exactamente lo que hace que el SDR se vea mal en modo HDR.
            linearColor = mul(kBt709ToBt2020, linearColor);
        }
        linearColor = max(linearColor, 0.0f);
        const float3 encoded = LinearToPq(linearColor / kPqMaxNits);
        return float4(ApplyImageAdjustments(encoded), 1.0f);
    }

    // --- Salida SDR con entrada HDR: mapeo de tonos ------------------------
    const float peakNits = max(c_srcPeakNits, c_sdrWhiteNits);

    float3 normalized = linearColor / c_sdrWhiteNits;      // 1.0 = blanco difuso
    normalized = TonemapReinhard(normalized, peakNits / c_sdrWhiteNits);

    // La gama ancha se reduce despues del mapeo de tonos. Al reves, los colores
    // fuera de gama se recortarian antes de que el mapeo pueda recuperarlos.
    float3 display = mul(kBt2020ToBt709, normalized);
    display = LinearToSrgb(max(display, 0.0f));

    return float4(saturate(ApplyImageAdjustments(display)), 1.0f);
}
