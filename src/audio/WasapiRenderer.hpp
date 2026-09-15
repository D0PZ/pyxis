// ============================================================================
//  WasapiRenderer.hpp - Salida de audio y reloj maestro de reproduccion
//
//  POR QUE EL AUDIO MANDA
//  ----------------------
//  La tarjeta de sonido consume muestras a una velocidad que ella decide y que
//  no coincide exactamente con el cristal del sistema: un dispositivo "a 48000
//  Hz" puede ir en realidad a 47999,7. Esa deriva es de decenas de partes por
//  millon, insignificante durante un segundo y de varios fotogramas al cabo de
//  dos horas.
//
//  Hay dos formas de resolverlo: estirar el audio para que siga al reloj del
//  sistema, o mover el video para que siga al audio. La primera es audible
//  -el oido detecta un salto de 10 ms, y cualquier remuestreo deja artefactos-
//  mientras que la segunda solo repite o descarta un fotograma de vez en
//  cuando, algo que el ojo no percibe.
//
//  Por eso este componente ANCLA el MediaClock: publica "en este instante real
//  se esta oyendo esta posicion del medio" y el renderizador de video se
//  limita a obedecer.
//
//  MODO DIRIGIDO POR EVENTOS
//  -------------------------
//  WASAPI avisa con un evento cada vez que necesita datos, en lugar de que el
//  programa sondee. Combinado con MMCSS "Pro Audio", el hilo despierta con
//  garantia de planificacion. Sondear con Sleep produciria chasquidos en cuanto
//  el sistema tuviera carga.
// ============================================================================
#pragma once

#include "core/Clock.hpp"
#include "core/Queue.hpp"
#include "media/Frame.hpp"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <atomic>
#include <thread>
#include <vector>

namespace pyxis {

class WasapiRenderer {
public:
    struct Format {
        int sampleRate = 48000;
        int channels   = 2;
    };

    WasapiRenderer() = default;
    ~WasapiRenderer();

    WasapiRenderer(const WasapiRenderer&)            = delete;
    WasapiRenderer& operator=(const WasapiRenderer&) = delete;

    // Abre el dispositivo predeterminado y negocia el formato. El decodificador
    // de audio debe configurarse con el formato que devuelve GetFormat().
    void Open();
    void Close() noexcept;

    [[nodiscard]] Format GetFormat() const noexcept { return format_; }
    [[nodiscard]] bool   IsOpen() const noexcept { return client_ != nullptr; }

    // Arranca el hilo de renderizado. `clock` recibe el anclaje temporal.
    void Start(MediaClock& clock);
    void Stop() noexcept;

    void SetPaused(bool paused);

    // Cola de entrada: la llena el hilo de decodificacion de audio.
    [[nodiscard]] BoundedQueue<AudioBuffer>& Queue() noexcept { return queue_; }

    // Descarta lo que haya en vuelo tras un salto de posicion.
    //
    // Se puede llamar desde cualquier hilo: lo unico que hace es vaciar la cola
    // (que tiene su propio cerrojo) y dejar una peticion. El trabajo de verdad
    // -soltar el bufer a medias y reiniciar el flujo- lo hace el hilo de audio.
    //
    // Antes esto se hacia aqui mismo, y era un error de los que no perdonan:
    // IAudioClient::Reset exige que el flujo este parado, y llamar a
    // Stop/Reset/Start mientras el hilo de audio esta dentro de GetBuffer es
    // una carrera con el controlador. Ademas pending_ y writePts_ son estado
    // exclusivo de ese hilo.
    void Flush();

    void SetVolume(float volume) noexcept;
    void SetMuted(bool muted) noexcept;
    [[nodiscard]] float Volume() const noexcept {
        return volume_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool Muted() const noexcept {
        return muted_.load(std::memory_order_relaxed);
    }

    // Cierto si Windows invalido el dispositivo (se desconecto el auricular, se
    // cambio el dispositivo predeterminado). El reproductor lo consulta para
    // reabrir el audio sin interrumpir el video.
    [[nodiscard]] bool DeviceLost() const noexcept {
        return deviceLost_.load(std::memory_order_acquire);
    }

    // Numero de veces que el hilo se quedo sin datos y tuvo que emitir
    // silencio. Es la metrica que delata un pipeline mal dimensionado.
    [[nodiscard]] std::uint64_t Underruns() const noexcept {
        return underruns_.load(std::memory_order_relaxed);
    }

private:
    void RenderThread(MediaClock& clock);

    // Rellena `destination` con `frameCount` fotogramas de la cola. Devuelve
    // cuantos se escribieron de verdad; el resto lo rellena el llamante con
    // silencio.
    [[nodiscard]] std::uint32_t FillBuffer(std::uint8_t* destination,
                                           std::uint32_t frameCount);

    void PublishClock(MediaClock& clock);

    // Atiende una peticion de vaciado. Solo se llama desde el hilo de audio.
    void ApplyPendingFlush();

    template <typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    ComPtr<IMMDeviceEnumerator> enumerator_;
    ComPtr<IMMDevice>           device_;
    ComPtr<IAudioClient>        client_;
    ComPtr<IAudioRenderClient>  renderClient_;

    HANDLE bufferEvent_ = nullptr;   // WASAPI pide datos
    HANDLE stopEvent_   = nullptr;   // peticion de apagado

    std::thread thread_;

    Format        format_{};
    bool          outputIsFloat_ = true;   // float32 o int16
    std::uint32_t bufferFrames_  = 0;
    Micros        deviceLatency_ = 0;

    BoundedQueue<AudioBuffer> queue_{64};

    // Resto del bufer de la cola que no cupo en la peticion anterior de WASAPI.
    AudioBuffer  pending_;
    std::size_t  pendingOffset_ = 0;   // en fotogramas

    // Marca de tiempo de la siguiente muestra que se escribira. De aqui sale la
    // posicion que se publica en el reloj.
    Micros writePts_ = kNoTimestamp;

    std::atomic<float>         volume_{1.0f};
    std::atomic<bool>          muted_{false};
    std::atomic<bool>          paused_{true};
    std::atomic<bool>          running_{false};
    std::atomic<bool>          deviceLost_{false};
    std::atomic<bool>          flushRequested_{false};
    std::atomic<std::uint64_t> underruns_{0};
};

}  // namespace pyxis
