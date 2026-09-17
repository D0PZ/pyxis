# Pruebas de Pyxis

```powershell
.\tests\run-tests.ps1                       # todo
.\tests\run-tests.ps1 -List                 # ver qué hay
.\tests\run-tests.ps1 -Case audio           # solo los que coincidan
.\tests\run-tests.ps1 -KeepArtifacts        # conservar registros en tests\.work
.\tests\run-tests.ps1 -Sample 'D:\algo 8K.mkv'
```

Requiere haber compilado antes (`.\scripts\build.ps1`). Devuelve 0 si todo pasa
y 1 si algo falla, así que sirve tal cual como puerta de un commit.

---

## Por qué son pruebas funcionales y no unitarias

Casi todo lo que hace interesante a Pyxis solo existe con una GPU real por
debajo y un archivo real delante: el zero-copy de D3D11VA, la sincronía contra
el reloj de audio, el avance fotograma a fotograma, el mapeo de tonos. Una
prueba unitaria de esas partes probaría los *mocks*, no el programa.

Así que la suite hace lo que haría una persona: abre la ventana, le manda teclas
y clics con `PostMessage`, y lee el registro (`--verbose`) para comprobar qué
pasó. Los mensajes van por `PostMessage` y no por `SendInput` a propósito — no
necesitan el foco, así que se puede seguir trabajando mientras corren y un clic
accidental no descarrila nada.

El coste de este enfoque es que las pruebas son lentas (minutos, no
milisegundos) y que dependen del texto del registro. A cambio, cada una de ellas
ha encontrado al menos un fallo real.

---

## Material de prueba

`tests/media/` se genera solo la primera vez, con `mkmedia` (se compila junto al
reproductor y usa el mismo FFmpeg, así que no añade dependencias):

| Archivo | Qué tiene | Para qué |
|---|---|---|
| `av-sync.mp4` | H.264 + AAC, 5 s, 640×360, tono de 440 Hz con pitido cada segundo | pista de audio de verdad |
| `hdr-pq.mkv` | ffvhuff 10 bits etiquetado PQ / BT.2020, 3 s | camino HDR y ruta por software |

No se versionan: el generador dice mejor que los archivos qué propiedades se
están probando, y son dos segundos de CPU.

`hdr-pq.mkv` no usa HEVC porque este binario no lleva codificador de HEVC (son
los que arrastran patentes). Da igual: lo que elige la rama del shader son las
**etiquetas** del flujo, no el códec. Y de paso, al no tener aceleración,
ejercita el repliegue por software.

Tampoco usa FFV1, que sería la elección natural: dentro de Matroska hace que el
decodificador emita `bytestream end mismatching by -7` en cada fotograma. Las
imágenes salen bien, pero un archivo de prueba que llena el registro de errores
inutiliza `Assert-NoErrors`, que es la mitad del valor de la suite. `ffvhuff`
decodifica limpio a cambio de pesar 16 MB en lugar de 130 KB — irrelevante para
un archivo local que no se versiona.

**Ejemplar grande.** Los archivos generados son diminutos a propósito y no
sirven para medir el avance manual a 8K. Pasa uno propio con `-Sample` o con la
variable `PYXIS_TEST_SAMPLE`; sin él, los casos que lo necesitan se adaptan al
material pequeño y los que no pueden se marcan como omitidos.

---

## Los casos

| Caso | Qué vigila |
|---|---|
| `01-arranque` | ventana, decodificador, cadena de intercambio, colorimetría |
| `02-avance-fotograma` | 120 pasos adelante sin quedarse clavado |
| `03-retroceso` | pasos atrás, ratio de rebobinado, y que el avance sobreviva al final del archivo |
| `04-captura` | el PNG sale a resolución nativa, no a la de la ventana |
| `05-recorte-copia` | recorte A/B sin recodificar, y que Pyxis sepa reabrirlo |
| `06-recorte-con-ediciones` | con encuadre, la exportación sale con las dimensiones recortadas |
| `07-audio-en-la-exportacion` | la exportación conserva la pista de audio |
| `08-hdr-mapeo-de-tonos` | PQ / BT.2020 / 10 bits se reconocen |
| `09-decodificacion-por-software` | `--no-hardware` funciona y no toca D3D11VA |
| `10-perdida-de-dispositivo` | se recupera de `DXGI_ERROR_DEVICE_REMOVED` y sigue respondiendo |
| `11-pool-que-no-cabe` | un pool imposible reintenta con lo justo en vez de caer a software |

