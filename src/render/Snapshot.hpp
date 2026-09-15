// ============================================================================
//  Snapshot.hpp - Captura del fotograma en pantalla a PNG
//
//  La captura NO es una copia del bufer trasero. Eso daria la resolucion de la
//  ventana, con las bandas negras, el zoom aplicado y la interfaz encima. Lo
//  que se guarda es el fotograma REDIBUJADO a su resolucion nativa: si el video
//  es 8K, el PNG sale a 8K aunque la ventana midiera 1280x720.
//
//  La codificacion usa WIC, que forma parte de Windows. Tentador seria enlazar
//  libpng, pero eso romperia la propiedad que define este proyecto: el binario
//  no depende de nada que no venga con el sistema.
// ============================================================================
#pragma once

#include "core/Clock.hpp"
#include "render/Device.hpp"

#include <string>

namespace pyxis {

// Guarda una textura BGRA de 32 bits como PNG. Lanza pyxis::Exception si falla.
//
// El llamante debe estar en un hilo con COM inicializado: WIC se instancia por
// CoCreateInstance.
void SaveTextureAsPng(Device& device, ID3D11Texture2D* texture,
                      const std::wstring& path);

// Compone la ruta de destino: Imagenes\Pyxis\<medio>_<posicion>.png
//
// El nombre lleva la posicion del fotograma, no la hora del reloj: al revisar
// una secuencia, saber de que instante del video salio cada captura es mucho
// mas util que saber a que hora se tomo. Crea el directorio si no existe.
[[nodiscard]] std::wstring BuildSnapshotPath(const std::wstring& mediaTitle,
                                             Micros position);

}  // namespace pyxis
