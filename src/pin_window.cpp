#include "pin_window.h"

#include "app.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace sp {
namespace {

constexpr unsigned kEdgeLeft = 1u;
constexpr unsigned kEdgeRight = 2u;
constexpr unsigned kEdgeTop = 4u;
constexpr unsigned kEdgeBottom = 8u;

constexpr UINT_PTR kModifierTimerId = 1;

// A real elevation shadow is directional: a wide ambient shadow biased
// downward, plus a tight contact shadow hugging the object.  The blur is
// computed on a half-resolution alpha mask and sampled back up.
constexpr float kShadowScale = 0.5f;
constexpr float kHaloBlur = 8.0f;
constexpr float kHaloOpacity = 0.10f;
constexpr float kAmbientOffsetY = 8.0f;
constexpr float kAmbientBlur = 7.0f;
constexpr float kAmbientOpacity = 0.30f;
constexpr float kContactOffsetY = 2.0f;
constexpr float kContactBlur = 3.0f;
constexpr float kContactOpacity = 0.38f;

struct AlphaMask {
    int width{};
    int height{};
    std::vector<std::uint8_t> pixels;

    AlphaMask(int w, int h)
        : width(w), height(h),
          pixels(static_cast<std::size_t>(w) * h, 0) {}

    std::uint8_t& at(int x, int y) {
        return pixels[static_cast<std::size_t>(y) * width + x];
    }
    std::uint8_t at(int x, int y) const {
        return pixels[static_cast<std::size_t>(y) * width + x];
    }
};

void fillRoundedRect(AlphaMask& mask, float left, float top, float right,
                     float bottom, float radius) {
    if (right <= left || bottom <= top) return;
    radius = std::clamp(radius, 0.0f,
                        std::min(right - left, bottom - top) * 0.5f);

    const float innerLeft = left + radius;
    const float innerRight = right - radius;
    const float innerTop = top + radius;
    const float innerBottom = bottom - radius;

    for (int y = 0; y < mask.height; ++y) {
        const float py = static_cast<float>(y) + 0.5f;
        const float cy = std::clamp(py, innerTop, innerBottom);
        for (int x = 0; x < mask.width; ++x) {
            const float px = static_cast<float>(x) + 0.5f;
            const float cx = std::clamp(px, innerLeft, innerRight);
            const float dx = px - cx;
            const float dy = py - cy;
            if (dx * dx + dy * dy <= radius * radius) {
                mask.at(x, y) = 255;
            }
        }
    }
}

void blurHorizontal(const std::vector<std::uint8_t>& source,
                    std::vector<std::uint8_t>& destination, int width,
                    int height, int radius) {
    const int window = radius * 2 + 1;
    for (int y = 0; y < height; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) * width;
        int sum = 0;
        for (int i = -radius; i <= radius; ++i) {
            sum += source[row + static_cast<std::size_t>(std::clamp(i, 0, width - 1))];
        }
        for (int x = 0; x < width; ++x) {
            destination[row + static_cast<std::size_t>(x)] =
                static_cast<std::uint8_t>(sum / window);
            const int incoming = std::clamp(x + radius + 1, 0, width - 1);
            const int outgoing = std::clamp(x - radius, 0, width - 1);
            sum += source[row + static_cast<std::size_t>(incoming)] -
                   source[row + static_cast<std::size_t>(outgoing)];
        }
    }
}

void blurVertical(const std::vector<std::uint8_t>& source,
                  std::vector<std::uint8_t>& destination, int width,
                  int height, int radius) {
    const int window = radius * 2 + 1;
    for (int x = 0; x < width; ++x) {
        int sum = 0;
        for (int i = -radius; i <= radius; ++i) {
            sum += source[static_cast<std::size_t>(std::clamp(i, 0, height - 1)) *
                              width + x];
        }
        for (int y = 0; y < height; ++y) {
            destination[static_cast<std::size_t>(y) * width + x] =
                static_cast<std::uint8_t>(sum / window);
            const int incoming = std::clamp(y + radius + 1, 0, height - 1);
            const int outgoing = std::clamp(y - radius, 0, height - 1);
            sum += source[static_cast<std::size_t>(incoming) * width + x] -
                   source[static_cast<std::size_t>(outgoing) * width + x];
        }
    }
}

