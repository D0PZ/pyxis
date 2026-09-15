#include "media/Player.hpp"

#include "core/Error.hpp"
#include "core/Log.hpp"
#include "core/Text.hpp"
#include "core/Thread.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>

namespace pyxis {
namespace {

// Tolerancia al elegir fotograma. Por debajo de medio fotograma a 60 Hz no hay
// diferencia perceptible, y ser estricto provocaria descartes innecesarios por
// el jitter normal del planificador.
constexpr Micros kFrameTolerance = 8000;   // 8 ms

// Si un fotograma llega mas tarde que esto, se cuenta como "tardio" en las
// estadisticas. Es diagnostico, no afecta a la reproduccion.
constexpr Micros kLateThreshold = 40000;   // 40 ms

}  // namespace

Player::~Player() {
    Close();
}

void Player::Initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
    device_        = device;
    deviceContext_ = context;
    av::InstallLogBridge();
}

// ---------------------------------------------------------------------------
//  Apertura
// ---------------------------------------------------------------------------
void Player::Open(const std::wstring& path) {
    Close();

    try {
        demuxer_.Open(path);

        duration_     = demuxer_.Duration();
        sampleAspect_ = demuxer_.SampleAspectRatio();
        hasVideo_     = demuxer_.Video().Valid();
        hasAudio_     = demuxer_.Audio().Valid();
        title_        = std::filesystem::path(path).filename().wstring();

        if (hasVideo_) {
            VideoDecoder::Config config;
            config.device          = device_;
            config.context         = deviceContext_;
            config.allowHardware   = device_ != nullptr;
            // El pool debe cubrir la cola de fotogramas, el fotograma que el
            // presentador esta mostrando y el que tiene reservado. Quedarse
            // corto bloquea al decodificador en cada fotograma.
            config.extraPoolFrames =
                static_cast<int>(videoFrames_.Capacity()) + 4;

            videoDecoder_.Open(demuxer_.Video(), config);
        }

        if (hasAudio_) {
            // El audio se abre ANTES que su decodificador: es el dispositivo
            // quien impone la frecuencia y el numero de canales, y el
            // decodificador debe remuestrear hacia ese formato.
            try {
                audio_.Open();
                AudioDecoder::Config config;
                config.sampleRate = audio_.GetFormat().sampleRate;
                config.channels   = audio_.GetFormat().channels;
                audioDecoder_.Open(demuxer_.Audio(), config);
            } catch (const Exception& error) {
                // Sin audio se puede ver la pelicula; sin video, no. Un fallo
                // aqui degrada, no aborta.
                PYXIS_WARN("el audio no esta disponible: {}", error.what());
                audio_.Close();
                hasAudio_ = false;
            }
        }

        PYXIS_REQUIRE(hasVideo_ || hasAudio_, "el archivo no tiene contenido reproducible");

        generation_.store(1, std::memory_order_release);
        seekPending_.store(false, std::memory_order_release);
        demuxFinished_.store(false, std::memory_order_release);
        clock_.Reset(0);
        clock_.SetPaused(true);

        StartThreads();
        state_.store(PlayerState::Paused, std::memory_order_release);

        PYXIS_INFO("Reproduciendo '{}' ({} video, {} audio)",
                   ToUtf8(title_), hasVideo_ ? "con" : "sin", hasAudio_ ? "con" : "sin");

    } catch (const Exception& error) {
        SetFailed(error.what());
        Close();
        state_.store(PlayerState::Failed, std::memory_order_release);
        throw;
    }
}

void Player::StartThreads() {
    running_.store(true, std::memory_order_release);

    videoPackets_.Reopen();
    audioPackets_.Reopen();
    videoFrames_.Reopen();

    demuxThread_ = std::thread([this] { DemuxThread(); });
    if (hasVideo_) videoThread_ = std::thread([this] { VideoDecodeThread(); });
    if (hasAudio_) {
        audioThread_ = std::thread([this] { AudioDecodeThread(); });
        audio_.Start(clock_);
        audio_.SetPaused(true);
    }
}

