#include "overlay.h"

#include <string_view>

namespace sp {
namespace {

constexpr int kMinSelection = 3;

class SelectionOverlay {
public:
    SelectionOverlay(HINSTANCE instance, RECT virtualRect, Bitmap32 shot)
        : instance_(instance),
          virtualRect_(virtualRect),
          shot_(std::move(shot)),
          dpi_(GetDpiForSystem()) {
        buildDimmed();
        canvas_ = Bitmap32(shot_.width(), shot_.height());
        createFont();
        cursorClient_ = {shot_.width() / 2, shot_.height() / 2};
    }

    ~SelectionOverlay() {
        if (fontOwned_ && font_) DeleteObject(font_);
    }

    SelectionOverlay(const SelectionOverlay&) = delete;
    SelectionOverlay& operator=(const SelectionOverlay&) = delete;

    std::optional<SelectionResult> run() {
        if (!shot_.valid() || !dim_.valid() || !canvas_.valid()) {
            return std::nullopt;
        }

        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = &SelectionOverlay::windowProc;
        windowClass.hInstance = instance_;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_CROSS);
        windowClass.lpszClassName = kOverlayClass;
        RegisterClassExW(&windowClass);

        hwnd_ = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kOverlayClass, L"", WS_POPUP,
            virtualRect_.left, virtualRect_.top, shot_.width(), shot_.height(),
            nullptr, nullptr, instance_, this);
        if (!hwnd_) return std::nullopt;

        ShowWindow(hwnd_, SW_SHOW);
        SetWindowPos(hwnd_, HWND_TOPMOST, virtualRect_.left, virtualRect_.top,
                     shot_.width(), shot_.height(),
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        // Paint the first complete frame before returning to the message loop;
        // otherwise the compositor can show one empty frame on a fast machine.
        UpdateWindow(hwnd_);
        forceForeground(hwnd_);
        SetCursor(LoadCursorW(nullptr, IDC_CROSS));

        MSG message{};
        while (!done_) {
            const BOOL result = GetMessageW(&message, nullptr, 0, 0);
            if (result <= 0) break;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        if (hwnd_ && IsWindow(hwnd_)) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
        return std::move(result_);
    }

private:
    // ------------------------------------------------------------- painting

    void buildDimmed() {
        dim_ = Bitmap32(shot_.width(), shot_.height());
        if (!dim_) return;

        const std::size_t count =
            static_cast<std::size_t>(shot_.width()) * shot_.height();
        const auto* source = shot_.data();
        auto* target = dim_.data();
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint32_t pixel = source[i];
            const std::uint32_t halved = ((pixel & 0x00FFFFFFu) >> 1) & 0x007F7F7Fu;
            target[i] = 0xFF000000u | halved;
        }
    }

    void createFont() {
        font_ = CreateFontW(-dip(14, dpi_), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE,
                            FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        if (font_) {
            fontOwned_ = true;
            return;
        }
        font_ = CreateFontW(-dip(14, dpi_), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE,
                            FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        if (font_) {
            fontOwned_ = true;
        } else {
            font_ = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        }
    }

    void paint(HDC windowDc) {
        HDC dc = windowDc;
        if (canvas_.valid()) dc = canvas_.dc();

        BitBlt(dc, 0, 0, shot_.width(), shot_.height(), dim_.dc(), 0, 0, SRCCOPY);

        if (dragging_) {
            const RECT selected = selectionRect();
            if (!rectEmpty(selected)) {
                BitBlt(dc, selected.left, selected.top, rectWidth(selected),
                       rectHeight(selected), shot_.dc(), selected.left,
                       selected.top, SRCCOPY);
                drawSelectionFrame(dc, selected);
                drawSizeLabel(dc, selected);
            }
        } else {
            drawHint(dc);
        }

        if (hasCursor_) drawMagnifier(dc);

        if (dc != windowDc) {
            BitBlt(windowDc, 0, 0, shot_.width(), shot_.height(), dc, 0, 0,
                   SRCCOPY);
        }
    }

    void drawSelectionFrame(HDC dc, const RECT& r) const {
        const int thickness = dip(2, dpi_);
        HPEN pen = CreatePen(PS_SOLID, thickness, kAccent);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, r.left, r.top, r.right, r.bottom);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(pen);

        // Small white corner ticks make the selection readable on dark and
        // light content alike.
        const int tick = dip(9, dpi_);
        const int width = dip(2, dpi_);
        HPEN tickPen = CreatePen(PS_SOLID, width, RGB(255, 255, 255));
        oldPen = SelectObject(dc, tickPen);
        MoveToEx(dc, r.left, r.top, nullptr);
        LineTo(dc, r.left + tick, r.top);
        MoveToEx(dc, r.left, r.top, nullptr);
        LineTo(dc, r.left, r.top + tick);
        MoveToEx(dc, r.right - tick, r.top, nullptr);
        LineTo(dc, r.right, r.top);
        MoveToEx(dc, r.right, r.top, nullptr);
        LineTo(dc, r.right, r.top + tick);
        MoveToEx(dc, r.left, r.bottom - tick, nullptr);
        LineTo(dc, r.left, r.bottom);
        MoveToEx(dc, r.left, r.bottom, nullptr);
        LineTo(dc, r.left + tick, r.bottom);
        MoveToEx(dc, r.right - tick, r.bottom, nullptr);
        LineTo(dc, r.right, r.bottom);
        MoveToEx(dc, r.right, r.bottom - tick, nullptr);
        LineTo(dc, r.right, r.bottom);
        SelectObject(dc, oldPen);
        DeleteObject(tickPen);
    }

    void drawSizeLabel(HDC dc, const RECT& selection) const {
        const std::wstring text =
            formatPixels(rectWidth(selection), rectHeight(selection));
        const int padX = dip(10, dpi_);
        const int padY = dip(5, dpi_);

        HGDIOBJ oldFont = SelectObject(dc, font_);
        SIZE textSize{};
        GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()),
                              &textSize);
        SelectObject(dc, oldFont);

