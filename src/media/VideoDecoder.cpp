#include "media/VideoDecoder.hpp"

#include "core/Error.hpp"
#include "core/Fault.hpp"
#include "core/Log.hpp"

#include <d3d11.h>

extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/mastering_display_metadata.h>
}

#include <algorithm>
#include <format>
#include <string>
#include <thread>

namespace pyxis {
namespace {

// ---------------------------------------------------------------------------
//  Derivacion de la descripcion de color
//
//  La mayoria de los archivos del mundo real dejan estos campos sin declarar.
//  Cuando eso ocurre, deducirlos por resolucion es lo que hace todo
//  reproductor serio, porque acierta en la practica totalidad de los casos:
//  el material SD se masterizo en BT.601 y el HD en BT.709.
// ---------------------------------------------------------------------------
YuvMatrix MapMatrix(AVColorSpace space, int height) noexcept {
    switch (space) {
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M:
            return YuvMatrix::BT601;
        case AVCOL_SPC_BT709:
            return YuvMatrix::BT709;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:
            return YuvMatrix::BT2020NCL;
        default:
            return height <= 576 ? YuvMatrix::BT601 : YuvMatrix::BT709;
    }
}

TransferFunction MapTransfer(AVColorTransferCharacteristic transfer) noexcept {
    switch (transfer) {
        case AVCOL_TRC_SMPTE2084:    return TransferFunction::Pq;
        case AVCOL_TRC_ARIB_STD_B67: return TransferFunction::Hlg;
        default:                     return TransferFunction::Sdr;
    }
}

}  // namespace

ColorInfo DeriveColorInfo(const AVFrame* frame) {
    ColorInfo info;
    if (frame == nullptr) return info;

    info.matrix   = MapMatrix(frame->colorspace, frame->height);
    info.transfer = MapTransfer(frame->color_trc);

    // AVCOL_RANGE_UNSPECIFIED se trata como limitado: es lo que asume
    // practicamente todo el material de video, y confundir rango completo con
    // limitado produce negros aplastados o grises lavados.
    info.range = frame->color_range == AVCOL_RANGE_JPEG ? ColorRange::Full
                                                        : ColorRange::Limited;

    // De donde sale la profundidad depende de la ruta. En software el formato
    // del fotograma ya lo dice; en la acelerada, AV_PIX_FMT_D3D11 es opaco y su
    // descriptor declara CERO bits. Hay que preguntarle al pool por el formato
    // real que hay detras de la textura.
    //
    // Sin esto, todo el material de 10 bits decodificado por la GPU se trataba
    // como de 8: la matriz YUV se construia con maxCode 255 en vez de 1023 y el
    // factor de P010 salia 1,0039 en lugar de 1,00096. El error es de decimas
    // de por ciento -por eso nunca se vio- pero afecta justo al caso que mas se
    // cuida en este reproductor.
    AVPixelFormat format = static_cast<AVPixelFormat>(frame->format);
    if (frame->hw_frames_ctx != nullptr) {
        const auto* frames =
            reinterpret_cast<const AVHWFramesContext*>(frame->hw_frames_ctx->data);
        if (frames != nullptr && frames->sw_format != AV_PIX_FMT_NONE) {
            format = frames->sw_format;
        }
    }
    if (const AVPixFmtDescriptor* desc = ::av_pix_fmt_desc_get(format)) {
        if (desc->comp[0].depth > 0) info.bitDepth = desc->comp[0].depth;
    }

    // Metadatos HDR: guian el mapeo de tonos cuando la pantalla no alcanza el
    // brillo con el que se masterizo el contenido.
    if (const AVFrameSideData* side =
            ::av_frame_get_side_data(frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL)) {
        const auto* light = reinterpret_cast<const AVContentLightMetadata*>(side->data);
        info.maxContentLightLevel = static_cast<float>(light->MaxCLL);
    }
    if (const AVFrameSideData* side =
            ::av_frame_get_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA)) {
        const auto* mastering =
            reinterpret_cast<const AVMasteringDisplayMetadata*>(side->data);
        if (mastering->has_luminance && mastering->max_luminance.den != 0) {
            info.maxMasteringLuminance =
                static_cast<float>(::av_q2d(mastering->max_luminance));
        }
    }
    return info;
}

