# CLAUDE.md

Guía para trabajar en Pyxis. Explica lo que el código no puede explicar por sí
mismo: qué decisiones son deliberadas, cuáles son las trampas y en qué orden
conviene tocar las cosas.

---

## Qué es este proyecto

Reproductor de vídeo nativo para **Windows 11 x64 exclusivamente**. Sin capas de
portabilidad, sin `#ifdef` de plataforma, sin abstracciones sobre el API
gráfico. Esa restricción es una característica: permite usar D3D11VA, DXGI en
modo flip y WASAPI directamente, sin el mínimo común denominador que impone
cualquier código multiplataforma.

El binario es **autocontenido**: FFmpeg y la CRT van estáticos, y todo lo demás
forma parte de Windows. No se distribuyen DLLs ni instalador. Cualquier cambio
que rompa esa propiedad hay que discutirlo antes de hacerlo.

---

## Invariantes que no se negocian

Romper cualquiera de estos convierte a Pyxis en otro reproductor más:

1. **Zero-copy en la ruta de hardware.** El fotograma decodificado no se copia a
   RAM ni pasa por `ID3D11VideoProcessor`. Si algún cambio introduce una copia
   por fotograma en la ruta acelerada, está mal, por más que funcione.

2. **Un solo `ID3D11Device` en todo el proceso.** Lo comparten FFmpeg, el
   renderizador y Direct2D. Es lo que hace posible el punto 1. Por eso el
   dispositivo está marcado con `ID3D10Multithread::SetMultithreadProtected`;
   ver `render/Device.cpp`.

3. **El audio es el reloj maestro.** El vídeo lo sigue descartando o repitiendo
   fotogramas. Nunca al revés: estirar el audio para seguir a un reloj de vídeo
   es audible y el vídeo ajustado no lo es. Ver la cabecera de
   `audio/WasapiRenderer.hpp`.

4. **Colas acotadas entre etapas.** Sin un límite, el demultiplexor —mucho más
   rápido que el decodificador— agota la memoria con un archivo 8K. El límite es
   la contrapresión.

5. **El decodificador normaliza a NV12 / P010.** Sea cual sea el códec y la ruta
   (hardware o software), el renderizador recibe siempre dos planos. Eso reduce
   el shader a un único camino en lugar de la combinatoria de los más de cien
   formatos de píxel de FFmpeg.

6. **Cero dependencias en tiempo de ejecución.** Antes de añadir una biblioteca,
   comprobar si Windows 11 ya trae lo necesario. Casi siempre sí.

---

## Mapa del código

```
src/
├── main.cpp            punto de entrada: PPP, COM, argumentos
│
├── core/               utilidades sin dependencias del dominio
│   ├── Error.hpp       excepciones para inicializar, códigos para el bucle caliente
│   ├── Log.hpp         registro con filtro de nivel de coste cero
│   ├── Clock.hpp       tiempo monótono + MediaClock (el reloj maestro)
│   ├── Queue.hpp       cola acotada y bloqueante entre etapas
│   ├── Thread.hpp      MMCSS y nombrado de hilos
│   ├── Paths.hpp       carpetas de salida y nombres de archivo
│   └── Text.hpp        la única frontera UTF-8 / UTF-16
│
├── media/              el pipeline
│   ├── FFmpegUtil.hpp  envoltorios RAII sobre libav*
│   ├── Frame.hpp       VideoFrame, AudioBuffer, descripción de color
│   ├── Demuxer.hpp     apertura de contenedores (componente pasivo)
│   ├── VideoDecoder.hpp   D3D11VA + repliegue por software
│   ├── AudioDecoder.hpp   decodificación + remuestreo
│   ├── Trimmer.hpp     recorte A/B por copia de flujo
│   └── Player.hpp      orquestación: hilos, saltos, sincronía
│
├── render/
│   ├── Device.hpp      el ID3D11Device compartido
│   ├── SwapChain.hpp   DXGI modo flip, HDR, VRR
│   ├── VideoRenderer.hpp  dibuja el fotograma; caché de vistas
│   ├── Overlay.hpp     interfaz con Direct2D sobre textura intermedia
│   ├── Snapshot.hpp    captura del fotograma a PNG (WIC)
│   └── shaders/
│       ├── color.hlsli     transferencias y matrices de gama
│       ├── fullscreen.hlsl vertex shader del triángulo completo
│       ├── video.hlsl      YUV→RGB, HDR, mapeo de tonos
│       └── overlay.hlsl    composición de la interfaz
│
├── audio/
│   └── WasapiRenderer.hpp  salida de audio + anclaje del reloj
│
└── ui/
    ├── Window.hpp      ventana Win32 (no dibuja nada)
    └── Controller.hpp  hilo de presentación, entrada, recuperación
```