void blurMask(AlphaMask& mask, int radius, int passes) {
    if (radius <= 0) return;
    std::vector<std::uint8_t> temporary(mask.pixels.size());
    for (int pass = 0; pass < passes; ++pass) {
        blurHorizontal(mask.pixels, temporary, mask.width, mask.height, radius);
        blurVertical(temporary, mask.pixels, mask.width, mask.height, radius);
    }
}

float sampleMask(const AlphaMask& mask, float x, float y) {
    x = std::clamp(x, 0.0f, static_cast<float>(mask.width - 1));
    y = std::clamp(y, 0.0f, static_cast<float>(mask.height - 1));

    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const int x1 = std::min(x0 + 1, mask.width - 1);
    const int y1 = std::min(y0 + 1, mask.height - 1);
    const float tx = x - static_cast<float>(x0);
    const float ty = y - static_cast<float>(y0);

    const float a00 = mask.at(x0, y0);
    const float a10 = mask.at(x1, y0);
    const float a01 = mask.at(x0, y1);
    const float a11 = mask.at(x1, y1);
    const float top = a00 + (a10 - a00) * tx;
    const float bottom = a01 + (a11 - a01) * tx;
    return (top + (bottom - top) * ty) / 255.0f;
}

AlphaMask buildShadowMask(const RECT& imageRect, int maskWidth, int maskHeight,
                          float offsetY, float blurRadius, int passes) {
    AlphaMask mask(maskWidth, maskHeight);
    fillRoundedRect(mask, imageRect.left * kShadowScale,
                    (imageRect.top + offsetY) * kShadowScale,
                    imageRect.right * kShadowScale,
                    (imageRect.bottom + offsetY) * kShadowScale,
                    2.0f * kShadowScale);

    const int radius =
        std::max(1, static_cast<int>(std::lround(blurRadius * kShadowScale)));
    blurMask(mask, radius, passes);
    return mask;
}

void compositeShadow(Bitmap32& target, const RECT& imageRect) {
    const int maskWidth =
        std::max(1, static_cast<int>(std::ceil(target.width() * kShadowScale)));
    const int maskHeight =
        std::max(1, static_cast<int>(std::ceil(target.height() * kShadowScale)));

    const AlphaMask halo = buildShadowMask(
        imageRect, maskWidth, maskHeight, 0.0f, kHaloBlur, 2);
    const AlphaMask ambient = buildShadowMask(
        imageRect, maskWidth, maskHeight, kAmbientOffsetY, kAmbientBlur, 2);
    const AlphaMask contact = buildShadowMask(
        imageRect, maskWidth, maskHeight, kContactOffsetY, kContactBlur, 1);

    auto* pixels = target.data();
    for (int y = 0; y < target.height(); ++y) {
        const float sampleY =
            (static_cast<float>(y) + 0.5f) * kShadowScale - 0.5f;
        for (int x = 0; x < target.width(); ++x) {
            const float sampleX =
                (static_cast<float>(x) + 0.5f) * kShadowScale - 0.5f;
            float alpha = sampleMask(halo, sampleX, sampleY) * kHaloOpacity;
            const float ambientAlpha =
                sampleMask(ambient, sampleX, sampleY) * kAmbientOpacity;
            alpha += ambientAlpha * (1.0f - alpha);
            const float contactAlpha =
                sampleMask(contact, sampleX, sampleY) * kContactOpacity;
            alpha += contactAlpha * (1.0f - alpha);
            if (alpha > 0.002f) {
                const auto value = static_cast<std::uint32_t>(
                    std::lround(std::clamp(alpha, 0.0f, 1.0f) * 255.0f));
                pixels[static_cast<std::size_t>(y) * target.width() + x] =
                    value << 24;  // premultiplied black
            }
        }
    }
}

}  // namespace