        const int width = textSize.cx + padX * 2;
        const int height = textSize.cy + padY * 2;

        const MONITORINFO monitor = monitorInfoFor(selection);
        const RECT bounds = monitor.rcMonitor;

        int x = selection.left;
        int y = selection.top - height - dip(8, dpi_);
        if (y < bounds.top + dip(4, dpi_)) {
            y = selection.top + dip(8, dpi_);
        }
        if (x + width > bounds.right - dip(4, dpi_)) {
            x = bounds.right - width - dip(4, dpi_);
        }
        if (x < bounds.left + dip(4, dpi_)) {
            x = bounds.left + dip(4, dpi_);
        }

        RECT pill{x, y, x + width, y + height};
        drawPill(dc, pill, dip(6, dpi_), kAccent, kAccent);
        drawCenteredText(dc, pill, text, RGB(255, 255, 255), font_);
    }

    void drawHint(HDC dc) const {
        const std::wstring_view text = L"拖动鼠标框选区域　·　右键 / Esc 取消";
        const int padX = dip(14, dpi_);
        const int padY = dip(8, dpi_);

        HGDIOBJ oldFont = SelectObject(dc, font_);
        SIZE textSize{};
        GetTextExtentPoint32W(dc, text.data(), static_cast<int>(text.size()),
                              &textSize);
        SelectObject(dc, oldFont);

        const int width = textSize.cx + padX * 2;
        const int height = textSize.cy + padY * 2;

        const MONITORINFO monitor = hintMonitor();
        const int x = monitor.rcMonitor.left +
                      (rectWidth(monitor.rcMonitor) - width) / 2;
        const int y = monitor.rcMonitor.top + dip(28, dpi_);

        RECT pill{x, y, x + width, y + height};
        drawPill(dc, pill, dip(8, dpi_), kPanel, RGB(70, 70, 74));
        drawCenteredText(dc, pill, text, RGB(245, 245, 245), font_);
    }