// ---------------------------------------------------------------------------
//  Negociacion de formato
// ---------------------------------------------------------------------------
AVPixelFormat VideoDecoder::NegotiateFormat(AVCodecContext* context,
                                            const AVPixelFormat* formats) noexcept {
    auto* self = static_cast<VideoDecoder*>(context->opaque);

    for (const AVPixelFormat* candidate = formats; *candidate != AV_PIX_FMT_NONE; ++candidate) {
        if (*candidate != AV_PIX_FMT_D3D11) continue;
        if (context->hw_device_ctx == nullptr) break;

        // Pedimos a FFmpeg los parametros del pool que el codec necesita y los
        // ajustamos ANTES de inicializarlo. Este es el unico instante en que se
        // puede anadir BIND_SHADER_RESOURCE.
        AVBufferRef* frames = nullptr;
        int rc = ::avcodec_get_hw_frames_parameters(context, context->hw_device_ctx,
                                                    AV_PIX_FMT_D3D11, &frames);
        if (rc < 0) {
            PYXIS_WARN("no se pudieron obtener los parametros del pool D3D11VA: {}",
                       DescribeError(ErrorDomain::FFmpeg, rc));
            break;
        }

        auto* framesContext = reinterpret_cast<AVHWFramesContext*>(frames->data);
        auto* d3dFrames     = static_cast<AVD3D11VAFramesContext*>(framesContext->hwctx);

        // SIN esta linea la textura solo puede alimentar al decodificador: el
        // shader no podria leerla y habria que copiarla fotograma a fotograma.
        // Con ella, el muestreo es directo.
        d3dFrames->BindFlags |= D3D11_BIND_SHADER_RESOURCE;

        // Holgura para los fotogramas que Pyxis mantiene en la cola de
        // presentacion y en el historial del avance manual. Sin ella el
        // decodificador se bloquea esperando a que el renderizador suelte
        // texturas, y aparecen microcortes.
        const int requested = self != nullptr ? self->extraPool_ : 8;
        const int baseSize  = framesContext->initial_pool_size;
        framesContext->initial_pool_size = baseSize + requested;

        rc = ::av_hwframe_ctx_init(frames);

        PYXIS_DEBUG("pool D3D11VA: base {} + holgura {} = {} -> {}",
                    baseSize, requested, framesContext->initial_pool_size,
                    rc < 0 ? DescribeError(ErrorDomain::FFmpeg, rc) : std::string("ok"));

        // Inyeccion de fallos (--fault pool-full). Pedir un pool absurdo NO
        // sirve para llegar hasta aqui: av_hwframe_ctx_init recorta a 64 antes
        // de intentar nada, asi que la peticion nunca es demasiado grande. El
        // reintento solo se recorre cuando la tarjeta se queda sin memoria de
        // verdad -64 texturas de 8K son mas de 3 GB-, de modo que la unica
        // forma de probarlo es dar el primer intento por fallido.
        if (rc >= 0 && requested > 4 && FaultActive(Fault::PoolFull)) {
            PYXIS_WARN("fallo inyectado: se da por fallido el pool de {} texturas",
                       framesContext->initial_pool_size);
            rc = AVERROR(ENOMEM);
        }

        // Si no cabe, se reintenta con lo justo ANTES de rendirse. Caer a
        // software por pedir un pool generoso seria un desastre: la holgura es
        // una comodidad, la aceleracion no.
        if (rc < 0 && requested > 4) {
            PYXIS_WARN("el pool de {} texturas no cupo ({}); se reintenta con {}",
                       baseSize + requested, DescribeError(ErrorDomain::FFmpeg, rc),
                       baseSize + 4);

            ::av_buffer_unref(&frames);
            if (::avcodec_get_hw_frames_parameters(context, context->hw_device_ctx,
                                                   AV_PIX_FMT_D3D11, &frames) < 0) {
                break;
            }
            framesContext = reinterpret_cast<AVHWFramesContext*>(frames->data);
            d3dFrames     = static_cast<AVD3D11VAFramesContext*>(framesContext->hwctx);

            d3dFrames->BindFlags |= D3D11_BIND_SHADER_RESOURCE;
            framesContext->initial_pool_size += 4;
            if (self != nullptr) self->extraPool_ = 4;

            rc = ::av_hwframe_ctx_init(frames);
        }

        if (rc < 0) {
            PYXIS_WARN("no se pudo inicializar el pool de texturas D3D11VA: {}",
                       DescribeError(ErrorDomain::FFmpeg, rc));
            ::av_buffer_unref(&frames);
            break;
        }

        context->hw_frames_ctx = frames;   // el contexto pasa a ser el dueno
        if (self != nullptr) {
            self->hardware_ = true;
            self->poolSlack_.store(framesContext->initial_pool_size - baseSize,
                                   std::memory_order_release);
        }
        return AV_PIX_FMT_D3D11;
    }

    // Repliegue explicito al primer formato por software. Se evita
    // avcodec_default_get_format a proposito: podria elegir otro formato
    // acelerado que este reproductor no sabe manejar.
    if (self != nullptr) self->hardware_ = false;
    for (const AVPixelFormat* candidate = formats; *candidate != AV_PIX_FMT_NONE; ++candidate) {
        const AVPixFmtDescriptor* desc = ::av_pix_fmt_desc_get(*candidate);
        if (desc != nullptr && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL) == 0) {
            PYXIS_WARN("D3D11VA no disponible; se decodificara por software en {}",
                       av::DescribePixelFormat(*candidate));
            return *candidate;
        }
    }
    return formats[0];
}

