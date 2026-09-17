#include "render/ClipExporter.hpp"

#include "core/Error.hpp"
#include "core/Log.hpp"
#include "core/Text.hpp"
#include "media/Demuxer.hpp"
#include "media/VideoDecoder.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace pyxis {
namespace {

// Codificadores disponibles. Ambos son envoltorios de Media Foundation: el
// trabajo lo hace el motor de video de la GPU y no hay nada que enlazar.
constexpr const char* kEncoderH264 = "h264_mf";
constexpr const char* kEncoderHevc = "hevc_mf";

// Por encima de esto, los codificadores de H.264 por hardware se niegan o
// producen un archivo que despues nadie puede decodificar por hardware: el
// perfil no llega. Medido con el recorte de un 8K, que sale a 5172 de ancho y
// obliga al reproductor a caer a software.
constexpr int kH264MaxDimension = 4096;

// Base de tiempo de la salida. Noventa mil es la de MPEG y divide exactamente
// las cadencias habituales, incluidas las de 23,976 y 29,97.
constexpr int kOutputTimeBase = 90000;

// Limites de tasa de bits. El suelo evita que un recorte muy pequeno salga
// borroso; el techo, que uno de 8K genere un archivo inmanejable.
constexpr std::int64_t kMinBitrate = 4'000'000;
constexpr std::int64_t kMaxBitrate = 120'000'000;

struct OutputContextDeleter {
    void operator()(AVFormatContext* p) const noexcept {
        if (p == nullptr) return;
        if (p->pb != nullptr && (p->oformat->flags & AVFMT_NOFILE) == 0) {
            ::avio_closep(&p->pb);
        }
        ::avformat_free_context(p);
    }
};
using OutputContextPtr = std::unique_ptr<AVFormatContext, OutputContextDeleter>;

// H.264 y HEVC trabajan con macrobloques, asi que las dimensiones tienen que
// ser pares. Se redondea hacia abajo para no inventar pixeles.
[[nodiscard]] int MakeEven(int value) noexcept {
    return std::max(2, value - (value % 2));
}

// Tasa de bits proporcional al area conservada. Recortar a un cuarto de la
// imagen y mantener la tasa original seria desperdiciar espacio; reducirla a
// ciegas, perder calidad.
[[nodiscard]] std::int64_t EstimateBitrate(const AVFormatContext* input,
                                           double areaRatio) noexcept {
    std::int64_t source = input->bit_rate;
    if (source <= 0) source = 30'000'000;

    const auto scaled = static_cast<std::int64_t>(source * std::clamp(areaRatio, 0.05, 1.0));
    return std::clamp(scaled, kMinBitrate, kMaxBitrate);
}

// Intenta abrir un codificador concreto. Devuelve el contexto listo o nulo.
//
// Se prueba ABRIENDOLO de verdad, no consultando capacidades: los
// codificadores de Media Foundation aceptan o rechazan una combinacion de
// resolucion, cadencia y tasa de bits segun la GPU y la version del
// controlador, y no hay forma fiable de preguntarlo por adelantado. El
// codificador HEVC de esta maquina, por ejemplo, rechaza con
// MF_E_INVALIDMEDIATYPE el mismo tamano que el de H.264 acepta sin problema.
[[nodiscard]] av::CodecContextPtr TryOpenEncoder(const char* name, int width, int height,
                                                 AVRational frameRate, std::int64_t bitrate,
                                                 bool globalHeader) {
    const AVCodec* encoder = ::avcodec_find_encoder_by_name(name);
    if (encoder == nullptr) return nullptr;

    av::CodecContextPtr context(::avcodec_alloc_context3(encoder));
    if (!context) return nullptr;

    context->width     = width;
    context->height    = height;
    context->pix_fmt   = AV_PIX_FMT_NV12;
    context->time_base = AVRational{1, kOutputTimeBase};
    context->framerate = frameRate;
    context->bit_rate  = bitrate;

    // Un fotograma clave cada dos segundos: es lo que permite luego buscar en
    // el recorte sin redecodificarlo entero.
    context->gop_size =
        std::max(1, static_cast<int>(std::lround(2.0 * ::av_q2d(frameRate))));

    if (globalHeader) context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    const int rc = ::avcodec_open2(context.get(), encoder, nullptr);
    if (rc < 0) {
        PYXIS_DEBUG("{} no admite {}x{}: {}", name, width, height,
                    DescribeError(ErrorDomain::FFmpeg, rc));
        return nullptr;
    }
    return context;
}

}  // namespace

