# Contribuir a Pyxis

Gracias por el interés. Esta guía es corta a propósito: solo lo que hace falta
saber para que un cambio entre sin fricción.

## Antes de escribir código

Lee [CLAUDE.md](CLAUDE.md). En particular la sección **Invariantes que no se
negocian**: hay cinco propiedades del diseño (zero-copy, dispositivo único,
reloj de audio, colas acotadas, normalización a NV12/P010) que definen lo que es
Pyxis. Un cambio que rompa alguna necesita justificarse en un issue antes de
abrir el PR, aunque funcione.

Si la idea es grande, abre un issue primero. Ahorra trabajo a todos.

## Entorno

```powershell
git clone https://github.com/D0PZ/pyxis.git
cd pyxis
.\scripts\build.ps1
```

La primera compilación construye FFmpeg desde fuente y tarda entre 15 y 45
minutos. Después, la caché binaria de vcpkg hace que sea cuestión de segundos.

Para desarrollar, usa la configuración `debug`: activa la capa de depuración de
Direct3D 11, que convierte un cuelgue silencioso del controlador en un mensaje
concreto en la salida del depurador.

```powershell
.\scripts\build.ps1 -Configuration debug
```

## Estilo

El proyecto trae un `.clang-format`. Pásalo antes de enviar:

```powershell
clang-format -i src/**/*.cpp src/**/*.hpp
```

Convenciones que `clang-format` no puede aplicar:

- **Código en inglés, comentarios en español.**
- Los comentarios explican el **porqué**, no el qué. Si se deduce leyendo la
  línea siguiente, sobra.
- Cada cabecera abre con un bloque que justifica las decisiones de diseño del
  módulo. Si añades un módulo, mantén la costumbre.
- Clases y funciones en `PascalCase`, variables en `camelCase`, miembros con
  `trailing_`, constantes con `kPrefijo`.
- Sin acentos ni caracteres no ASCII en los archivos de `src/` y `res/`: el
  compilador de recursos y algunas herramientas de la cadena no los tratan
  igual. La documentación en Markdown sí los lleva.

## Errores

Dos mecanismos, sin mezclarlos:

- **Inicialización** (crear el dispositivo, abrir un archivo): lanza
  `pyxis::Exception` con `PYXIS_CHECK_HR` / `PYXIS_CHECK_AV` /
  `PYXIS_CHECK_WIN32` / `PYXIS_REQUIRE`.
- **Bucle caliente** (un paquete corrupto, un fotograma tardío): devuelve un
  enum. Ocurre miles de veces por minuto y tiene que ser barato.

Nunca descartes un código de error sin registrarlo. Las macros conservan el
valor original, que es lo único útil al depurar.

## Probar un cambio

No hay suite automatizada todavía (los PR que añadan una son muy bienvenidos).
Mientras tanto, el mínimo antes de enviar:

1. Compila en `release` **y** en `debug` sin avisos nuevos.
2. Reproduce al menos un archivo de cada grupo:
   - H.264 8 bits SDR (lo más común)
   - HEVC 10 bits HDR10 (ejercita el camino PQ y el mapeo de tonos)
   - AV1 (ejercita dav1d y el repliegue por software)
   - un MKV con varias pistas de audio
3. Ejercita: pausa, saltos hacia delante y hacia atrás, saltar al final,
   redimensionar, pantalla completa, cambiar de monitor arrastrando la ventana.
4. Abre el panel de estadísticas con `I` y comprueba que *descartados* y *cortes
   de audio* no crecen durante la reproducción normal.
5. Si tocaste la ruta de hardware, verifica también con `--no-hardware` que el
   repliegue por software sigue funcionando.

## Commits y PR

- Un commit por cambio lógico. Mensaje en imperativo y en español:
  `Corrige el desbordamiento de QPC tras 2,5 horas`.
- El cuerpo explica el **porqué**, no el diff.
- En el PR, describe qué archivos usaste para probar y en qué GPU. El
  comportamiento de D3D11VA varía bastante entre Intel, AMD y NVIDIA, y ese dato
  ahorra mucho tiempo al revisar.

## Licencia

Al contribuir aceptas que tu código se distribuya bajo
[GPL-3.0-or-later](LICENSE), igual que el resto del proyecto.
