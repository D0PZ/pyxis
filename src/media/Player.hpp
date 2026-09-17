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
#include <deque>
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

    // Los metadatos los escribe Open/Close desde el hilo de interfaz y los lee
    // el de presentacion en cada fotograma, asi que van bajo cerrojo. Title()
    // devuelve una COPIA: entregar una referencia a un std::wstring que otro
    // hilo puede reasignar es una carrera de manual.
    [[nodiscard]] Micros       Duration() const;
    [[nodiscard]] std::wstring Title() const;
    [[nodiscard]] AVRational   SampleAspectRatio() const;

    // Estos dos se consultan en los bucles calientes de los hilos de
    // decodificacion, de ahi que sean atomicos en lugar de ir con los demas.
    [[nodiscard]] bool HasVideo() const noexcept {
        return hasVideo_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool HasAudio() const noexcept {
        return hasAudio_.load(std::memory_order_acquire);
    }

    // Cierto mientras el demultiplexor no ha atendido aun el ultimo salto. La
    // interfaz lo usa para no encadenar saltos mas rapido de lo que el pipeline
    // puede absorberlos al arrastrar la barra de progreso.
    [[nodiscard]] bool SeekPending() const noexcept {
        return seekPending_.load(std::memory_order_acquire);
    }

    // Dimensiones codificadas del video. Solo cambian al abrir un medio, asi
    // que el hilo de interfaz puede leerlas para calcular el encuadre.
    [[nodiscard]] int VideoWidth() const noexcept { return videoDecoder_.Width(); }
    [[nodiscard]] int VideoHeight() const noexcept { return videoDecoder_.Height(); }

    // Mensaje del ultimo error, si el estado es Failed.
    [[nodiscard]] std::string LastError() const;

    // ---- Avance fotograma a fotograma ------------------------------------
    //
    //  Adelante es barato: el siguiente fotograma ya viene de camino en la
    //  cola, basta con tomarlo sin esperar al reloj.
    //
    //  Atras es caro, y no por descuido. Un codec inter-fotograma solo puede
    //  empezar a decodificar en un fotograma clave, asi que retroceder UNO
    //  obliga a rebobinar hasta la clave anterior y redecodificar hacia
    //  delante hasta el objetivo. Con GOP corto es inmediato; con GOP largo
    //  (dos segundos es habitual en HEVC) puede costar decenas de fotogramas
    //  de trabajo. Por eso StepPending() existe: quien mantenga la tecla
    //  pulsada debe esperar a que el paso anterior aterrice en lugar de
    //  encolar peticiones que el decodificador no puede absorber.
    //  Para que retroceder no se sienta, los fotogramas ya mostrados se
    //  conservan en un HISTORIAL. No cuesta memoria de verdad: guardar un
    //  fotograma es un av_frame_ref, que solo incrementa un contador; la
    //  textura es la misma. Lo unico que hay que pagar son plazas extra en el
    //  pool del decodificador, y por eso el tamano del historial se calcula a
    //  partir de la resolucion (ver ComputeHistoryLimit).
    //
    //  Cuando el historial no alcanza, el salto recupera un LOTE completo en
    //  una sola pasada en lugar de un unico fotograma: una busqueda cada N
    //  pasos en vez de una por paso.
    void StepFrame(int direction);   // -1 atras, +1 adelante

    // Cierto mientras un paso solicitado aun no ha llegado a pantalla.
    [[nodiscard]] bool StepPending() const noexcept {
        return stepPending_.load(std::memory_order_acquire);
    }

    // Marca de tiempo del fotograma que hay en pantalla ahora mismo.
    [[nodiscard]] Micros DisplayedPts() const noexcept {
        return displayedPts_.load(std::memory_order_relaxed);
    }

    // Duracion del fotograma actual. Es lo que mide un "paso".
    [[nodiscard]] Micros DisplayedFrameDuration() const noexcept {
        return displayedDuration_.load(std::memory_order_relaxed);
    }

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

    // Rama de SelectFrame cuando hay un paso pendiente: ignora el reloj y
    // busca el fotograma concreto que se pidio.
    [[nodiscard]] FrameSelection SelectSteppedFrame(VideoFrame& out);
    void NoteDisplayedFrame(const VideoFrame& frame) noexcept;

    // Historial. Todo esto pertenece en exclusiva al hilo de presentacion.
    [[nodiscard]] static VideoFrame CloneFrameRef(const VideoFrame& source);
    [[nodiscard]] std::size_t ComputeHistoryLimit(int width, int height,
                                                 std::size_t reservedFrames) const noexcept;

    void PushHistory(const VideoFrame& frame);
    void ForgetHistory() noexcept;

    // Intenta satisfacer el paso con lo que ya hay en memoria.
    [[nodiscard]] bool ResolveStepFromHistory(int direction, VideoFrame& out);

    // Prepara la recogida desde la cola cuando el historial no alcanza.
    void PrepareStepCollection(int direction);

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

    // Modo de avance manual. Mientras esta activo, el demultiplexor DEJA DE
    // encolar audio.
    //
    // No es un capricho: en pausa el renderizador de audio no consume, su cola
    // se llena, el decodificador de audio se bloquea al empujar, la cola de
    // paquetes de audio se llena tambien y el demultiplexor acaba dormido
    // dentro de un Push de audio. A partir de ahi deja de alimentar video, y el
    // avance fotograma a fotograma se clava en cuanto agota lo que hubiera
    // precargado. Con la reproduccion normal el problema no existe porque el
    // audio se consume; solo aparece al consumir video sin consumir audio.
    //
    // Al volver a reproducir se resincroniza con un salto a la posicion del
    // fotograma mostrado, que es ademas lo correcto: el audio que estaba
    // encolado pertenecia a donde se pauso, no a donde se ha llegado pasando
    // fotogramas.
    std::atomic<bool> steppingMode_{false};

    // Fotograma ya extraido de la cola pero cuyo momento aun no ha llegado.
    VideoFrame pendingFrame_;

    // Coordinacion del avance por fotogramas. `stepLowerBound_` es la marca de
    // tiempo minima que debe tener el fotograma buscado: los que lleguen antes
    // se descartan sin mostrarse.
    std::atomic<bool> stepPending_{false};

    // Direccion de un paso aun sin resolver. La interfaz solo deja la peticion;
    // quien decide si se sirve desde el historial o hace falta rebobinar es el
    // hilo de presentacion, que es el dueno del historial.
    std::atomic<int> stepRequest_{0};

    // Recogida en curso (hilo de presentacion).
    //
    //  Los limites se expresan como COMPARACIONES con la marca de tiempo, no
    //  como un objetivo calculado. La diferencia importa: en material de tasa
    //  variable -grabaciones de movil, por ejemplo- dos fotogramas consecutivos
    //  pueden estar a 33 ms o a 266 ms, asi que "posicion menos una duracion"
    //  no identifica al fotograma anterior. Preguntar por "el ultimo que hay
    //  antes del actual" si funciona siempre.
    //
    //      adelante : el primero con pts >= collectFrom_
    //      atras    : el ultimo con pts < collectBefore_
    Micros collectFrom_     = kNoTimestamp;
    Micros collectBefore_   = kNoTimestamp;
    bool   collectBackward_ = false;

    std::deque<VideoFrame> history_;          // del mas antiguo al mas reciente
    std::size_t            historyCursor_ = 0;
    std::atomic<std::size_t> historyLimit_{0};

    // Estado del fotograma en pantalla. Lo escribe el hilo de presentacion y lo
    // lee el de interfaz para decidir a donde saltar en el siguiente paso.
    std::atomic<Micros> displayedPts_{kNoTimestamp};
    std::atomic<Micros> displayedDuration_{0};

    mutable std::mutex metadataMutex_;
    Micros       duration_     = kNoTimestamp;
    AVRational   sampleAspect_ = AVRational{1, 1};
    std::wstring title_;

    std::atomic<bool> hasVideo_{false};
    std::atomic<bool> hasAudio_{false};

    // Generacion que el hilo de presentacion vio la ultima vez. Solo la toca
    // ese hilo, y es como descubre que el fotograma que tenia reservado ya no
    // corresponde a la posicion actual.
    std::uint32_t presenterGeneration_ = 0;

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
