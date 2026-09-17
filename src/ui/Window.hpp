// ============================================================================
//  Window.hpp - Ventana Win32
//
//  La ventana NO dibuja. Su unica responsabilidad es traducir mensajes de
//  Windows en llamadas a los callbacks; el dibujado vive en un hilo aparte
//  (ver ui/Controller.hpp).
//
//  Esa separacion no es purismo. Al arrastrar o redimensionar una ventana,
//  Windows entra en un bucle modal propio que NO devuelve el control al bucle
//  de mensajes de la aplicacion hasta que el usuario suelta el raton. Si el
//  dibujado dependiera de ese bucle, el video se congelaria cada vez que
//  alguien mueve la ventana. Con el hilo de presentacion separado, sigue
//  reproduciendose.
// ============================================================================
#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace pyxis {

class Window {
public:
    // Mensaje propio para pedir la apertura de un archivo desde otro hilo. El
    // LPARAM es un std::wstring* reservado con new del que la ventana se hace
    // cargo. Es el modo canonico de devolver trabajo al hilo de la interfaz sin
    // cerrojos: la cola de mensajes de Windows ya esta sincronizada.
    static constexpr UINT kOpenRequestMessage = WM_APP + 1;

    struct Callbacks {
        std::function<void(unsigned width, unsigned height)> onResize;
        std::function<void(const std::vector<std::wstring>& paths)> onFilesDropped;
        // `repeated` distingue la pulsacion real de la autorrepeticion del
        // teclado. El avance fotograma a fotograma marca su propio ritmo, asi
        // que ignora la repeticion del sistema en lugar de heredar la cadencia
        // que cada usuario tenga configurada en el panel de control.
        std::function<void(int virtualKey, bool shift, bool control, bool repeated)> onKeyDown;
        std::function<void(int virtualKey)> onKeyUp;

        // Al perder el foco no llega ningun WM_KEYUP, asi que sin esto una
        // tecla mantenida se quedaria "pulsada" para siempre. El par tambien
        // gobierna la barra de controles, que se esconde con la ventana en
        // segundo plano.
        std::function<void()> onFocusLost;
        std::function<void()> onFocusGained;

        std::function<void(int x, int y)> onMouseMove;
        std::function<void(int x, int y)> onLeftButtonDown;
        std::function<void(int x, int y)> onLeftButtonUp;
        std::function<void(int x, int y)> onRightButtonDown;
        std::function<void()> onDoubleClick;
        std::function<void(int wheelDelta, int x, int y, bool control)> onWheel;
        std::function<void()> onClose;
    };

    Window() = default;
    ~Window();

    Window(const Window&)            = delete;
    Window& operator=(const Window&) = delete;

    void Create(const std::wstring& title, int width, int height, Callbacks callbacks);
    void Destroy() noexcept;

    void Show(int commandShow);
    void SetTitle(const std::wstring& title);

    // Procesa todos los mensajes pendientes. Devuelve falso cuando llega
    // WM_QUIT, que es la senal de terminar.
    [[nodiscard]] bool PumpMessages();

    [[nodiscard]] HWND Handle() const noexcept { return window_; }

    // Pantalla completa sin bordes sobre el monitor que contiene la ventana.
    // Se prefiere al modo exclusivo de DXGI: en Windows 11 el compositor tiene
    // una ruta directa para ventanas a pantalla completa que da la misma
    // latencia sin los problemas de cambio de modo (parpadeos al hacer Alt+Tab,
    // reordenacion de iconos del escritorio).
    void SetFullscreen(bool enabled);
    [[nodiscard]] bool IsFullscreen() const noexcept { return fullscreen_; }

    void SetCursorVisible(bool visible);

    // Impide que el equipo se suspenda o active el salvapantallas mientras se
    // reproduce. Hay que renovarlo, de ahi que sea una llamada y no un ajuste.
    static void KeepDisplayAwake(bool keepAwake) noexcept;

private:
    static LRESULT CALLBACK WindowProcedure(HWND window, UINT message,
                                            WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    void ApplyDarkTitleBar() noexcept;
    void HandleDroppedFiles(WPARAM wParam);

    HWND       window_ = nullptr;
    Callbacks  callbacks_{};
    bool       fullscreen_    = false;
    bool       cursorVisible_ = true;

    // Estado guardado para volver de pantalla completa.
    WINDOWPLACEMENT savedPlacement_{};
    LONG_PTR        savedStyle_   = 0;
    LONG_PTR        savedExStyle_ = 0;
};

}  // namespace pyxis