void Player::StopThreads() noexcept {
    running_.store(false, std::memory_order_release);

    // Cerrar las colas despierta a cualquier hilo bloqueado en Push o Pop.
    // Sin esto, join() se quedaria esperando para siempre.
    videoPackets_.Close();
    audioPackets_.Close();
    videoFrames_.Close();

    audio_.Stop();

    if (demuxThread_.joinable()) demuxThread_.join();
    if (videoThread_.joinable()) videoThread_.join();
    if (audioThread_.joinable()) audioThread_.join();
}

void Player::Close() noexcept {
    StopThreads();

    pendingFrame_ = VideoFrame{};

    // Orden importante: los fotogramas de la cola referencian texturas del pool
    // del decodificador, asi que hay que soltarlos ANTES de cerrarlo.
    videoFrames_.Flush();
    videoPackets_.Flush();
    audioPackets_.Flush();

    videoDecoder_.Close();
    audioDecoder_.Close();
    audio_.Close();
    demuxer_.Close();

    duration_     = kNoTimestamp;
    sampleAspect_ = AVRational{1, 1};
    hasVideo_     = false;
    hasAudio_     = false;
    title_.clear();

    stepPending_.store(false, std::memory_order_release);
    displayedPts_.store(kNoTimestamp, std::memory_order_relaxed);
    displayedDuration_.store(0, std::memory_order_relaxed);

    framesDecoded_.store(0, std::memory_order_relaxed);
    framesDropped_.store(0, std::memory_order_relaxed);
    framesLate_.store(0, std::memory_order_relaxed);
    measuredFps_.store(0.0, std::memory_order_relaxed);
    fpsWindowStart_ = 0;
    fpsWindowCount_ = 0;

    if (state_.load(std::memory_order_acquire) != PlayerState::Failed) {
        state_.store(PlayerState::Idle, std::memory_order_release);
    }
}

// ---------------------------------------------------------------------------
//  Hilo de demultiplexado
// ---------------------------------------------------------------------------
void Player::DemuxThread() {
    SetCurrentThreadName(L"pyxis-demux");

    av::PacketPtr packet;
    try {
        packet = av::MakePacket();
    } catch (const Exception& error) {
        SetFailed(error.what());
        return;
    }

    bool reachedEnd = false;

    while (running_.load(std::memory_order_acquire)) {
        // --- Salto pendiente ------------------------------------------------
        if (seekPending_.exchange(false, std::memory_order_acq_rel)) {
            const Micros target = seekTarget_.load(std::memory_order_acquire);
            demuxer_.Seek(target, true);
            reachedEnd = false;
            demuxFinished_.store(false, std::memory_order_release);
        }

        if (reachedEnd) {
            // Al final del archivo el hilo no debe girar en vacio quemando CPU,
            // pero tampoco puede morir: un salto hacia atras lo reactiva.
            demuxFinished_.store(true, std::memory_order_release);
            ::Sleep(20);
            continue;
        }

        // --- Lectura --------------------------------------------------------
        ::av_packet_unref(packet.get());
        const Demuxer::ReadResult result = demuxer_.Read(packet.get());

        if (result == Demuxer::ReadResult::EndOfFile) {
            reachedEnd = true;
            // Sentinela de fin: hace que los decodificadores se vacien y
            // entreguen los fotogramas que retienen por reordenamiento B.
            if (hasVideo_) {
                TaggedPacket sentinel;
                sentinel.generation = generation_.load(std::memory_order_acquire);
                sentinel.endOfFile  = true;
                (void)videoPackets_.Push(std::move(sentinel));
            }
            if (hasAudio_) {
                TaggedPacket sentinel;
                sentinel.generation = generation_.load(std::memory_order_acquire);
                sentinel.endOfFile  = true;
                (void)audioPackets_.Push(std::move(sentinel));
            }
            continue;
        }

        if (result == Demuxer::ReadResult::Recoverable) {
            ::Sleep(5);
            continue;
        }
        if (result == Demuxer::ReadResult::Fatal) {
            SetFailed("no se pudo seguir leyendo el medio");
            state_.store(PlayerState::Failed, std::memory_order_release);
            return;
        }

        // --- Encolado -------------------------------------------------------
        const int index = packet->stream_index;
        BoundedQueue<TaggedPacket>* target = nullptr;

        if (hasVideo_ && index == demuxer_.Video().index)      target = &videoPackets_;
        else if (hasAudio_ && index == demuxer_.Audio().index) target = &audioPackets_;
        else continue;   // pista descartada

        TaggedPacket item;
        item.packet     = av::MakePacket();
        item.generation = generation_.load(std::memory_order_acquire);
        ::av_packet_move_ref(item.packet.get(), packet.get());

        // Push bloquea cuando la cola esta llena: esa es la contrapresion que
        // mantiene acotada la memoria. Si devuelve Flushed, hay un salto en
        // curso y este paquete ya no sirve.
        (void)target->Push(std::move(item));
    }
}