PinWindow::PinWindow(App& app, Bitmap32 image, POINT origin)
    : app_(app), source_(std::move(image)) {
    crop_ = {0, 0, source_.width(), source_.height()};
    rect_ = {origin.x, origin.y, origin.x + source_.width(),
             origin.y + source_.height()};
    padding_ = dip(24, GetDpiForSystem());
}

PinWindow::~PinWindow() = default;

PinWindow* PinWindow::create(App& app, Bitmap32 image, POINT origin) {
    if (!image.valid()) return nullptr;
    debugLog(L"pin: create image=%dx%d at %d,%d", image.width(),
             image.height(), origin.x, origin.y);

    auto* pin = new PinWindow(app, std::move(image), origin);
    if (!pin->createWindow()) {
        debugLog(L"pin: createWindow failed");
        delete pin;
        return nullptr;
    }
    debugLog(L"pin: created hwnd=%p", static_cast<void*>(pin->hwnd_));
    return pin;
}

bool PinWindow::createWindow() {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_DBLCLKS;
    windowClass.lpfnWndProc = &PinWindow::windowProc;
    windowClass.hInstance = app_.instanceHandle();
    windowClass.hCursor = nullptr;
    windowClass.lpszClassName = kPinClass;
    RegisterClassExW(&windowClass);

    const RECT window = windowRect();
    hwnd_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kPinClass, L"贴图",
        WS_POPUP, window.left, window.top, rectWidth(window),
        rectHeight(window), nullptr, nullptr, app_.instanceHandle(), this);
    debugLog(L"pin: CreateWindowEx -> %p (err=%lu)", static_cast<void*>(hwnd_),
             hwnd_ ? 0UL : GetLastError());
    if (!hwnd_) return false;

    refresh();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return true;
}

void PinWindow::close() {
    if (hwnd_) DestroyWindow(hwnd_);
}

Bitmap32 PinWindow::visibleImage() const {
    return source_.cropped(crop_);
}

void PinWindow::copyImage() {
    Bitmap32 visible = visibleImage();
    if (visible) {
        copyBitmapToClipboard(hwnd_, visible);
    }
}

bool PinWindow::saveImage() {
    std::wstring path;
    if (!pickPngSavePath(hwnd_, path)) return false;

    Bitmap32 visible = visibleImage();
    if (!visible || !saveBitmapAsPng(visible, path)) {
        MessageBoxW(hwnd_, L"保存 PNG 失败。", kAppTitle,
                    MB_OK | MB_ICONERROR);
        return false;
    }
    return true;
}

// ------------------------------------------------------------- geometry

RECT PinWindow::windowRect() const {
    RECT window = rect_;
    InflateRect(&window, padding_, padding_);
    return window;
}

POINT PinWindow::toImagePoint(POINT clientPoint) const {
    return {clientPoint.x - padding_, clientPoint.y - padding_};
}

// --------------------------------------------------------------- render

void PinWindow::refresh() {
    compose();
}

void PinWindow::premultiply() {
    if (!buffer_.valid()) return;

    auto* pixels = buffer_.data();
    const std::size_t count =
        static_cast<std::size_t>(buffer_.width()) * buffer_.height();
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t value = pixels[i];
        const std::uint32_t alpha = value >> 24;
        if (alpha == 0) {
            pixels[i] = 0;
            continue;
        }
        const std::uint32_t red = ((value >> 16) & 0xFFu) * alpha / 255u;
        const std::uint32_t green = ((value >> 8) & 0xFFu) * alpha / 255u;
        const std::uint32_t blue = (value & 0xFFu) * alpha / 255u;
        pixels[i] = (alpha << 24) | (red << 16) | (green << 8) | blue;
    }
}

