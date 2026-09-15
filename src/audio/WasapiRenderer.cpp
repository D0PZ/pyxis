#include "audio/WasapiRenderer.hpp"

#include "core/Error.hpp"
#include "core/Log.hpp"
#include "core/Thread.hpp"

#include <mmreg.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

namespace pyxis {
namespace {

// WASAPI mide el tiempo en unidades de 100 nanosegundos ("REFERENCE_TIME").
constexpr Micros FromReferenceTime(REFERENCE_TIME value) noexcept {
    return value / 10;
}

// Libera la memoria que devuelve GetMixFormat, que se reserva con CoTaskMemAlloc.
struct CoTaskMemDeleter {
    void operator()(void* p) const noexcept { ::CoTaskMemFree(p); }
};
using WaveFormatPtr = std::unique_ptr<WAVEFORMATEX, CoTaskMemDeleter>;

// El formato de mezcla puede venir como WAVEFORMATEXTENSIBLE, en cuyo caso el
// tipo real esta en el SubFormat y no en wFormatTag.
//
// Se compara solo Data1 en lugar de usar KSDATAFORMAT_SUBTYPE_IEEE_FLOAT: toda
// esa familia de GUID tiene la forma {0000xxxx-0000-0010-8000-00AA00389B71},
// donde xxxx es precisamente la etiqueta de formato WAVE_FORMAT_*. Asi se evita
// arrastrar ksmedia.h, que no es autocontenido.
bool IsFloatFormat(const WAVEFORMATEX* format) noexcept {
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        return extensible->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT;
    }
    return false;
}

}  // namespace

WasapiRenderer::~WasapiRenderer() {
    Close();
}

// ---------------------------------------------------------------------------
//  Apertura del dispositivo
// ---------------------------------------------------------------------------
void WasapiRenderer::Open() {
    Close();

    PYXIS_CHECK_HR(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      IID_PPV_ARGS(&enumerator_)),
                   "no se pudo crear el enumerador de dispositivos de audio");

    PYXIS_CHECK_HR(enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_),
                   "no hay ningun dispositivo de reproduccion de audio disponible");

    PYXIS_CHECK_HR(device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                     reinterpret_cast<void**>(client_.GetAddressOf())),
                   "no se pudo activar el cliente de audio");

    WAVEFORMATEX* rawFormat = nullptr;
    PYXIS_CHECK_HR(client_->GetMixFormat(&rawFormat),
                   "no se pudo consultar el formato de mezcla");
    WaveFormatPtr mixFormat(rawFormat);

    // Se adopta el formato del motor de audio en lugar de imponer uno propio.
    // En modo compartido, cualquier otro formato obligaria a Windows a
    // reconvertir, anadiendo latencia y una etapa de remuestreo que ya hace
    // mejor el decodificador con swresample.
    format_.sampleRate = static_cast<int>(mixFormat->nSamplesPerSec);
    format_.channels   = static_cast<int>(mixFormat->nChannels);
    outputIsFloat_     = IsFloatFormat(mixFormat.get());

    if (!outputIsFloat_ && mixFormat->wBitsPerSample != 16) {
        ThrowLogic("el dispositivo de audio usa un formato de muestra no soportado");
    }

    // Duracion 0 = periodo nativo del dispositivo (tipicamente 10 ms). Es el
    // minimo que WASAPI acepta en modo compartido dirigido por eventos, y la
    // cola de este componente ya absorbe los altibajos de la decodificacion.
    PYXIS_CHECK_HR(client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                       AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                       0, 0, mixFormat.get(), nullptr),
                   "no se pudo inicializar el flujo de audio");

    PYXIS_CHECK_HR(client_->GetBufferSize(&bufferFrames_),
                   "no se pudo consultar el tamano del bufer de audio");

    REFERENCE_TIME latency = 0;
    if (SUCCEEDED(client_->GetStreamLatency(&latency))) {
        deviceLatency_ = FromReferenceTime(latency);
    }

    PYXIS_CHECK_HR(client_->GetService(IID_PPV_ARGS(&renderClient_)),
                   "no se pudo obtener el servicio de renderizado de audio");

    bufferEvent_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    PYXIS_CHECK_WIN32(bufferEvent_ != nullptr, "no se pudo crear el evento de audio");

    stopEvent_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    PYXIS_CHECK_WIN32(stopEvent_ != nullptr, "no se pudo crear el evento de parada");

    PYXIS_CHECK_HR(client_->SetEventHandle(bufferEvent_),
                   "no se pudo asociar el evento al flujo de audio");

    deviceLost_.store(false, std::memory_order_release);

    PYXIS_INFO("Audio: {} Hz, {} canales, {}, bufer {} fotogramas ({} us), latencia {} us",
               format_.sampleRate, format_.channels,
               outputIsFloat_ ? "float32" : "int16",
               bufferFrames_,
               static_cast<Micros>(bufferFrames_) * kMicrosPerSecond / format_.sampleRate,
               deviceLatency_);
}