// ---------------------------------------------------------------------------
//  Hilo de decodificacion de video
// ---------------------------------------------------------------------------
void Player::VideoDecodeThread() {
    SetCurrentThreadName(L"pyxis-video");
    const MmcssScope mmcss(MmcssTask::Playback);

    std::uint32_t currentGeneration = generation_.load(std::memory_order_acquire);

    while (running_.load(std::memory_order_acquire)) {
        TaggedPacket item;
        const QueueStatus status = videoPackets_.Pop(item);

        if (status == QueueStatus::Closed)  break;
        if (status == QueueStatus::Flushed) continue;

        // Cambio de generacion: el paquete pertenece a una posicion nueva, asi
        // que el decodificador debe olvidar sus fotogramas de referencia.
        if (item.generation != currentGeneration) {
            videoDecoder_.Flush();
            currentGeneration = item.generation;
        }

        const AVPacket* packet = item.endOfFile ? nullptr : item.packet.get();
        const VideoDecoder::Status sent = videoDecoder_.Send(packet);

        if (sent == VideoDecoder::Status::Error) {
            SetFailed("fallo la decodificacion de video");
            state_.store(PlayerState::Failed, std::memory_order_release);
            return;
        }

        // Drenaje: un solo Send puede liberar varios fotogramas.
        for (;;) {
            VideoFrame frame;
            const VideoDecoder::Status received = videoDecoder_.Receive(frame);

            if (received == VideoDecoder::Status::Again ||
                received == VideoDecoder::Status::EndOfFile) {
                break;
            }
            if (received == VideoDecoder::Status::Error) break;

            framesDecoded_.fetch_add(1, std::memory_order_relaxed);

            // Un fotograma de una generacion anterior se descarta sin llegar a
            // la cola: mostrarlo produciria un parpadeo tras el salto.
            if (currentGeneration != generation_.load(std::memory_order_acquire)) {
                continue;
            }

            if (videoFrames_.Push(std::move(frame)) == QueueStatus::Closed) return;
        }
    }
}

// ---------------------------------------------------------------------------
//  Hilo de decodificacion de audio
// ---------------------------------------------------------------------------
void Player::AudioDecodeThread() {
    SetCurrentThreadName(L"pyxis-audio-decode");
    const MmcssScope mmcss(MmcssTask::Playback);

    std::uint32_t currentGeneration = generation_.load(std::memory_order_acquire);

    while (running_.load(std::memory_order_acquire)) {
        TaggedPacket item;
        const QueueStatus status = audioPackets_.Pop(item);

        if (status == QueueStatus::Closed)  break;
        if (status == QueueStatus::Flushed) continue;

        if (item.generation != currentGeneration) {
            audioDecoder_.Flush();
            currentGeneration = item.generation;
        }

        const AVPacket* packet = item.endOfFile ? nullptr : item.packet.get();
        if (audioDecoder_.Send(packet) == AudioDecoder::Status::Error) {
            PYXIS_WARN("fallo la decodificacion de audio; se continua sin sonido");
            return;
        }

        for (;;) {
            AudioBuffer buffer;
            const AudioDecoder::Status received = audioDecoder_.Receive(buffer);

            if (received != AudioDecoder::Status::Ok) break;

            if (currentGeneration != generation_.load(std::memory_order_acquire)) {
                continue;
            }
            if (audio_.Queue().Push(std::move(buffer)) == QueueStatus::Closed) return;
        }
    }
}

