// ============================================================================
//  ClipExporter.hpp - Exportacion de un recorte CON encuadre y ajustes
//
//  Hermano de media/Trimmer, que copia el flujo tal cual. Se elige uno u otro
//  segun lo que haya que entregar:
//
//    sin ediciones  -> Trimmer: copia de paquetes, instantaneo y sin perdida.
//    con ediciones  -> este: decodificar, redibujar y volver a codificar.
//
//  Recortar el encuadre o tocar el color obliga a tocar los pixeles, y eso
//  significa recodificar. No hay atajo.
//
//  EL CODIFICADOR ES WINDOWS
//  -------------------------
//  Se usa h264_mf, el envoltorio de FFmpeg sobre los codificadores de Media
//  Foundation, que a su vez usan el motor de video de la GPU. Enlazar libx264
//  habria sido lo habitual, pero rompe la propiedad que define el proyecto: MF
//  ya viene con el sistema, como D3D11 o WASAPI.
//
//  La salida es H.264 8 bits en SDR. Si el original era HDR de 10 bits, el
//  mapeo de tonos que ya hace el shader para verlo en pantalla es tambien el
//  que se graba: lo que se exporta es lo que se estaba viendo.
//
//  RECURSOS PROPIOS
//  ----------------
//  Abre su propio demultiplexor, su propio decodificador y su propio
//  renderizador. Comparte unicamente el ID3D11Device -que esta protegido para
//  multihilo- porque es lo que permite que el fotograma decodificado llegue al
//  shader sin salir de la memoria de video. Exportar mientras se ve el video no
//  debe mover el cabezal ni robarle las vistas cacheadas al renderizador de
//  pantalla.
// ============================================================================
#pragma once

#include "core/Clock.hpp"
#include "render/Device.hpp"
#include "render/VideoRenderer.hpp"

#include <atomic>
#include <functional>
#include <string>

namespace pyxis {

struct ClipExportRequest {
    std::wstring inputPath;
    std::wstring outputPath;
    Micros       start = 0;
    Micros       end   = 0;

    CropRect         crop{};
    ImageAdjustments adjustments{};
};

struct ClipExportResult {
    bool         ok = false;
    std::wstring path;
    std::string  error;

    int           width  = 0;
    int           height = 0;
    std::uint64_t frames = 0;
};

// Exporta el intervalo aplicando encuadre y ajustes. Operacion bloqueante y
// larga: el llamante decide en que hilo corre y puede seguir el avance con
// `progress` (0 a 100) o abortarla levantando `cancel`.
[[nodiscard]] ClipExportResult ExportClip(Device& device,
                                          const ClipExportRequest& request,
                                          const std::function<void(int)>& progress,
                                          const std::atomic<bool>& cancel);

}  // namespace pyxis
