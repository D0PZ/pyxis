#include "ui/Window.hpp"

#include "core/Error.hpp"
#include "core/Log.hpp"

#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include <memory>

namespace pyxis {
namespace {

constexpr const wchar_t* kWindowClass = L"PyxisMainWindow";

// El valor de DWMWA_USE_IMMERSIVE_DARK_MODE cambio entre versiones de Windows.
// 20 es el definitivo desde la 20H1; Pyxis exige Windows 11, asi que basta.
constexpr DWORD kDwmUseImmersiveDarkMode = 20;

}  // namespace

Window::~Window() {
    Destroy();
}

// ---------------------------------------------------------------------------
//  Creacion
// ---------------------------------------------------------------------------
void Window::Create(const std::wstring& title, int width, int height,
                    Callbacks callbacks) {
    callbacks_ = std::move(callbacks);

    const HINSTANCE instance = ::GetModuleHandleW(nullptr);

    WNDCLASSEXW windowClass{};
    windowClass.cbSize        = sizeof(windowClass);
    // Sin CS_HREDRAW/CS_VREDRAW: repintar al redimensionar es trabajo tirado,
    // porque quien dibuja es el hilo de presentacion en su propio ritmo.
    windowClass.style         = CS_DBLCLKS;
    windowClass.lpfnWndProc   = &Window::WindowProcedure;
    windowClass.hInstance     = instance;
    windowClass.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    // Fondo nulo: el bufer trasero cubre toda la ventana y dejar que Windows
    // pinte primero produciria un parpadeo blanco al redimensionar.
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hIcon         = ::LoadIconW(instance, MAKEINTRESOURCEW(1));
    windowClass.hIconSm       = windowClass.hIcon;

    if (::RegisterClassExW(&windowClass) == 0 &&
        ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        ThrowWin32(::GetLastError(), "no se pudo registrar la clase de ventana");
    }

    // Se ajusta el rectangulo para que el AREA DE CLIENTE tenga el tamano
    // pedido: sin esto, los bordes y la barra de titulo se comerian parte del
    // video y la resolucion del bufer no coincidiria con lo esperado.
    RECT desired{0, 0, width, height};
    ::AdjustWindowRectEx(&desired, WS_OVERLAPPEDWINDOW, FALSE, 0);

    window_ = ::CreateWindowExW(
        0, kWindowClass, title.c_str(), WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        desired.right - desired.left, desired.bottom - desired.top,
        nullptr, nullptr, instance, this);

    PYXIS_CHECK_WIN32(window_ != nullptr, "no se pudo crear la ventana");

    ApplyDarkTitleBar();
    ::DragAcceptFiles(window_, TRUE);
}

void Window::ApplyDarkTitleBar() noexcept {
    // Un reproductor de video con barra de titulo blanca destroza la
    // adaptacion del ojo a la oscuridad en una escena nocturna.
    const BOOL dark = TRUE;
    ::DwmSetWindowAttribute(window_, kDwmUseImmersiveDarkMode, &dark, sizeof(dark));
}

void Window::Show(int commandShow) {
    ::ShowWindow(window_, commandShow);
    ::UpdateWindow(window_);
}

void Window::SetTitle(const std::wstring& title) {
    if (window_ != nullptr) ::SetWindowTextW(window_, title.c_str());
}

// ---------------------------------------------------------------------------
//  Bucle de mensajes
// ---------------------------------------------------------------------------
bool Window::PumpMessages() {
    MSG message{};
    while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) return false;
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }
    return true;
}

LRESULT CALLBACK Window::WindowProcedure(HWND window, UINT message,
                                         WPARAM wParam, LPARAM lParam) {
    Window* self = nullptr;

    if (message == WM_NCCREATE) {
        // El puntero a la instancia se guarda en el primer mensaje que recibe
        // la ventana, para que el resto del procedimiento pueda encontrarlo.
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<Window*>(create->lpCreateParams);
        ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->window_ = window;
    } else {
        self = reinterpret_cast<Window*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
    }

    if (self != nullptr) return self->HandleMessage(message, wParam, lParam);
    return ::DefWindowProcW(window, message, wParam, lParam);
}

