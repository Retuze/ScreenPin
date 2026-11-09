#include "common.h"

#include <cstdarg>
#include <cstring>

namespace sp {
namespace {

bool openClipboardWithRetry(HWND owner) noexcept {
    // The clipboard is a shared, occasionally busy resource.  A short retry
    // loop avoids failing a capture just because another app is mid-copy.
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (OpenClipboard(owner)) return true;
        Sleep(5);
    }
    return false;
}

bool findEncoderClsid(const wchar_t* mime, CLSID& clsid) {
    UINT count = 0;
    UINT bytes = 0;
    if (Gdiplus::GetImageEncodersSize(&count, &bytes) != Gdiplus::Ok || bytes == 0) {
        return false;
    }

    std::vector<BYTE> storage(bytes);
    auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(storage.data());
    if (Gdiplus::GetImageEncoders(count, bytes, codecs) != Gdiplus::Ok) {
        return false;
    }

    for (UINT i = 0; i < count; ++i) {
        if (std::wcscmp(codecs[i].MimeType, mime) == 0) {
            clsid = codecs[i].Clsid;
            return true;
        }
    }
    return false;
}

struct CaptureContext {
    Bitmap32* target{};
    RECT virtualRect{};
    bool capturedAny{};
};

BOOL CALLBACK captureMonitorProc(HMONITOR monitor, HDC, LPRECT, LPARAM param) {
    auto& context = *reinterpret_cast<CaptureContext*>(param);

    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return TRUE;

    HDC monitorDc = CreateDCW(L"DISPLAY", info.szDevice, nullptr, nullptr);
    if (!monitorDc) return TRUE;

    const int x = info.rcMonitor.left - context.virtualRect.left;
    const int y = info.rcMonitor.top - context.virtualRect.top;
    const int w = rectWidth(info.rcMonitor);
    const int h = rectHeight(info.rcMonitor);

    BOOL ok = BitBlt(context.target->dc(), x, y, w, h, monitorDc, 0, 0,
                     SRCCOPY | CAPTUREBLT);
    if (!ok) {
        ok = BitBlt(context.target->dc(), x, y, w, h, monitorDc, 0, 0, SRCCOPY);
    }

    DeleteDC(monitorDc);
    context.capturedAny = context.capturedAny || (ok != FALSE);
    return TRUE;
}

}  // namespace

// --------------------------------------------------------------- Bitmap32

bool Bitmap32::create(int width, int height) {
    if (width <= 0 || height <= 0) return false;
    destroy();

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;  // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap || !pixels) {
        if (bitmap) DeleteObject(bitmap);
        return false;
    }

    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) {
        DeleteObject(bitmap);
        return false;
    }

    HGDIOBJ previous = SelectObject(dc, bitmap);
    width_ = width;
    height_ = height;
    dc_ = dc;
    bitmap_ = bitmap;
    previous_ = previous;
    pixels_ = pixels;
    std::memset(pixels, 0, static_cast<std::size_t>(width) * height * sizeof(std::uint32_t));
    return true;
}

void Bitmap32::destroy() noexcept {
    if (dc_) {
        if (previous_) SelectObject(dc_, previous_);
        DeleteDC(dc_);
    }
    if (bitmap_) DeleteObject(bitmap_);
    width_ = 0;
    height_ = 0;
    dc_ = nullptr;
    bitmap_ = nullptr;
    previous_ = nullptr;
    pixels_ = nullptr;
}

void Bitmap32::swap(Bitmap32& other) noexcept {
    std::swap(width_, other.width_);
    std::swap(height_, other.height_);
    std::swap(dc_, other.dc_);
    std::swap(bitmap_, other.bitmap_);
    std::swap(previous_, other.previous_);
    std::swap(pixels_, other.pixels_);
}

void Bitmap32::fill(std::uint32_t bgra) noexcept {
    if (!valid()) return;
    auto* first = data();
    std::fill(first, first + static_cast<std::size_t>(width_) * height_, bgra);
}

void Bitmap32::makeOpaque() noexcept {
    if (!valid()) return;
    auto* pixels = data();
    const std::size_t count = static_cast<std::size_t>(width_) * height_;
    for (std::size_t i = 0; i < count; ++i) {
        pixels[i] |= 0xFF000000u;
    }
}

Bitmap32 Bitmap32::cropped(const RECT& region) const {
    if (!valid()) return {};

    RECT r = region;
    r.left = clampLong(r.left, 0, width_);
    r.right = clampLong(r.right, 0, width_);
    r.top = clampLong(r.top, 0, height_);
    r.bottom = clampLong(r.bottom, 0, height_);
    if (rectEmpty(r)) return {};

    Bitmap32 out(rectWidth(r), rectHeight(r));
    if (!out) return out;
    BitBlt(out.dc_, 0, 0, out.width(), out.height(), dc_, r.left, r.top, SRCCOPY);
    return out;
}

// ---------------------------------------------------------------- screen

