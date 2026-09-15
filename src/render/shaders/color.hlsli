// ============================================================================
//  color.hlsli - Funciones de transferencia y matrices de gama
//
//  Compartido por video.hlsl y overlay.hlsl. La superposicion necesita las
//  mismas conversiones que el video: cuando la salida esta en HDR10, un texto
//  blanco escrito en sRGB sin convertir saldria con el brillo equivocado, que
//  en PQ significa un blanco cegador de 10.000 nits.
// ============================================================================
#ifndef PYXIS_COLOR_HLSLI
#define PYXIS_COLOR_HLSLI

static const float kPqM1 = 0.1593017578125f;   // 2610 / 16384
static const float kPqM2 = 78.84375f;          // 2523 / 32
static const float kPqC1 = 0.8359375f;         // 3424 / 4096
static const float kPqC2 = 18.8515625f;        // 2413 / 128
static const float kPqC3 = 18.6875f;           // 2392 / 128

static const float kPqMaxNits = 10000.0f;

// Coeficientes de luminancia (ITU-R BT.2020-2 tabla 4, ITU-R BT.709-6).
static const float3 kLumaBt2020 = float3(0.2627f, 0.6780f, 0.0593f);
static const float3 kLumaBt709  = float3(0.2126f, 0.7152f, 0.0722f);

// Conversiones de gama en espacio LINEAL, derivadas de las primarias de cada
// norma con punto blanco D65.
static const float3x3 kBt2020ToBt709 = float3x3(
     1.6605f, -0.5876f, -0.0728f,
    -0.1246f,  1.1329f, -0.0083f,
    -0.0182f, -0.1006f,  1.1187f);

static const float3x3 kBt709ToBt2020 = float3x3(
    0.6274f, 0.3293f, 0.0433f,
    0.0691f, 0.9195f, 0.0114f,
    0.0164f, 0.0880f, 0.8956f);

// ---------------------------------------------------------------------------
//  SMPTE ST 2084 (PQ). La forma normalizada devuelve 1.0 para 10.000 nits.
// ---------------------------------------------------------------------------
float3 PqToLinear(float3 encoded) {
    const float3 p   = pow(max(encoded, 0.0f), 1.0f / kPqM2);
    const float3 num = max(p - kPqC1, 0.0f);
    const float3 den = max(kPqC2 - kPqC3 * p, 1e-6f);
    return pow(num / den, 1.0f / kPqM1);
}

float3 LinearToPq(float3 linearValue) {
    const float3 p = pow(max(linearValue, 0.0f), kPqM1);
    return pow((kPqC1 + kPqC2 * p) / (1.0f + kPqC3 * p), kPqM2);
}

// ---------------------------------------------------------------------------
//  ARIB STD-B67 (HLG). Rama cuadratica por debajo de 0.5 y logaritmica encima,
//  tal como define la norma.
// ---------------------------------------------------------------------------
float3 HlgToLinear(float3 encoded) {
    const float a = 0.17883277f;
    const float b = 0.28466892f;
    const float c = 0.55991073f;

    const float3 lower = (encoded * encoded) / 3.0f;
    const float3 upper = (exp((encoded - c) / a) + b) / 12.0f;
    return lerp(lower, upper, step(0.5f, encoded));
}

// ---------------------------------------------------------------------------
//  sRGB, con el tramo lineal cerca del negro que exige la norma. La
//  aproximacion pow(x, 1/2.2) que se ve por todas partes aplasta las sombras.
// ---------------------------------------------------------------------------
float3 LinearToSrgb(float3 linearValue) {
    const float3 clamped = saturate(linearValue);
    const float3 lower   = clamped * 12.92f;
    const float3 upper   = 1.055f * pow(clamped, 1.0f / 2.4f) - 0.055f;
    return lerp(lower, upper, step(0.0031308f, clamped));
}

float3 SrgbToLinear(float3 encoded) {
    const float3 clamped = saturate(encoded);
    const float3 lower   = clamped / 12.92f;
    const float3 upper   = pow((clamped + 0.055f) / 1.055f, 2.4f);
    return lerp(lower, upper, step(0.04045f, clamped));
}

// ---------------------------------------------------------------------------
//  Lleva un color sRGB (el que produce Direct2D) al espacio de salida HDR10.
//  `whiteNits` fija a que brillo corresponde el blanco puro de la interfaz;
//  203 nits es la referencia de BT.2408 y evita que el texto deslumbre.
// ---------------------------------------------------------------------------
float3 SrgbToHdr10(float3 srgb, float whiteNits) {
    float3 linearColor = SrgbToLinear(srgb) * whiteNits;
    linearColor = mul(kBt709ToBt2020, linearColor);
    return LinearToPq(max(linearColor, 0.0f) / kPqMaxNits);
}

#endif  // PYXIS_COLOR_HLSLI