    void drawMagnifier(HDC dc) const {
        constexpr int kSourceSpan = 19;
        const int box = dip(140, dpi_);
        const int border = dip(2, dpi_);
        const int total = box + border * 2;

        RECT client{};
        GetClientRect(hwnd_, &client);

        int left = cursorClient_.x + dip(26, dpi_);
        int top = cursorClient_.y + dip(26, dpi_);
        if (left + total > client.right) {
            left = cursorClient_.x - dip(26, dpi_) - total;
        }
        if (top + total > client.bottom) {
            top = cursorClient_.y - dip(26, dpi_) - total;
        }
        const int maxLeft = std::max(0, static_cast<int>(client.right) - total);
        const int maxTop = std::max(0, static_cast<int>(client.bottom) - total);
        left = std::clamp(left, 0, maxLeft);
        top = std::clamp(top, 0, maxTop);

        const int sourceX = static_cast<int>(clampLong(
            cursorClient_.x - kSourceSpan / 2, 0,
            shot_.width() - kSourceSpan));
        const int sourceY = static_cast<int>(clampLong(
            cursorClient_.y - kSourceSpan / 2, 0,
            shot_.height() - kSourceSpan));

        RECT frame{left, top, left + total, top + total};
        HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
        FillRect(dc, &frame, white);
        DeleteObject(white);

        SetStretchBltMode(dc, COLORONCOLOR);
        StretchBlt(dc, left + border, top + border, box, box, shot_.dc(),
                   sourceX, sourceY, kSourceSpan, kSourceSpan, SRCCOPY);

        const int centerX = left + border + box / 2;
        const int centerY = top + border + box / 2;
        HPEN crosshair = CreatePen(PS_SOLID, 1, RGB(255, 80, 80));
        HGDIOBJ oldPen = SelectObject(dc, crosshair);
        MoveToEx(dc, centerX - box / 2, centerY, nullptr);
        LineTo(dc, centerX - dip(4, dpi_), centerY);
        MoveToEx(dc, centerX + dip(4, dpi_), centerY, nullptr);
        LineTo(dc, centerX + box / 2, centerY);
        MoveToEx(dc, centerX, centerY - box / 2, nullptr);
        LineTo(dc, centerX, centerY - dip(4, dpi_));
        MoveToEx(dc, centerX, centerY + dip(4, dpi_), nullptr);
        LineTo(dc, centerX, centerY + box / 2);
        SelectObject(dc, oldPen);
        DeleteObject(crosshair);

        // Screen coordinate readout below the loupe.
        wchar_t coordinateText[64]{};
        std::swprintf(coordinateText, std::size(coordinateText), L"%d, %d",
                      virtualRect_.left + cursorClient_.x,
                      virtualRect_.top + cursorClient_.y);
        const int labelPad = dip(6, dpi_);
        SIZE labelSize{};
        HGDIOBJ oldFont = SelectObject(dc, font_);
        GetTextExtentPoint32W(dc, coordinateText,
                              static_cast<int>(std::wcslen(coordinateText)),
                              &labelSize);
        SelectObject(dc, oldFont);

        int labelX = left + (total - (labelSize.cx + labelPad * 2)) / 2;
        int labelY = top + total + dip(6, dpi_);
        if (labelY + labelSize.cy + labelPad * 2 > client.bottom) {
            labelY = top - labelSize.cy - labelPad * 2 - dip(6, dpi_);
        }
        RECT label{labelX, labelY, labelX + labelSize.cx + labelPad * 2,
                   labelY + labelSize.cy + labelPad * 2};
        drawPill(dc, label, dip(5, dpi_), kPanel, RGB(70, 70, 74));
        drawCenteredText(dc, label, coordinateText, RGB(240, 240, 240), font_);
    }

    // ------------------------------------------------------------- helpers

    void drawPill(HDC dc, const RECT& r, int radius, COLORREF fill,
                  COLORREF border) const {
        HBRUSH brush = CreateSolidBrush(fill);
        HPEN pen = CreatePen(PS_SOLID, 1, border);
        HGDIOBJ oldBrush = SelectObject(dc, brush);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(pen);
        DeleteObject(brush);
    }