// ---------------------------------------------------------------------------
//  Hilo de renderizado
// ---------------------------------------------------------------------------
void WasapiRenderer::Start(MediaClock& clock) {
    if (running_.exchange(true, std::memory_order_acq_rel)) return;
    PYXIS_REQUIRE(client_ != nullptr, "WasapiRenderer::Start sin dispositivo abierto");

    ::ResetEvent(stopEvent_);
    thread_ = std::thread([this, &clock] { RenderThread(clock); });
}

void WasapiRenderer::Stop() noexcept {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;

    if (stopEvent_ != nullptr) ::SetEvent(stopEvent_);
    queue_.Close();

    if (thread_.joinable()) thread_.join();

    if (client_) client_->Stop();
    queue_.Reopen();
}

void WasapiRenderer::RenderThread(MediaClock& clock) {
    SetCurrentThreadName(L"pyxis-audio");

    // "Pro Audio" es la clase MMCSS de maxima prioridad. Un hilo de audio que
    // pierde su plazo produce un chasquido audible, no un fotograma perdido.
    const MmcssScope mmcss(MmcssTask::ProAudio);

    // El hilo de audio hace llamadas COM propias, asi que necesita su
    // apartamento. MULTITHREADED porque WASAPI no es de apartamento unico.
    const HRESULT comInit = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comInitialized = SUCCEEDED(comInit);

    // Se prellena el bufer con silencio antes de arrancar: si se arranca vacio,
    // WASAPI reproduce basura de la memoria antes de la primera entrega.
    BYTE* initial = nullptr;
    if (SUCCEEDED(renderClient_->GetBuffer(bufferFrames_, &initial))) {
        renderClient_->ReleaseBuffer(bufferFrames_, AUDCLNT_BUFFERFLAGS_SILENT);
    }

    const HRESULT startResult = client_->Start();
    if (FAILED(startResult)) {
        PYXIS_ERROR("no se pudo arrancar el flujo de audio: {}",
                    DescribeError(ErrorDomain::HResult, startResult));
        if (comInitialized) ::CoUninitialize();
        return;
    }

    const std::uint32_t bytesPerFrame =
        static_cast<std::uint32_t>(format_.channels) * (outputIsFloat_ ? 4u : 2u);

    HANDLE waitHandles[2] = {stopEvent_, bufferEvent_};

    while (running_.load(std::memory_order_acquire)) {
        const DWORD waited = ::WaitForMultipleObjects(2, waitHandles, FALSE, 2000);

        if (waited == WAIT_OBJECT_0) break;          // parada solicitada
        if (waited == WAIT_TIMEOUT) {
            // El dispositivo dejo de pedir datos. Casi siempre significa que
            // desaparecio; se confirma en la siguiente llamada.
            PYXIS_WARN("el dispositivo de audio no responde");
            continue;
        }
        if (waited != WAIT_OBJECT_0 + 1) break;

        UINT32 padding = 0;
        HRESULT hr = client_->GetCurrentPadding(&padding);
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
            PYXIS_WARN("el dispositivo de audio se invalido");
            deviceLost_.store(true, std::memory_order_release);
            break;
        }
        if (FAILED(hr)) continue;

        const UINT32 available = bufferFrames_ - padding;
        if (available == 0) continue;

        BYTE* destination = nullptr;
        hr = renderClient_->GetBuffer(available, &destination);
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
            deviceLost_.store(true, std::memory_order_release);
            break;
        }
        if (FAILED(hr)) continue;

        std::uint32_t written = 0;
        if (!paused_.load(std::memory_order_relaxed)) {
            written = FillBuffer(destination, available);
        }

        if (written < available) {
            // Silencio en lo que falte. Es preferible a repetir el bufer
            // anterior, que se oiria como un zumbido.
            std::memset(destination + static_cast<std::size_t>(written) * bytesPerFrame,
                        0,
                        static_cast<std::size_t>(available - written) * bytesPerFrame);

            if (written == 0 && !paused_.load(std::memory_order_relaxed)) {
                underruns_.fetch_add(1, std::memory_order_relaxed);
            }
        }

        renderClient_->ReleaseBuffer(available, 0);

        if (written > 0) PublishClock(clock);
    }

    client_->Stop();
    if (comInitialized) ::CoUninitialize();
}

