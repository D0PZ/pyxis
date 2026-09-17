#include "media/Trimmer.hpp"

#include "core/Error.hpp"
#include "core/Log.hpp"
#include "core/Paths.hpp"
#include "core/Text.hpp"
#include "media/FFmpegUtil.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace pyxis {
namespace {

// Cierra el archivo de salida. avio_closep es lo unico que hay que deshacer a
// mano en el contexto de salida; el resto lo libera avformat_free_context.
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

[[nodiscard]] std::wstring FormatStamp(Micros position) {
    const long long ms = (position > 0 ? position : 0) / 1000;

    std::array<wchar_t, 32> buffer{};
    std::swprintf(buffer.data(), buffer.size(), L"%02lld-%02lld-%02lld",
                  ms / 3600000, (ms / 60000) % 60, (ms / 1000) % 60);
    return buffer.data();
}

}  // namespace

std::wstring BuildClipPath(const std::wstring& mediaPath, Micros start, Micros end) {
    const std::filesystem::path source(mediaPath);

    std::wstring stem = source.stem().wstring();
    if (stem.empty()) stem = L"corte";

    std::wstring extension = source.extension().wstring();
    if (extension.empty()) extension = L".mp4";

    std::filesystem::path destination = PyxisOutputFolder(UserFolder::Videos);
    destination /= SanitizeFileName(stem) + L"_corte_" + FormatStamp(start) + L"_a_" +
                   FormatStamp(end) + extension;

    return destination.wstring();
}

