#include "media/Player.hpp"

#include "core/Error.hpp"
#include "core/Log.hpp"
#include "core/Text.hpp"
#include "core/Thread.hpp"

#include <d3d11.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

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

// Historial de fotogramas: cuantos pasos atras son instantaneos.
//
// Con la ruta zero-copy, conservar un fotograma no copia pixeles pero si retiene
// una plaza del pool de texturas. A 8K eso son unos 100 MiB por plaza, asi que
// el limite NO puede ser un numero fijo: lo que sobra en una tarjeta de 16 GiB
// ahoga a una de 4. Se consulta la memoria realmente disponible y se toma una
// fraccion.
//
// La fraccion es conservadora a proposito. Pyxis no es la unica aplicacion con
// derecho a la memoria de video, y ademas el presupuesto que reporta DXGI baja
// en cuanto otro programa empieza a pedir: quedarse con la mitad seria correcto
// hoy y un problema en cuanto se abra un navegador.
constexpr double      kHistoryMemoryShare = 0.25;
constexpr std::size_t kHistoryMinFrames   = 3;
constexpr std::size_t kHistoryMaxFrames   = 48;

// Tope del rebobinado. Con un historial grande la ventana crece con el, pero
// decodificar varios segundos de 8K para mostrar un fotograma tampoco vale la
// pena: a partir de aqui se prefiere rebobinar mas veces.
constexpr Micros kMaxRewindWindow = 2 * kMicrosPerSecond;

// Memoria de video libre, en bytes. Devuelve cero si no se puede averiguar, y
// entonces se cae al minimo.
[[nodiscard]] std::size_t AvailableVideoMemory(ID3D11Device* device) noexcept {
    if (device == nullptr) return 0;

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) return 0;

    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(&adapter))) return 0;

    // QueryVideoMemoryInfo da el presupuesto que el sistema asigna AHORA a este
    // proceso, no la memoria total de la tarjeta. Es el numero correcto: la
    // total no descuenta lo que ya estan usando el escritorio y el resto de
    // aplicaciones.
    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
    if (FAILED(adapter.As(&adapter3))) return 0;

    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
        return 0;
    }
    return info.Budget > info.CurrentUsage
               ? static_cast<std::size_t>(info.Budget - info.CurrentUsage)
               : 0;
}

