// ============================================================================
//  FFmpegUtil.hpp - Envoltorios RAII sobre las estructuras de libav*
//
//  La API de FFmpeg es C pura con liberacion manual y, lo que es peor, con
//  funciones que reciben un doble puntero y lo ponen a nulo (av_frame_free
//  toma AVFrame**). Eso hace que un `delete` ingenuo no compile y que los
//  escapes de memoria sean facilisimos en las rutas de error.
//
//  Aqui se encapsula una sola vez para que el resto del proyecto nunca vuelva
//  a escribir un av_*_free. Cada tipo tiene su unique_ptr con el borrador
//  correcto, y la conversion de marcas de tiempo vive en un unico sitio.
// ============================================================================
#pragma once

#include "core/Clock.hpp"

#include <memory>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace pyxis::av {

// ---------------------------------------------------------------------------
//  Borradores. Todos toman el doble puntero que exige la API de FFmpeg.
// ---------------------------------------------------------------------------
struct FrameDeleter {
    void operator()(AVFrame* p) const noexcept { ::av_frame_free(&p); }
};
struct PacketDeleter {
    void operator()(AVPacket* p) const noexcept { ::av_packet_free(&p); }
};
struct FormatContextDeleter {
    void operator()(AVFormatContext* p) const noexcept { ::avformat_close_input(&p); }
};
struct CodecContextDeleter {
    void operator()(AVCodecContext* p) const noexcept { ::avcodec_free_context(&p); }
};
struct BufferRefDeleter {
    void operator()(AVBufferRef* p) const noexcept { ::av_buffer_unref(&p); }
};
struct SwrDeleter {
    void operator()(SwrContext* p) const noexcept { ::swr_free(&p); }
};
struct SwsDeleter {
    void operator()(SwsContext* p) const noexcept { ::sws_freeContext(p); }
};
struct DictDeleter {
    void operator()(AVDictionary* p) const noexcept { ::av_dict_free(&p); }
};

using FramePtr         = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr        = std::unique_ptr<AVPacket, PacketDeleter>;
using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;
using CodecContextPtr  = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using BufferRefPtr     = std::unique_ptr<AVBufferRef, BufferRefDeleter>;
using SwrPtr           = std::unique_ptr<SwrContext, SwrDeleter>;
using SwsPtr           = std::unique_ptr<SwsContext, SwsDeleter>;
using DictPtr          = std::unique_ptr<AVDictionary, DictDeleter>;

// Asignadores que lanzan si FFmpeg se queda sin memoria.
[[nodiscard]] FramePtr  MakeFrame();
[[nodiscard]] PacketPtr MakePacket();

// Clona la referencia de un AVBufferRef (incrementa el contador, no copia).
[[nodiscard]] BufferRefPtr CloneRef(AVBufferRef* ref);

// ---------------------------------------------------------------------------
//  Marcas de tiempo
//
//  FFmpeg entrega las marcas en la base de tiempo del contenedor, que varia
//  por archivo y por pista (1/90000 en MPEG-TS, 1/1000 en Matroska...). Pyxis
//  las normaliza a microsegundos en la frontera del demultiplexor, de modo
//  que ninguna capa superior necesita conocer AVRational.
// ---------------------------------------------------------------------------
[[nodiscard]] inline Micros ToMicros(std::int64_t timestamp, AVRational timeBase) noexcept {
    if (timestamp == AV_NOPTS_VALUE) return kNoTimestamp;
    return ::av_rescale_q(timestamp, timeBase, AVRational{1, static_cast<int>(kMicrosPerSecond)});
}

[[nodiscard]] inline std::int64_t FromMicros(Micros micros, AVRational timeBase) noexcept {
    if (micros == kNoTimestamp) return AV_NOPTS_VALUE;
    return ::av_rescale_q(micros, AVRational{1, static_cast<int>(kMicrosPerSecond)}, timeBase);
}

// ---------------------------------------------------------------------------
//  Diagnostico
// ---------------------------------------------------------------------------

// Redirige el registro de FFmpeg al de Pyxis en lugar de a stderr (que en una
// aplicacion /SUBSYSTEM:WINDOWS no va a ninguna parte).
void InstallLogBridge();

[[nodiscard]] std::string DescribeCodec(const AVCodecParameters* params);
[[nodiscard]] std::string DescribePixelFormat(AVPixelFormat format);

}  // namespace pyxis::av