TrimResult TrimToFile(const std::wstring& inputPath, const std::wstring& outputPath,
                      Micros start, Micros end) {
    TrimResult result;
    result.path = outputPath;

    if (end <= start) {
        result.error = "el punto final debe ir despues del inicial";
        return result;
    }

    const std::string utf8Input  = ToUtf8(inputPath);
    const std::string utf8Output = ToUtf8(outputPath);

    // --- Entrada -----------------------------------------------------------
    AVFormatContext* rawInput = nullptr;
    int rc = ::avformat_open_input(&rawInput, utf8Input.c_str(), nullptr, nullptr);
    if (rc < 0) {
        result.error = "no se pudo abrir el original: " +
                       DescribeError(ErrorDomain::FFmpeg, rc);
        return result;
    }
    av::FormatContextPtr input(rawInput);

    rc = ::avformat_find_stream_info(input.get(), nullptr);
    if (rc < 0) {
        result.error = "no se pudo analizar el original: " +
                       DescribeError(ErrorDomain::FFmpeg, rc);
        return result;
    }

    // --- Salida ------------------------------------------------------------
    AVFormatContext* rawOutput = nullptr;
    rc = ::avformat_alloc_output_context2(&rawOutput, nullptr, nullptr, utf8Output.c_str());
    if (rc < 0 || rawOutput == nullptr) {
        result.error = "no se reconoce el formato de destino: " +
                       DescribeError(ErrorDomain::FFmpeg, rc);
        return result;
    }
    OutputContextPtr output(rawOutput);

    // --- Correspondencia de pistas -----------------------------------------
    // Solo video y audio. Los subtitulos y las pistas de datos se descartan: en
    // copia de flujo arrastran problemas de compatibilidad entre contenedores y
    // no es lo que se pide de una herramienta de recorte.
    std::vector<int> streamMap(input->nb_streams, -1);

    for (unsigned i = 0; i < input->nb_streams; ++i) {
        AVStream* source = input->streams[i];
        const AVMediaType type = source->codecpar->codec_type;

        const bool wanted = (type == AVMEDIA_TYPE_VIDEO || type == AVMEDIA_TYPE_AUDIO) &&
                            (source->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0;
        if (!wanted) continue;

        AVStream* destination = ::avformat_new_stream(output.get(), nullptr);
        if (destination == nullptr) {
            result.error = "no se pudo crear la pista de destino";
            return result;
        }

        rc = ::avcodec_parameters_copy(destination->codecpar, source->codecpar);
        if (rc < 0) {
            result.error = "no se pudieron copiar los parametros del codec: " +
                           DescribeError(ErrorDomain::FFmpeg, rc);
            return result;
        }

        // El identificador de codec del contenedor de origen puede no valer en
        // el de destino; a cero, el multiplexor elige el correcto.
        destination->codecpar->codec_tag = 0;
        destination->time_base = source->time_base;

        streamMap[i] = destination->index;
    }

    if (output->nb_streams == 0) {
        result.error = "el original no tiene pistas que se puedan copiar";
        return result;
    }

    if ((output->oformat->flags & AVFMT_NOFILE) == 0) {
        rc = ::avio_open(&output->pb, utf8Output.c_str(), AVIO_FLAG_WRITE);
        if (rc < 0) {
            result.error = "no se pudo crear el archivo: " +
                           DescribeError(ErrorDomain::FFmpeg, rc);
            return result;
        }
    }

    // faststart coloca el indice al principio del MP4, para que el recorte se
    // pueda abrir en streaming sin descargarlo entero. Cuesta una pasada extra
    // al cerrar; en un recorte es despreciable.
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

    // --- Posicionamiento ----------------------------------------------------
    // Hacia atras: se aterriza en el fotograma clave anterior o igual al punto
    // pedido, que es el unico sitio donde un decodificador puede arrancar.
    const std::int64_t seekTarget =
        ::av_rescale_q(start, AVRational{1, static_cast<int>(kMicrosPerSecond)},
                       AVRational{1, AV_TIME_BASE});
    rc = ::av_seek_frame(input.get(), -1, seekTarget, AVSEEK_FLAG_BACKWARD);
    if (rc < 0) {
        PYXIS_WARN("el posicionamiento del recorte fallo: {}",
                   DescribeError(ErrorDomain::FFmpeg, rc));
    }

    // --- Copia de paquetes --------------------------------------------------
    av::PacketPtr packet = av::MakePacket();

    // Desplazamiento comun a todas las pistas, en la base de tiempo global. Uno
    // por pista romperia la sincronia entre imagen y sonido.
    std::int64_t offset = AV_NOPTS_VALUE;
    std::vector<bool> finished(input->nb_streams, false);
    std::uint64_t written = 0;

    while (::av_read_frame(input.get(), packet.get()) >= 0) {
        const int sourceIndex = packet->stream_index;
        const int targetIndex = streamMap[sourceIndex];

        if (targetIndex < 0) {
            ::av_packet_unref(packet.get());
            continue;
        }

        AVStream* source = input->streams[sourceIndex];
        const std::int64_t stamp =
            packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;

        if (stamp != AV_NOPTS_VALUE) {
            const Micros position = av::ToMicros(stamp, source->time_base);

            if (position > end) {
                finished[sourceIndex] = true;

                // Se para cuando TODAS las pistas han pasado del final. Cortar
                // en cuanto lo hace la primera dejaria el recorte con sonido de
                // menos, porque audio y video no van a la par en el archivo.
                bool allDone = true;
                for (unsigned i = 0; i < input->nb_streams; ++i) {
                    if (streamMap[i] >= 0 && !finished[i]) { allDone = false; break; }
                }
                if (allDone) {
                    ::av_packet_unref(packet.get());
                    break;
                }
                ::av_packet_unref(packet.get());
                continue;
            }

            if (offset == AV_NOPTS_VALUE) {
                const std::int64_t base =
                    packet->dts != AV_NOPTS_VALUE ? packet->dts : packet->pts;
                offset = ::av_rescale_q(base, source->time_base, AVRational{1, AV_TIME_BASE});
                result.actualStart = av::ToMicros(base, source->time_base);
            }
            result.actualEnd = position;
        }

        if (offset != AV_NOPTS_VALUE) {
            const std::int64_t localOffset =
                ::av_rescale_q(offset, AVRational{1, AV_TIME_BASE}, source->time_base);

            if (packet->pts != AV_NOPTS_VALUE) packet->pts -= localOffset;
            if (packet->dts != AV_NOPTS_VALUE) packet->dts -= localOffset;
        }

        ::av_packet_rescale_ts(packet.get(), source->time_base,
                               output->streams[targetIndex]->time_base);
        packet->stream_index = targetIndex;
        packet->pos = -1;

        rc = ::av_interleaved_write_frame(output.get(), packet.get());
        ::av_packet_unref(packet.get());

        if (rc < 0) {
            result.error = "fallo al escribir: " + DescribeError(ErrorDomain::FFmpeg, rc);
            return result;
        }
        ++written;
    }

    rc = ::av_write_trailer(output.get());
    if (rc < 0) {
        result.error = "no se pudo cerrar el archivo: " +
                       DescribeError(ErrorDomain::FFmpeg, rc);
        return result;
    }

    if (written == 0) {
        result.error = "el intervalo no contiene datos";
        return result;
    }

    result.ok = true;
    PYXIS_INFO("Recorte guardado en '{}' ({} paquetes, {} ms reales)",
               ToUtf8(outputPath), written,
               result.actualEnd != kNoTimestamp && result.actualStart != kNoTimestamp
                   ? (result.actualEnd - result.actualStart) / 1000
                   : 0);
    return result;
}

}  // namespace pyxis
