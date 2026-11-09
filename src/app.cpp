#include "app.h"

#include "overlay.h"
#include "pin_window.h"

namespace sp {

App& App::instance() {
    static App app;
    return app;
}

int App::run(HINSTANCE instance) {
    instance_ = instance;

    mutex_ = CreateMutexW(nullptr, TRUE, kInstanceMutex);
    const DWORD mutexError = GetLastError();
    if (!mutex_) return 1;

    if (mutexError == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(kMainClass, nullptr)) {
            PostMessageW(existing, kSecondLaunchMessage, 0, 0);
        }
        CloseHandle(mutex_);
        mutex_ = nullptr;
        return 0;
    }

    Gdiplus::GdiplusStartupInput gdiplusInput;
    if (Gdiplus::GdiplusStartup(&gdiplusToken_, &gdiplusInput, nullptr) !=
        Gdiplus::Ok) {
        return 1;
    }

    if (!createMainWindow()) {
        Gdiplus::GdiplusShutdown(gdiplusToken_);
        gdiplusToken_ = 0;
        ReleaseMutex(mutex_);
        CloseHandle(mutex_);
        mutex_ = nullptr;
        return 1;
    }

    taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");
    createTrayIcon();

    hotkeyRegistered_ =
        RegisterHotKey(main_, kHotkeyId, kHotkeyModifiers, kHotkeyVirtualKey) !=
        FALSE;
    if (!hotkeyRegistered_) {
        notify(L"热键不可用",
               L"Alt+Shift+S 注册失败，可能被其他程序占用。请从托盘菜单截图。");
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    closeAllPins();
    removeTrayIcon();
    if (hotkeyRegistered_) {
        UnregisterHotKey(main_, kHotkeyId);
        hotkeyRegistered_ = false;
    }
    if (main_) DestroyWindow(main_);

    if (gdiplusToken_) {
        Gdiplus::GdiplusShutdown(gdiplusToken_);
        gdiplusToken_ = 0;
    }
    if (mutex_) {
        ReleaseMutex(mutex_);
        CloseHandle(mutex_);
        mutex_ = nullptr;
    }
    return static_cast<int>(message.wParam);
}

// ----------------------------------------------------------------- capture

void App::requestCapture() {
    if (capturing_ || !instance_) return;

    debugLog(L"capture: begin");
    capturing_ = true;
    auto result = runSelectionOverlay(instance_);
    capturing_ = false;
    debugLog(L"capture: overlay result=%d", result ? 1 : 0);

    if (!result) return;

    const POINT origin = result->origin;
    auto* pin = PinWindow::create(*this, std::move(result->image), origin);
    debugLog(L"capture: pin=%p", static_cast<void*>(pin));
    if (!pin) {
        MessageBoxW(main_, L"创建贴图窗口失败。", kAppTitle,
                    MB_OK | MB_ICONERROR);
        return;
    }

    pins_.push_back(pin);
    copyBitmapToClipboard(pin->hwnd(), pin->visibleImage());
}

void App::requestCloseAll() {
    if (main_) PostMessageW(main_, kCloseAllMessage, 0, 0);
}

void App::closeAllPins() {
    // removePin() mutates pins_ from inside WM_NCDESTROY, so iterate a copy.
    const std::vector<PinWindow*> snapshot = pins_;
    for (PinWindow* pin : snapshot) {
        if (pin && pin->hwnd()) pin->close();
    }
}

void App::removePin(PinWindow* pin) {
    pins_.erase(std::remove(pins_.begin(), pins_.end(), pin), pins_.end());
}

// -------------------------------------------------------------------- tray

bool App::createMainWindow() {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &App::mainProc;
    windowClass.hInstance = instance_;
    windowClass.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP));
    windowClass.lpszClassName = kMainClass;

    if (!RegisterClassExW(&windowClass) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    // A visible (but off-screen and 1x1) window is required for
    // SetForegroundWindow() to work; a hidden window cannot own the tray
    // popup menu's foreground state, which makes the menu ignore Esc/outside
    // clicks.  WS_EX_TOOLWINDOW keeps it out of the taskbar and Alt+Tab.
    main_ = CreateWindowExW(WS_EX_TOOLWINDOW, kMainClass, kAppTitle, WS_POPUP,
                            -32000, -32000, 1, 1, nullptr, nullptr,
                            instance_, this);
    if (!main_) return false;

    ShowWindow(main_, SW_SHOWNOACTIVATE);
    return true;
}