RECT virtualScreenRect() noexcept {
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int cx = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int cy = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (cx <= 0 || cy <= 0) {
        return {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    }
    return {x, y, x + cx, y + cy};
}

std::optional<Bitmap32> captureVirtualScreen() {
    const RECT virtualRect = virtualScreenRect();
    Bitmap32 shot(rectWidth(virtualRect), rectHeight(virtualRect));
    if (!shot) return std::nullopt;

    CaptureContext context{&shot, virtualRect, false};
    EnumDisplayMonitors(nullptr, nullptr, &captureMonitorProc,
                        reinterpret_cast<LPARAM>(&context));

    if (!context.capturedAny) {
        HDC screen = GetDC(nullptr);
        if (screen) {
            BitBlt(shot.dc(), 0, 0, shot.width(), shot.height(), screen,
                   virtualRect.left, virtualRect.top, SRCCOPY | CAPTUREBLT);
            ReleaseDC(nullptr, screen);
        }
    }

    shot.makeOpaque();
    return shot;
}

// ------------------------------------------------------------- clipboard

bool copyBitmapToClipboard(HWND owner, const Bitmap32& bitmap) {
    if (!bitmap.valid()) return false;

    const int width = bitmap.width();
    const int height = bitmap.height();
    const int stride = (width * 3 + 3) & ~3;
    const SIZE_T imageBytes = static_cast<SIZE_T>(stride) * height;

    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + imageBytes);
    if (!memory) return false;

    auto* header = static_cast<BITMAPINFOHEADER*>(GlobalLock(memory));
    if (!header) {
        GlobalFree(memory);
        return false;
    }

    *header = {};
    header->biSize = sizeof(BITMAPINFOHEADER);
    header->biWidth = width;
    header->biHeight = height;  // bottom-up for CF_DIB
    header->biPlanes = 1;
    header->biBitCount = 24;
    header->biCompression = BI_RGB;
    header->biSizeImage = static_cast<DWORD>(imageBytes);

    auto* bytes = reinterpret_cast<BYTE*>(header + 1);
    for (int y = 0; y < height; ++y) {
        const auto source = bitmap.row(height - 1 - y);
        BYTE* destination = bytes + static_cast<std::ptrdiff_t>(y) * stride;
        for (int x = 0; x < width; ++x) {
            const std::uint32_t pixel = source[static_cast<std::size_t>(x)];
            destination[x * 3 + 0] = static_cast<BYTE>(pixel & 0xFF);
            destination[x * 3 + 1] = static_cast<BYTE>((pixel >> 8) & 0xFF);
            destination[x * 3 + 2] = static_cast<BYTE>((pixel >> 16) & 0xFF);
        }
    }
    GlobalUnlock(memory);

    if (!openClipboardWithRetry(owner)) {
        GlobalFree(memory);
        return false;
    }

    EmptyClipboard();
    if (!SetClipboardData(CF_DIB, memory)) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

// -------------------------------------------------------------------- png

bool saveBitmapAsPng(const Bitmap32& bitmap, const std::wstring& path) {
    if (!bitmap.valid() || path.empty()) return false;

    Gdiplus::Bitmap image(
        bitmap.width(), bitmap.height(), bitmap.width() * 4,
        PixelFormat32bppRGB,
        reinterpret_cast<BYTE*>(const_cast<std::uint32_t*>(bitmap.data())));
    if (image.GetLastStatus() != Gdiplus::Ok) return false;

    CLSID encoder{};
    if (!findEncoderClsid(L"image/png", encoder)) return false;
    return image.Save(path.c_str(), &encoder, nullptr) == Gdiplus::Ok;
}

bool pickPngSavePath(HWND owner, std::wstring& path) {
    wchar_t file[MAX_PATH] = L"ScreenPin.png";
    const wchar_t filter[] =
        L"PNG 图片 (*.png)\0*.png\0所有文件 (*.*)\0*.*\0\0";

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = file;
    dialog.nMaxFile = static_cast<DWORD>(std::size(file));
    dialog.lpstrDefExt = L"png";
    dialog.lpstrTitle = L"另存为 PNG";
    dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT |
                   OFN_NOCHANGEDIR;

    if (!GetSaveFileNameW(&dialog)) return false;
    path = file;
    return true;
}

// ------------------------------------------------------------------- misc

void forceForeground(HWND hwnd) {
    if (!hwnd) return;

    HWND foreground = GetForegroundWindow();
    if (foreground == hwnd) {
        SetFocus(hwnd);
        return;
    }

    const DWORD foregroundThread =
        foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    const DWORD currentThread = GetCurrentThreadId();

    bool attached = false;
    if (foregroundThread && foregroundThread != currentThread) {
        attached = AttachThreadInput(currentThread, foregroundThread, TRUE) != FALSE;
    }

    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);

    if (attached) {
        AttachThreadInput(currentThread, foregroundThread, FALSE);
    }
}

std::wstring formatPixels(int width, int height) {
    wchar_t buffer[64]{};
    std::swprintf(buffer, std::size(buffer), L"%d × %d", width, height);
    return buffer;
}

void debugLog(const wchar_t* format, ...) {
    static const bool enabled = [] {
        wchar_t value[8]{};
        const DWORD length = GetEnvironmentVariableW(
            L"SCREENPIN_LOG", value, static_cast<DWORD>(std::size(value)));
        return length > 0 && (value[0] == L'1' || value[0] == L't' ||
                              value[0] == L'T');
    }();
    if (!enabled) return;

    wchar_t message[1024]{};
    va_list args;
    va_start(args, format);
    std::vswprintf(message, std::size(message), format, args);
    va_end(args);

    wchar_t directory[MAX_PATH]{};
    const DWORD length = GetTempPathW(MAX_PATH, directory);
    if (length == 0 || length >= MAX_PATH) return;

    std::wstring path(directory);
    path += L"ScreenPin.log";

    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t line[1200]{};
    std::swprintf(line, std::size(line),
                  L"%02u:%02u:%02u.%03u [%lu] %ls\r\n", now.wHour,
                  now.wMinute, now.wSecond, now.wMilliseconds,
                  GetCurrentThreadId(), message);

    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, line,
              static_cast<DWORD>(std::wcslen(line) * sizeof(wchar_t)),
              &written, nullptr);
    CloseHandle(file);
}

}  // namespace sp