Los cuatro últimos existen porque sus caminos **nunca se habían ejecutado**. Son
los que más fácil pasan desapercibidos rotos: ninguno lanza un error visible
cuando falla, solo se ve mal o se oye mal.

---

## Inyección de fallos

Tres caminos solo ocurren cuando algo se rompe de verdad. Para llegar a ellos
sin desconectar hardware, el binario acepta `--fault`:

| Valor | Qué provoca |
|---|---|
| `device-loss` | `DXGI_ERROR_DEVICE_REMOVED` desde dentro de `Present`, una sola vez y a los 3 s |
| `huge-pool` | pide un pool D3D11VA de un millón de texturas, que no cabe en ninguna tarjeta |

Se implementan en `src/core/Fault.hpp`. Son un global a propósito: pasarlos por
las firmas de `Player`, `VideoDecoder`, `SwapChain` y `Controller` ensuciaría
cuatro interfaces con un parámetro que en producción siempre vale `None`.

No simulan la recuperación, simulan la **avería**: el error sale por el mismo
sitio por el que saldría el de verdad y recorre el mismo código de vuelta.

---

## Escribir un caso nuevo

Un archivo en `cases/`, numerado. Recibe `$Ctx` con las rutas
(`Exe`, `AvSync`, `Hdr`, `Sample`, `Work`, `Videos`, `Images`) y tiene el módulo
`Pyxis.Testing` ya importado. Falla lanzando; se omite con `Skip-Case`.

```powershell
# Una línea de resumen: aparece en -List.
$session = Start-Pyxis -Exe $Ctx.Exe -Media $Ctx.AvSync `
                       -Log (Join-Path $Ctx.Work 'mi-caso.log')
try {
    Send-PyxisKey $session -Key 'Space'
    $point = Get-PyxisControlPoint $session -Control 'Snapshot'
    Send-PyxisClick $session -X $point.X -Y $point.Y

    $hit = Wait-PyxisLog $session -Pattern 'Captura guardada' -TimeoutSeconds 30
    Assert-True ($hit.Count -gt 0) "no se guardó la captura"
    Assert-NoErrors $session
} finally {
    Stop-Pyxis $session
}
```

Reglas que conviene respetar:

- **`Stop-Pyxis` en un `finally`.** Un proceso huérfano se come la ventana del
  caso siguiente y encadena fallos que no son reales. El corredor barre los
  restos de todos modos, pero no hay que depender de eso.
- **Nada de esperas fijas para lo que se puede esperar de verdad.** `Wait-PyxisLog`
  y `Wait-NewOutput` vuelven en cuanto ocurre lo que esperan; un `Start-Sleep 30`
  es medio minuto perdido y sigue siendo frágil en una máquina lenta.
- **Las posiciones de los controles, con `Get-PyxisControlPoint`.** La geometría
  de la barra está copiada de `render/Overlay.cpp` en un solo sitio del arnés. No
  la repartas por los casos con restas al borde derecho.
- **Limpia lo que crees.** Las capturas y los recortes van a las carpetas del
  usuario, donde puede haber material suyo. Un `$since = Get-Date` al empezar,
  `Wait-NewOutput -Since $since` para localizar la salida, y borrar solo eso.
  Por marca de tiempo y no por nombre: Pyxis nombra los recortes con el
  intervalo, así que dos ejecuciones de la misma prueba producen el mismo
  nombre y la segunda no vería ningún archivo «nuevo».
- **Si falta algo del entorno, `Skip-Case`, no `Assert`.** Un monitor SDR o un
  ejemplar de 8K que no está no son fallos del programa.

Si añades un control a la barra, actualiza `$script:Layout` en
`Pyxis.Testing.psm1`. Es la única duplicación del arnés y está reconocida: las
posiciones no se pueden consultar desde fuera del proceso.