// ---------------------------------------------------------------------------
//  Seleccion del fotograma a presentar
// ---------------------------------------------------------------------------
Player::FrameSelection Player::SelectFrame(VideoFrame& out) {
    if (!hasVideo_) return FrameSelection::None;

    // El avance manual manda sobre el reloj: mientras hay un paso pendiente se
    // busca un fotograma concreto, no el que toque por tiempo.
    if (stepPending_.load(std::memory_order_acquire)) {
        return SelectSteppedFrame(out);
    }

    const Micros now = clock_.Position();
    bool selected = false;

    for (;;) {
        if (!pendingFrame_.IsValid()) {
            if (!videoFrames_.TryPop(pendingFrame_)) break;
        }

        // Un fotograma sin marca de tiempo se muestra de inmediato: es lo unico
        // razonable, y ocurre en flujos crudos sin contenedor.
        const bool due = pendingFrame_.pts == kNoTimestamp ||
                         pendingFrame_.pts <= now + kFrameTolerance;
        if (!due) break;

        if (selected) {
            // Ya habia un fotograma elegido y este tambien toca: el anterior
            // llego tarde y se descarta sin mostrarse. Esto es lo que mantiene
            // la sincronia cuando la GPU no da abasto.
            framesDropped_.fetch_add(1, std::memory_order_relaxed);
        }

        if (pendingFrame_.pts != kNoTimestamp && now - pendingFrame_.pts > kLateThreshold) {
            framesLate_.fetch_add(1, std::memory_order_relaxed);
        }

        out = std::move(pendingFrame_);
        pendingFrame_ = VideoFrame{};
        selected = true;
    }

    if (!selected) {
        // Fin de la reproduccion: no quedan fotogramas, el demultiplexor acabo
        // y las colas estan vacias.
        if (demuxFinished_.load(std::memory_order_acquire) &&
            videoFrames_.Size() == 0 && videoPackets_.Size() == 0 &&
            state_.load(std::memory_order_acquire) == PlayerState::Playing) {
            clock_.SetPaused(true);
            audio_.SetPaused(true);
            state_.store(PlayerState::Ended, std::memory_order_release);
        }
        return FrameSelection::None;
    }

    // Sin pista de audio no hay quien ancle el reloj, asi que lo hace el video.
    if (!hasAudio_ && out.pts != kNoTimestamp &&
        state_.load(std::memory_order_acquire) == PlayerState::Playing) {
        // Solo se reancla si la deriva es grande; hacerlo en cada fotograma
        // convertiria el reloj en una escalera con la cadencia del video.
        if (std::abs(now - out.pts) > 100000) {
            clock_.Anchor(out.pts, NowMicros());
        }
    }

    NoteDisplayedFrame(out);

    // Tasa real de presentacion, en ventanas de un segundo.
    const Micros nowReal = NowMicros();
    if (fpsWindowStart_ == 0) fpsWindowStart_ = nowReal;
    ++fpsWindowCount_;

    if (nowReal - fpsWindowStart_ >= kMicrosPerSecond) {
        const double seconds =
            static_cast<double>(nowReal - fpsWindowStart_) / kMicrosPerSecond;
        measuredFps_.store(static_cast<double>(fpsWindowCount_) / seconds,
                           std::memory_order_relaxed);
        fpsWindowStart_ = nowReal;
        fpsWindowCount_ = 0;
    }

    return FrameSelection::Updated;
}

void Player::NoteDisplayedFrame(const VideoFrame& frame) noexcept {
    if (frame.pts != kNoTimestamp) {
        displayedPts_.store(frame.pts, std::memory_order_relaxed);
    }
    if (frame.duration > 0) {
        displayedDuration_.store(frame.duration, std::memory_order_relaxed);
    }
}