void PinWindow::compose() {
    const RECT window = windowRect();
    const int width = rectWidth(window);
    const int height = rectHeight(window);
    if (width <= 0 || height <= 0) return;

    if (buffer_.width() != width || buffer_.height() != height) {
        buffer_ = Bitmap32(width, height);
    }
    if (!buffer_.valid()) return;

    std::memset(buffer_.data(), 0,
                static_cast<std::size_t>(width) * height *
                    sizeof(std::uint32_t));

    const int imageWidth = rectWidth(rect_);
    const int imageHeight = rectHeight(rect_);
    if (imageWidth <= 0 || imageHeight <= 0) return;

    const float left = static_cast<float>(padding_);
    const float top = static_cast<float>(padding_);
    const RECT imageClient{padding_, padding_, padding_ + imageWidth,
                           padding_ + imageHeight};

    compositeShadow(buffer_, imageClient);

    {
        Gdiplus::Bitmap canvas(width, height, width * 4,
                               PixelFormat32bppARGB,
                               reinterpret_cast<BYTE*>(buffer_.data()));
        if (canvas.GetLastStatus() != Gdiplus::Ok) return;

        Gdiplus::Graphics graphics(&canvas);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        graphics.SetInterpolationMode(
            Gdiplus::InterpolationModeHighQualityBicubic);

        // The captured pixels, scaled by GDI+ high-quality sampling.
        Gdiplus::Bitmap source(
            source_.width(), source_.height(), source_.width() * 4,
            PixelFormat32bppRGB,
            reinterpret_cast<BYTE*>(source_.data()));
        const Gdiplus::Rect destination(
            static_cast<INT>(left), static_cast<INT>(top), imageWidth,
            imageHeight);
        graphics.DrawImage(&source, destination, crop_.left, crop_.top,
                           rectWidth(crop_), rectHeight(crop_),
                           Gdiplus::UnitPixel);

        if (ctrlDown_ || dragMode_ == DragMode::crop) {
            drawCropFrame(graphics);
        }
    }

    // GDI+ leaves the alpha byte of opaque image pixels at zero in some
    // driver/back-end combinations.  The image must be fully opaque, so
    // normalize the alpha channel over the image rectangle before the
    // premultiply step.
    for (int y = 0; y < imageHeight; ++y) {
        std::span<std::uint32_t> row = buffer_.row(padding_ + y);
        for (int x = 0; x < imageWidth; ++x) {
            row[static_cast<std::size_t>(padding_ + x)] |= 0xFF000000u;
        }
    }

    premultiply();
    if (buffer_.valid() && imageWidth > 0 && imageHeight > 0) {
        const std::uint32_t center =
            buffer_.row(padding_ + imageHeight / 2)
                [static_cast<std::size_t>(padding_ + imageWidth / 2)];
        debugLog(L"compose: image=%dx%d center=%08X alpha=%u opacity=%d",
                 imageWidth, imageHeight, center, center >> 24, opacity_);
    }
    updateLayered();
}

void PinWindow::updateLayered() {
    if (!hwnd_ || !buffer_.valid()) return;

    const RECT window = windowRect();
    POINT destination{window.left, window.top};
    SIZE size{rectWidth(window), rectHeight(window)};
    POINT sourcePoint{0, 0};

    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = static_cast<BYTE>(opacity_);
    blend.AlphaFormat = AC_SRC_ALPHA;

    const BOOL ok = UpdateLayeredWindow(
        hwnd_, nullptr, &destination, &size, buffer_.dc(), &sourcePoint, 0,
        &blend, ULW_ALPHA);
    debugLog(L"layered: ok=%d err=%lu constAlpha=%u", ok ? 1 : 0,
             ok ? 0UL : GetLastError(), static_cast<unsigned>(blend.SourceConstantAlpha));
}