LRESULT Window::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_SIZE: {
            if (callbacks_.onResize) {
                callbacks_.onResize(LOWORD(lParam), HIWORD(lParam));
            }
            return 0;
        }

        case WM_ERASEBKGND:
            // Se declara manejado para que Windows no borre el fondo: el bufer
            // trasero ya cubre el area completa.
            return 1;

        case WM_PAINT: {
            // Hay que validar la region o Windows reenviaria WM_PAINT sin
            // parar. No se dibuja nada aqui: de eso se encarga el hilo de
            // presentacion.
            PAINTSTRUCT paint{};
            ::BeginPaint(window_, &paint);
            ::EndPaint(window_, &paint);
            return 0;
        }

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            const bool shift   = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
            const bool control = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;

            // Bit 30 de lParam: estado anterior de la tecla. Si ya estaba
            // pulsada, este mensaje lo genero la autorrepeticion de Windows.
            const bool repeated = (lParam & (1LL << 30)) != 0;

            if (callbacks_.onKeyDown) {
                callbacks_.onKeyDown(static_cast<int>(wParam), shift, control, repeated);
            }
            // Alt+F4 y demas combinaciones del sistema deben seguir su curso.
            if (message == WM_SYSKEYDOWN && wParam == VK_F4) break;
            return 0;
        }

        case WM_KEYUP:
        case WM_SYSKEYUP: {
            if (callbacks_.onKeyUp) callbacks_.onKeyUp(static_cast<int>(wParam));
            return 0;
        }

        case WM_KILLFOCUS: {
            // Sin esto, soltar la tecla con la ventana ya desenfocada dejaria
            // el avance por fotogramas corriendo indefinidamente.
            if (callbacks_.onFocusLost) callbacks_.onFocusLost();
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (callbacks_.onMouseMove) {
                callbacks_.onMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            // Capturar el raton permite seguir recibiendo el movimiento aunque
            // el puntero salga de la ventana mientras se arrastra la barra.
            ::SetCapture(window_);
            if (callbacks_.onLeftButtonDown) {
                callbacks_.onLeftButtonDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            ::ReleaseCapture();
            if (callbacks_.onLeftButtonUp) {
                callbacks_.onLeftButtonUp(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            }
            return 0;
        }

        case WM_LBUTTONDBLCLK: {
            if (callbacks_.onDoubleClick) callbacks_.onDoubleClick();
            return 0;
        }

        case WM_MOUSEWHEEL: {
            if (callbacks_.onWheel) {
                // WM_MOUSEWHEEL trae las coordenadas en espacio de PANTALLA,
                // no de cliente, a diferencia del resto de mensajes de raton.
                // Olvidarlo hace que el zoom se ancle en el punto equivocado
                // en cuanto la ventana no esta en el origen del escritorio.
                POINT cursor{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ::ScreenToClient(window_, &cursor);

                const bool control = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
                callbacks_.onWheel(GET_WHEEL_DELTA_WPARAM(wParam),
                                   cursor.x, cursor.y, control);
            }
            return 0;
        }

        case WM_DROPFILES: {
            HandleDroppedFiles(wParam);
            return 0;
        }

        case kOpenRequestMessage: {
            // La cadena llega reservada en el monton desde otro hilo; el
            // unique_ptr garantiza que se libere aunque el callback lance.
            const std::unique_ptr<std::wstring> path(reinterpret_cast<std::wstring*>(lParam));
            if (path && !path->empty() && callbacks_.onFilesDropped) {
                callbacks_.onFilesDropped({*path});
            }
            return 0;
        }

        case WM_SETCURSOR: {
            // Ocultar el cursor en pantalla completa solo tiene efecto si se
            // intercepta aqui; Windows lo restaura en cada movimiento.
            if (LOWORD(lParam) == HTCLIENT && !cursorVisible_) {
                ::SetCursor(nullptr);
                return TRUE;
            }
            break;
        }

        case WM_GETMINMAXINFO: {
            // Un tamano minimo evita divisiones por cero en el calculo de
            // proporciones y cadenas de intercambio degeneradas.
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = 320;
            info->ptMinTrackSize.y = 240;
            return 0;
        }

        case WM_CLOSE: {
            if (callbacks_.onClose) callbacks_.onClose();
            ::DestroyWindow(window_);
            return 0;
        }

        case WM_DESTROY: {
            ::PostQuitMessage(0);
            return 0;
        }

        default:
            break;
    }

    return ::DefWindowProcW(window_, message, wParam, lParam);
}

void Window::HandleDroppedFiles(WPARAM wParam) {
    auto drop = reinterpret_cast<HDROP>(wParam);
    const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);

    std::vector<std::wstring> paths;
    paths.reserve(count);

    for (UINT i = 0; i < count; ++i) {
        const UINT length = ::DragQueryFileW(drop, i, nullptr, 0);
        if (length == 0) continue;

        std::wstring path(length, L'\0');
        // El +1 es el hueco del terminador nulo, que DragQueryFileW escribe
        // siempre aunque no lo cuente en la longitud.
        ::DragQueryFileW(drop, i, path.data(), length + 1);
        paths.push_back(std::move(path));
    }
    ::DragFinish(drop);

    if (!paths.empty() && callbacks_.onFilesDropped) {
        callbacks_.onFilesDropped(paths);
    }
}

// ---------------------------------------------------------------------------
//  Pantalla completa
// ---------------------------------------------------------------------------
void Window::SetFullscreen(bool enabled) {
    if (enabled == fullscreen_ || window_ == nullptr) return;

    if (enabled) {
        savedPlacement_.length = sizeof(savedPlacement_);
        ::GetWindowPlacement(window_, &savedPlacement_);
        savedStyle_   = ::GetWindowLongPtrW(window_, GWL_STYLE);
        savedExStyle_ = ::GetWindowLongPtrW(window_, GWL_EXSTYLE);

        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        ::GetMonitorInfoW(::MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor);

        ::SetWindowLongPtrW(window_, GWL_STYLE,
                            (savedStyle_ & ~(WS_CAPTION | WS_THICKFRAME)) | WS_POPUP);
        ::SetWindowLongPtrW(window_, GWL_EXSTYLE,
                            savedExStyle_ & ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE |
                                              WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));

        const RECT& area = monitor.rcMonitor;
        ::SetWindowPos(window_, HWND_TOP, area.left, area.top,
                       area.right - area.left, area.bottom - area.top,
                       SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        ::SetWindowLongPtrW(window_, GWL_STYLE, savedStyle_);
        ::SetWindowLongPtrW(window_, GWL_EXSTYLE, savedExStyle_);
        ::SetWindowPlacement(window_, &savedPlacement_);
        ::SetWindowPos(window_, nullptr, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER |
                           SWP_FRAMECHANGED);
    }

    fullscreen_ = enabled;
    SetCursorVisible(!enabled);
}

void Window::SetCursorVisible(bool visible) {
    if (visible == cursorVisible_) return;
    cursorVisible_ = visible;

    // Se fuerza un WM_SETCURSOR inmediato para que el cambio se note sin tener
    // que mover el raton.
    ::SetCursor(visible ? ::LoadCursorW(nullptr, IDC_ARROW) : nullptr);
}

void Window::KeepDisplayAwake(bool keepAwake) noexcept {
    // ES_CONTINUOUS mantiene el estado hasta la siguiente llamada; sin el, el
    // efecto duraria un instante.
    ::SetThreadExecutionState(keepAwake
                                  ? (ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED)
                                  : ES_CONTINUOUS);
}

void Window::Destroy() noexcept {
    if (window_ != nullptr) {
        ::DragAcceptFiles(window_, FALSE);
        ::SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
        ::DestroyWindow(window_);
        window_ = nullptr;
    }
    KeepDisplayAwake(false);
}

}  // namespace pyxis
