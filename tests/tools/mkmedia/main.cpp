// ============================================================================
//  mkmedia - Generador de material de prueba
//
//  La suite de Pyxis necesita archivos con propiedades concretas: uno con pista
//  de audio para comprobar que la exportacion la copia, y otro etiquetado como
//  HDR de 10 bits para ejercitar el mapeo de tonos. Pedirle al que ejecuta las
//  pruebas que aporte sus propios videos no funciona: cada uno tendria material
//  distinto y los fallos dejarian de ser reproducibles.
//
//  Se generan con las mismas bibliotecas que ya lleva el reproductor, asi que no
//  hace falta instalar nada. Los archivos son pequenos y se regeneran en
//  segundos; no se versionan.
//
//  Uso:  mkmedia <directorio de salida>
// ============================================================================

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

namespace {

constexpr int kWidth      = 640;
constexpr int kHeight     = 360;
constexpr int kFrameRate  = 30;
constexpr int kSampleRate = 48000;
constexpr int kChannels   = 2;

struct FormatDeleter {
    void operator()(AVFormatContext* p) const noexcept {
        if (p == nullptr) return;
        if (p->pb != nullptr && (p->oformat->flags & AVFMT_NOFILE) == 0) {
            avio_closep(&p->pb);
        }
        avformat_free_context(p);
    }
};
struct CodecDeleter {
    void operator()(AVCodecContext* p) const noexcept { avcodec_free_context(&p); }
};
struct FrameDeleter {
    void operator()(AVFrame* p) const noexcept { av_frame_free(&p); }
};
struct PacketDeleter {
    void operator()(AVPacket* p) const noexcept { av_packet_free(&p); }
};

using FormatPtr = std::unique_ptr<AVFormatContext, FormatDeleter>;
using CodecPtr  = std::unique_ptr<AVCodecContext, CodecDeleter>;
using FramePtr  = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;

void Report(const char* what, int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, buffer, sizeof(buffer));
    std::fprintf(stderr, "  %s: %s\n", what, buffer);
}

// Patron visual: un fondo en degradado y una barra que recorre la imagen, de
// modo que cada fotograma sea distinguible del anterior a simple vista. Con un
// patron estatico no habria forma de ver si el avance manual se mueve.
void DrawPattern(AVFrame* frame, int index, int total, int maxValue) {
    const int shift = maxValue > 255 ? 6 : 0;   // 10 bits van en los bits altos

    for (int y = 0; y < frame->height; ++y) {
        auto* row = frame->data[0] + static_cast<std::ptrdiff_t>(y) * frame->linesize[0];
        for (int x = 0; x < frame->width; ++x) {
            const int value = 16 + (x * 200) / frame->width;
            if (shift == 0) {
                row[x] = static_cast<std::uint8_t>(value);
            } else {
                reinterpret_cast<std::uint16_t*>(row)[x] =
                    static_cast<std::uint16_t>(value << shift);
            }
        }
    }

    // Barra vertical que avanza con el numero de fotograma.
    const int barX = (frame->width - 24) * index / std::max(1, total - 1);
    for (int y = frame->height / 4; y < frame->height * 3 / 4; ++y) {
        auto* row = frame->data[0] + static_cast<std::ptrdiff_t>(y) * frame->linesize[0];
        for (int x = barX; x < barX + 24 && x < frame->width; ++x) {
            if (shift == 0) {
                row[x] = static_cast<std::uint8_t>(maxValue);
            } else {
                reinterpret_cast<std::uint16_t*>(row)[x] =
                    static_cast<std::uint16_t>(maxValue << shift);
            }
        }
    }

    // Croma neutro: el patron es en escala de grises a proposito, para que un
    // fallo de conversion de color salte a la vista como un tinte.
    const int chromaHeight = frame->height / 2;
    const int neutral = (maxValue + 1) / 2;
    for (int plane = 1; plane < 3 && frame->data[plane] != nullptr; ++plane) {
        for (int y = 0; y < chromaHeight; ++y) {
            auto* row =
                frame->data[plane] + static_cast<std::ptrdiff_t>(y) * frame->linesize[plane];
            for (int x = 0; x < frame->width / 2; ++x) {
                if (shift == 0) {
                    row[x] = static_cast<std::uint8_t>(neutral);
                } else {
                    reinterpret_cast<std::uint16_t*>(row)[x] =
                        static_cast<std::uint16_t>(neutral << shift);
                }
            }
        }
    }
}

