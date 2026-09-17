// ============================================================================
//  main.cpp - Punto de entrada de Pyxis
//
//  Aqui solo ocurren cuatro cosas, en este orden y por este motivo:
//
//    1. CONCIENCIA DE PPP. Debe fijarse antes de crear ninguna ventana. Si no,
//       Windows escala el programa por su cuenta en pantallas de alta densidad
//       y el video se muestra reescalado por el sistema en lugar de a
//       resolucion nativa, que es justo lo contrario de lo que busca este
//       reproductor.
//
//    2. APARTAMENTO COM. Se elige MTA de forma deliberada. WASAPI crea sus
//       objetos en el hilo principal y los usa desde el hilo de audio; en un
//       apartamento monohilo eso exigiria marshalling y seria un error sutil
//       de los que solo aparecen bajo carga. El unico componente que necesita
//       STA es el dialogo de archivo, y ese se abre en su propio hilo.
//
//    3. ARGUMENTOS de la linea de comandos.
//
//    4. La aplicacion, con un unico manejador de excepciones alrededor.
// ============================================================================

#include "core/Error.hpp"
#include "core/Fault.hpp"
#include "core/Log.hpp"
#include "core/Text.hpp"
#include "ui/Controller.hpp"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace {

constexpr const wchar_t* kUsage =
    L"Pyxis " PYXIS_VERSION L"  -  reproductor de vídeo para Windows 11 x64\n"
    L"\n"
    L"Uso:  pyxis [opciones] [archivo o URL]\n"
    L"\n"
    L"Opciones:\n"
    L"  -f, --fullscreen     arrancar a pantalla completa\n"
    L"      --no-hardware    desactivar la decodificación por GPU (diagnóstico)\n"
    L"      --volume N       volumen inicial, de 0 a 100\n"
    L"      --fault NOMBRE   provocar un fallo a proposito (pruebas):\n"
    L"                       device-loss, pool-full\n"
    L"  -v, --verbose        registro detallado\n"
    L"      --log ARCHIVO    duplicar el registro en un archivo\n"
    L"  -h, --help           mostrar esta ayuda\n"
    L"\n"
    L"Controles:\n"
    L"  Espacio / K          reproducir o pausar\n"
    L"  Izq / Der            un fotograma atrás o adelante\n"
    L"                       (mantener pulsado avanza a 20 fotogramas/s)\n"
    L"  Mayús + Izq / Der    +- 5 s\n"
    L"  Ctrl + Izq / Der     +- 60 s\n"
    L"  J / L                +- 10 s\n"
    L"  0-9                  saltar al 0%-90% de la duración\n"
    L"  Arriba / Abajo       volumen\n"
    L"  M                    silencio\n"
    L"  F / F11 / doble clic pantalla completa\n"
    L"  Ctrl + rueda         ampliar sobre el punto del ratón\n"
    L"  arrastrar            desplazar la imagen ampliada\n"
    L"  Z                    ajustar a la ventana\n"
    L"  X                    tamaño original (1:1)\n"
    L"  [ / ]                velocidad de reproducción\n"
    L"  Retroceso            velocidad normal\n"
    L"  S                    guardar el fotograma como PNG\n"
    L"  A / B                marcar el inicio y el final de un recorte\n"
    L"  C                    descartar el recorte marcado\n"
    L"  E                    editar el encuadre (arrastra las esquinas)\n"
    L"  R                    encuadre completo\n"
    L"  G                    panel de ajustes de imagen\n"
    L"  I                    estadísticas\n"
    L"  O                    abrir archivo\n"
    L"  Q / Esc              salir\n"
    L"\n"
    L"A la derecha de la barra de progreso hay un indicador de velocidad\n"
    L"-clic para avanzar por la lista, clic derecho para elegir- y un botón\n"
    L"de captura y una tijera. Las capturas van a Imágenes\\Pyxis a resolución\n"
    L"nativa; los recortes, a Vídeos\\Pyxis, copiando el flujo sin recodificar.\n";

struct ParsedCommandLine {
    pyxis::Options options;
    std::wstring   logFile;
    std::wstring   badFault;   // --fault con un nombre que no existe
    bool           showHelp = false;
};

