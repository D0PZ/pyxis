// ============================================================================
//  AudioDecoder.hpp - Decodificacion de audio y remuestreo al formato del
//  dispositivo
//
//  El decodificador entrega SIEMPRE float32 intercalado, ya remuestreado a la
//  frecuencia y al numero de canales que pide WASAPI. Hacerlo aqui y no en el
//  renderizador tiene dos ventajas:
//
//    1. El hilo de audio de WASAPI es el mas sensible a la latencia de todo el
//       programa; si se pasa de su plazo, se oye un chasquido. Todo trabajo que
//       se le pueda quitar de encima es tiempo de plazo recuperado.
//    2. El remuestreo es la unica operacion de audio costosa, y aqui se ejecuta
//       en un hilo que puede bloquearse sin consecuencias audibles.
// ============================================================================
#pragma once

#include "media/Demuxer.hpp"
#include "media/Frame.hpp"

namespace pyxis {

class AudioDecoder {
public:
    struct Config {
        int sampleRate = 48000;   // el que negocio WASAPI
        int channels   = 2;
    };

    enum class Status {
        Ok,
        Again,
        EndOfFile,
        Error,
    };

    AudioDecoder() = default;
    ~AudioDecoder();

    AudioDecoder(const AudioDecoder&)            = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;

    void Open(const Demuxer::Track& track, const Config& config);
    void Close() noexcept;

    [[nodiscard]] Status Send(const AVPacket* packet);
    [[nodiscard]] Status Receive(AudioBuffer& out);

    void Flush() noexcept;

    [[nodiscard]] int SampleRate() const noexcept { return config_.sampleRate; }
    [[nodiscard]] int Channels() const noexcept { return config_.channels; }
    [[nodiscard]] const std::string& DecoderName() const noexcept { return decoderName_; }

private:
    // (Re)crea el remuestreador cuando el formato de entrada cambia a mitad del
    // flujo, cosa que ocurre de verdad en emisiones y en archivos concatenados.
    void EnsureResampler(const AVFrame* source);

    av::CodecContextPtr codec_;
    av::FramePtr        scratch_;
    av::SwrPtr          resampler_;

    Config         config_{};
    AVRational     timeBase_ = AVRational{0, 1};
    AVChannelLayout inputLayout_{};
    AVSampleFormat inputFormat_ = AV_SAMPLE_FMT_NONE;
    int            inputRate_   = 0;
    std::string    decoderName_;
};

}  // namespace pyxis