**Regla de dependencias**: `core` no depende de nada; `media` y `render`
dependen de `core`; `ui` depende de todo. No introducir dependencias hacia
arriba (que `media` incluya algo de `ui`, por ejemplo).

---

## Los cinco hilos

| Hilo | Nombre | Qué hace | MMCSS |
|---|---|---|---|
| Principal | — | bucle de mensajes de Windows, entrada | — |
| Presentación | `pyxis-present` | espera de DXGI, elige fotograma, dibuja, presenta | Playback |
| Demultiplexado | `pyxis-demux` | lee paquetes, los enruta, ejecuta los saltos | — |
| Decod. vídeo | `pyxis-video` | paquetes → fotogramas | Playback |
| Decod. audio | `pyxis-audio-decode` | paquetes → bloques float32 | Playback |
| Salida audio | `pyxis-audio` | rellena WASAPI, ancla el reloj | **Pro Audio** |

Los nombres se ven en el depurador y en las trazas ETW; se asignan con
`SetCurrentThreadName`.

**El hilo principal nunca dibuja.** Al arrastrar o redimensionar una ventana,
Windows entra en un bucle modal propio que no devuelve el control hasta que el
usuario suelta el ratón. Si el dibujado dependiera de él, el vídeo se congelaría
al mover la ventana.

---

## Cómo funcionan los saltos de posición

El punto más delicado del programa. Vaciar las colas **no basta**: cada hilo
puede tener ya en la mano un paquete de la posición anterior, y el decodificador
guarda fotogramas de referencia internos.

La solución es un **contador de generación** (`Player::generation_`):

1. Quien pide el salto incrementa el contador.
2. Vacía las tres colas con `Flush`, lo que además despierta a los hilos
   bloqueados. El vaciado se señaliza con una **época**, no con una bandera:
   cada operación compara el valor que vio al entrar con el actual, así que
   quien esperaba se entera una vez y quien llegue después se bloquea con
   normalidad. Con una bandera persistente, los hilos de decodificación girarían
   en vacío hasta que alguien la bajara.
3. Publica el destino y levanta `seekPending_`.
4. El hilo de demultiplexado ve la bandera, ejecuta el salto (es el único dueño
   del `AVFormatContext`) y empieza a etiquetar con la generación nueva.
5. Cada hilo de decodificación, al ver una etiqueta distinta de la suya, vacía
   **su propio** decodificador y adopta la nueva.

Ningún hilo toca el estado de otro. Si hay que tocar esta parte, mantener esa
propiedad.

---

## Colorimetría

Interpretar mal el color es la causa número uno de que un reproductor se vea
«lavado» o «demasiado saturado». Reglas:

- Los metadatos del flujo mandan. Cuando faltan (lo normal), se deduce por
  resolución: ≤576 líneas → BT.601, más → BT.709. Ver `DeriveColorInfo`.
- `AVCOL_RANGE_UNSPECIFIED` se trata como **limitado**. Es lo que asume
  prácticamente todo el material de vídeo.
- La matriz YUV→RGB se calcula en la CPU a partir de Kr y Kb, e **incluye** la
  expansión de rango. Va al shader como una `float4x4` que se aplica a
  `(y, u, v, 1)`, así que el desplazamiento sale gratis.
- La matriz del cbuffer está declarada `row_major`. HLSL empaqueta en orden de
  columnas por defecto y la CPU la rellena por filas: quitar ese `row_major`
  produce colores psicodélicos.