ParsedCommandLine ParseCommandLine() {
    ParsedCommandLine parsed;

    int count = 0;
    wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &count);
    if (argv == nullptr) return parsed;

    for (int i = 1; i < count; ++i) {
        const std::wstring argument = argv[i];

        const auto nextValue = [&]() -> std::wstring {
            return (i + 1 < count) ? argv[++i] : std::wstring{};
        };

        if (argument == L"-h" || argument == L"--help") {
            parsed.showHelp = true;
        } else if (argument == L"-f" || argument == L"--fullscreen") {
            parsed.options.startFullscreen = true;
        } else if (argument == L"--no-hardware") {
            parsed.options.disableHardware = true;
        } else if (argument == L"--fault") {
            // Solo para las pruebas: recorre a proposito los caminos de error
            // que en uso normal exigen que se rompa algo de verdad.
            const std::wstring value = nextValue();
            const pyxis::Fault fault = pyxis::ParseFault(value);
            if (fault == pyxis::Fault::None) {
                parsed.badFault = value;
            } else {
                pyxis::SetFault(fault);
            }
        } else if (argument == L"-v" || argument == L"--verbose") {
            parsed.options.verbose = true;
        } else if (argument == L"--log") {
            parsed.logFile = nextValue();
        } else if (argument == L"--volume") {
            const std::wstring value = nextValue();
            if (!value.empty()) {
                try {
                    parsed.options.volume =
                        std::min(1.0f, std::max(0.0f, std::stof(value) / 100.0f));
                } catch (const std::exception&) {
                    // Un volumen mal escrito no justifica no arrancar.
                }
            }
        } else if (!argument.empty() && argument.front() != L'-') {
            // Primer argumento suelto: el medio a reproducir.
            if (parsed.options.path.empty()) parsed.options.path = argument;
        }
    }

    ::LocalFree(argv);
    return parsed;
}

// Una aplicacion /SUBSYSTEM:WINDOWS no tiene consola, asi que la ayuda y los
// errores fatales se muestran en un cuadro de dialogo.
void ShowMessage(const std::wstring& text, UINT icon) {
    ::MessageBoxW(nullptr, text.c_str(), L"Pyxis", icon | MB_OK | MB_SETFOREGROUND);
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, PWSTR, int commandShow) {
    // 1. PPP por monitor v2: Windows no reescala nada y la ventana recibe el
    //    tamano real en pixeles, que es lo que necesita la cadena de
    //    intercambio para presentar sin filtrado intermedio.
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const ParsedCommandLine parsed = ParseCommandLine();

    if (parsed.showHelp) {
        ShowMessage(kUsage, MB_ICONINFORMATION);
        return 0;
    }

    pyxis::log::SetLevel(parsed.options.verbose ? pyxis::log::Level::Debug
                                                : pyxis::log::Level::Info);
    if (!parsed.logFile.empty()) {
        pyxis::log::SetLogFile(parsed.logFile);
    }

    // Un --fault mal escrito tiene que cantar. Arrancar como si nada dejaria
    // una prueba en verde sin haber ejercitado el camino que pretendia probar,
    // que es peor que no tenerla.
    if (!parsed.badFault.empty()) {
        ShowMessage(L"Fallo inyectado desconocido: " + parsed.badFault +
                        L"\n\nValores admitidos: device-loss, pool-full",
                    MB_ICONERROR);
        return 2;
    }

    PYXIS_INFO("Pyxis {} arrancando", PYXIS_VERSION);

    // 2. Apartamento COM multihilo (ver la nota de cabecera).
    const HRESULT comInit = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(comInit)) {
        ShowMessage(L"No se pudo inicializar COM.", MB_ICONERROR);
        return 1;
    }

    int exitCode = 0;
    try {
        pyxis::Controller controller;
        exitCode = controller.Run(parsed.options, commandShow);

    } catch (const pyxis::Exception& error) {
        PYXIS_ERROR("error fatal: {}", error.what());
        ShowMessage(L"Pyxis no pudo continuar:\n\n" + pyxis::ToUtf16(error.what()),
                    MB_ICONERROR);
        exitCode = 1;

    } catch (const std::exception& error) {
        PYXIS_ERROR("excepcion no controlada: {}", error.what());
        ShowMessage(L"Error inesperado:\n\n" + pyxis::ToUtf16(error.what()), MB_ICONERROR);
        exitCode = 1;
    }

    PYXIS_INFO("Pyxis finalizado con codigo {}", exitCode);

    ::CoUninitialize();
    pyxis::log::Shutdown();
    return exitCode;
}