void PinWindow::drawCropFrame(Gdiplus::Graphics& graphics) const {
    const float left = static_cast<float>(padding_);
    const float top = static_cast<float>(padding_);
    const float width = static_cast<float>(rectWidth(rect_));
    const float height = static_cast<float>(rectHeight(rect_));

    Gdiplus::Pen pen(Gdiplus::Color(255, 0, 170, 255), 2.0f);
    graphics.DrawRectangle(&pen, left + 1.0f, top + 1.0f, width - 2.0f,
                           height - 2.0f);

    if (width < 28.0f || height < 28.0f) return;

    const float handle = 8.0f;
    Gdiplus::SolidBrush fill(Gdiplus::Color(255, 255, 255, 255));
    Gdiplus::Pen edge(Gdiplus::Color(255, 0, 120, 212), 1.0f);
    const Gdiplus::PointF corners[] = {
        {left + 1.0f, top + 1.0f},
        {left + width - handle - 1.0f, top + 1.0f},
        {left + 1.0f, top + height - handle - 1.0f},
        {left + width - handle - 1.0f, top + height - handle - 1.0f},
    };
    for (const Gdiplus::PointF& corner : corners) {
        graphics.FillRectangle(&fill, corner.X, corner.Y, handle, handle);
        graphics.DrawRectangle(&edge, corner.X, corner.Y, handle, handle);
    }
}

// --------------------------------------------------------------- dragging

void PinWindow::beginMove() {
    dragMode_ = DragMode::move;
    cropEdge_ = 0;
    GetCursorPos(&dragStartCursor_);
    dragStartRect_ = rect_;
    SetCapture(hwnd_);
    SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
}