// ---------------------------------------------------------------------------
//  Copia desde la cola al bufer de WASAPI
// ---------------------------------------------------------------------------
std::uint32_t WasapiRenderer::FillBuffer(std::uint8_t* destination,
                                         std::uint32_t frameCount) {
    const int channels = format_.channels;
    const float gain = muted_.load(std::memory_order_relaxed)
                           ? 0.0f
                           : volume_.load(std::memory_order_relaxed);

    std::uint32_t framesWritten = 0;

    while (framesWritten < frameCount) {
        // Se agota primero lo que quedo del bufer anterior.
        if (pendingOffset_ >= pending_.FrameCount()) {
            AudioBuffer next;
            if (!queue_.TryPop(next)) break;   // sin datos: el llamante pone silencio

            pending_       = std::move(next);
            pendingOffset_ = 0;

            // La marca de tiempo del bufer manda: reancla la escritura despues
            // de un salto o de un hueco en el flujo.
            if (pending_.pts != kNoTimestamp) writePts_ = pending_.pts;
        }

        const std::size_t availableFrames = pending_.FrameCount() - pendingOffset_;
        const std::uint32_t toCopy = static_cast<std::uint32_t>(
            std::min<std::size_t>(availableFrames, frameCount - framesWritten));

        const float* source =
            pending_.samples.data() + pendingOffset_ * static_cast<std::size_t>(channels);
        const std::size_t sampleCount = static_cast<std::size_t>(toCopy) *
                                        static_cast<std::size_t>(channels);

        if (outputIsFloat_) {
            auto* out = reinterpret_cast<float*>(destination) +
                        static_cast<std::size_t>(framesWritten) *
                            static_cast<std::size_t>(channels);

            if (gain == 1.0f) {
                std::memcpy(out, source, sampleCount * sizeof(float));
            } else {
                // Bucle sencillo a proposito: el auto-vectorizador de MSVC lo
                // convierte en SIMD, y una version manual con intrinsecos
                // costaria mantenimiento sin ganar nada medible.
                for (std::size_t i = 0; i < sampleCount; ++i) {
                    out[i] = source[i] * gain;
                }
            }
        } else {
            auto* out = reinterpret_cast<std::int16_t*>(destination) +
                        static_cast<std::size_t>(framesWritten) *
                            static_cast<std::size_t>(channels);

            for (std::size_t i = 0; i < sampleCount; ++i) {
                // Se sujeta antes de convertir: el audio puede superar 1.0 tras
                // la mezcla, y sin sujecion el entero desbordaria y sonaria a
                // distorsion violenta.
                const float sample = std::clamp(source[i] * gain, -1.0f, 1.0f);
                out[i] = static_cast<std::int16_t>(std::lround(sample * 32767.0f));
            }
        }

        pendingOffset_ += toCopy;
        framesWritten  += toCopy;

        if (writePts_ != kNoTimestamp) {
            writePts_ += static_cast<Micros>(toCopy) * kMicrosPerSecond / format_.sampleRate;
        }
    }

    return framesWritten;
}

// ---------------------------------------------------------------------------
//  Publicacion del reloj maestro
// ---------------------------------------------------------------------------
void WasapiRenderer::PublishClock(MediaClock& clock) {
    if (writePts_ == kNoTimestamp) return;

    UINT32 padding = 0;
    if (FAILED(client_->GetCurrentPadding(&padding))) return;

    // writePts_ apunta al final de lo ya entregado a WASAPI. Lo que el usuario
    // OYE en este instante va por detras: le faltan por sonar las muestras que
    // siguen en el bufer del dispositivo, mas la latencia del propio hardware.
    const Micros buffered =
        static_cast<Micros>(padding) * kMicrosPerSecond / format_.sampleRate;

    const Micros audible = writePts_ - buffered - deviceLatency_;
    clock.Anchor(audible, NowMicros());
}

// ---------------------------------------------------------------------------
//  Control
// ---------------------------------------------------------------------------
void WasapiRenderer::SetPaused(bool paused) {
    paused_.store(paused, std::memory_order_relaxed);
}

void WasapiRenderer::Flush() {
    queue_.Flush();
    pending_       = AudioBuffer{};
    pendingOffset_ = 0;
    writePts_      = kNoTimestamp;

    // Reiniciar el flujo descarta lo que el dispositivo tenga en su bufer. Sin
    // esto, tras un salto se oiria un fragmento de la posicion anterior.
    if (client_) {
        client_->Stop();
        client_->Reset();
        client_->Start();
    }
}

void WasapiRenderer::SetVolume(float volume) noexcept {
    volume_.store(std::clamp(volume, 0.0f, 1.0f), std::memory_order_relaxed);
}

void WasapiRenderer::SetMuted(bool muted) noexcept {
    muted_.store(muted, std::memory_order_relaxed);
}

void WasapiRenderer::Close() noexcept {
    Stop();

    renderClient_.Reset();
    client_.Reset();
    device_.Reset();
    enumerator_.Reset();

    if (bufferEvent_ != nullptr) {
        ::CloseHandle(bufferEvent_);
        bufferEvent_ = nullptr;
    }
    if (stopEvent_ != nullptr) {
        ::CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }

    pending_       = AudioBuffer{};
    pendingOffset_ = 0;
    writePts_      = kNoTimestamp;
    bufferFrames_  = 0;
}

}  // namespace pyxis