- P010 guarda 10 bits en los bits **altos** de cada palabra de 16. El factor
  `c_bitScale` lo corrige; sin él, la imagen sale ligeramente oscura.

### HDR

El HDR se activa solo cuando el contenido **y** el monitor lo admiten. Forzarlo
con contenido SDR lo deja apagado; forzarlo en un monitor SDR lo deja ilegible.

Los cuatro caminos del shader están documentados en `video.hlsl`. El más sutil
es SDR→HDR: el contenido SDR se coloca en el contenedor BT.2020 **sin
estirarlo**. Subirle el brillo para «aprovechar» el HDR es justo lo que hace que
el SDR se vea mal en modo HDR.

---

## Avance fotograma a fotograma y encuadre

**Pasos.** `Player::StepFrame` no mueve el reloj y espera: publica un umbral
(`stepLowerBound_`) y marca `stepPending_`. La siguiente llamada a
`SelectFrame` se desvía a `SelectSteppedFrame`, que descarta todo lo anterior al
umbral y se planta en el primer fotograma que lo alcanza. Si aún no ha llegado,
devuelve `None` y se conserva el fotograma en pantalla: así retroceder no
produce un parpadeo mientras se redecodifica el GOP.

El umbral del paso atrás está a **1,5 duraciones** por debajo del fotograma
actual. Es el único valor que deja fuera al ante-anterior y dentro al anterior;
con una duración justa, el redondeo de PTS hace que a veces caiga en el
equivocado.

Quien mantenga pulsada la flecha debe consultar `StepPending()` antes de pedir
otro paso. Sin eso, el retroceso en material con GOP largo acumula peticiones
que el decodificador nunca puede atender.

**Encuadre.** El zoom se aplica al *viewport*, no en el shader. Un viewport de
D3D11 puede salirse del destino y el rasterizador recorta, así que ampliar no
cuesta trabajo de fragmento adicional. `zoom = 1.0` significa «ajustado a la
ventana», **no** tamaño original: eso último depende de la resolución del vídeo
y se calcula con `ZoomForOriginalSize`, que además corrige la relación de
aspecto del píxel.

El estado vive en `Controller::view_` bajo cerrojo (lo escribe la interfaz, lo
lee la presentación en cada fotograma) y se publica con `SetViewTransform`
justo antes de dibujar.

## El bloqueo del avance manual

Merece su propia sección porque no es evidente y se puede reintroducir.

En pausa, el renderizador de audio no consume. Su cola se llena, el
decodificador de audio se bloquea al empujar, la cola de paquetes de audio se
llena también y el demultiplexor acaba dormido dentro de un `Push` de audio.
Desde ese momento **deja de alimentar vídeo**. Con reproducción normal no se
nota, porque el audio se consume; solo aparece al consumir vídeo sin consumir
audio, que es exactamente lo que hace el avance fotograma a fotograma: se clava
en cuanto agota lo precargado.

La solución es `Player::steppingMode_`: mientras está activo, el demultiplexor
descarta el audio en lugar de encolarlo. Al volver a reproducir se resincroniza
con un salto al fotograma mostrado — que además es lo correcto, porque el audio
encolado pertenecía a donde se pausó, no a donde se ha llegado pasando
fotogramas.

La segunda mitad del problema es la cadencia: una pulsación cuyo paso anterior
no ha aterrizado **se descarta** (`if (stepPending_) return;`). Sin esa guarda,
cada tecla reinicia la recogida y a 8K la secuencia no termina nunca.

**Nada de aritmética con la duración del fotograma.** En material de tasa
variable dos fotogramas consecutivos pueden distar 33 ms o 266 ms, así que
«posición menos una duración» no identifica al anterior. Los límites se
expresan como comparaciones: *el último que hay antes del actual*.

## Exportar: capturas y recortes

Las dos salidas comparten un principio: **no reutilizan el pipeline de
reproducción**. Una captura redibuja el fotograma a resolución nativa en su
propia textura en vez de copiar el búfer trasero —que daría el tamaño de la
ventana, con bandas negras, zoom y la interfaz encima—. Un recorte abre el
archivo por segunda vez en vez de mover el demultiplexor de la reproducción.