void PinWindow::updateMove() {
    POINT cursor{};
    GetCursorPos(&cursor);
    const int dx = cursor.x - dragStartCursor_.x;
    const int dy = cursor.y - dragStartCursor_.y;

    rect_ = dragStartRect_;
    OffsetRect(&rect_, dx, dy);

    const RECT window = windowRect();
    SetWindowPos(hwnd_, nullptr, window.left, window.top, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void PinWindow::beginCrop(unsigned edge) {
    dragMode_ = DragMode::crop;
    cropEdge_ = edge;
    GetCursorPos(&dragStartCursor_);
    dragStartCrop_ = crop_;
    dragStartRect_ = rect_;
    SetCapture(hwnd_);
    refresh();
}

void PinWindow::updateCrop() {
    POINT cursor{};
    GetCursorPos(&cursor);

    const int dx = static_cast<int>(
        std::lround((cursor.x - dragStartCursor_.x) / scale_));
    const int dy = static_cast<int>(
        std::lround((cursor.y - dragStartCursor_.y) / scale_));

    constexpr int kMinCrop = 8;
    RECT crop = dragStartCrop_;

    if (cropEdge_ & kEdgeLeft) {
        crop.left = clampLong(dragStartCrop_.left + dx, 0,
                              dragStartCrop_.right - kMinCrop);
    }
    if (cropEdge_ & kEdgeRight) {
        crop.right = clampLong(dragStartCrop_.right + dx,
                               dragStartCrop_.left + kMinCrop,
                               source_.width());
    }
    if (cropEdge_ & kEdgeTop) {
        crop.top = clampLong(dragStartCrop_.top + dy, 0,
                             dragStartCrop_.bottom - kMinCrop);
    }
    if (cropEdge_ & kEdgeBottom) {
        crop.bottom = clampLong(dragStartCrop_.bottom + dy,
                                dragStartCrop_.top + kMinCrop,
                                source_.height());
    }

    const int left = dragStartRect_.left + static_cast<int>(std::lround(
                                               (crop.left - dragStartCrop_.left) * scale_));
    const int top = dragStartRect_.top + static_cast<int>(std::lround(
                                              (crop.top - dragStartCrop_.top) * scale_));
    const int right = dragStartRect_.left + static_cast<int>(std::lround(
                                                (crop.right - dragStartCrop_.left) * scale_));
    const int bottom = dragStartRect_.top + static_cast<int>(std::lround(
                                                 (crop.bottom - dragStartCrop_.top) * scale_));

    if (right - left < 1 || bottom - top < 1) return;

    crop_ = crop;
    rect_ = {left, top, right, bottom};

    const RECT window = windowRect();
    SetWindowPos(hwnd_, nullptr, window.left, window.top, rectWidth(window),
                 rectHeight(window), SWP_NOZORDER | SWP_NOACTIVATE);
    refresh();
}

void PinWindow::endDrag() {
    if (dragMode_ == DragMode::none) return;
    dragMode_ = DragMode::none;
    cropEdge_ = 0;
    ReleaseCapture();
    refresh();
}

// ---------------------------------------------------------------- opacity

void PinWindow::setOpacity(int value) {
    opacity_ = std::clamp(value, 25, 255);
    updateLayered();
}

void PinWindow::scaleBy(double factor, POINT anchor) {
    double newScale = std::clamp(scale_ * factor, 0.1, 8.0);

    const int cropWidth = rectWidth(crop_);
    const int cropHeight = rectHeight(crop_);
    if (cropWidth <= 0 || cropHeight <= 0) return;

    const double maxScale =
        std::min(16384.0 / cropWidth, 16384.0 / cropHeight);
    newScale = std::max(0.05, std::min(newScale, maxScale));
    if (std::abs(newScale - scale_) < 1e-9) return;

    // Keep the image point currently under the cursor stationary.
    const double relativeX = (anchor.x - rect_.left) / scale_;
    const double relativeY = (anchor.y - rect_.top) / scale_;

    const int newWidth = std::max(
        8, static_cast<int>(std::lround(cropWidth * newScale)));
    const int newHeight = std::max(
        8, static_cast<int>(std::lround(cropHeight * newScale)));
    const int left = static_cast<int>(std::lround(
        anchor.x - relativeX * newScale));
    const int top = static_cast<int>(std::lround(
        anchor.y - relativeY * newScale));

    scale_ = newScale;
    rect_ = {left, top, left + newWidth, top + newHeight};

    const RECT window = windowRect();
    SetWindowPos(hwnd_, nullptr, window.left, window.top, rectWidth(window),
                 rectHeight(window), SWP_NOZORDER | SWP_NOACTIVATE);
    refresh();
}

// ---------------------------------------------------------------- hit test

unsigned PinWindow::hitTestEdge(POINT imagePoint) const {
    const int width = rectWidth(rect_);
    const int height = rectHeight(rect_);
    if (width <= 0 || height <= 0) return 0;
    if (imagePoint.x < 0 || imagePoint.y < 0 || imagePoint.x >= width ||
        imagePoint.y >= height) {
        return 0;
    }

    int margin = dip(8, dpiFor(hwnd_));
    margin = std::min(margin, std::min(width, height) / 3);
    margin = std::max(2, margin);

    unsigned edge = 0;
    if (imagePoint.x <= margin) {
        edge |= kEdgeLeft;
    } else if (imagePoint.x >= width - 1 - margin) {
        edge |= kEdgeRight;
    }
    if (imagePoint.y <= margin) {
        edge |= kEdgeTop;
    } else if (imagePoint.y >= height - 1 - margin) {
        edge |= kEdgeBottom;
    }
    return edge;
}

LPCWSTR PinWindow::cursorForState() const {
    if (dragMode_ == DragMode::move) return IDC_SIZEALL;

    if (ctrlDown_) {
        const bool left = (hoverEdge_ & kEdgeLeft) != 0;
        const bool right = (hoverEdge_ & kEdgeRight) != 0;
        const bool top = (hoverEdge_ & kEdgeTop) != 0;
        const bool bottom = (hoverEdge_ & kEdgeBottom) != 0;

        if ((left && top) || (right && bottom)) return IDC_SIZENWSE;
        if ((right && top) || (left && bottom)) return IDC_SIZENESW;
        if (left || right) return IDC_SIZEWE;
        if (top || bottom) return IDC_SIZENS;
        return IDC_CROSS;
    }
    return IDC_ARROW;
}

void PinWindow::ensureControlTimer() {
    if (!controlTimer_ && hwnd_) {
        controlTimer_ = SetTimer(hwnd_, kModifierTimerId, 100, nullptr);
    }
}

void PinWindow::refreshModifiers() {
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    if (ctrl != ctrlDown_) {
        ctrlDown_ = ctrl;
        refresh();
    }
}

// ------------------------------------------------------------------ mouse

void PinWindow::onMouseMove(POINT clientPoint) {
    hover_ = true;
    if (!trackingLeave_) {
        TRACKMOUSEEVENT event{};
        event.cbSize = sizeof(event);
        event.dwFlags = TME_LEAVE;
        event.hwndTrack = hwnd_;
        if (TrackMouseEvent(&event)) trackingLeave_ = true;
    }

    refreshModifiers();

    if (dragMode_ == DragMode::crop) {
        updateCrop();
        return;
    }
    if (dragMode_ == DragMode::move) {
        updateMove();
        return;
    }

    const POINT imagePoint = toImagePoint(clientPoint);
    hoverEdge_ = ctrlDown_ ? hitTestEdge(imagePoint) : 0;
    SetCursor(LoadCursorW(nullptr, cursorForState()));
    ensureControlTimer();
}

void PinWindow::onMouseLeave() {
    trackingLeave_ = false;
    hover_ = false;
    ctrlDown_ = false;
    hoverEdge_ = 0;
    if (controlTimer_) {
        KillTimer(hwnd_, kModifierTimerId);
        controlTimer_ = 0;
    }
    refresh();
}

void PinWindow::onWheel(WPARAM wParam, LPARAM lParam) {
    POINT screenPoint{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    POINT clientPoint = screenPoint;
    ScreenToClient(hwnd_, &clientPoint);

    const int width = rectWidth(rect_);
    const int height = rectHeight(rect_);
    const RECT imageRect{padding_, padding_, padding_ + width,
                         padding_ + height};
    if (!PtInRect(&imageRect, clientPoint)) return;

    const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
    if (delta == 0) return;

    const bool ctrl = (GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) != 0 ||
                      (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;

    int steps = delta / WHEEL_DELTA;
    if (steps == 0) steps = delta > 0 ? 1 : -1;

    if (ctrl) {
        const double factor = steps > 0 ? 1.1 : (1.0 / 1.1);
        const int repetitions = std::min(std::abs(steps), 5);
        for (int i = 0; i < repetitions; ++i) {
            scaleBy(factor, screenPoint);
        }
    } else {
        setOpacity(opacity_ + steps * 15);
    }
}

// ------------------------------------------------------------------- menu

void PinWindow::showContextMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    AppendMenuW(menu, MF_STRING, kCmdPinCopy, L"复制图片\tCtrl+C");
    AppendMenuW(menu, MF_STRING, kCmdPinSave, L"另存为 PNG...\tCtrl+S");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdPinClose, L"关闭贴图\t双击 / Esc");
    if (app_.pinCount() > 1) {
        AppendMenuW(menu, MF_STRING, kCmdPinCloseAll, L"关闭全部贴图");
    }

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(hwnd_);
    const UINT command = static_cast<UINT>(TrackPopupMenu(
        menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, cursor.x,
        cursor.y, 0, hwnd_, nullptr));
    DestroyMenu(menu);
    PostMessageW(hwnd_, WM_NULL, 0, 0);

    if (command) handleContextCommand(command);
}

void PinWindow::handleContextCommand(UINT command) {
    switch (command) {
        case kCmdPinCopy:
            copyImage();
            break;
        case kCmdPinSave:
            saveImage();
            break;
        case kCmdPinClose:
            PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            break;
        case kCmdPinCloseAll:
            app_.requestCloseAll();
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------- messages

LRESULT CALLBACK PinWindow::windowProc(HWND hwnd, UINT message, WPARAM wParam,
                                       LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* self = static_cast<PinWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
        return TRUE;
    }

    auto* self =
        reinterpret_cast<PinWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, message, wParam, lParam);
    return self->handleMessage(message, wParam, lParam);
}

LRESULT PinWindow::handleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT paintStruct{};
            HDC dc = BeginPaint(hwnd_, &paintStruct);
            EndPaint(hwnd_, &paintStruct);
            (void)dc;
            return 0;
        }

        case WM_MOVE: {
            RECT window{};
            GetWindowRect(hwnd_, &window);
            rect_ = window;
            InflateRect(&rect_, -padding_, -padding_);
            return 0;
        }

        case WM_NCHITTEST: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd_, &point);
            const int width = rectWidth(rect_);
            const int height = rectHeight(rect_);
            if (point.x < padding_ || point.y < padding_ ||
                point.x >= padding_ + width ||
                point.y >= padding_ + height) {
                return HTTRANSPARENT;
            }
            return HTCLIENT;
        }

        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, cursorForState()));
                return TRUE;
            }
            break;

        case WM_MOUSEMOVE:
            onMouseMove({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            return 0;

        case WM_MOUSELEAVE:
            onMouseLeave();
            return 0;

        case WM_LBUTTONDOWN: {
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const POINT imagePoint = toImagePoint(point);
            const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            if (ctrl) {
                const unsigned edge = hitTestEdge(imagePoint);
                if (edge) {
                    beginCrop(edge);
                }
                return 0;
            }
            beginMove();
            return 0;
        }

        case WM_LBUTTONUP:
            endDrag();
            return 0;

        case WM_LBUTTONDBLCLK:
            debugLog(L"pin: WM_LBUTTONDBLCLK %p", static_cast<void*>(hwnd_));
            close();
            return 0;

        case WM_RBUTTONUP:
            showContextMenu();
            return 0;

        case WM_MOUSEWHEEL:
            onWheel(wParam, lParam);
            return 0;

        case WM_TIMER:
            if (wParam == kModifierTimerId) {
                refreshModifiers();
                POINT cursor{};
                GetCursorPos(&cursor);
                ScreenToClient(hwnd_, &cursor);
                hoverEdge_ = ctrlDown_ ? hitTestEdge(toImagePoint(cursor)) : 0;
                SetCursor(LoadCursorW(nullptr, cursorForState()));
                if (!hover_ && controlTimer_) {
                    KillTimer(hwnd_, kModifierTimerId);
                    controlTimer_ = 0;
                }
            }
            return 0;

        case WM_DPICHANGED:
            return 0;

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                debugLog(L"pin: Escape %p", static_cast<void*>(hwnd_));
                close();
                return 0;
            }
            if (wParam == 'C' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                copyImage();
                return 0;
            }
            if (wParam == 'S' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                saveImage();
                return 0;
            }
            break;

        case WM_CLOSE:
            debugLog(L"pin: WM_CLOSE %p", static_cast<void*>(hwnd_));
            DestroyWindow(hwnd_);
            return 0;

        case WM_DESTROY:
            debugLog(L"pin: WM_DESTROY %p", static_cast<void*>(hwnd_));
            if (controlTimer_) {
                KillTimer(hwnd_, kModifierTimerId);
                controlTimer_ = 0;
            }
            return 0;

        case WM_NCDESTROY: {
            HWND window = hwnd_;
            debugLog(L"pin: WM_NCDESTROY %p", static_cast<void*>(hwnd_));
            hwnd_ = nullptr;
            app_.removePin(this);
            delete this;
            return DefWindowProcW(window, message, wParam, lParam);
        }

        default:
            break;
    }
    return DefWindowProcW(hwnd_, message, wParam, lParam);
}

}  // namespace sp