// ---------------------------------------------------------------------------
//  Ciclo de vida
// ---------------------------------------------------------------------------
VideoDecoder::~VideoDecoder() {
    Close();
}

void VideoDecoder::CreateHardwareDevice(const Config& config) {
    PYXIS_REQUIRE(config.device != nullptr, "D3D11VA requiere un ID3D11Device");

    AVBufferRef* raw = ::av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    PYXIS_REQUIRE(raw != nullptr, "av_hwdevice_ctx_alloc(D3D11VA) fallo");
    av::BufferRefPtr reference(raw);

    auto* deviceContext = reinterpret_cast<AVHWDeviceContext*>(reference->data);
    auto* d3dDevice     = static_cast<AVD3D11VADeviceContext*>(deviceContext->hwctx);

    // Compartimos NUESTRO dispositivo en lugar de dejar que FFmpeg cree uno
    // propio. Es imprescindible: una textura creada por un dispositivo no se
    // puede muestrear desde otro sin compartirla explicitamente, y eso
    // implicaria una copia por fotograma, justo lo que se quiere evitar.
    //
    // FFmpeg libera esta referencia en su destructor, de ahi el AddRef.
    d3dDevice->device = config.device;
    config.device->AddRef();

    // device_context, video_device y video_context los deriva FFmpeg del propio
    // dispositivo, y crea ademas su mutex de sincronizacion. Es correcto porque
    // el dispositivo esta marcado como multihilo (ver render/Device.cpp).
    const int rc = ::av_hwdevice_ctx_init(reference.get());
    if (rc < 0) {
        ThrowAv(rc, "no se pudo inicializar el contexto D3D11VA");
    }
    hwDevice_ = std::move(reference);
}

void VideoDecoder::ConfigureDecodeThreads(bool hardwareAvailable) {
    codec_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;

    if (hardwareAvailable) {
        // CON ACELERACION, POCOS HILOS.
        //
        // Es facil pensar que el numero de hilos solo importa en la ruta por
        // software -el trabajo lo hace la GPU-, pero es falso y sale caro: con
        // FF_THREAD_FRAME cada hilo mantiene SU PROPIO juego de fotogramas de
        // referencia, y en la ruta acelerada esos fotogramas son plazas del pool
        // de texturas. Pedir treinta y dos hilos multiplica la demanda del pool
        // hasta agotarlo, y entonces el decodificador deja de producir: el
        // sintoma es "Static surface pool size exceeded" seguido de
        // "get_buffer() failed", con la imagen congelada.
        //
        // Cuatro bastan para que el controlador encadene envios a la GPU sin
        // burbujas. La decodificacion no es el cuello de botella aqui.
        codec_->thread_count = 4;
        return;
    }

    // Por software si hacen falta todos: es donde el paralelismo decide si un
    // 4K va fluido o a trompicones.
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    codec_->thread_count = static_cast<int>(std::min(cores, 32u));
}