El recorte copia paquetes sin decodificar. Es lo correcto (instantáneo y sin
pérdida) y además lo único posible: el binario se compila sin codificadores
H.264/HEVC. La contrapartida es el alineamiento al fotograma clave, que se
informa al usuario en lugar de disimularlo.

Ambas operaciones corren fuera del hilo de interfaz: la captura en el de
presentación (dueño del fotograma y del renderizador, y por eso inicializa COM,
que WIC necesita) y el recorte en un hilo propio que el `Controller` recoge en
su destructor.

## Trampas conocidas

- **`D3D11_BIND_SHADER_RESOURCE` en el pool de D3D11VA.** Se añade en
  `VideoDecoder::NegotiateFormat`, en el único instante en que se puede. Sin esa
  línea el shader no puede leer la textura decodificada y todo el diseño se cae.

- **Texturas con relleno.** El pool alinea a múltiplos del macrobloque: una
  imagen de 1920×1080 suele vivir en una textura de 1920×**1088**. De ahí
  `c_uvScale`. Consultar siempre el `D3D11_TEXTURE2D_DESC` real, no asumir las
  dimensiones visibles.

- **Direct2D no admite `R10G10B10A2`**, que es el formato del búfer trasero en
  HDR10. Por eso la interfaz se dibuja en una textura BGRA intermedia y se
  compone con un shader. No intentar dibujar D2D directamente sobre la cadena.

- **`ResizeBuffers` falla si queda cualquier referencia al búfer trasero**,
  incluidas las que el contexto mantiene por estar enlazadas. Ver
  `SwapChain::ReleaseRenderTarget`.

- **`ALLOW_TEARING` exige `syncInterval = 0`.** Cualquier otra combinación la
  rechaza DXGI con un error difícil de diagnosticar.

- **Apartamento COM.** El hilo principal es **MTA** porque WASAPI crea sus
  objetos ahí y los usa desde el hilo de audio. El diálogo de archivo necesita
  STA, así que se abre en un hilo efímero propio. No cambiar el hilo principal a
  STA.

- **Orden de destrucción.** Los `VideoFrame` de la cola referencian texturas del
  pool del decodificador. Hay que soltarlos **antes** de cerrar el decodificador;
  ver `Player::Close`.

- **`pendingFrame_`, el historial y `currentFrame_` pertenecen al hilo de
  presentación.** Ningún otro hilo los toca. `Player::Close` y `OpenMedia` se
  limitan a incrementar un contador (la generación y el número de medio) y ese
  hilo los suelta en su siguiente pasada. Manipularlos desde la interfaz es una
  carrera con el dibujado en curso.

- **`IAudioClient::Reset` exige el flujo parado**, y el hilo de audio puede estar
  dentro de `GetBuffer`. Por eso `WasapiRenderer::Flush` solo deja una petición
  y el vaciado lo ejecuta el propio hilo de audio.

- **Los paquetes se etiquetan con la generación que el demultiplexor tenía al
  posicionarse**, no con la que haya al encolar. Leer el contador en el momento
  de encolar hacía que un paquete anterior al salto viajara con la etiqueta
  nueva: el decodificador se vaciaba y acto seguido lo decodificaba sin sus
  fotogramas de referencia (`Could not find ref with POC`).

- **Los saltos van sobre la pista de vídeo, no sobre la línea de tiempo global.**
  Con `-1`, en un MP4 fragmentado se aterriza a mitad de grupo de imágenes.

- **Arrastrar la barra genera más de cien mensajes por segundo.** Cada salto
  vacía tres colas y reinicia el audio, así que se limitan (`RequestScrubSeek`)
  y solo el de soltar está garantizado.

---

## Convenciones

**Errores.** Los fallos de inicialización lanzan `pyxis::Exception` (se capturan
en `main` y en el hilo de presentación). Los fallos del bucle caliente devuelven
enums. No mezclar. Usar `PYXIS_CHECK_HR`, `PYXIS_CHECK_AV`, `PYXIS_CHECK_WIN32`
y `PYXIS_REQUIRE`; conservan el código de error original, que es lo único útil
al depurar.

