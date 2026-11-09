#pragma once

#include "common.h"

namespace sp {

class App;

// A pinned, always-on-top screenshot.
//
// Interaction model:
//   drag                 move the pin
//   double click         close
//   wheel                opacity
//   Ctrl + wheel         scale around the cursor
//   Ctrl + drag an edge  crop that edge (in source pixels)
//   right click          copy / save / close
//
// The window is larger than the image: a transparent margin holds a soft
// drop shadow and hairline border.  The window is per-pixel layered, so the
// margin is click-through.
class PinWindow {
public:
    static PinWindow* create(App& app, Bitmap32 image, POINT origin);
    ~PinWindow();

    PinWindow(const PinWindow&) = delete;
    PinWindow& operator=(const PinWindow&) = delete;

    HWND hwnd() const noexcept { return hwnd_; }

    void close();
    void copyImage();
    bool saveImage();
    Bitmap32 visibleImage() const;

private:
    PinWindow(App& app, Bitmap32 image, POINT origin);

    bool createWindow();
    void refresh();
    void compose();
    void updateLayered();
    void premultiply();
    void drawCropFrame(Gdiplus::Graphics& graphics) const;

    RECT windowRect() const;
    POINT toImagePoint(POINT clientPoint) const;

    void beginMove();
    void updateMove();
    void beginCrop(unsigned edge);
    void updateCrop();
    void endDrag();

    void setOpacity(int value);
    void scaleBy(double factor, POINT anchor);

    unsigned hitTestEdge(POINT imagePoint) const;
    LPCWSTR cursorForState() const;
    void ensureControlTimer();
    void refreshModifiers();

    void showContextMenu();
    void handleContextCommand(UINT command);

    void onMouseMove(POINT clientPoint);
    void onMouseLeave();
    void onWheel(WPARAM wParam, LPARAM lParam);

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam,
                                       LPARAM lParam);
    LRESULT handleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    App& app_;
    HWND hwnd_{};

    Bitmap32 source_;
    RECT crop_{};   // region of source_ currently shown (source pixels)
    RECT rect_{};   // screen rectangle of the image itself
    int padding_{}; // transparent shadow margin around rect_
    double scale_{1.0};
    int opacity_{255};

    Bitmap32 buffer_;
    bool ctrlDown_{};
    bool hover_{};
    bool trackingLeave_{};
    unsigned hoverEdge_{};
    UINT_PTR controlTimer_{};

    enum class DragMode { none, move, crop };
    DragMode dragMode_{DragMode::none};
    unsigned cropEdge_{};
    POINT dragStartCursor_{};
    RECT dragStartRect_{};
    RECT dragStartCrop_{};
};

}  // namespace sp
