<div align="center">

# Pyxis

**Reproductor de vídeo de alto rendimiento para Windows 11 x64**

Un solo `.exe`. Sin instalador, sin DLLs, sin códecs de la Store, sin VC++ Redistributable.

[![Licencia](https://img.shields.io/badge/licencia-GPL--3.0-blue)](LICENSE)
[![Plataforma](https://img.shields.io/badge/plataforma-Windows%2011%20x64-0078D4)](#requisitos)
[![C++](https://img.shields.io/badge/C%2B%2B-20-00599C)](#)

</div>

---

## Qué es

Pyxis reproduce vídeo en resoluciones altas —4K, 6K, 8K— con decodificación por
GPU y **zero-copy**: el fotograma decodificado nunca sale de la memoria de vídeo.
No se copia a RAM, no se vuelve a subir, no pasa por un conversor intermedio. Se
decodifica en una textura y esa misma textura la lee el shader que la dibuja.

A 8K 10 bits eso son unos 99 MB por fotograma que **no** cruzan el bus PCIe. A
60 fps, unos 6 GB/s de ancho de banda que no se gastan. Esa es la diferencia
entre reproducir 8K con holgura y no poder reproducirlo.

## Por qué existe

Los reproductores de Windows suelen elegir una de estas dos vías:

- **Apoyarse en Media Foundation**, y entonces el usuario necesita extensiones
  de la Microsoft Store para HEVC (de pago) y AV1, y no puede abrir un MKV.
- **Envolver libmpv o LAV Filters**, y entonces hay que distribuir una carpeta
  con decenas de DLLs y un instalador.

Pyxis toma la tercera: **FFmpeg enlazado estáticamente** más las APIs que ya
trae Windows (Direct3D 11, DXGI, Direct2D, WASAPI). Todos los códecs viven
dentro del ejecutable. Con la CRT también estática, el resultado es un archivo
suelto que se copia a un pendrive y funciona.

## Características

| | |
|---|---|
| **Decodificación** | D3D11VA zero-copy con repliegue automático a software |
| **Códecs** | H.264, HEVC, AV1 (dav1d), VP9, VP8, MPEG-2, VC-1, ProRes y el resto del catálogo de FFmpeg |
| **Contenedores** | MKV, MP4, MOV, AVI, WebM, TS, M2TS, FLV, y flujos HTTP/HLS |
| **Profundidad** | 8, 10 y 12 bits (NV12 / P010) |
| **HDR** | HDR10 (PQ) y HLG. Salida nativa PQ BT.2020 si el monitor lo admite; mapeo de tonos a SDR si no |
| **Color** | BT.601 / BT.709 / BT.2020, rango limitado y completo, relación de aspecto anamórfica |
| **Presentación** | DXGI en modo flip, 3 búferes, objeto de latencia, VRR disponible |
| **Audio** | WASAPI dirigido por eventos con prioridad MMCSS «Pro Audio» |
| **Sincronía** | Reloj maestro de audio; el vídeo descarta o repite fotogramas para seguirlo |
| **Interfaz** | Direct2D sobre textura intermedia, repintada solo cuando cambia |

## Requisitos

- Windows 11 x64 (build 22000 o posterior)
- GPU compatible con Direct3D 11 nivel 11.0
- Para decodificar por hardware, un adaptador con soporte D3D11VA del códec en
  cuestión. Si no lo hay, Pyxis decodifica por software sin avisar más que en el
  registro.

## Uso

```
pyxis [opciones] [archivo o URL]
```

| Opción | Efecto |
|---|---|
| `-f`, `--fullscreen` | arrancar a pantalla completa |
| `--no-hardware` | desactivar la decodificación por GPU (para diagnóstico) |
| `--volume N` | volumen inicial, de 0 a 100 |
| `-v`, `--verbose` | registro detallado |
| `--log ARCHIVO` | duplicar el registro en un archivo |
| `-h`, `--help` | ayuda |

### Controles

| Tecla | Acción |
|---|---|
| `Espacio` · `K` | reproducir / pausar |
| `←` `→` | **un fotograma** atrás / adelante — mantener pulsado avanza a 20 fps |
| `Mayús`+`←` `→` | ± 5 s |
| `Ctrl`+`←` `→` | ± 60 s |
| `J` `L` | ± 10 s |
| `0`–`9` | saltar al 0 %–90 % |
| `↑` `↓` · rueda | volumen |
| `M` | silencio |
| `F` · `F11` · doble clic | pantalla completa |
| `Ctrl`+rueda | **zoom anclado al puntero** |
| arrastrar | desplazar la imagen ampliada |
| `Z` | ajustar a la ventana |
| `X` | tamaño original (1:1) |
| `[` `]` | velocidad de reproducción |
| `Retroceso` | velocidad normal |
| `I` | panel de estadísticas |
| `O` | abrir archivo |
| `Q` · `Esc` | salir |

También acepta archivos arrastrados sobre la ventana.

### Avance fotograma a fotograma

Las flechas dan **exactamente un fotograma**. Al mantenerlas pulsadas se repite
a 20 fotogramas por segundo — un ritmo propio, no el de la autorrepetición del
teclado, para que revisar un plano se sienta igual en cualquier equipo.

Avanzar es inmediato: el siguiente fotograma ya viene de camino. **Retroceder
cuesta más**, y no por descuido: un códec inter-fotograma solo puede empezar a
decodificar en un fotograma clave, así que ir uno atrás obliga a rebobinar hasta
la clave anterior y redecodificar hacia delante. Con GOP corto es instantáneo;
con GOP largo (dos segundos es habitual en HEVC) el retroceso mantenido va más
despacio. Pyxis espera a que cada paso aterrice antes de pedir el siguiente, de
modo que el ritmo se ajusta solo a lo que la máquina aguanta en vez de acumular
peticiones.

### Zoom

`Ctrl`+rueda amplía **sobre el punto donde está el ratón**, no hacia el centro,
que es lo que hace falta para inspeccionar una esquina. Con la imagen ampliada,
arrastrar con el botón izquierdo la desplaza; un clic sin arrastre sigue siendo
pausa.

El zoom no se hace en el shader, sino agrandando el *viewport*. El rasterizador
recorta lo que se sale y el pixel shader solo se ejecuta sobre lo visible, así
que ampliar 8× cuesta lo mismo que llenar la ventana.

`Z` vuelve al ajuste a la ventana y `X` muestra el vídeo a tamaño original —un
píxel del vídeo por píxel de pantalla—, corrigiendo la relación de aspecto del
píxel en material anamórfico.

## Compilar

```powershell
git clone https://github.com/D0PZ/pyxis.git
cd pyxis
.\scripts\build.ps1
```

El script localiza Visual Studio, instala vcpkg si hace falta y compila. La
**primera** ejecución compila FFmpeg desde fuente y tarda entre 15 y 45 minutos;
las siguientes reutilizan la caché binaria de vcpkg y son casi instantáneas.

El resultado queda en `build\release\bin\pyxis.exe`.

<details>
<summary>Requisitos de compilación</summary>

- Visual Studio 2022 o posterior (o Build Tools) con la carga **Desarrollo para
  el escritorio con C++**
- Windows 11 SDK (aporta `fxc.exe` para los shaders)
- CMake 3.25+ · Ninja · vcpkg

Los tres últimos los instala el script si no están:

```powershell
winget install Kitware.CMake Ninja-build.Ninja
```

</details>

<details>
<summary>Configuraciones</summary>

```powershell
.\scripts\build.ps1 -Configuration debug            # capa de depuración D3D11
.\scripts\build.ps1 -Configuration relwithdebinfo   # optimizado con símbolos
.\scripts\build.ps1 -Clean                          # desde cero
```

</details>

## Arquitectura

```
                  ┌──────────────┐
   archivo  ─────▶│  Demuxer     │  libavformat
                  └──────┬───────┘
                         │ paquetes (cola acotada)
            ┌────────────┴────────────┐
            ▼                         ▼
    ┌───────────────┐         ┌───────────────┐
    │ VideoDecoder  │         │ AudioDecoder  │
    │  D3D11VA      │         │  + swresample │
    └───────┬───────┘         └───────┬───────┘
            │ ID3D11Texture2D          │ float32
            │ (nunca sale de la GPU)   │
            ▼                          ▼
    ┌───────────────┐         ┌───────────────┐
    │ VideoRenderer │◀────────│ WasapiRenderer│
    │  shader HLSL  │  reloj  │  RELOJ MAESTRO│
    └───────┬───────┘         └───────────────┘
            ▼
    ┌───────────────┐
    │  SwapChain    │  DXGI modo flip
    └───────────────┘
```

Cinco hilos: demultiplexado, decodificación de vídeo, decodificación de audio,
presentación y el de la interfaz. Se comunican con colas **acotadas**, que es lo
que impide que el demultiplexor —mucho más rápido que el decodificador— agote la
memoria con un archivo 8K.

Los saltos de posición se coordinan con un **contador de generación**: cada
paquete y cada fotograma viajan etiquetados, y cada hilo vacía su propio
decodificador cuando ve una etiqueta nueva. Ningún hilo toca el estado de otro.

Hay más detalle, y el porqué de cada decisión, en [CLAUDE.md](CLAUDE.md) y en los
comentarios de cabecera de cada módulo.

## Licencia

[GPL-3.0-or-later](LICENSE).

Pyxis enlaza FFmpeg de forma estática. FFmpeg se compila aquí en su
configuración LGPL (sin el componente `gpl`), pero el enlazado estático hace que
la licencia del conjunto sea GPL. Es la misma situación que VLC y mpv.