// ---------------------------------------------------------------------------
//  Avance fotograma a fotograma
// ---------------------------------------------------------------------------
Player::FrameSelection Player::SelectSteppedFrame(VideoFrame& out) {
    const Micros lowerBound = stepLowerBound_.load(std::memory_order_acquire);

    for (;;) {
        if (!pendingFrame_.IsValid()) {
            if (!videoFrames_.TryPop(pendingFrame_)) {
                // El fotograma buscado todavia se esta decodificando. Se
                // conserva el que hay en pantalla -nada de fundidos a negro- y
                // se reintenta en la siguiente presentacion.
                return FrameSelection::None;
            }
        }

        // Los fotogramas anteriores al objetivo son el precio de rebobinar
        // hasta el fotograma clave: se descartan sin llegar a mostrarse.
        if (pendingFrame_.pts != kNoTimestamp && pendingFrame_.pts < lowerBound) {
            pendingFrame_ = VideoFrame{};
            continue;
        }

        out = std::move(pendingFrame_);
        pendingFrame_ = VideoFrame{};

        // El reloj se planta exactamente en el fotograma mostrado. Asi la barra
        // de progreso acompana al avance manual y, al reanudar, la reproduccion
        // continua desde aqui y no desde donde estaba antes de empezar a pasar
        // fotogramas.
        if (out.pts != kNoTimestamp) clock_.Reset(out.pts);

        NoteDisplayedFrame(out);
        stepPending_.store(false, std::memory_order_release);
        return FrameSelection::Updated;
    }
}

void Player::StepFrame(int direction) {
    if (!hasVideo_ || direction == 0) return;

    const PlayerState current = state_.load(std::memory_order_acquire);
    if (current == PlayerState::Idle || current == PlayerState::Failed) return;

    // Avanzar de uno en uno implica pausa: no tiene sentido pedir un fotograma
    // concreto mientras el reloj sigue corriendo por debajo.
    Pause();

    // Duracion de referencia del paso. Se prefiere la del fotograma en pantalla
    // (correcta con tasa variable) y se cae a la nominal del flujo.
    Micros step = displayedDuration_.load(std::memory_order_relaxed);
    if (step <= 0) step = videoDecoder_.NominalFrameDuration();
    if (step <= 0) step = kMicrosPerSecond / 25;   // ultimo recurso

    Micros position = displayedPts_.load(std::memory_order_relaxed);
    if (position == kNoTimestamp) position = clock_.Position();

    if (direction > 0) {
        // Adelante: el siguiente fotograma ya esta en la cola o en camino. El
        // umbral a media duracion descarta el actual sin descartar el siguiente.
        stepLowerBound_.store(position + step / 2, std::memory_order_release);
        stepPending_.store(true, std::memory_order_release);
        return;
    }

    // Atras: hay que rebobinar y redecodificar. El umbral se situa a una
    // duracion y media, que es el unico punto que deja fuera al ante-anterior
    // y dentro al anterior:
    //
    //      ... P-2        P-1        P (en pantalla)
    //           |    umbral |         |
    //           |<-- 1.5 duraciones ->|
    const Micros target = position - step;
    stepLowerBound_.store(position - step - step / 2, std::memory_order_release);
    stepPending_.store(true, std::memory_order_release);

    RequestSeekInternal(target > 0 ? target : 0);
}

// ---------------------------------------------------------------------------
//  Control de reproduccion
// ---------------------------------------------------------------------------
void Player::Play() {
    const PlayerState current = state_.load(std::memory_order_acquire);
    if (current == PlayerState::Idle || current == PlayerState::Failed) return;

    // Reproducir desde el final reinicia: es lo que espera cualquiera que le da
    // al play tras ver los creditos.
    if (current == PlayerState::Ended) {
        Seek(0);
    }

    // Un paso a medias dejaria el reloj clavado esperando un fotograma que
    // ya no interesa.
    stepPending_.store(false, std::memory_order_release);

    clock_.SetPaused(false);
    audio_.SetPaused(false);
    state_.store(PlayerState::Playing, std::memory_order_release);
}

void Player::Pause() {
    if (state_.load(std::memory_order_acquire) != PlayerState::Playing) return;

    clock_.SetPaused(true);
    audio_.SetPaused(true);
    state_.store(PlayerState::Paused, std::memory_order_release);
}

