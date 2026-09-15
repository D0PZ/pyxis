#include "media/Demuxer.hpp"

#include "core/Error.hpp"
#include "core/Log.hpp"
#include "core/Text.hpp"

#include <windows.h>

namespace pyxis {
Demuxer::~Demuxer() {
    Close();
}

void Demuxer::Open(const std::wstring& path) {
    Close();
    path_ = path;

    AVFormatContext* raw = ::avformat_alloc_context();
    PYXIS_REQUIRE(raw != nullptr, "avformat_alloc_context: sin memoria");
    format_.reset(raw);

    // Un bufer de E/S grande importa de verdad con material UHD: un flujo 8K
    // HEVC puede pasar de 100 Mbit/s, y el bufer por defecto de 32 KiB obliga
    // a una lectura de disco cada pocos milisegundos.
    format_->flags |= AVFMT_FLAG_GENPTS;   // reconstruye PTS ausentes

    av::DictPtr options;
    AVDictionary* rawOptions = nullptr;

    // Sondeo generoso: los contenedores UHD tardan mas en revelar todas sus
    // pistas, y un sondeo corto hace que se pierdan pistas de audio.
    ::av_dict_set(&rawOptions, "probesize", "16777216", 0);        // 16 MiB
    ::av_dict_set(&rawOptions, "analyzeduration", "10000000", 0);  // 10 s
    // Reconexion automatica para fuentes de red; inofensivo en archivos locales.
    ::av_dict_set(&rawOptions, "reconnect", "1", 0);
    ::av_dict_set(&rawOptions, "reconnect_streamed", "1", 0);
    ::av_dict_set(&rawOptions, "reconnect_delay_max", "5", 0);
    options.reset(rawOptions);

    // FFmpeg espera rutas en UTF-8 incluso en Windows; su protocolo `file`
    // las reconvierte a UTF-16 internamente.
    const std::string utf8Path = ToUtf8(path);

    AVFormatContext* opening = format_.release();
    AVDictionary*    passed  = options.release();
    const int rc = ::avformat_open_input(&opening, utf8Path.c_str(), nullptr, &passed);
    options.reset(passed);   // FFmpeg deja aqui las opciones no reconocidas

    if (rc < 0) {
        // avformat_open_input ya libero el contexto en caso de fallo.
        format_.reset();
        ThrowAv(rc, "no se pudo abrir el medio");
    }
    format_.reset(opening);

    PYXIS_CHECK_AV(::avformat_find_stream_info(format_.get(), nullptr),
                   "no se pudo analizar el contenido del medio");

    containerName_ = format_->iformat != nullptr && format_->iformat->long_name != nullptr
                         ? format_->iformat->long_name
                         : "desconocido";

    duration_ = format_->duration != AV_NOPTS_VALUE
                    ? av::ToMicros(format_->duration, AVRational{1, AV_TIME_BASE})
                    : kNoTimestamp;

    SelectTracks();

    PYXIS_INFO("Medio abierto: contenedor='{}' duracion={} ms video={} audio={}",
               containerName_,
               duration_ == kNoTimestamp ? -1 : duration_ / 1000,
               video_.Valid() ? av::DescribeCodec(video_.params) : "ninguno",
               audio_.Valid() ? av::DescribeCodec(audio_.params) : "ninguno");
}

void Demuxer::SelectTracks() {
    // av_find_best_stream aplica las heuristicas de FFmpeg (pista por defecto,
    // mayor resolucion, mas canales), que son mejores que quedarse con la
    // primera pista que aparezca.
    const int videoIndex =
        ::av_find_best_stream(format_.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int audioIndex =
        ::av_find_best_stream(format_.get(), AVMEDIA_TYPE_AUDIO, -1, videoIndex, nullptr, 0);

    if (videoIndex >= 0) {
        AVStream* stream = format_->streams[videoIndex];
        const AVRational rate = stream->avg_frame_rate.num > 0 ? stream->avg_frame_rate
                                                               : stream->r_frame_rate;
        video_ = Track{videoIndex, stream->time_base, rate, stream->codecpar};

        const AVRational aspect = stream->sample_aspect_ratio.num > 0
                                      ? stream->sample_aspect_ratio
                                      : stream->codecpar->sample_aspect_ratio;
        sampleAspect_ = aspect.num > 0 && aspect.den > 0 ? aspect : AVRational{1, 1};
    }
    if (audioIndex >= 0) {
        AVStream* stream = format_->streams[audioIndex];
        audio_ = Track{audioIndex, stream->time_base, AVRational{0, 1}, stream->codecpar};
    }

    PYXIS_REQUIRE(video_.Valid() || audio_.Valid(),
                  "el archivo no contiene ninguna pista reproducible");

    // Descartar las pistas que no se van a usar evita que av_read_frame gaste
    // tiempo en subtitulos, capitulos y pistas de datos.
    for (unsigned i = 0; i < format_->nb_streams; ++i) {
        const int index = static_cast<int>(i);
        format_->streams[i]->discard =
            (index == video_.index || index == audio_.index) ? AVDISCARD_DEFAULT
                                                             : AVDISCARD_ALL;
    }
}

Demuxer::ReadResult Demuxer::Read(AVPacket* packet) {
    PYXIS_REQUIRE(format_ != nullptr, "Demuxer::Read sin medio abierto");

    const int rc = ::av_read_frame(format_.get(), packet);
    if (rc >= 0) return ReadResult::Ok;

    if (rc == AVERROR_EOF) return ReadResult::EndOfFile;

    // EAGAIN en una fuente no bloqueante no es un error: simplemente aun no
    // hay datos. Conviene reintentar en lugar de abortar la reproduccion.
    if (rc == AVERROR(EAGAIN)) return ReadResult::Recoverable;

    PYXIS_ERROR("av_read_frame fallo: {}", DescribeError(ErrorDomain::FFmpeg, rc));
    return ReadResult::Fatal;
}

void Demuxer::Seek(Micros target, bool backward) {
    PYXIS_REQUIRE(format_ != nullptr, "Demuxer::Seek sin medio abierto");

    if (target < 0) target = 0;

    // Se salta sobre la pista de VIDEO cuando existe, no sobre la linea de
    // tiempo global.
    //
    // La diferencia es real: con el indice global (-1) FFmpeg elige la pista
    // por su cuenta y en un MP4 fragmentado puede aterrizar a mitad de un grupo
    // de imagenes. El decodificador arranca entonces sin sus fotogramas de
    // referencia y escupe "Could not find ref with POC" hasta encontrar el
    // siguiente fotograma clave. Indicando la pista de video, FFmpeg usa su
    // indice de claves y cae en una de verdad.
    int          streamIndex = -1;
    std::int64_t timestamp   = 0;

    if (video_.Valid()) {
        streamIndex = video_.index;
        timestamp   = av::FromMicros(target, video_.timeBase);
    } else {
        timestamp = ::av_rescale_q(target, AVRational{1, static_cast<int>(kMicrosPerSecond)},
                                   AVRational{1, AV_TIME_BASE});
    }

    const int flags = backward ? AVSEEK_FLAG_BACKWARD : 0;
    const int rc = ::av_seek_frame(format_.get(), streamIndex, timestamp, flags);

    if (rc < 0) {
        // Un salto fallido no debe tumbar la reproduccion: el contenedor
        // simplemente sigue donde estaba.
        PYXIS_WARN("el salto a {} ms fallo: {}", target / 1000,
                   DescribeError(ErrorDomain::FFmpeg, rc));
        return;
    }
    PYXIS_DEBUG("Salto a {} ms (hacia atras={})", target / 1000, backward);
}

void Demuxer::Close() noexcept {
    format_.reset();
    video_ = Track{};
    audio_ = Track{};
    duration_ = kNoTimestamp;
    sampleAspect_ = AVRational{1, 1};
    containerName_.clear();
    path_.clear();
}

}  // namespace pyxis