**Tiempo.** Microsegundos enteros (`Micros`) en todas partes. Nada de `double`:
a ocho horas de metraje acumula error suficiente para desincronizar el audio.

**Texto.** UTF-16 en la frontera con Win32, UTF-8 hacia dentro. Toda conversión
pasa por `core/Text.hpp`.

Los comentarios y el código van en ASCII; el texto que ve el usuario, no. Los
literales anchos (`L"Arrastra un vídeo aquí"`) se compilan con `/utf-8` y
llegan correctos a DirectWrite. La única excepción de verdad es `res/*.rc`, que
el compilador de recursos no lee como UTF-8.

**Comentarios.** Explican el **porqué**, nunca el qué. Si un comentario se puede
deducir leyendo la línea siguiente, sobra. Cada cabecera abre con un bloque que
justifica las decisiones de diseño del módulo; mantener esa costumbre al añadir
módulos nuevos.

**Nombres.** Clases y funciones en `PascalCase`, variables en `camelCase`,
miembros con `trailing_`, constantes con `kPrefijo`. El código está en inglés y
los comentarios en español.

---

## Compilar y probar

```powershell
.\scripts\build.ps1                              # release
.\scripts\build.ps1 -Configuration debug         # capa de depuración D3D11
.\scripts\build.ps1 -Clean                       # desde cero
```

La primera compilación construye FFmpeg desde fuente (15–45 min). Las siguientes
usan la caché binaria de vcpkg.

Para diagnosticar:

```powershell
.\build\release\bin\pyxis.exe --verbose --log pyxis.log pelicula.mkv
```

La tecla `I` abre el panel de estadísticas en vivo: decodificador en uso,
fotogramas descartados, profundidad de las colas y cortes de audio. **Es la
primera herramienta a mirar** ante cualquier problema de fluidez:

- *descartados* subiendo → la GPU no llega a dibujar a tiempo
- *cortes de audio* subiendo → el pipeline de audio va justo
- *cola v-frm* siempre a cero → el decodificador no alimenta al renderizador
- *v-pkt* siempre lleno → el decodificador es el cuello de botella

---

## Al añadir funcionalidad

- **Nuevo formato de píxel** → normalizarlo en `VideoDecoder`, no tocar el
  shader. El renderizador solo entiende NV12 y P010, y así debe seguir.
- **Nuevo ajuste de imagen** → añadirlo al cbuffer de `video.hlsl` y actualizar
  el `static_assert` del tamaño en `VideoRenderer.hpp`. Ese assert existe
  precisamente para que un desajuste falle al compilar y no en pantalla.
- **Nuevo atajo de teclado** → `Controller::OnKeyDown` y la tabla del README.
  Ojo: ese manejador descarta la autorrepetición del sistema (`repeated`), así
  que una acción que deba repetirse al mantener la tecla necesita su propio
  temporizador en el hilo de presentación, como hace `UpdateFrameStepping`.
- **Nuevo modo de encuadre** → `ViewTransform` en `render/VideoRenderer.hpp`. La
  aritmética de geometría vive en los estáticos públicos de `VideoRenderer`
  (`ComputeFitRect`, `ApplyView`, `ClampPan`) precisamente para que la interfaz
  y el renderizador no puedan divergir. No la dupliques en `Controller`.
- **Nueva métrica** → `PlayerStats` y `Controller::BuildStatsText`.
- **Nuevo control en la barra** → dibujo en `Overlay::DrawControlBar` (o un
  `Draw*` propio), prueba de impacto como `HitTestSpeed`, y reparto del clic en
  `Controller::OnLeftButtonDown`. El orden de ese reparto importa: el menú
  desplegado se queda con el clic antes que nadie, los tiradores del recorte van
  antes que la barra de progreso porque están encima de ella, y la barra va la
  última porque ocupa casi todo el ancho.
- **Nueva carpeta de salida** → `core/Paths.hpp`. No repitas la resolución de
  carpetas conocidas ni el saneado del nombre: las dos salidas que ya existen
  pasan por ahí precisamente para no divergir.
