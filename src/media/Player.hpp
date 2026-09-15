// ============================================================================
//  Player.hpp - Orquestacion del pipeline de reproduccion
//
//  TOPOLOGIA DE HILOS
//  ------------------
//      [demultiplexado] --paquetes--> [decod. video] --fotogramas--> (cola)
//                       \--paquetes--> [decod. audio] --bloques----> WASAPI
//
//  Cuatro hilos, uno por etapa, unidos por colas ACOTADAS. El limite de cada
//  cola es lo que impide que una etapa rapida devore la memoria: un archivo 8K
//  se demultiplexa mucho mas deprisa de lo que se decodifica, y sin tope el
//  demultiplexor llenaria varios gigabytes en segundos.
//
//  La presentacion NO vive aqui. El reproductor expone SelectFrame() y es el
//  hilo de presentacion (en ui/Controller) quien decide cuando llamarlo. Asi
//  el pipeline no sabe nada de ventanas ni de vsync.
//
//  SALTOS DE POSICION: EL CONTADOR DE GENERACION
//  ---------------------------------------------
//  Vaciar las colas no basta. Cuando se solicita un salto, cada hilo puede
//  tener ya en la mano un paquete de la posicion ANTERIOR, y el decodificador
//  guarda ademas fotogramas de referencia internos. Llamar a
//  avcodec_flush_buffers desde el hilo que pide el salto seria una carrera con
//  el hilo que esta decodificando.
//
//  La solucion es un contador que se incrementa en cada salto. Cada paquete y
//  cada fotograma viajan etiquetados con la generacion en que nacieron. Cuando
//  un hilo ve una etiqueta distinta de la suya, sabe que debe vaciar SU propio
//  decodificador y adoptar la nueva. Ningun hilo toca el estado de otro y no
//  hace falta ningun cerrojo entre ellos.
// ============================================================================
#pragma once

#include "audio/WasapiRenderer.hpp"
#include "core/Clock.hpp"
#include "core/Queue.hpp"
#include "media/AudioDecoder.hpp"
#include "media/Demuxer.hpp"
#include "media/Frame.hpp"
#include "media/VideoDecoder.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace pyxis {

enum class PlayerState {
    Idle,      // sin medio
    Playing,
    Paused,
    Ended,     // se llego al final
    Failed,    // error irrecuperable
};

// Instantanea de diagnostico. Se lee desde el hilo de interfaz, asi que todos
// los campos provienen de atomicos o de copias protegidas.
struct PlayerStats {
    std::string decoderName;
    bool        hardwareDecoding = false;
    int         width  = 0;
    int         height = 0;
    std::string pixelFormat;
    std::string containerName;

    std::uint64_t framesDecoded  = 0;
    std::uint64_t framesDropped  = 0;
    std::uint64_t framesLate     = 0;
    std::uint64_t audioUnderruns = 0;

    std::size_t videoPacketQueue = 0;
    std::size_t audioPacketQueue = 0;
    std::size_t videoFrameQueue  = 0;

    double measuredFps = 0.0;
    bool   isHdr       = false;
};

class Player {
public:
    Player() = default;
    ~Player();

    Player(const Player&)            = delete;
    Player& operator=(const Player&) = delete;

    // El dispositivo D3D11 se comparte con el decodificador para el zero-copy.
    void Initialize(ID3D11Device* device, ID3D11DeviceContext* context);

    void Open(const std::wstring& path);
    void Close() noexcept;

    void Play();
    void Pause();
    void TogglePause();

    // Salto absoluto y relativo. Ambos son asincronos: vuelven de inmediato y
    // el pipeline se reconfigura en sus propios hilos.
    void Seek(Micros target);
    void SeekRelative(Micros delta);

    void SetVolume(float volume);
    void SetMuted(bool muted);
    void SetRateMilli(int rateMilli);