void App::createTrayIcon() {
    trayData_ = {};
    trayData_.cbSize = sizeof(trayData_);
    trayData_.hWnd = main_;
    trayData_.uID = kTrayIconId;
    trayData_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    trayData_.uCallbackMessage = kTrayCallbackMessage;
    trayData_.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP));
    if (!trayData_.hIcon) {
        trayData_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    lstrcpynW(trayData_.szTip, L"ScreenPin · Alt+Shift+S 截图贴图",
              static_cast<int>(std::size(trayData_.szTip)));

    trayAdded_ = Shell_NotifyIconW(NIM_ADD, &trayData_) != FALSE;
}

void App::removeTrayIcon() {
    if (!trayAdded_) return;
    Shell_NotifyIconW(NIM_DELETE, &trayData_);
    trayAdded_ = false;
}

void App::showTrayMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    HWND previousForeground = GetForegroundWindow();

    AppendMenuW(menu, MF_STRING, kCmdCapture, L"截图并贴图\tAlt+Shift+S");
    AppendMenuW(menu, MF_STRING | (pins_.empty() ? MF_GRAYED : MF_ENABLED),
                kCmdCloseAll, L"关闭全部贴图");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdExit, L"退出");

    POINT cursor{};
    GetCursorPos(&cursor);

    forceForeground(main_);
    const UINT command = static_cast<UINT>(TrackPopupMenu(
        menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, cursor.x,
        cursor.y, 0, main_, nullptr));
    DestroyMenu(menu);
    PostMessageW(main_, WM_NULL, 0, 0);

    if (command) handleCommand(command);

    // The popup had to take foreground so Esc/outside-click work.  Hand focus
    // back to the previous app unless the command was "exit".
    if (command != kCmdExit && previousForeground &&
        previousForeground != main_ && IsWindow(previousForeground)) {
        SetForegroundWindow(previousForeground);
    }
}

void App::handleCommand(UINT command) {
    switch (command) {
        case kCmdCapture:
            requestCapture();
            break;
        case kCmdCloseAll:
            closeAllPins();
            break;
        case kCmdExit:
            shutdown();
            break;
        default:
            break;
    }
}

void App::notify(const wchar_t* title, const wchar_t* message) {
    if (!trayAdded_) return;

    NOTIFYICONDATAW data = trayData_;
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = NIIF_WARNING;
    data.uTimeout = 5000;
    lstrcpynW(data.szInfoTitle, title,
              static_cast<int>(std::size(data.szInfoTitle)));
    lstrcpynW(data.szInfo, message, static_cast<int>(std::size(data.szInfo)));
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void App::shutdown() {
    closeAllPins();
    removeTrayIcon();
    if (hotkeyRegistered_) {
        UnregisterHotKey(main_, kHotkeyId);
        hotkeyRegistered_ = false;
    }
    if (main_) DestroyWindow(main_);
}

// ---------------------------------------------------------------- messages

LRESULT CALLBACK App::mainProc(HWND hwnd, UINT message, WPARAM wParam,
                               LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* self = static_cast<App*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->main_ = hwnd;
        return TRUE;
    }

    auto* self = reinterpret_cast<App*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, message, wParam, lParam);
    return self->handleMainMessage(hwnd, message, wParam, lParam);
}

LRESULT App::handleMainMessage(HWND hwnd, UINT message, WPARAM wParam,
                               LPARAM lParam) {
    if (taskbarCreatedMessage_ != 0 && message == taskbarCreatedMessage_) {
        // Explorer restarted; put the tray icon back.
        trayAdded_ = false;
        createTrayIcon();
        return 0;
    }

    switch (message) {
        case WM_HOTKEY:
            if (wParam == kHotkeyId) requestCapture();
            return 0;

        case kTrayCallbackMessage:
            switch (LOWORD(lParam)) {
                case WM_LBUTTONDBLCLK:
                    requestCapture();
                    break;
                case WM_RBUTTONUP:
                case WM_CONTEXTMENU:
                    showTrayMenu();
                    break;
                default:
                    break;
            }
            return 0;

        case kSecondLaunchMessage:
            requestCapture();
            return 0;

        case kCloseAllMessage:
            closeAllPins();
            return 0;

        case WM_COMMAND:
            handleCommand(LOWORD(wParam));
            return 0;

        case WM_DESTROY:
            main_ = nullptr;
            PostQuitMessage(0);
            return 0;

        case WM_CLOSE:
            shutdown();
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace sp