    void drawCenteredText(HDC dc, const RECT& r, std::wstring_view text,
                          COLORREF color, HFONT font) const {
        HGDIOBJ oldFont = SelectObject(dc, font);
        const int oldMode = SetBkMode(dc, TRANSPARENT);
        const COLORREF oldColor = SetTextColor(dc, color);
        RECT area = r;
        DrawTextW(dc, text.data(), static_cast<int>(text.size()), &area,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SetTextColor(dc, oldColor);
        SetBkMode(dc, oldMode);
        SelectObject(dc, oldFont);
    }

    MONITORINFO monitorInfoFor(const RECT& clientRect) const {
        RECT screenRect = clientRect;
        OffsetRect(&screenRect, virtualRect_.left, virtualRect_.top);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        const HMONITOR monitor =
            MonitorFromRect(&screenRect, MONITOR_DEFAULTTONEAREST);
        GetMonitorInfoW(monitor, &info);
        return info;
    }

    MONITORINFO hintMonitor() const {
        POINT screenPoint{};
        if (hasCursor_) {
            screenPoint = {virtualRect_.left + cursorClient_.x,
                           virtualRect_.top + cursorClient_.y};
        } else {
            screenPoint = {virtualRect_.left + shot_.width() / 2,
                           virtualRect_.top + shot_.height() / 2};
        }
        const HMONITOR monitor =
            MonitorFromPoint(screenPoint, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        GetMonitorInfoW(monitor, &info);
        return info;
    }

    RECT selectionRect() const {
        RECT r = normalizedRect(start_, current_);
        r.left = clampLong(r.left, 0, shot_.width());
        r.right = clampLong(r.right, 0, shot_.width());
        r.top = clampLong(r.top, 0, shot_.height());
        r.bottom = clampLong(r.bottom, 0, shot_.height());
        return r;
    }

    POINT clampPoint(POINT p) const {
        return {clampLong(p.x, 0, std::max(0, shot_.width() - 1)),
                clampLong(p.y, 0, std::max(0, shot_.height() - 1))};
    }

    void trackLeave() {
        if (trackingLeave_) return;
        TRACKMOUSEEVENT event{};
        event.cbSize = sizeof(event);
        event.dwFlags = TME_LEAVE;
        event.hwndTrack = hwnd_;
        if (TrackMouseEvent(&event)) trackingLeave_ = true;
    }

    void completeSelection() {
        if (done_) return;
        const RECT r = selectionRect();
        if (rectWidth(r) >= kMinSelection && rectHeight(r) >= kMinSelection) {
            Bitmap32 image = shot_.cropped(r);
            if (image) {
                result_ = SelectionResult{
                    std::move(image),
                    {virtualRect_.left + r.left, virtualRect_.top + r.top}};
                done_ = true;
            }
        }
        if (!done_) {
            dragging_ = false;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        ReleaseCapture();
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }

    void cancel() {
        if (done_) return;
        done_ = true;
        ReleaseCapture();
        if (hwnd_) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
    }

    // ------------------------------------------------------------- messages

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam,
                                       LPARAM lParam) {
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            auto* self = static_cast<SelectionOverlay*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
            return TRUE;
        }

        auto* self = reinterpret_cast<SelectionOverlay*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!self) return DefWindowProcW(hwnd, message, wParam, lParam);
        return self->handleMessage(message, wParam, lParam);
    }

    LRESULT handleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
            case WM_ERASEBKGND:
                return 1;

            case WM_PAINT: {
                PAINTSTRUCT paintStruct{};
                HDC dc = BeginPaint(hwnd_, &paintStruct);
                paint(dc);
                EndPaint(hwnd_, &paintStruct);
                return 0;
            }

            case WM_MOUSEACTIVATE:
                return MA_ACTIVATE;

            case WM_SETCURSOR:
                if (LOWORD(lParam) == HTCLIENT) {
                    SetCursor(LoadCursorW(nullptr, IDC_CROSS));
                    return TRUE;
                }
                break;

            case WM_MOUSEMOVE: {
                POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                point = clampPoint(point);
                cursorClient_ = point;
                hasCursor_ = true;
                trackLeave();
                if (dragging_) current_ = point;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }

            case WM_MOUSELEAVE:
                trackingLeave_ = false;
                hasCursor_ = false;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;

            case WM_LBUTTONDOWN: {
                POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                point = clampPoint(point);
                dragging_ = true;
                start_ = point;
                current_ = point;
                hasCursor_ = true;
                SetCapture(hwnd_);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }

            case WM_LBUTTONUP:
                if (dragging_) {
                    dragging_ = false;
                    ReleaseCapture();
                    completeSelection();
                }
                return 0;

            case WM_RBUTTONDOWN:
                cancel();
                return 0;

            case WM_KEYDOWN:
                if (wParam == VK_ESCAPE) {
                    cancel();
                    return 0;
                }
                break;

            case WM_CLOSE:
                cancel();
                return 0;

            case WM_DESTROY:
                done_ = true;
                return 0;

            default:
                break;
        }
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    }

    // --------------------------------------------------------------- state

    HINSTANCE instance_{};
    RECT virtualRect_{};
    Bitmap32 shot_;
    Bitmap32 dim_;
    Bitmap32 canvas_;

    HWND hwnd_{};
    UINT dpi_{96};
    HFONT font_{};
    bool fontOwned_{};

    POINT cursorClient_{};
    bool hasCursor_{};
    bool trackingLeave_{};

    bool dragging_{};
    POINT start_{};
    POINT current_{};

    bool done_{};
    std::optional<SelectionResult> result_;
};

}  // namespace

std::optional<SelectionResult> runSelectionOverlay(HINSTANCE instance) {
    const RECT virtualRect = virtualScreenRect();
    auto shot = captureVirtualScreen();
    if (!shot) return std::nullopt;

    SelectionOverlay overlay(instance, virtualRect, std::move(*shot));
    return overlay.run();
}

}  // namespace sp