ClipExportResult ExportClip(Device& device, const ClipExportRequest& request,
                            const std::function<void(int)>& progress,
                            const std::atomic<bool>& cancel) {
    ClipExportResult result;
    result.path = request.outputPath;

    if (request.end <= request.start) {
        result.error = "el punto final debe ir despues del inicial";
        return result;
    }

    // ---------------------------------------------------------------- Entrada
    Demuxer demuxer;
    try {
        demuxer.Open(request.inputPath);
    } catch (const Exception& error) {
        result.error = error.what();
        return result;
    }
    if (!demuxer.Video().Valid()) {
        result.error = "el original no tiene pista de video";
        return result;
    }

    VideoDecoder decoder;
    try {
        VideoDecoder::Config config;
        config.device        = device.Handle();
        config.context       = device.Context();
        config.allowHardware = true;
        // Un pool pequeno: aqui no hay historial ni cola de presentacion, los
        // fotogramas se consumen segun salen.
        config.extraPoolFrames = 6;

        decoder.Open(demuxer.Video(), config);
    } catch (const Exception& error) {
        result.error = error.what();
        return result;
    }

    // -------------------------------------------------------------- Geometria
    const AVRational sampleAspect = demuxer.SampleAspectRatio();

    int width = static_cast<int>(std::lround(decoder.Width() * request.crop.Width()));
    if (sampleAspect.num > 0 && sampleAspect.den > 0 && sampleAspect.num != sampleAspect.den) {
        width = static_cast<int>(std::lround(static_cast<double>(width) * sampleAspect.num /
                                             sampleAspect.den));
    }
    width = MakeEven(width);
    const int height =
        MakeEven(static_cast<int>(std::lround(decoder.Height() * request.crop.Height())));

    result.width  = width;
    result.height = height;

    const double areaRatio = static_cast<double>(width) * height /
                             std::max(1.0, static_cast<double>(decoder.Width()) *
                                               decoder.Height());

    AVRational frameRate = demuxer.Video().frameRate;
    if (frameRate.num <= 0 || frameRate.den <= 0) frameRate = AVRational{30, 1};

    const std::int64_t bitrate = EstimateBitrate(demuxer.Context(), areaRatio);

    // ------------------------------------------------------------------ Salida
    AVFormatContext* rawOutput = nullptr;
    const std::string utf8Output = ToUtf8(request.outputPath);
    int rc = ::avformat_alloc_output_context2(&rawOutput, nullptr, nullptr,
                                              utf8Output.c_str());
    if (rc < 0 || rawOutput == nullptr) {
        result.error = "no se reconoce el formato de destino";
        return result;
    }
    OutputContextPtr output(rawOutput);
    const bool globalHeader = (output->oformat->flags & AVFMT_GLOBALHEADER) != 0;

    // ------------------------------------------------------------ Codificador
    //
    // H.264 es lo mas compatible y se prefiere siempre que quepa. Por encima de
    // 4096 pixeles se intenta HEVC primero: un H.264 mas grande se reproduce,
    // pero sin aceleracion, que es lo que este reproductor existe para evitar.
    // Si el preferido rechaza la combinacion, se cae al otro antes que fallar.
    const bool oversized = width > kH264MaxDimension || height > kH264MaxDimension;
    const std::array<const char*, 2> candidates =
        oversized ? std::array<const char*, 2>{kEncoderHevc, kEncoderH264}
                  : std::array<const char*, 2>{kEncoderH264, kEncoderHevc};

    av::CodecContextPtr encoderContext;
    const char* encoderName = nullptr;
    for (const char* name : candidates) {
        encoderContext = TryOpenEncoder(name, width, height, frameRate, bitrate,
                                        globalHeader);
        if (encoderContext) { encoderName = name; break; }
    }

    if (!encoderContext) {
        result.error = "ningun codificador de Windows admite " +
                       std::to_string(width) + "x" + std::to_string(height);
        return result;
    }
    if (oversized && encoderName == kEncoderH264) {
        PYXIS_WARN("HEVC rechazo {}x{}; se usa H.264, que el reproductor tendra "
                   "que decodificar por software", width, height);
    }

    AVStream* videoStream = ::avformat_new_stream(output.get(), nullptr);
    if (videoStream == nullptr) {
        result.error = "no se pudo crear la pista de video";
        return result;
    }
    ::avcodec_parameters_from_context(videoStream->codecpar, encoderContext.get());
    videoStream->time_base = encoderContext->time_base;

    // El audio se COPIA. Recodificarlo no aportaria nada: ni el encuadre ni los
    // ajustes de imagen lo tocan.
    int audioSourceIndex = -1;
    int audioTargetIndex = -1;
    if (demuxer.Audio().Valid()) {
        AVStream* audioStream = ::avformat_new_stream(output.get(), nullptr);
        if (audioStream != nullptr &&
            ::avcodec_parameters_copy(audioStream->codecpar, demuxer.Audio().params) >= 0) {
            audioStream->codecpar->codec_tag = 0;
            audioStream->time_base = demuxer.Audio().timeBase;
            audioSourceIndex = demuxer.Audio().index;
            audioTargetIndex = audioStream->index;
        }
    }

    if ((output->oformat->flags & AVFMT_NOFILE) == 0) {
        rc = ::avio_open(&output->pb, utf8Output.c_str(), AVIO_FLAG_WRITE);
        if (rc < 0) {
            result.error = "no se pudo crear el archivo: " +
                           DescribeError(ErrorDomain::FFmpeg, rc);
            return result;
        }
    }

    av::DictPtr options;
    AVDictionary* rawOptions = nullptr;
    ::av_dict_set(&rawOptions, "movflags", "+faststart", 0);
    options.reset(rawOptions);

    AVDictionary* passed = options.release();
    rc = ::avformat_write_header(output.get(), &passed);
    options.reset(passed);
    if (rc < 0) {
        result.error = "no se pudo escribir la cabecera: " +
                       DescribeError(ErrorDomain::FFmpeg, rc);
        return result;
    }

    // ------------------------------------------------- Renderizado y lectura
    VideoRenderer renderer;
    try {
        renderer.Create(device);
    } catch (const Exception& error) {
        result.error = error.what();
        return result;
    }
    renderer.SetCrop(request.crop);
    renderer.SetAdjustments(request.adjustments);

    D3D11_TEXTURE2D_DESC targetDesc{};
    targetDesc.Width      = static_cast<UINT>(width);
    targetDesc.Height     = static_cast<UINT>(height);
    targetDesc.MipLevels  = 1;
    targetDesc.ArraySize  = 1;
    targetDesc.Format     = DXGI_FORMAT_B8G8R8A8_UNORM;
    targetDesc.SampleDesc = {1, 0};
    targetDesc.Usage      = D3D11_USAGE_DEFAULT;
    targetDesc.BindFlags  = D3D11_BIND_RENDER_TARGET;

    ComPtr<ID3D11Texture2D> target;
    if (FAILED(device.Handle()->CreateTexture2D(&targetDesc, nullptr, &target))) {
        result.error = "no se pudo crear el destino de render";
        return result;
    }

    ComPtr<ID3D11RenderTargetView> targetView;
    if (FAILED(device.Handle()->CreateRenderTargetView(target.Get(), nullptr, &targetView))) {
        result.error = "no se pudo crear la vista de destino";
        return result;
    }

    // Textura de lectura, reutilizada en todos los fotogramas. Es la unica copia
    // de la ruta y es inevitable: el codificador de Media Foundation recibe los
    // pixeles por memoria del sistema.
    D3D11_TEXTURE2D_DESC stagingDesc = targetDesc;
    stagingDesc.Usage          = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags      = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device.Handle()->CreateTexture2D(&stagingDesc, nullptr, &staging))) {
        result.error = "no se pudo crear la textura de lectura";
        return result;
    }

    av::SwsPtr converter(::sws_getContext(width, height, AV_PIX_FMT_BGRA,
                                          width, height, AV_PIX_FMT_NV12,
                                          SWS_BILINEAR, nullptr, nullptr, nullptr));
    if (!converter) {
        result.error = "no se pudo crear el conversor de color";
        return result;
    }

    av::FramePtr encoded = av::MakeFrame();
    encoded->format = AV_PIX_FMT_NV12;
    encoded->width  = width;
    encoded->height = height;
    if (::av_frame_get_buffer(encoded.get(), 32) < 0) {
        result.error = "sin memoria para el fotograma de salida";
        return result;
    }

    // ------------------------------------------------------------ Bucle
    demuxer.Seek(request.start, true);

    av::PacketPtr packet   = av::MakePacket();
    av::PacketPtr outPacket = av::MakePacket();

    const Micros span = request.end - request.start;
    int lastPercent = -1;
    bool flushed = false;

    const auto writeEncoded = [&]() -> bool {
        for (;;) {
            const int got = ::avcodec_receive_packet(encoderContext.get(), outPacket.get());
            if (got == AVERROR(EAGAIN) || got == AVERROR_EOF) return true;
            if (got < 0) {
                result.error = "fallo al codificar: " +
                               DescribeError(ErrorDomain::FFmpeg, got);
                return false;
            }

            ::av_packet_rescale_ts(outPacket.get(), encoderContext->time_base,
                                   videoStream->time_base);
            outPacket->stream_index = videoStream->index;

            const int written =
                ::av_interleaved_write_frame(output.get(), outPacket.get());
            ::av_packet_unref(outPacket.get());

            if (written < 0) {
                result.error = "fallo al escribir: " +
                               DescribeError(ErrorDomain::FFmpeg, written);
                return false;
            }
        }
    };

    bool failed = false;

    while (!cancel.load(std::memory_order_relaxed)) {
        ::av_packet_unref(packet.get());
        const Demuxer::ReadResult read = demuxer.Read(packet.get());

        if (read == Demuxer::ReadResult::Recoverable) continue;
        if (read == Demuxer::ReadResult::Fatal) break;
        if (read == Demuxer::ReadResult::EndOfFile) {
            if (decoder.Send(nullptr) == VideoDecoder::Status::Error) break;
            flushed = true;
        }

        // ---- Audio: copia directa -----------------------------------------
        if (!flushed && packet->stream_index == audioSourceIndex && audioTargetIndex >= 0) {
            const Micros position = av::ToMicros(packet->pts, demuxer.Audio().timeBase);
            if (position != kNoTimestamp &&
                (position < request.start || position > request.end)) {
                continue;
            }

            const std::int64_t offset = av::FromMicros(request.start, demuxer.Audio().timeBase);
            if (packet->pts != AV_NOPTS_VALUE) packet->pts -= offset;
            if (packet->dts != AV_NOPTS_VALUE) packet->dts -= offset;

            // Los paquetes anteriores al inicio exacto -el posicionamiento cae
            // en el fotograma clave previo- quedarian con marca negativa.
            if (packet->pts != AV_NOPTS_VALUE && packet->pts < 0) continue;

            ::av_packet_rescale_ts(packet.get(), demuxer.Audio().timeBase,
                                   output->streams[audioTargetIndex]->time_base);
            packet->stream_index = audioTargetIndex;
            packet->pos = -1;
            (void)::av_interleaved_write_frame(output.get(), packet.get());
            continue;
        }

        // ---- Video ---------------------------------------------------------
        if (!flushed) {
            if (packet->stream_index != demuxer.Video().index) continue;
            if (decoder.Send(packet.get()) == VideoDecoder::Status::Error) break;
        }

        for (;;) {
            VideoFrame frame;
            const VideoDecoder::Status status = decoder.Receive(frame);
            if (status != VideoDecoder::Status::Ok) break;

            if (frame.pts == kNoTimestamp) continue;
            if (frame.pts < request.start) continue;
            if (frame.pts > request.end) { flushed = true; break; }

            // Redibujado con encuadre y ajustes, y lectura a memoria.
            renderer.RenderToTarget(targetView.Get(), static_cast<unsigned>(width),
                                    static_cast<unsigned>(height), frame);
            device.Context()->CopyResource(staging.Get(), target.Get());

            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(device.Context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                result.error = "no se pudo leer el fotograma renderizado";
                failed = true;
                break;
            }

            const auto* source = static_cast<const std::uint8_t*>(mapped.pData);
            const int sourceStride = static_cast<int>(mapped.RowPitch);
            ::sws_scale(converter.get(), &source, &sourceStride, 0, height,
                        encoded->data, encoded->linesize);

            device.Context()->Unmap(staging.Get(), 0);

            encoded->pts = ::av_rescale_q(frame.pts - request.start,
                                          AVRational{1, static_cast<int>(kMicrosPerSecond)},
                                          encoderContext->time_base);

            const int sent = ::avcodec_send_frame(encoderContext.get(), encoded.get());
            if (sent < 0) {
                result.error = "fallo al enviar al codificador: " +
                               DescribeError(ErrorDomain::FFmpeg, sent);
                failed = true;
                break;
            }
            if (!writeEncoded()) { failed = true; break; }

            ++result.frames;

            if (span > 0 && progress) {
                const int percent = static_cast<int>(
                    std::clamp((frame.pts - request.start) * 100 / span, Micros{0}, Micros{100}));
                if (percent != lastPercent) {
                    lastPercent = percent;
                    progress(percent);
                }
            }
        }

        if (failed || flushed) break;
    }

    if (!failed) {
        // Vaciado del codificador: retiene fotogramas por reordenamiento.
        (void)::avcodec_send_frame(encoderContext.get(), nullptr);
        if (!writeEncoded()) failed = true;
    }

    if (!failed) {
        rc = ::av_write_trailer(output.get());
        if (rc < 0) {
            result.error = "no se pudo cerrar el archivo: " +
                           DescribeError(ErrorDomain::FFmpeg, rc);
            failed = true;
        }
    }

    // El renderizador tiene vistas sobre las texturas del pool del
    // decodificador; hay que soltarlas antes de que el decodificador muera.
    renderer.Destroy();

    if (cancel.load(std::memory_order_relaxed)) {
        result.error = "cancelado";
        return result;
    }
    if (failed) return result;
    if (result.frames == 0) {
        result.error = "el intervalo no contiene fotogramas";
        return result;
    }

    result.ok = true;
    PYXIS_INFO("Recorte con ediciones guardado en '{}' ({}x{}, {} fotogramas, {})",
               ToUtf8(request.outputPath), width, height, result.frames, encoderName);
    return result;
}

}  // namespace pyxis