void Player::TogglePause() {
    if (state_.load(std::memory_order_acquire) == PlayerState::Playing) Pause();
    else Play();
}

void Player::Seek(Micros target) {
    const PlayerState current = state_.load(std::memory_order_acquire);
    if (current == PlayerState::Idle || current == PlayerState::Failed) return;

    if (duration_ != kNoTimestamp) {
        target = std::clamp<Micros>(target, 0, duration_);
    } else if (target < 0) {
        target = 0;
    }

    // Un salto normal invalida cualquier paso en curso.
    stepPending_.store(false, std::memory_order_release);
    RequestSeekInternal(target);
}

void Player::SeekRelative(Micros delta) {
    Seek(Position() + delta);
}

void Player::RequestSeekInternal(Micros target) {
    // 1. Nueva generacion. A partir de aqui, todo lo que este en vuelo queda
    //    marcado como obsoleto para los hilos que lo reciban.
    generation_.fetch_add(1, std::memory_order_acq_rel);

    // 2. Vaciado de las colas. Flush las deja vacias Y despierta a los hilos
    //    bloqueados, que es lo que permite que el demultiplexor llegue a ver la
    //    peticion de salto en lugar de seguir dormido sobre una cola llena.
    videoPackets_.Flush();
    audioPackets_.Flush();
    videoFrames_.Flush();

    pendingFrame_ = VideoFrame{};

    // 3. Peticion al demultiplexor, que es el unico dueno del contenedor.
    seekTarget_.store(target, std::memory_order_release);
    seekPending_.store(true, std::memory_order_release);

    // 4. El audio descarta su bufer de dispositivo para no reproducir un
    //    fragmento de la posicion anterior.
    audio_.Flush();

    // 5. El reloj salta al destino. La primera entrega de audio lo reancla con
    //    la marca de tiempo real, que puede diferir si el salto cayo en un
    //    fotograma clave anterior.
    clock_.Reset(target);

    if (state_.load(std::memory_order_acquire) == PlayerState::Ended) {
        state_.store(PlayerState::Paused, std::memory_order_release);
    }

    PYXIS_DEBUG("Salto solicitado a {} ms (generacion {})",
                target / 1000, generation_.load(std::memory_order_relaxed));
}

void Player::SetVolume(float volume) { audio_.SetVolume(volume); }
void Player::SetMuted(bool muted)    { audio_.SetMuted(muted); }

void Player::SetRateMilli(int rateMilli) {
    // Fuera de este rango, el remuestreo del audio deja de ser inteligible y la
    // decodificacion no puede seguir el ritmo.
    clock_.SetRateMilli(std::clamp(rateMilli, 250, 4000));
}

Micros Player::Position() const noexcept {
    const Micros position = clock_.Position();
    if (position < 0) return 0;
    if (duration_ != kNoTimestamp && position > duration_) return duration_;
    return position;
}

void Player::SetFailed(const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(errorMutex_);
        lastError_ = message;
    }
    PYXIS_ERROR("{}", message);
}

std::string Player::LastError() const {
    std::lock_guard<std::mutex> lock(errorMutex_);
    return lastError_;
}

PlayerStats Player::Stats() const {
    PlayerStats stats;
    stats.decoderName      = videoDecoder_.DecoderName();
    stats.hardwareDecoding = videoDecoder_.IsHardware();
    stats.width            = videoDecoder_.Width();
    stats.height           = videoDecoder_.Height();
    stats.containerName    = demuxer_.ContainerName();

    stats.framesDecoded  = framesDecoded_.load(std::memory_order_relaxed);
    stats.framesDropped  = framesDropped_.load(std::memory_order_relaxed);
    stats.framesLate     = framesLate_.load(std::memory_order_relaxed);
    stats.audioUnderruns = audio_.Underruns();

    stats.videoPacketQueue = videoPackets_.Size();
    stats.audioPacketQueue = audioPackets_.Size();
    stats.videoFrameQueue  = videoFrames_.Size();

    stats.measuredFps = measuredFps_.load(std::memory_order_relaxed);
    return stats;
}

}  // namespace pyxis