void VideoDecoder::Open(const Demuxer::Track& track, const Config& config) {
    Close();
    PYXIS_REQUIRE(track.Valid(), "VideoDecoder::Open sin pista de video");

    extraPool_ = std::max(2, config.extraPoolFrames);

    timeBase_  = track.timeBase;

    const AVCodec* decoder = ::avcodec_find_decoder(track.params->codec_id);
    PYXIS_REQUIRE(decoder != nullptr, "el codec de video del archivo no esta soportado");

    codec_.reset(::avcodec_alloc_context3(decoder));
    PYXIS_REQUIRE(codec_ != nullptr, "avcodec_alloc_context3: sin memoria");

    PYXIS_CHECK_AV(::avcodec_parameters_to_context(codec_.get(), track.params),
                   "no se pudieron aplicar los parametros del codec de video");

    codec_->pkt_timebase = track.timeBase;
    codec_->opaque       = this;

    if (config.allowHardware && config.device != nullptr) {
        try {
            CreateHardwareDevice(config);
            codec_->hw_device_ctx = ::av_buffer_ref(hwDevice_.get());
            codec_->get_format    = &VideoDecoder::NegotiateFormat;
        } catch (const Exception& error) {
            // La aceleracion es una optimizacion, no un requisito. Si el
            // dispositivo no la soporta, se sigue por software.
            PYXIS_WARN("aceleracion por hardware no disponible: {}", error.what());
            hwDevice_.reset();
        }
    }

    // El numero de hilos depende de si habra aceleracion, y eso hay que decidirlo
    // ANTES de abrir el codec: la negociacion real ocurre en el primer
    // fotograma, demasiado tarde para cambiarlo.
    ConfigureDecodeThreads(hwDevice_ != nullptr);

    PYXIS_CHECK_AV(::avcodec_open2(codec_.get(), decoder, nullptr),
                   "no se pudo abrir el decodificador de video");

    width_       = codec_->width;
    height_      = codec_->height;
    decoderName_ = decoder->name;
    scratch_     = av::MakeFrame();

    frameDuration_ = (track.frameRate.num > 0 && track.frameRate.den > 0)
                         ? static_cast<Micros>(kMicrosPerSecond) * track.frameRate.den /
                               track.frameRate.num
                         : 0;

    PYXIS_INFO("Decodificador de video: {} {}x{} ({} us por fotograma nominal)",
               decoderName_, width_, height_, frameDuration_);
}

void VideoDecoder::Close() noexcept {
    codec_.reset();
    hwDevice_.reset();
    scratch_.reset();
    converter_.reset();
    hardware_        = false;
    poolSlack_.store(0, std::memory_order_release);
    width_           = 0;
    height_          = 0;
    frameDuration_   = 0;
    decoderName_.clear();
}

void VideoDecoder::Flush() noexcept {
    if (codec_) ::avcodec_flush_buffers(codec_.get());
}

// ---------------------------------------------------------------------------
//  Bucle de decodificacion
// ---------------------------------------------------------------------------
VideoDecoder::Status VideoDecoder::Send(const AVPacket* packet) {
    PYXIS_REQUIRE(codec_ != nullptr, "VideoDecoder::Send sin decodificador abierto");

    const int rc = ::avcodec_send_packet(codec_.get(), packet);
    if (rc == 0)               return Status::Ok;
    if (rc == AVERROR(EAGAIN)) return Status::Again;   // hay que drenar la salida
    if (rc == AVERROR_EOF)     return Status::EndOfFile;

    // Un paquete corrupto es normal en emisiones y en archivos danados: se
    // descarta y se continua. Solo los fallos estructurales abortan el flujo.
    if (rc == AVERROR_INVALIDDATA) {
        PYXIS_DEBUG("paquete de video descartado por datos invalidos");
        return Status::Ok;
    }

    PYXIS_ERROR("avcodec_send_packet fallo: {}", DescribeError(ErrorDomain::FFmpeg, rc));
    return Status::Error;
}

