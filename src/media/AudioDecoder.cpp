#include "media/AudioDecoder.hpp"

#include "core/Error.hpp"
#include "core/Log.hpp"

#include <algorithm>
#include <thread>

namespace pyxis {

AudioDecoder::~AudioDecoder() {
    Close();
}

void AudioDecoder::Open(const Demuxer::Track& track, const Config& config) {
    Close();
    PYXIS_REQUIRE(track.Valid(), "AudioDecoder::Open sin pista de audio");
    PYXIS_REQUIRE(config.sampleRate > 0 && config.channels > 0,
                  "formato de audio de destino invalido");

    config_   = config;
    timeBase_ = track.timeBase;

    const AVCodec* decoder = ::avcodec_find_decoder(track.params->codec_id);
    PYXIS_REQUIRE(decoder != nullptr, "el codec de audio del archivo no esta soportado");

    codec_.reset(::avcodec_alloc_context3(decoder));
    PYXIS_REQUIRE(codec_ != nullptr, "avcodec_alloc_context3: sin memoria");

    PYXIS_CHECK_AV(::avcodec_parameters_to_context(codec_.get(), track.params),
                   "no se pudieron aplicar los parametros del codec de audio");

    codec_->pkt_timebase = track.timeBase;

    // El audio se decodifica muy rapido; mas de cuatro hilos no aporta nada y
    // solo anade cambios de contexto que compiten con el video.
    codec_->thread_count = static_cast<int>(
        std::min(4u, std::max(1u, std::thread::hardware_concurrency())));

    PYXIS_CHECK_AV(::avcodec_open2(codec_.get(), decoder, nullptr),
                   "no se pudo abrir el decodificador de audio");

    scratch_     = av::MakeFrame();
    decoderName_ = decoder->name;

    PYXIS_INFO("Decodificador de audio: {} -> {} Hz, {} canales, float32",
               decoderName_, config_.sampleRate, config_.channels);
}

void AudioDecoder::Close() noexcept {
    codec_.reset();
    scratch_.reset();
    resampler_.reset();
    ::av_channel_layout_uninit(&inputLayout_);
    inputFormat_ = AV_SAMPLE_FMT_NONE;
    inputRate_   = 0;
    decoderName_.clear();
}

void AudioDecoder::Flush() noexcept {
    if (codec_) ::avcodec_flush_buffers(codec_.get());
    // El remuestreador guarda muestras de la cola del bloque anterior para
    // interpolar sin discontinuidades. Tras un salto esas muestras pertenecen a
    // otra posicion del medio, asi que hay que descartarlas o se oye un clic.
    resampler_.reset();
    inputFormat_ = AV_SAMPLE_FMT_NONE;
}

void AudioDecoder::EnsureResampler(const AVFrame* source) {
    const auto sourceFormat = static_cast<AVSampleFormat>(source->format);

    const bool unchanged = resampler_ != nullptr &&
                           inputFormat_ == sourceFormat &&
                           inputRate_ == source->sample_rate &&
                           ::av_channel_layout_compare(&inputLayout_, &source->ch_layout) == 0;
    if (unchanged) return;

    AVChannelLayout outputLayout{};
    ::av_channel_layout_default(&outputLayout, config_.channels);

    SwrContext* raw = nullptr;
    const int rc = ::swr_alloc_set_opts2(
        &raw,
        &outputLayout, AV_SAMPLE_FMT_FLT, config_.sampleRate,
        &source->ch_layout, sourceFormat, source->sample_rate,
        0, nullptr);
    ::av_channel_layout_uninit(&outputLayout);

    if (rc < 0) ThrowAv(rc, "no se pudo configurar el remuestreador de audio");
    resampler_.reset(raw);

    PYXIS_CHECK_AV(::swr_init(resampler_.get()),
                   "no se pudo inicializar el remuestreador de audio");

    ::av_channel_layout_uninit(&inputLayout_);
    PYXIS_CHECK_AV(::av_channel_layout_copy(&inputLayout_, &source->ch_layout),
                   "no se pudo copiar la disposicion de canales");
    inputFormat_ = sourceFormat;
    inputRate_   = source->sample_rate;

    PYXIS_DEBUG("Remuestreador: {} Hz {} canales ({}) -> {} Hz {} canales (flt)",
                inputRate_, inputLayout_.nb_channels,
                ::av_get_sample_fmt_name(inputFormat_),
                config_.sampleRate, config_.channels);
}

AudioDecoder::Status AudioDecoder::Send(const AVPacket* packet) {
    PYXIS_REQUIRE(codec_ != nullptr, "AudioDecoder::Send sin decodificador abierto");

    const int rc = ::avcodec_send_packet(codec_.get(), packet);
    if (rc == 0)               return Status::Ok;
    if (rc == AVERROR(EAGAIN)) return Status::Again;
    if (rc == AVERROR_EOF)     return Status::EndOfFile;

    if (rc == AVERROR_INVALIDDATA) {
        PYXIS_DEBUG("paquete de audio descartado por datos invalidos");
        return Status::Ok;
    }

    PYXIS_ERROR("avcodec_send_packet (audio) fallo: {}",
                DescribeError(ErrorDomain::FFmpeg, rc));
    return Status::Error;
}

AudioDecoder::Status AudioDecoder::Receive(AudioBuffer& out) {
    PYXIS_REQUIRE(codec_ != nullptr, "AudioDecoder::Receive sin decodificador abierto");

    const int rc = ::avcodec_receive_frame(codec_.get(), scratch_.get());
    if (rc == AVERROR(EAGAIN)) return Status::Again;
    if (rc == AVERROR_EOF)     return Status::EndOfFile;
    if (rc < 0) {
        PYXIS_ERROR("avcodec_receive_frame (audio) fallo: {}",
                    DescribeError(ErrorDomain::FFmpeg, rc));
        return Status::Error;
    }

    AVFrame* source = scratch_.get();

    try {
        EnsureResampler(source);
    } catch (const Exception& error) {
        PYXIS_ERROR("{}", error.what());
        ::av_frame_unref(source);
        return Status::Error;
    }

    // swr_get_out_samples incluye las muestras que el remuestreador tiene
    // retenidas de bloques anteriores, asi que este tamano nunca se queda corto.
    const int maxOutputFrames = ::swr_get_out_samples(resampler_.get(), source->nb_samples);
    if (maxOutputFrames <= 0) {
        ::av_frame_unref(source);
        return Status::Again;
    }

    out.channels   = config_.channels;
    out.sampleRate = config_.sampleRate;
    out.samples.resize(static_cast<std::size_t>(maxOutputFrames) *
                       static_cast<std::size_t>(config_.channels));

    auto* destination = reinterpret_cast<std::uint8_t*>(out.samples.data());
    const int produced = ::swr_convert(
        resampler_.get(), &destination, maxOutputFrames,
        const_cast<const std::uint8_t**>(source->extended_data), source->nb_samples);

    if (produced < 0) {
        PYXIS_ERROR("swr_convert fallo: {}", DescribeError(ErrorDomain::FFmpeg, produced));
        ::av_frame_unref(source);
        return Status::Error;
    }

    // Se recorta al numero real de muestras producidas: el remuestreador puede
    // devolver menos de lo estimado cuando esta llenando su ventana interna.
    out.samples.resize(static_cast<std::size_t>(produced) *
                       static_cast<std::size_t>(config_.channels));

    const Micros pts = av::ToMicros(
        source->best_effort_timestamp != AV_NOPTS_VALUE ? source->best_effort_timestamp
                                                        : source->pts,
        timeBase_);

    // El remuestreador introduce un retardo: las muestras que salen ahora
    // entraron hace `swr_get_delay` muestras. Descontarlo mantiene la marca de
    // tiempo alineada con el contenido real del bufer, que es lo que ancla el
    // reloj maestro.
    if (pts != kNoTimestamp) {
        const std::int64_t delay =
            ::swr_get_delay(resampler_.get(), config_.sampleRate);
        out.pts = pts - (delay * kMicrosPerSecond) / config_.sampleRate;
    } else {
        out.pts = kNoTimestamp;
    }

    ::av_frame_unref(source);
    return produced > 0 ? Status::Ok : Status::Again;
}

}  // namespace pyxis
