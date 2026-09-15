// ============================================================================
//  Frame.hpp - Fotogramas de video y audio que circulan por el pipeline
//
//  Un VideoFrame decodificado por hardware NO contiene pixeles: contiene un
//  puntero a una textura de la GPU que sigue siendo propiedad del pool de
//  FFmpeg. Esa es justo la idea de "zero-copy": el fotograma nunca viaja por
//  el bus PCIe hacia la RAM del sistema y de vuelta. A 8K 10 bits eso son
//  ~99 MB por fotograma que no se copian; a 60 fps, ~6 GB/s de ancho de banda
//  que no se gasta.
//
//  La consecuencia practica es que VideoFrame debe mantener viva la
//  referencia al AVFrame mientras la GPU pueda estar leyendo la textura. Por
//  eso es movible pero no copiable, y por eso el pool de fotogramas de
//  hardware se dimensiona con holgura (ver VideoDecoder.cpp).
// ============================================================================
#pragma once

#include "core/Clock.hpp"
#include "media/FFmpegUtil.hpp"

#include <cstdint>
#include <vector>

struct ID3D11Texture2D;

namespace pyxis {

// ---------------------------------------------------------------------------
//  Descripcion de color
//
//  Interpretar mal estos campos es la causa numero uno de que un reproductor
//  se vea "lavado" o "demasiado saturado". Se derivan del flujo y se pasan
//  intactos al shader; nunca se adivinan a medio camino.
// ---------------------------------------------------------------------------
enum class YuvMatrix : std::uint8_t {
    BT601,       // SD heredado
    BT709,       // HD, lo mas comun
    BT2020NCL,   // UHD / HDR
};

enum class TransferFunction : std::uint8_t {
    Sdr,   // BT.1886 / gamma ~2.4, o sRGB
    Pq,    // SMPTE ST 2084, usado por HDR10
    Hlg,   // Hybrid Log-Gamma, usado en emision
};

enum class ColorRange : std::uint8_t {
    Limited,   // "TV": luma 16-235 en 8 bits
    Full,      // "PC": luma 0-255
};

struct ColorInfo {
    YuvMatrix        matrix   = YuvMatrix::BT709;
    TransferFunction transfer = TransferFunction::Sdr;
    ColorRange       range    = ColorRange::Limited;
    int              bitDepth = 8;

    // Metadatos HDR del flujo, en nits. Cero significa "no declarado", en cuyo
    // caso el mapeo de tonos cae a los valores por defecto de HDR10 (1000 nits).
    float maxContentLightLevel = 0.0f;   // MaxCLL
    float maxMasteringLuminance = 0.0f;  // del display de masterizacion

    [[nodiscard]] bool IsHdr() const noexcept {
        return transfer == TransferFunction::Pq || transfer == TransferFunction::Hlg;
    }

    [[nodiscard]] bool operator==(const ColorInfo&) const = default;
};

// Deriva la descripcion de color a partir del fotograma, cayendo a los
// valores que la especificacion marca como implicitos cuando el flujo no los
// declara (que es lo habitual en archivos reales).
[[nodiscard]] ColorInfo DeriveColorInfo(const AVFrame* frame);

// ---------------------------------------------------------------------------
//  VideoFrame
// ---------------------------------------------------------------------------
struct VideoFrame {
    av::FramePtr frame;                  // propietario de los pixeles o de la textura
    Micros       pts      = kNoTimestamp;
    Micros       duration = 0;
    ColorInfo    color{};

    int  width  = 0;   // dimensiones visibles, ya recortadas
    int  height = 0;

    // Generacion de reproduccion en la que se decodifico. Un fotograma que
    // sobreviva a un salto -porque ya estaba en vuelo cuando se pidio- llega
    // con la etiqueta vieja y se descarta sin mostrarse. Sin este sello, ese
    // rezagado se confunde con material valido y hace que el avance manual
    // crea que ya alcanzo su objetivo.
    std::uint32_t generation = 0;

    // Ruta por hardware: la textura pertenece al pool de FFmpeg y es un ARRAY.
    // `arraySlice` indica que capa de ese array contiene este fotograma.
    ID3D11Texture2D* texture    = nullptr;
    std::uint32_t    arraySlice = 0;

    [[nodiscard]] bool IsHardware() const noexcept { return texture != nullptr; }
    [[nodiscard]] bool IsValid() const noexcept { return frame != nullptr; }

    // Movible, no copiable: dos VideoFrame no pueden creerse duenos del mismo
    // AVFrame, y copiar uno por descuido reintroduciria la copia de memoria que
    // todo este diseno existe para evitar.
    VideoFrame() = default;
    VideoFrame(VideoFrame&&) noexcept = default;
    VideoFrame& operator=(VideoFrame&&) noexcept = default;
    VideoFrame(const VideoFrame&) = delete;
    VideoFrame& operator=(const VideoFrame&) = delete;
};

// ---------------------------------------------------------------------------
//  AudioBuffer
//
//  Siempre float32 intercalado: es el formato nativo de WASAPI en modo
//  compartido, asi que convertir una sola vez en el decodificador evita una
//  segunda conversion en el renderizador.
// ---------------------------------------------------------------------------
struct AudioBuffer {
    std::vector<float> samples;      // intercalados
    Micros             pts = kNoTimestamp;
    int                channels   = 0;
    int                sampleRate = 0;

    [[nodiscard]] std::size_t FrameCount() const noexcept {
        return channels > 0 ? samples.size() / static_cast<std::size_t>(channels) : 0;
    }

    [[nodiscard]] Micros Duration() const noexcept {
        if (sampleRate <= 0) return 0;
        return static_cast<Micros>(FrameCount()) * kMicrosPerSecond / sampleRate;
    }
};

}  // namespace pyxis