VideoDecoder::Status VideoDecoder::Receive(VideoFrame& out) {
    PYXIS_REQUIRE(codec_ != nullptr, "VideoDecoder::Receive sin decodificador abierto");

    const int rc = ::avcodec_receive_frame(codec_.get(), scratch_.get());
    if (rc == AVERROR(EAGAIN)) return Status::Again;
    if (rc == AVERROR_EOF)     return Status::EndOfFile;
    if (rc < 0) {
        PYXIS_ERROR("avcodec_receive_frame fallo: {}", DescribeError(ErrorDomain::FFmpeg, rc));
        return Status::Error;
    }

    AVFrame* source = scratch_.get();

    const Micros pts = av::ToMicros(
        source->best_effort_timestamp != AV_NOPTS_VALUE ? source->best_effort_timestamp
                                                        : source->pts,
        timeBase_);

    Micros duration = av::ToMicros(source->duration, timeBase_);
    if (duration == kNoTimestamp || duration <= 0) duration = frameDuration_;

    const ColorInfo color = DeriveColorInfo(source);

    // La colorimetria solo se conoce con un fotograma decodificado delante: las
    // etiquetas del contenedor mienten a menudo y las del flujo llegan con el
    // primer fotograma. Se registra una vez porque interpretarla mal es la
    // causa numero uno de que un reproductor se vea lavado, y sin esta linea no
    // hay forma de saber desde fuera que camino del shader se esta usando.
    if (!colorLogged_) {
        colorLogged_ = true;
        PYXIS_INFO("Colorimetria: {} {} {} bits, rango {}{}",
                   DescribeMatrix(color.matrix), DescribeTransfer(color.transfer),
                   color.bitDepth,
                   color.range == ColorRange::Full ? "completo" : "limitado",
                   color.maxMasteringLuminance > 0
                       ? std::format(", masterizado a {:.0f} cd/m2",
                                     color.maxMasteringLuminance)
                       : std::string{});
    }

    if (source->format == AV_PIX_FMT_D3D11) {
        // Ruta zero-copy: data[0] es el ID3D11Texture2D del pool y data[1] es
        // el indice de la capa dentro de ese array de texturas.
        out.frame = av::MakeFrame();
        ::av_frame_move_ref(out.frame.get(), source);
        out.texture    = reinterpret_cast<ID3D11Texture2D*>(out.frame->data[0]);
        out.arraySlice = static_cast<std::uint32_t>(
            reinterpret_cast<std::intptr_t>(out.frame->data[1]));
    } else {
        out.texture    = nullptr;
        out.arraySlice = 0;
        if (!NormalizeSoftwareFrame(source, out)) {
            ::av_frame_unref(source);
            return Status::Error;
        }
    }

    out.pts      = pts;
    out.duration = duration;
    out.color    = color;
    out.width    = out.frame->width;
    out.height   = out.frame->height;

    ::av_frame_unref(source);
    return Status::Ok;
}

// ---------------------------------------------------------------------------
//  Repliegue por software: cualquier formato -> NV12 / P010
// ---------------------------------------------------------------------------
bool VideoDecoder::NormalizeSoftwareFrame(AVFrame* source, VideoFrame& out) {
    const auto sourceFormat = static_cast<AVPixelFormat>(source->format);
    const AVPixFmtDescriptor* desc = ::av_pix_fmt_desc_get(sourceFormat);
    const int depth = desc != nullptr ? desc->comp[0].depth : 8;

    const AVPixelFormat target = depth > 8 ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;

    // Si el codec ya produjo el formato que necesitamos no hay nada que
    // convertir: basta con quedarse con la referencia.
    if (sourceFormat == target) {
        out.frame = av::MakeFrame();
        ::av_frame_move_ref(out.frame.get(), source);
        return true;
    }

    // sws_getCachedContext reutiliza el contexto mientras los parametros no
    // cambien; recrearlo por fotograma costaria mas que la propia conversion.
    converter_.reset(::sws_getCachedContext(
        converter_.release(),
        source->width, source->height, sourceFormat,
        source->width, source->height, target,
        SWS_BILINEAR, nullptr, nullptr, nullptr));

    if (converter_ == nullptr) {
        PYXIS_ERROR("no se pudo crear el conversor {} -> {}",
                    av::DescribePixelFormat(sourceFormat),
                    av::DescribePixelFormat(target));
        return false;
    }

    av::FramePtr destination = av::MakeFrame();
    destination->format = target;
    destination->width  = source->width;
    destination->height = source->height;

    // Alineacion de 32 bytes: permite que las rutinas AVX2 de swscale y la
    // subida a la GPU trabajen sin caminos no alineados.
    const int rc = ::av_frame_get_buffer(destination.get(), 32);
    if (rc < 0) {
        PYXIS_ERROR("no se pudo reservar el fotograma de destino: {}",
                    DescribeError(ErrorDomain::FFmpeg, rc));
        return false;
    }

    ::sws_scale(converter_.get(), source->data, source->linesize, 0, source->height,
                destination->data, destination->linesize);

    // La conversion de formato no altera la colorimetria, asi que los metadatos
    // de color se copian tal cual.
    ::av_frame_copy_props(destination.get(), source);

    out.frame = std::move(destination);
    return true;
}

}  // namespace pyxis
