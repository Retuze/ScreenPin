#pragma once

#include "common.h"

#include <cstddef>

namespace sp {

class PinWindow;

class App {
public:
    static App& instance();

    int run(HINSTANCE instance);

    void requestCapture();
    void requestCloseAll();
    void closeAllPins();
    void removePin(PinWindow* pin);

    std::size_t pinCount() const noexcept { return pins_.size(); }
    HINSTANCE instanceHandle() const noexcept { return instance_; }
    HWND mainWindow() const noexcept { return main_; }

private:
    App() = default;
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    static LRESULT CALLBACK mainProc(HWND hwnd, UINT message, WPARAM wParam,
                                     LPARAM lParam);
    LRESULT handleMainMessage(HWND hwnd, UINT message, WPARAM wParam,
                              LPARAM lParam);

    bool createMainWindow();
    void createTrayIcon();
    void removeTrayIcon();
    void showTrayMenu();
    void handleCommand(UINT command);
    void notify(const wchar_t* title, const wchar_t* message);
    void shutdown();

    HINSTANCE instance_{};
    HWND main_{};
    HANDLE mutex_{};
    ULONG_PTR gdiplusToken_{};
    std::vector<PinWindow*> pins_;
    bool capturing_{};
    bool trayAdded_{};
    bool hotkeyRegistered_{};
    UINT taskbarCreatedMessage_{};
    NOTIFYICONDATAW trayData_{};
};

}  // namespace sp