// Ventana minima que se rebobina al retroceder cuando el historial no alcanza.
// Con tasa variable, la duracion del fotograma actual es mala estimacion de la
// distancia al anterior, asi que se toma un suelo generoso: pasarse cuesta algo
// de decodificacion, quedarse corto cuesta otro salto entero.
constexpr Micros kMinRewindWindow = 400000;   // 400 ms

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

        {
            std::lock_guard<std::mutex> lock(metadataMutex_);
            duration_     = demuxer_.Duration();
            sampleAspect_ = demuxer_.SampleAspectRatio();
            title_        = std::filesystem::path(path).filename().wstring();
        }
        hasVideo_.store(demuxer_.Video().Valid(), std::memory_order_release);
        hasAudio_.store(demuxer_.Audio().Valid(), std::memory_order_release);

        if (HasVideo()) {
            const AVCodecParameters* params = demuxer_.Video().params;

            // Plazas que no son historial: la cola de presentacion, el
            // fotograma en pantalla, el reservado y un margen para los
            // fotogramas que los hilos del decodificador tienen en vuelo.
            const std::size_t reserved = videoFrames_.Capacity() + 10;
            const std::size_t historyLimit =
                ComputeHistoryLimit(params->width, params->height, reserved);
            historyLimit_.store(historyLimit, std::memory_order_release);

            VideoDecoder::Config config;
            config.device        = device_;
            config.context       = deviceContext_;
            config.allowHardware = device_ != nullptr;

            // El pool debe cubrir la cola de fotogramas, el historial, el
            // fotograma que el presentador esta mostrando y el que tiene
            // reservado. Quedarse corto bloquea al decodificador en cada
            // fotograma.
            config.extraPoolFrames = static_cast<int>(reserved + historyLimit);

            videoDecoder_.Open(demuxer_.Video(), config);

            PYXIS_INFO("Historial de avance manual: {} fotogramas "
                       "({} MiB de memoria de video libre)",
                       historyLimit, AvailableVideoMemory(device_) / (1024 * 1024));
        }

        if (HasAudio()) {
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
                hasAudio_.store(false, std::memory_order_release);
            }
        }

        PYXIS_REQUIRE(HasVideo() || HasAudio(),
                      "el archivo no tiene contenido reproducible");

        // La generacion NUNCA se reinicia. Si volviera a 1 en cada apertura, el
        // hilo de presentacion podria encontrarse el mismo numero que tenia del
        // medio anterior y conservar un fotograma que ya no existe.
        generation_.fetch_add(1, std::memory_order_acq_rel);
        steppingMode_.store(false, std::memory_order_release);
        seekPending_.store(false, std::memory_order_release);
        demuxFinished_.store(false, std::memory_order_release);
        clock_.Reset(0);
        clock_.SetPaused(true);

        StartThreads();
        state_.store(PlayerState::Paused, std::memory_order_release);

        PYXIS_INFO("Reproduciendo '{}' ({} video, {} audio)",
                   ToUtf8(Title()), HasVideo() ? "con" : "sin", HasAudio() ? "con" : "sin");

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
    if (HasVideo()) videoThread_ = std::thread([this] { VideoDecodeThread(); });
    if (HasAudio()) {
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

    // pendingFrame_ pertenece al hilo de presentacion; tocarlo desde aqui seria
    // una carrera. Se incrementa la generacion y ese hilo lo suelta el solo en
    // su siguiente pasada. Mientras tanto el fotograma mantiene viva su textura
    // por conteo de referencias, asi que cerrar el decodificador es seguro.
    generation_.fetch_add(1, std::memory_order_acq_rel);

    // Orden importante: los fotogramas de la cola referencian texturas del pool
    // del decodificador, asi que hay que soltarlos ANTES de cerrarlo.
    videoFrames_.Flush();
    videoPackets_.Flush();
    audioPackets_.Flush();

    videoDecoder_.Close();
    audioDecoder_.Close();
    audio_.Close();
    demuxer_.Close();

    {
        std::lock_guard<std::mutex> lock(metadataMutex_);
        duration_     = kNoTimestamp;
        sampleAspect_ = AVRational{1, 1};
        title_.clear();
    }
    hasVideo_.store(false, std::memory_order_release);
    hasAudio_.store(false, std::memory_order_release);

    steppingMode_.store(false, std::memory_order_release);
    stepPending_.store(false, std::memory_order_release);
    stepRequest_.store(0, std::memory_order_release);
    historyLimit_.store(0, std::memory_order_release);
    collectFrom_     = kNoTimestamp;
    collectBefore_   = kNoTimestamp;
    collectBackward_ = false;
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

    // Generacion que este hilo esta produciendo AHORA MISMO. Es local a
    // proposito, y no una lectura de generation_ en cada paquete.
    //
    // Leerla al encolar era un error sutil: entre que se lee un paquete y se
    // encola, otro hilo puede pedir un salto e incrementar el contador. Ese
    // paquete -que pertenece a la posicion ANTERIOR- viajaria con la etiqueta
    // NUEVA, el decodificador se vaciaria y acto seguido lo decodificaria sin
    // sus fotogramas de referencia. En HEVC eso se manifiesta como "Could not
    // find ref with POC" y deja al decodificador produciendo basura.
    std::uint32_t demuxGeneration = generation_.load(std::memory_order_acquire);

    while (running_.load(std::memory_order_acquire)) {
        // --- Salto pendiente ------------------------------------------------
        if (seekPending_.exchange(false, std::memory_order_acq_rel)) {
            const Micros target = seekTarget_.load(std::memory_order_acquire);
            demuxer_.Seek(target, true);
            reachedEnd = false;
            demuxFinished_.store(false, std::memory_order_release);

            // A partir de aqui los paquetes pertenecen a la posicion nueva.
            demuxGeneration = generation_.load(std::memory_order_acquire);
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
            if (HasVideo()) {
                TaggedPacket sentinel;
                sentinel.generation = demuxGeneration;
                sentinel.endOfFile  = true;
                (void)videoPackets_.Push(std::move(sentinel));
            }
            if (HasAudio() && !steppingMode_.load(std::memory_order_acquire)) {
                TaggedPacket sentinel;
                sentinel.generation = demuxGeneration;
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

        if (HasVideo() && index == demuxer_.Video().index) {
            target = &videoPackets_;
        } else if (HasAudio() && index == demuxer_.Audio().index) {
            // En modo paso el audio se descarta. Ver la nota de steppingMode_
            // en Player.hpp: encolarlo bloquearia este hilo y con el moriria la
            // alimentacion de video, que es justo lo unico que el avance manual
            // necesita.
            if (steppingMode_.load(std::memory_order_acquire)) continue;
            target = &audioPackets_;
        } else {
            continue;   // pista descartada
        }

        // Si mientras se leia se pidio un salto, este paquete pertenece a la
        // posicion anterior y ya no sirve para nada.
        if (seekPending_.load(std::memory_order_acquire)) continue;

        TaggedPacket item;
        item.packet     = av::MakePacket();
        item.generation = demuxGeneration;
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
            frame.generation = currentGeneration;

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
    // La generacion es tambien como este hilo se entera de que el fotograma que
    // tenia reservado pertenece a otra posicion, o a otro medio. Se comprueba
    // ANTES que nada para que cerrar un archivo libere la reserva aunque ya no
    // quede pista de video.
    const std::uint32_t generation = generation_.load(std::memory_order_acquire);
    if (generation != presenterGeneration_) {
        presenterGeneration_ = generation;
        pendingFrame_ = VideoFrame{};
        ForgetHistory();   // pertenece a la posicion anterior
    }

    if (!HasVideo()) return FrameSelection::None;

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

        // Rezagado de antes del ultimo salto: fuera.
        if (pendingFrame_.generation != presenterGeneration_) {
            pendingFrame_ = VideoFrame{};
            continue;
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
    if (!HasAudio() && out.pts != kNoTimestamp &&
        state_.load(std::memory_order_acquire) == PlayerState::Playing) {
        // Solo se reancla si la deriva es grande; hacerlo en cada fotograma
        // convertiria el reloj en una escalera con la cadencia del video.
        if (std::abs(now - out.pts) > 100000) {
            clock_.Anchor(out.pts, NowMicros());
        }
    }

    NoteDisplayedFrame(out);

    // Todo lo que se muestra entra en el historial, de modo que pausar y
    // retroceder funcione al instante sin haber tenido que anticiparlo.
    PushHistory(out);

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

std::size_t Player::ComputeHistoryLimit(int width, int height,
                                        std::size_t reservedFrames) const noexcept {
    if (width <= 0 || height <= 0) return kHistoryMinFrames;

    // Tres bytes por pixel es el peor caso (P010: luma de 16 bits mas croma
    // submuestreado). Pasarse por arriba es preferible a quedarse corto y
    // agotar la memoria de video en material UHD.
    const std::size_t bytesPerFrame =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u;

    const std::size_t available = AvailableVideoMemory(device_);
    if (available == 0) return kHistoryMinFrames;

    const auto budget = static_cast<std::size_t>(available * kHistoryMemoryShare);
    const std::size_t affordable = budget / std::max<std::size_t>(bytesPerFrame, 1);

    // El presupuesto cubre TODO el pool extra, no solo el historial: la cola de
    // presentacion y los fotogramas en vuelo tambien ocupan plaza.
    if (affordable <= reservedFrames) return kHistoryMinFrames;

    return std::clamp(affordable - reservedFrames, kHistoryMinFrames, kHistoryMaxFrames);
}

VideoFrame Player::CloneFrameRef(const VideoFrame& source) {
    VideoFrame copy;
    if (!source.IsValid()) return copy;

    copy.frame = av::MakeFrame();

    // av_frame_ref NO copia pixeles: incrementa el contador de los buferes. En
    // la ruta por hardware eso significa que el historial no cuesta ni una
    // transferencia, solo una plaza mas en el pool de texturas.
    if (::av_frame_ref(copy.frame.get(), source.frame.get()) < 0) {
        copy.frame.reset();
        return copy;
    }

    copy.pts        = source.pts;
    copy.duration   = source.duration;
    copy.color      = source.color;
    copy.width      = source.width;
    copy.height     = source.height;
    copy.texture    = source.texture;
    copy.arraySlice = source.arraySlice;
    copy.generation = source.generation;
    return copy;
}

void Player::PushHistory(const VideoFrame& frame) {
    const std::size_t limit = historyLimit_.load(std::memory_order_relaxed);
    if (limit == 0) return;

    VideoFrame copy = CloneFrameRef(frame);
    if (!copy.IsValid()) return;

    history_.push_back(std::move(copy));
    while (history_.size() > limit) history_.pop_front();
    historyCursor_ = history_.size() - 1;
}

void Player::ForgetHistory() noexcept {
    history_.clear();
    historyCursor_ = 0;
}

bool Player::ResolveStepFromHistory(int direction, VideoFrame& out) {
    if (history_.empty()) return false;

    if (direction < 0) {
        if (historyCursor_ == 0) return false;
        --historyCursor_;
    } else {
        if (historyCursor_ + 1 >= history_.size()) return false;
        ++historyCursor_;
    }

    out = CloneFrameRef(history_[historyCursor_]);
    if (!out.IsValid()) return false;

    if (out.pts != kNoTimestamp) clock_.Reset(out.pts);
    NoteDisplayedFrame(out);

    PYXIS_DEBUG("Paso {} -> {} ms (historial {}/{})", direction > 0 ? "+1" : "-1",
                out.pts / 1000, historyCursor_ + 1, history_.size());
    return true;
}

void Player::PrepareStepCollection(int direction) {
    // Duracion de referencia del paso. Se prefiere la del fotograma en pantalla
    // (correcta con tasa variable) y se cae a la nominal del flujo.
    Micros step = displayedDuration_.load(std::memory_order_relaxed);
    if (step <= 0) step = videoDecoder_.NominalFrameDuration();
    if (step <= 0) step = kMicrosPerSecond / 25;   // ultimo recurso

    Micros position = displayedPts_.load(std::memory_order_relaxed);
    if (position == kNoTimestamp) position = clock_.Position();

    if (direction > 0) {
        // Adelante: el siguiente fotograma ya esta en la cola o en camino, y es
        // simplemente el primero que venga DESPUES del actual. Nada de calcular
        // "posicion mas una duracion": con tasa variable eso se saltaria los
        // fotogramas que llegan antes de lo nominal.
        collectFrom_     = position + 1;
        collectBefore_   = kNoTimestamp;
        collectBackward_ = false;
        return;
    }

    // Atras y sin historial util: toca rebobinar. Se aprovecha el viaje para
    // repoblar el historial entero, de modo que los siguientes pasos hacia
    // atras salgan de memoria.
    //
    //      salto        conservar desde aqui        limite (el actual)
    //        |                   |                         |
    //        v                   v                         v
    //   ... [K] . . . . . . . [P-n] ... [P-2] [P-1] [P (en pantalla)]
    //                                          ^
    //                                   este es el que se muestra:
    //                                   el ultimo anterior al limite
    const auto batch = static_cast<Micros>(
        std::max<std::size_t>(historyLimit_.load(std::memory_order_relaxed), 1));

    Micros window = step * batch;
    if (window < kMinRewindWindow) window = kMinRewindWindow;
    if (window > kMaxRewindWindow) window = kMaxRewindWindow;

    ForgetHistory();

    collectFrom_     = position - window > 0 ? position - window : 0;
    collectBefore_   = position;
    collectBackward_ = true;

    RequestSeekInternal(collectFrom_);

    // La generacion acaba de cambiar por el salto. Se adopta aqui mismo para
    // que la comprobacion de SelectFrame no vuelva a vaciar el historial que
    // estamos a punto de repoblar.
    presenterGeneration_ = generation_.load(std::memory_order_acquire);
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
    // 1. Peticion nueva: primero se intenta servir de memoria. Este es el
    //    camino habitual al revisar un plano adelante y atras, y es inmediato.
    const int request = stepRequest_.exchange(0, std::memory_order_acq_rel);
    if (request != 0) {
        if (ResolveStepFromHistory(request, out)) {
            stepPending_.store(false, std::memory_order_release);
            return FrameSelection::Updated;
        }
        PrepareStepCollection(request);
    }

    // 2. El historial no alcanzaba: hay que recoger de la cola. Puede requerir
    //    varias presentaciones mientras el decodificador trabaja.
    for (;;) {
        if (!pendingFrame_.IsValid()) {
            if (!videoFrames_.TryPop(pendingFrame_)) {
                // Si ya no puede llegar nada mas, el paso se CANCELA. Dejarlo
                // pendiente dejaba el avance muerto: StepFrame descarta las
                // pulsaciones mientras hay uno en vuelo, asi que un paso hacia
                // delante en el ultimo fotograma bloqueaba la herramienta hasta
                // reproducir o saltar.
                if (demuxFinished_.load(std::memory_order_acquire) &&
                    videoPackets_.Size() == 0) {
                    stepPending_.store(false, std::memory_order_release);
                }

                // Si no, el fotograma buscado todavia se esta decodificando: se
                // conserva el que hay en pantalla -nada de fundidos a negro- y
                // se reintenta en la siguiente presentacion.
                return FrameSelection::None;
            }
        }

        // Rezagado de antes del salto. Descartarlo aqui es lo que evita el
        // reintento espurio: sin esta comprobacion, un fotograma viejo con marca
        // de tiempo posterior al limite hace creer a la recogida que ya llego al
        // final, y se lanza un segundo salto innecesario.
        if (pendingFrame_.generation != presenterGeneration_) {
            pendingFrame_ = VideoFrame{};
            continue;
        }

        const Micros pts = pendingFrame_.pts;

        if (collectBackward_) {
            // Hemos alcanzado el fotograma actual: el que buscabamos es el
            // ultimo que entro en el historial. pendingFrame_ NO se consume, se
            // deja en su sitio como siguiente de la secuencia.
            if (pts != kNoTimestamp && pts >= collectBefore_) {
                if (history_.empty()) {
                    // El salto no llego lo bastante atras (fotograma clave muy
                    // cercano, o tasa variable con un hueco enorme). Se amplia
                    // la ventana y se reintenta.
                    if (collectFrom_ <= 0) {
                        // Ya estabamos en el principio del archivo: no hay
                        // fotograma anterior que mostrar.
                        stepPending_.store(false, std::memory_order_release);
                        return FrameSelection::None;
                    }

                    // Duplicar la ventana. El suelo importa: si por lo que
                    // fuera llegase vacia, el destino coincidiria con el limite
                    // y el reintento repetiria el mismo salto para siempre.
                    Micros window = collectBefore_ - collectFrom_;
                    if (window < kMinRewindWindow) window = kMinRewindWindow;
                    window *= 2;

                    collectFrom_ = collectBefore_ > window ? collectBefore_ - window : 0;
                    pendingFrame_ = VideoFrame{};
                    RequestSeekInternal(collectFrom_);
                    presenterGeneration_ = generation_.load(std::memory_order_acquire);
                    return FrameSelection::None;
                }

                historyCursor_ = history_.size() - 1;
                out = CloneFrameRef(history_[historyCursor_]);
                if (!out.IsValid()) return FrameSelection::None;

                if (out.pts != kNoTimestamp) clock_.Reset(out.pts);
                NoteDisplayedFrame(out);
                stepPending_.store(false, std::memory_order_release);

                PYXIS_DEBUG("Paso -1 -> {} ms (rebobinado, historial {}/{})",
                            out.pts / 1000, historyCursor_ + 1, history_.size());
                return FrameSelection::Updated;
            }

            // Un fotograma sin marca de tiempo no se puede situar respecto al
            // limite, asi que el paso atras no tendria forma de terminar. Es
            // propio de flujos crudos sin contenedor; se cancela en vez de
            // girar indefinidamente.
            if (pts == kNoTimestamp) {
                PYXIS_DEBUG("paso atras cancelado: el flujo no tiene marcas de tiempo");
                pendingFrame_ = VideoFrame{};
                stepPending_.store(false, std::memory_order_release);
                return FrameSelection::None;
            }

            // Anterior al actual: se guarda en el historial y se sigue. Esto es
            // lo que convierte un salto en varios pasos atras gratis.
            if (pts >= collectFrom_) PushHistory(pendingFrame_);

            pendingFrame_ = VideoFrame{};
            continue;
        }

        // Adelante: lo anterior al corte se descarta sin mostrarse.
        if (pts != kNoTimestamp && collectFrom_ != kNoTimestamp && pts < collectFrom_) {
            pendingFrame_ = VideoFrame{};
            continue;
        }

        PushHistory(pendingFrame_);

        out = std::move(pendingFrame_);
        pendingFrame_ = VideoFrame{};

        // El reloj se planta exactamente en el fotograma mostrado. Asi la barra
        // de progreso acompana al avance manual y, al reanudar, la reproduccion
        // continua desde aqui y no desde donde estaba antes de empezar a pasar
        // fotogramas.
        if (out.pts != kNoTimestamp) clock_.Reset(out.pts);

        NoteDisplayedFrame(out);
        stepPending_.store(false, std::memory_order_release);

        PYXIS_DEBUG("Paso +1 -> {} ms (decodificado, historial {}/{})",
                    out.pts / 1000, historyCursor_ + 1, history_.size());
        return FrameSelection::Updated;
    }
}

void Player::StepFrame(int direction) {
    if (!HasVideo() || direction == 0) return;

    const PlayerState current = state_.load(std::memory_order_acquire);
    if (current == PlayerState::Idle || current == PlayerState::Failed) return;

    // Si el paso anterior aun no ha aterrizado, este se descarta.
    //
    // Es LA condicion que evita que el avance se quede clavado. Retroceder
    // exige rebobinar al fotograma clave y redecodificar hacia delante, y a 8K
    // eso tarda mas que el intervalo entre pulsaciones. Sin esta guarda, cada
    // tecla reinicia la recogida desde cero -vaciando el historial que se
    // estaba repoblando- y la secuencia no llega nunca al final: el reproductor
    // encadena saltos al mismo punto sin mostrar un solo fotograma.
    //
    // Descartar la pulsacion es lo correcto: el paso en vuelo ya va en esa
    // direccion, y quien mantenga la tecla recibira el siguiente en cuanto
    // este aterrice.
    if (stepPending_.load(std::memory_order_acquire)) return;

    // Avanzar de uno en uno implica pausa: no tiene sentido pedir un fotograma
    // concreto mientras el reloj sigue corriendo por debajo.
    Pause();

    // Al entrar en modo paso se sueltan las colas de audio. El demultiplexor
    // dejara de alimentarlas (ver steppingMode_), asi que dejarlas llenas solo
    // retendria memoria y mantendria bloqueado al decodificador de audio.
    if (!steppingMode_.exchange(true, std::memory_order_acq_rel)) {
        audioPackets_.Flush();
        audio_.Flush();
    }

    // Aqui solo se deja la peticion. Decidir si se sirve del historial o si hay
    // que rebobinar le corresponde al hilo de presentacion, que es el dueno del
    // historial; resolverlo desde este hilo seria una carrera con el dibujado.
    stepRequest_.store(direction, std::memory_order_release);
    stepPending_.store(true, std::memory_order_release);
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

    // Salir del modo paso exige resincronizar: el audio dejo de leerse donde se
    // pauso, y a estas alturas el video puede estar segundos por delante. Un
    // salto a la posicion del fotograma en pantalla deja las dos pistas
    // alineadas y es el unico punto de partida que el usuario espera.
    if (steppingMode_.exchange(false, std::memory_order_acq_rel)) {
        const Micros shown = displayedPts_.load(std::memory_order_relaxed);
        if (shown != kNoTimestamp) RequestSeekInternal(shown);
    }

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

    const Micros total = Duration();
    if (total != kNoTimestamp) {
        target = std::clamp<Micros>(target, 0, total);
    } else if (target < 0) {
        target = 0;
    }

    // Un salto normal invalida cualquier paso en curso y saca del modo paso:
    // a partir de aqui el audio vuelve a alimentarse con normalidad.
    stepPending_.store(false, std::memory_order_release);
    steppingMode_.store(false, std::memory_order_release);
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

    // pendingFrame_ no se toca aqui: pertenece al hilo de presentacion, que lo
    // suelta solo al ver la generacion nueva (ver SelectFrame).

    // 3. Peticion al demultiplexor, que es el unico dueno del contenedor.
    // Se limpia aqui y no solo en el hilo de demultiplexado: entre la peticion
    // y su atencion hay una ventana en la que el avance manual creeria que ya no
    // puede llegar nada mas y cancelaria el paso.
    demuxFinished_.store(false, std::memory_order_release);

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
    const int clamped = std::clamp(rateMilli, 250, 4000);
    clock_.SetRateMilli(clamped);
    PYXIS_INFO("Velocidad de reproduccion: x{}.{:03}", clamped / 1000, clamped % 1000);
}

Micros Player::Position() const noexcept {
    const Micros position = clock_.Position();
    if (position < 0) return 0;

    const Micros total = Duration();
    if (total != kNoTimestamp && position > total) return total;
    return position;
}

Micros Player::Duration() const {
    std::lock_guard<std::mutex> lock(metadataMutex_);
    return duration_;
}

std::wstring Player::Title() const {
    std::lock_guard<std::mutex> lock(metadataMutex_);
    return title_;
}

AVRational Player::SampleAspectRatio() const {
    std::lock_guard<std::mutex> lock(metadataMutex_);
    return sampleAspect_;
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
