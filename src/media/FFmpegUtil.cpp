#include "media/FFmpegUtil.hpp"

#include "core/Error.hpp"
#include "core/Log.hpp"

#include <array>
#include <cstdarg>
#include <cstdio>

namespace pyxis::av {
namespace {

log::Level MapLogLevel(int avLevel) noexcept {
    if (avLevel <= AV_LOG_ERROR)   return log::Level::Error;
    if (avLevel <= AV_LOG_WARNING) return log::Level::Warn;
    if (avLevel <= AV_LOG_INFO)    return log::Level::Info;
    if (avLevel <= AV_LOG_VERBOSE) return log::Level::Debug;
    return log::Level::Trace;
}

void LogCallback(void* avcl, int level, const char* fmt, std::va_list args) {
    if (level > ::av_log_get_level()) return;

    // av_log_format_line2 maneja el prefijo de clase ("[h264 @ 0x...]") y el
    // troceado de lineas, cosas que un vsnprintf directo no hace.
    std::array<char, 1024> buffer{};
    int printPrefix = 1;
    const int written = ::av_log_format_line2(avcl, level, fmt, args,
                                              buffer.data(), static_cast<int>(buffer.size()),
                                              &printPrefix);
    if (written <= 0) return;

    std::string_view line(buffer.data(),
                          static_cast<std::size_t>(written) < buffer.size()
                              ? static_cast<std::size_t>(written)
                              : buffer.size() - 1);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.remove_suffix(1);
    }
    if (line.empty()) return;

    const log::Level mapped = MapLogLevel(level);
    if (log::Enabled(mapped)) {
        log::Write(mapped, "[ffmpeg] {}", line);
    }
}

}  // namespace

FramePtr MakeFrame() {
    FramePtr frame(::av_frame_alloc());
    PYXIS_REQUIRE(frame != nullptr, "av_frame_alloc: sin memoria");
    return frame;
}

PacketPtr MakePacket() {
    PacketPtr packet(::av_packet_alloc());
    PYXIS_REQUIRE(packet != nullptr, "av_packet_alloc: sin memoria");
    return packet;
}

BufferRefPtr CloneRef(AVBufferRef* ref) {
    if (ref == nullptr) return nullptr;
    BufferRefPtr copy(::av_buffer_ref(ref));
    PYXIS_REQUIRE(copy != nullptr, "av_buffer_ref: sin memoria");
    return copy;
}

void InstallLogBridge() {
    // AV_LOG_INFO de FFmpeg es muy verboso (vuelca la cabecera completa de
    // cada archivo). Se deja en WARNING salvo que Pyxis este en modo depuracion.
    ::av_log_set_level(log::GetLevel() <= log::Level::Debug ? AV_LOG_VERBOSE : AV_LOG_WARNING);
    ::av_log_set_callback(&LogCallback);
}

std::string DescribeCodec(const AVCodecParameters* params) {
    if (params == nullptr) return "desconocido";

    const AVCodec* codec = ::avcodec_find_decoder(params->codec_id);
    std::string name = codec != nullptr ? codec->name : "desconocido";

    if (params->codec_type == AVMEDIA_TYPE_VIDEO) {
        std::array<char, 128> buffer{};
        std::snprintf(buffer.data(), buffer.size(), "%s %dx%d %s",
                      name.c_str(), params->width, params->height,
                      DescribePixelFormat(static_cast<AVPixelFormat>(params->format)).c_str());
        return buffer.data();
    }

    if (params->codec_type == AVMEDIA_TYPE_AUDIO) {
        std::array<char, 128> buffer{};
        std::snprintf(buffer.data(), buffer.size(), "%s %d Hz %d canales",
                      name.c_str(), params->sample_rate, params->ch_layout.nb_channels);
        return buffer.data();
    }

    return name;
}

std::string DescribePixelFormat(AVPixelFormat format) {
    const char* name = ::av_get_pix_fmt_name(format);
    return name != nullptr ? name : "desconocido";
}

}  // namespace pyxis::av
