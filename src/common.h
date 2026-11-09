// ScreenPin - a small Win32 screenshot-to-pin utility.
// Common helpers: geometry, DPI, 32-bit DIB wrapper, capture, clipboard, PNG.
#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <shellapi.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "resource.h"

namespace sp {

// ---------------------------------------------------------------- constants

inline constexpr wchar_t kAppName[] = L"ScreenPin";
inline constexpr wchar_t kAppTitle[] = L"ScreenPin · 贴图截图";
inline constexpr wchar_t kMainClass[] = L"ScreenPin.MainWindow";
inline constexpr wchar_t kOverlayClass[] = L"ScreenPin.SelectionOverlay";
inline constexpr wchar_t kPinClass[] = L"ScreenPin.PinWindow";
inline constexpr wchar_t kInstanceMutex[] = L"ScreenPin.SingleInstance.v1";

inline constexpr int kHotkeyId = 1;
inline constexpr UINT kHotkeyModifiers = MOD_ALT | MOD_SHIFT | MOD_NOREPEAT;
inline constexpr UINT kHotkeyVirtualKey = 'S';

inline constexpr UINT kTrayCallbackMessage = WM_APP + 1;
inline constexpr UINT kSecondLaunchMessage = WM_APP + 2;
inline constexpr UINT kCloseAllMessage = WM_APP + 3;
inline constexpr UINT kTrayIconId = 1;

enum MenuCommand : UINT {
    kCmdCapture = 1001,
    kCmdCloseAll,
    kCmdAutoStart,
    kCmdExit,

    kCmdPinCopy = 1101,
    kCmdPinSave,
    kCmdPinClose,
    kCmdPinCloseAll,
};

inline constexpr COLORREF kAccent = RGB(0, 120, 212);
inline constexpr COLORREF kAccentBright = RGB(0, 170, 255);
inline constexpr COLORREF kPanel = RGB(28, 28, 30);

// ----------------------------------------------------------------- geometry

inline int rectWidth(const RECT& r) noexcept { return r.right - r.left; }
inline int rectHeight(const RECT& r) noexcept { return r.bottom - r.top; }

inline bool rectEmpty(const RECT& r) noexcept {
    return r.right <= r.left || r.bottom <= r.top;
}

inline POINT rectCenter(const RECT& r) noexcept {
    return {r.left + rectWidth(r) / 2, r.top + rectHeight(r) / 2};
}

inline RECT normalizedRect(POINT a, POINT b) noexcept {
    return {std::min(a.x, b.x), std::min(a.y, b.y),
            std::max(a.x, b.x), std::max(a.y, b.y)};
}

inline bool rectContains(const RECT& r, POINT p) noexcept {
    return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
}

inline RECT insetRect(RECT r, int dx, int dy) noexcept {
    r.left += dx;
    r.top += dy;
    r.right -= dx;
    r.bottom -= dy;
    return r;
}

// ---------------------------------------------------------------------- dpi

inline UINT dpiFor(HWND hwnd) noexcept {
    return hwnd ? GetDpiForWindow(hwnd) : GetDpiForSystem();
}

inline int dip(int value, UINT dpi) noexcept {
    return MulDiv(value, static_cast<int>(dpi), 96);
}

// RECT / POINT members are LONG on Windows.  These overloads keep call sites
// free of ambiguous std::clamp deductions.
inline LONG clampLong(LONG value, LONG low, LONG high) noexcept {
    return std::clamp(value, low, high);
}

inline int clampInt(int value, int low, int high) noexcept {
    return std::clamp(value, low, high);
}

// ------------------------------------------------------------- 32-bit DIB

// Top-down 32bpp BI_RGB DIB with an owning memory DC.  GDI ignores the alpha
// byte for BI_RGB; capture() normalizes it to 0xFF so the same buffer can be
// handed to GDI+ / clipboard code without surprises.
class Bitmap32 {
public:
    Bitmap32() = default;
    explicit Bitmap32(int width, int height) { create(width, height); }
    ~Bitmap32() { destroy(); }

    Bitmap32(const Bitmap32&) = delete;
    Bitmap32& operator=(const Bitmap32&) = delete;
    Bitmap32(Bitmap32&& other) noexcept { swap(other); }
    Bitmap32& operator=(Bitmap32&& other) noexcept {
        if (this != &other) {
            destroy();
            swap(other);
        }
        return *this;
    }

    bool create(int width, int height);
    bool valid() const noexcept { return dc_ != nullptr && pixels_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    int width() const noexcept { return width_; }
    int height() const noexcept { return height_; }
    HDC dc() const noexcept { return dc_; }
    HBITMAP bitmap() const noexcept { return bitmap_; }
    std::uint32_t* data() noexcept { return static_cast<std::uint32_t*>(pixels_); }
    const std::uint32_t* data() const noexcept {
        return static_cast<const std::uint32_t*>(pixels_);
    }

    std::span<std::uint32_t> row(int y) noexcept {
        return {data() + static_cast<std::ptrdiff_t>(y) * width_, static_cast<std::size_t>(width_)};
    }
    std::span<const std::uint32_t> row(int y) const noexcept {
        return {data() + static_cast<std::ptrdiff_t>(y) * width_, static_cast<std::size_t>(width_)};
    }

    void fill(std::uint32_t bgra) noexcept;
    void makeOpaque() noexcept;
    Bitmap32 cropped(const RECT& region) const;

    void swap(Bitmap32& other) noexcept;

private:
    void destroy() noexcept;

    int width_{};
    int height_{};
    HDC dc_{};
    HBITMAP bitmap_{};
    HGDIOBJ previous_{};
    void* pixels_{};
};

// ------------------------------------------------------------------ screen

RECT virtualScreenRect() noexcept;
std::optional<Bitmap32> captureVirtualScreen();

// --------------------------------------------------------------- clipboard

bool copyBitmapToClipboard(HWND owner, const Bitmap32& bitmap);

// --------------------------------------------------------------------- png

bool saveBitmapAsPng(const Bitmap32& bitmap, const std::wstring& path);
bool pickPngSavePath(HWND owner, std::wstring& path);

// ------------------------------------------------------------------- misc

void forceForeground(HWND hwnd);
std::wstring formatPixels(int width, int height);

// Lightweight diagnostics; appended to %TEMP%\ScreenPin.log.
void debugLog(const wchar_t* format, ...);

struct SelectionResult {
    Bitmap32 image;
    POINT origin{};
};

}  // namespace sp
