// ============================================================================
//  Demuxer.hpp - Apertura de contenedores y extraccion de paquetes
//
//  Componente PASIVO a proposito: no tiene hilos ni colas propias. Abre el
//  contenedor, expone las pistas y entrega paquetes cuando se le piden. Quien
//  decide cuando leer y donde poner el resultado es Player.
//
//  Separar la politica (Player) del mecanismo (Demuxer) hace que esta clase
//  sea trivial de razonar y de probar: es una funcion de archivo a paquetes.
// ============================================================================
#pragma once

#include "core/Clock.hpp"
#include "media/FFmpegUtil.hpp"

#include <string>

namespace pyxis {

class Demuxer {
public:
    struct Track {
        int                      index     = -1;
        AVRational               timeBase  = AVRational{0, 1};
        // Tasa de fotogramas promedio del contenedor. Sirve para estimar la
        // duracion de un fotograma cuando el flujo no la declara por fotograma.
        AVRational               frameRate = AVRational{0, 1};
        const AVCodecParameters* params    = nullptr;

        [[nodiscard]] bool Valid() const noexcept { return index >= 0; }
    };

    enum class ReadResult {
        Ok,           // se entrego un paquete
        EndOfFile,    // no hay mas datos
        Recoverable,  // fallo puntual de lectura; reintentar
        Fatal,        // el flujo ya no es utilizable
    };

    Demuxer() = default;
    ~Demuxer();

    Demuxer(const Demuxer&)            = delete;
    Demuxer& operator=(const Demuxer&) = delete;

    // Abre un archivo local o una URL. Lanza pyxis::Exception si falla.
    void Open(const std::wstring& path);
    void Close() noexcept;

    [[nodiscard]] bool IsOpen() const noexcept { return format_ != nullptr; }

    [[nodiscard]] const Track& Video() const noexcept { return video_; }
    [[nodiscard]] const Track& Audio() const noexcept { return audio_; }

    // Duracion total, o kNoTimestamp si el contenedor no la declara (flujos en
    // directo, por ejemplo).
    [[nodiscard]] Micros Duration() const noexcept { return duration_; }

    [[nodiscard]] const std::string& ContainerName() const noexcept { return containerName_; }
    [[nodiscard]] const std::wstring& Path() const noexcept { return path_; }

    // Relacion de aspecto del pixel declarada por el contenedor, o {1,1}.
    [[nodiscard]] AVRational SampleAspectRatio() const noexcept { return sampleAspect_; }

    // Lee el siguiente paquete de cualquier pista. El llamante es dueno de
    // `packet` y debe desreferenciarlo entre llamadas.
    [[nodiscard]] ReadResult Read(AVPacket* packet);

    // Salta a `target`. `backward` busca el fotograma clave anterior, que es
    // lo que se quiere al retroceder para no mostrar macrobloques rotos.
    void Seek(Micros target, bool backward);

    [[nodiscard]] AVFormatContext* Context() noexcept { return format_.get(); }

private:
    void SelectTracks();

    av::FormatContextPtr format_;
    Track                video_;
    Track                audio_;
    Micros               duration_     = kNoTimestamp;
    AVRational           sampleAspect_ = AVRational{1, 1};
    std::string          containerName_;
    std::wstring         path_;
};

}  // namespace pyxis