[[nodiscard]] bool Drain(AVFormatContext* output, AVCodecContext* codec, AVStream* stream,
                         AVPacket* packet) {
    for (;;) {
        const int got = avcodec_receive_packet(codec, packet);
        if (got == AVERROR(EAGAIN) || got == AVERROR_EOF) return true;
        if (got < 0) { Report("recibir paquete", got); return false; }

        av_packet_rescale_ts(packet, codec->time_base, stream->time_base);
        packet->stream_index = stream->index;

        const int written = av_interleaved_write_frame(output, packet);
        av_packet_unref(packet);
        if (written < 0) { Report("escribir", written); return false; }
    }
}

// ---------------------------------------------------------------------------
//  Archivo con video y AUDIO: prueba que la exportacion copia la pista.
// ---------------------------------------------------------------------------
[[nodiscard]] bool WriteAudioVideo(const std::filesystem::path& path, int seconds) {
    const std::string name = path.string();

    AVFormatContext* rawOutput = nullptr;
    int rc = avformat_alloc_output_context2(&rawOutput, nullptr, nullptr, name.c_str());
    if (rc < 0) { Report("crear contexto", rc); return false; }
    FormatPtr output(rawOutput);

    // h264_mf es el codificador de Windows; es el mismo que usa la exportacion,
    // asi que el material de prueba se parece al que produce el programa.
    const AVCodec* videoEncoder = avcodec_find_encoder_by_name("h264_mf");
    if (videoEncoder == nullptr) videoEncoder = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    const AVCodec* audioEncoder = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (videoEncoder == nullptr || audioEncoder == nullptr) {
        std::fprintf(stderr, "  faltan codificadores\n");
        return false;
    }

    CodecPtr video(avcodec_alloc_context3(videoEncoder));
    video->width     = kWidth;
    video->height    = kHeight;
    video->pix_fmt   = AV_PIX_FMT_YUV420P;
    video->time_base = AVRational{1, kFrameRate};
    video->framerate = AVRational{kFrameRate, 1};
    video->bit_rate  = 2'000'000;
    video->gop_size  = kFrameRate;

    CodecPtr audio(avcodec_alloc_context3(audioEncoder));
    audio->sample_fmt  = AV_SAMPLE_FMT_FLTP;
    audio->sample_rate = kSampleRate;
    audio->bit_rate    = 128'000;
    audio->time_base   = AVRational{1, kSampleRate};
    av_channel_layout_default(&audio->ch_layout, kChannels);

    if ((output->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
        video->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        audio->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    rc = avcodec_open2(video.get(), videoEncoder, nullptr);
    if (rc < 0) { Report("abrir video", rc); return false; }
    rc = avcodec_open2(audio.get(), audioEncoder, nullptr);
    if (rc < 0) { Report("abrir audio", rc); return false; }

    AVStream* videoStream = avformat_new_stream(output.get(), nullptr);
    avcodec_parameters_from_context(videoStream->codecpar, video.get());
    videoStream->time_base = video->time_base;

    AVStream* audioStream = avformat_new_stream(output.get(), nullptr);
    avcodec_parameters_from_context(audioStream->codecpar, audio.get());
    audioStream->time_base = audio->time_base;

    rc = avio_open(&output->pb, name.c_str(), AVIO_FLAG_WRITE);
    if (rc < 0) { Report("crear archivo", rc); return false; }
    rc = avformat_write_header(output.get(), nullptr);
    if (rc < 0) { Report("cabecera", rc); return false; }

    FramePtr videoFrame(av_frame_alloc());
    videoFrame->format = video->pix_fmt;
    videoFrame->width  = kWidth;
    videoFrame->height = kHeight;
    av_frame_get_buffer(videoFrame.get(), 32);

    const int samplesPerFrame = audio->frame_size > 0 ? audio->frame_size : 1024;
    FramePtr audioFrame(av_frame_alloc());
    audioFrame->format      = audio->sample_fmt;
    audioFrame->sample_rate = kSampleRate;
    audioFrame->nb_samples  = samplesPerFrame;
    av_channel_layout_copy(&audioFrame->ch_layout, &audio->ch_layout);
    av_frame_get_buffer(audioFrame.get(), 0);

    PacketPtr packet(av_packet_alloc());

    const int totalVideo = seconds * kFrameRate;
    const int totalAudio = (seconds * kSampleRate) / samplesPerFrame;

    int videoIndex = 0;
    int audioIndex = 0;
    double phase = 0.0;

    // Se intercalan por marca de tiempo para que el multiplexor no tenga que
    // almacenar una pista entera esperando a la otra.
    while (videoIndex < totalVideo || audioIndex < totalAudio) {
        const double videoTime = static_cast<double>(videoIndex) / kFrameRate;
        const double audioTime =
            static_cast<double>(audioIndex) * samplesPerFrame / kSampleRate;

        if (videoIndex < totalVideo && (audioIndex >= totalAudio || videoTime <= audioTime)) {
            av_frame_make_writable(videoFrame.get());
            DrawPattern(videoFrame.get(), videoIndex, totalVideo, 235);
            videoFrame->pts = videoIndex++;

            rc = avcodec_send_frame(video.get(), videoFrame.get());
            if (rc < 0) { Report("enviar video", rc); return false; }
            if (!Drain(output.get(), video.get(), videoStream, packet.get())) return false;
        } else {
            av_frame_make_writable(audioFrame.get());

            // Tono de 440 Hz con un pitido mas agudo cada segundo: sirve para
            // comprobar a oido si la imagen y el sonido van juntos.
            const bool beep = (audioIndex * samplesPerFrame / kSampleRate) % 2 == 1;
            const double frequency = beep ? 880.0 : 440.0;

            for (int i = 0; i < samplesPerFrame; ++i) {
                const auto value = static_cast<float>(0.25 * std::sin(phase));
                phase += 2.0 * std::numbers::pi * frequency / kSampleRate;
                if (phase > 2.0 * std::numbers::pi) phase -= 2.0 * std::numbers::pi;

                for (int channel = 0; channel < kChannels; ++channel) {
                    reinterpret_cast<float*>(audioFrame->data[channel])[i] = value;
                }
            }
            audioFrame->pts = static_cast<std::int64_t>(audioIndex++) * samplesPerFrame;

            rc = avcodec_send_frame(audio.get(), audioFrame.get());
            if (rc < 0) { Report("enviar audio", rc); return false; }
            if (!Drain(output.get(), audio.get(), audioStream, packet.get())) return false;
        }
    }

    // El vaciado importa: los codificadores retienen fotogramas, y saltarselo
    // produce un archivo mas corto de lo pedido sin que nada avise.
    avcodec_send_frame(video.get(), nullptr);
    if (!Drain(output.get(), video.get(), videoStream, packet.get())) return false;
    avcodec_send_frame(audio.get(), nullptr);
    if (!Drain(output.get(), audio.get(), audioStream, packet.get())) return false;

    rc = av_write_trailer(output.get());
    if (rc < 0) { Report("cerrar", rc); return false; }
    return true;
}

// ---------------------------------------------------------------------------
//  Archivo de 10 bits etiquetado como HDR (PQ / BT.2020).
//
//  No existe codificador de HEVC en este binario -son los que arrastran
//  patentes-, pero para la prueba da igual: lo que se ejercita es el camino de
//  color, y ese depende de las ETIQUETAS del flujo, no del codec. De paso se
//  recorre el repliegue por software, porque ningun codec sin perdida tiene
//  aceleracion por hardware.
//
//  Se usa ffvhuff y no FFV1, que seria la eleccion obvia: FFV1 dentro de
//  Matroska hace que el decodificador emita "bytestream end mismatching by -7"
//  en cada fotograma. Las imagenes salen bien, pero un archivo de prueba que
//  llena el registro de errores inutiliza la comprobacion de que no hay
//  errores, que es la mitad del valor de la suite. ffvhuff decodifica limpio.
//  El precio es el tamano -unos 16 MB frente a 130 KB-, irrelevante para un
//  archivo local que no se versiona.
// ---------------------------------------------------------------------------
[[nodiscard]] bool WriteHdrTagged(const std::filesystem::path& path, int seconds) {
    const std::string name = path.string();

    AVFormatContext* rawOutput = nullptr;
    int rc = avformat_alloc_output_context2(&rawOutput, nullptr, nullptr, name.c_str());
    if (rc < 0) { Report("crear contexto", rc); return false; }
    FormatPtr output(rawOutput);

    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_FFVHUFF);
    if (encoder == nullptr) { std::fprintf(stderr, "  falta FFV1\n"); return false; }

    CodecPtr video(avcodec_alloc_context3(encoder));
    video->width     = kWidth;
    video->height    = kHeight;
    video->pix_fmt   = AV_PIX_FMT_YUV420P10LE;
    video->time_base = AVRational{1, kFrameRate};
    video->framerate = AVRational{kFrameRate, 1};

    // Las etiquetas son el objeto de la prueba.
    video->color_primaries = AVCOL_PRI_BT2020;
    video->color_trc       = AVCOL_TRC_SMPTE2084;   // PQ
    video->colorspace      = AVCOL_SPC_BT2020_NCL;
    video->color_range     = AVCOL_RANGE_MPEG;


    if ((output->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
        video->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    rc = avcodec_open2(video.get(), encoder, nullptr);
    if (rc < 0) { Report("abrir el codificador", rc); return false; }

    AVStream* stream = avformat_new_stream(output.get(), nullptr);
    avcodec_parameters_from_context(stream->codecpar, video.get());
    stream->time_base = video->time_base;

    rc = avio_open(&output->pb, name.c_str(), AVIO_FLAG_WRITE);
    if (rc < 0) { Report("crear archivo", rc); return false; }
    rc = avformat_write_header(output.get(), nullptr);
    if (rc < 0) { Report("cabecera", rc); return false; }

    FramePtr frame(av_frame_alloc());
    frame->format = video->pix_fmt;
    frame->width  = kWidth;
    frame->height = kHeight;
    av_frame_get_buffer(frame.get(), 32);

    PacketPtr packet(av_packet_alloc());
    const int total = seconds * kFrameRate;

    for (int i = 0; i < total; ++i) {
        av_frame_make_writable(frame.get());
        DrawPattern(frame.get(), i, total, 1023);
        frame->pts = i;

        rc = avcodec_send_frame(video.get(), frame.get());
        if (rc < 0) { Report("enviar", rc); return false; }
        if (!Drain(output.get(), video.get(), stream, packet.get())) return false;
    }

    avcodec_send_frame(video.get(), nullptr);
    if (!Drain(output.get(), video.get(), stream, packet.get())) return false;

    rc = av_write_trailer(output.get());
    if (rc < 0) { Report("cerrar", rc); return false; }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "uso: mkmedia <directorio de salida>\n");
        return 2;
    }

    av_log_set_level(AV_LOG_ERROR);

    const std::filesystem::path directory(argv[1]);
    std::error_code error;
    std::filesystem::create_directories(directory, error);

    struct Target {
        const char* name;
        bool (*write)(const std::filesystem::path&, int);
        int seconds;
    };

    const Target targets[] = {
        {"av-sync.mp4", &WriteAudioVideo, 5},
        {"hdr-pq.mkv",  &WriteHdrTagged,  3},
    };

    bool ok = true;
    for (const Target& target : targets) {
        const std::filesystem::path path = directory / target.name;
        std::printf("generando %s ... ", target.name);
        std::fflush(stdout);

        if (target.write(path, target.seconds)) {
            std::printf("%ju bytes\n",
                        static_cast<std::uintmax_t>(std::filesystem::file_size(path, error)));
        } else {
            std::printf("FALLO\n");
            ok = false;
        }
    }
    return ok ? 0 : 1;
}