    [[nodiscard]] float Volume() const noexcept { return audio_.Volume(); }
    [[nodiscard]] bool  Muted() const noexcept { return audio_.Muted(); }
    [[nodiscard]] int   RateMilli() const noexcept { return clock_.RateMilli(); }

    [[nodiscard]] PlayerState State() const noexcept {
        return state_.load(std::memory_order_acquire);
    }
    [[nodiscard]] Micros Position() const noexcept;
    [[nodiscard]] Micros Duration() const noexcept { return duration_; }
    [[nodiscard]] bool   HasVideo() const noexcept { return hasVideo_; }
    [[nodiscard]] bool   HasAudio() const noexcept { return hasAudio_; }
    [[nodiscard]] const std::wstring& Title() const noexcept { return title_; }
    [[nodiscard]] AVRational SampleAspectRatio() const noexcept { return sampleAspect_; }

    // Mensaje del ultimo error, si el estado es Failed.
    [[nodiscard]] std::string LastError() const;

    enum class FrameSelection {
        None,       // no hay fotograma nuevo; conservar el anterior
        Updated,    // `out` trae un fotograma nuevo
    };

    // Elige el fotograma que corresponde al instante actual del reloj maestro,
    // descartando los que ya pasaron. Lo llama el hilo de presentacion.
    [[nodiscard]] FrameSelection SelectFrame(VideoFrame& out);

    [[nodiscard]] PlayerStats Stats() const;

private:
    void StartThreads();
    void StopThreads() noexcept;

    void DemuxThread();
    void VideoDecodeThread();
    void AudioDecodeThread();

    void RequestSeekInternal(Micros target);
    void SetFailed(const std::string& message);

    // Un paquete etiquetado con la generacion en que se leyo.
    struct TaggedPacket {
        av::PacketPtr packet;
        std::uint32_t generation = 0;
        bool          endOfFile  = false;
    };

    // Componentes del pipeline
    Demuxer        demuxer_;
    VideoDecoder   videoDecoder_;
    AudioDecoder   audioDecoder_;
    WasapiRenderer audio_;
    MediaClock     clock_;

    ID3D11Device*        device_        = nullptr;
    ID3D11DeviceContext* deviceContext_ = nullptr;

    // Colas. Los limites se eligieron midiendo: 64 paquetes de video son unos
    // 2-3 segundos de holgura incluso con GOP largos, y 6 fotogramas
    // decodificados bastan para absorber un hipo del planificador sin inflar la
    // memoria de video (a 8K cada fotograma son ~50 MiB).
    BoundedQueue<TaggedPacket> videoPackets_{64};
    BoundedQueue<TaggedPacket> audioPackets_{128};
    BoundedQueue<VideoFrame>   videoFrames_{6};

    std::thread demuxThread_;
    std::thread videoThread_;
    std::thread audioThread_;

    std::atomic<bool>        running_{false};
    std::atomic<PlayerState> state_{PlayerState::Idle};

    // Coordinacion de saltos
    std::atomic<std::uint32_t> generation_{0};
    std::atomic<bool>          seekPending_{false};
    std::atomic<Micros>        seekTarget_{0};
    std::atomic<bool>          demuxFinished_{false};

    // Fotograma ya extraido de la cola pero cuyo momento aun no ha llegado.
    VideoFrame pendingFrame_;

    Micros       duration_     = kNoTimestamp;
    AVRational   sampleAspect_ = AVRational{1, 1};
    bool         hasVideo_     = false;
    bool         hasAudio_     = false;
    std::wstring title_;

    // Metricas
    std::atomic<std::uint64_t> framesDecoded_{0};
    std::atomic<std::uint64_t> framesDropped_{0};
    std::atomic<std::uint64_t> framesLate_{0};

    // Medicion de la tasa real de presentacion
    Micros              fpsWindowStart_ = 0;
    std::uint64_t       fpsWindowCount_ = 0;
    std::atomic<double> measuredFps_{0.0};

    mutable std::mutex errorMutex_;
    std::string        lastError_;
};

}  // namespace pyxis
