// ============================================================================
//  VideoDecoder.hpp - Decodificacion de video con D3D11VA (zero-copy)
//
//  CONTRATO DE SALIDA
//  ------------------
//  Sea cual sea el codec de entrada y sea por hardware o por software, este
//  decodificador entrega SIEMPRE fotogramas en formato de dos planos:
//
//      8 bits  -> NV12   (luma R8,  croma R8G8 submuestreado 2x2)
//      >8 bits -> P010   (luma R16, croma R16G16, datos en los bits altos)
//
//  Normalizar aqui es una decision deliberada. El renderizador soporta una
//  unica forma de entrada, lo que reduce su shader a un solo camino y elimina
//  la combinatoria de formatos de pixel de FFmpeg (que son mas de cien).
//  El coste recae en la ruta por software, que ya es la lenta de todos modos.
//
//  RUTA POR HARDWARE
//  -----------------
//  El pool de texturas lo asigna FFmpeg, no nosotros, pero le pedimos que lo
//  cree con D3D11_BIND_SHADER_RESOURCE ademas del BIND_DECODER habitual. Ese
//  detalle es lo que permite muestrear la textura decodificada directamente
//  desde el shader, sin pasar por ID3D11VideoProcessor ni por una copia
//  intermedia. Es la diferencia entre reproducir 8K y no poder.
// ============================================================================
#pragma once

#include "media/Demuxer.hpp"
#include "media/Frame.hpp"

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace pyxis {

class VideoDecoder {
public:
    struct Config {
        ID3D11Device*        device        = nullptr;
        ID3D11DeviceContext* context       = nullptr;
        bool                 allowHardware = true;
        // Fotogramas extra en el pool de hardware, por encima de lo que pide el
        // codec. Son los que Pyxis puede tener en vuelo (en la cola de
        // presentacion) sin bloquear al decodificador.
        int                  extraPoolFrames = 8;
    };

    enum class Status {
        Ok,        // se produjo trabajo util
        Again,     // hace falta mas entrada / no hay salida todavia
        EndOfFile, // el decodificador se vacio por completo
        Error,     // fallo irrecuperable de este flujo
    };

    VideoDecoder() = default;
    ~VideoDecoder();

    VideoDecoder(const VideoDecoder&)            = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    void Open(const Demuxer::Track& track, const Config& config);
    void Close() noexcept;

    // Entrega un paquete al decodificador. `packet == nullptr` inicia el
    // vaciado final (necesario para recuperar los fotogramas que el codec
    // retiene por reordenamiento B).
    [[nodiscard]] Status Send(const AVPacket* packet);

    // Recoge un fotograma ya decodificado.
    [[nodiscard]] Status Receive(VideoFrame& out);

    // Descarta el estado interno tras un salto de posicion.
    void Flush() noexcept;

    [[nodiscard]] bool IsHardware() const noexcept { return hardware_; }
    [[nodiscard]] int  Width() const noexcept  { return width_; }
    [[nodiscard]] int  Height() const noexcept { return height_; }
    [[nodiscard]] const std::string& DecoderName() const noexcept { return decoderName_; }

    // Duracion nominal de un fotograma, derivada de la tasa de fotogramas.
    [[nodiscard]] Micros NominalFrameDuration() const noexcept { return frameDuration_; }

private:
    // Callback de negociacion de formato de FFmpeg. Aqui es donde se crea el
    // pool de texturas con el BindFlags adecuado.
    static AVPixelFormat NegotiateFormat(AVCodecContext* context,
                                         const AVPixelFormat* formats) noexcept;

    void CreateHardwareDevice(const Config& config);
    void ConfigureDecodeThreads(bool hardwareAvailable);

    // Convierte un fotograma de software a NV12/P010 para cumplir el contrato.
    [[nodiscard]] bool NormalizeSoftwareFrame(AVFrame* source, VideoFrame& out);

    av::CodecContextPtr codec_;
    av::BufferRefPtr    hwDevice_;
    av::FramePtr        scratch_;      // reutilizado en cada Receive
    av::SwsPtr          converter_;

    AVRational   timeBase_     = AVRational{0, 1};
    Micros       frameDuration_ = 0;
    int          width_        = 0;
    int          height_       = 0;
    bool         hardware_     = false;
    int          extraPool_    = 8;
    std::string  decoderName_;
};

}  // namespace pyxis
