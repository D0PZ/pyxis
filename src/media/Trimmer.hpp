// ============================================================================
//  Trimmer.hpp - Recorte de un intervalo a un archivo nuevo
//
//  COPIA DE FLUJO, NO RECODIFICACION
//  ---------------------------------
//  Los paquetes se copian tal cual del contenedor de origen al de destino. No
//  se decodifica ni se vuelve a codificar nada, con tres consecuencias:
//
//    * Es casi instantaneo: recortar treinta segundos de un 8K a 150 Mbit/s es
//      mover unos cientos de megabytes de disco a disco.
//    * No hay perdida de calidad. Ni una generacion.
//    * El corte de entrada se ALINEA AL FOTOGRAMA CLAVE anterior.
//
//  Ese ultimo punto no es una limitacion de este codigo sino de como funciona
//  el video inter-fotograma: un decodificador no puede arrancar a mitad de un
//  grupo de imagenes. Cortar exactamente en el fotograma pedido exigiria
//  recodificar el primer grupo, y ademas este binario no lleva codificadores
//  H.264/HEVC -son los que arrastran dependencias y patentes-. El resultado se
//  informa con el inicio real para que no haya sorpresas.
//
//  Se abre el archivo POR SEGUNDA VEZ en lugar de reutilizar el demultiplexor
//  de la reproduccion: recortar mientras se ve el video no debe mover el
//  cabezal ni vaciar las colas del reproductor.
// ============================================================================
#pragma once

#include "core/Clock.hpp"

#include <string>

namespace pyxis {

struct TrimResult {
    bool         ok = false;
    std::wstring path;
    std::string  error;

    // Inicio real del recorte. Puede ser anterior al solicitado por el
    // alineamiento al fotograma clave.
    Micros actualStart = kNoTimestamp;
    Micros actualEnd   = kNoTimestamp;
};

// Copia [start, end] de `inputPath` a un archivo nuevo. Operacion bloqueante:
// el llamante decide en que hilo corre.
[[nodiscard]] TrimResult TrimToFile(const std::wstring& inputPath,
                                    const std::wstring& outputPath,
                                    Micros start, Micros end);

// Compone la ruta de destino: Videos\Pyxis\<medio>_corte_<inicio>-<fin>.<ext>
//
// Se conserva la extension del original a proposito: el contenedor de salida
// tiene que admitir los mismos codecs que el de entrada, y el que ya los
// contenia es la eleccion segura.
[[nodiscard]] std::wstring BuildClipPath(const std::wstring& mediaPath,
                                         Micros start, Micros end);

}  // namespace pyxis
