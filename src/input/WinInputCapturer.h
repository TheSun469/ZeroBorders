#pragma once

#include "IInputCapturer.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>

namespace zb {

class WinInputCapturer : public IInputCapturer {
public:
    WinInputCapturer();
    ~WinInputCapturer() override;

    bool start(EventCallback cb) override;
    void stop() override;
    void setSuppress(bool suppress) override { suppress_.store(suppress); }
    void warpCursor(int32_t x, int32_t y) override;
    void releaseAllButtons() override;
    void setCursorVisible(bool visible) override;
    uint32_t pressedButtons() const override;

private:
    void hookThreadFunc();

    static LRESULT CALLBACK keyboardProc(int code, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK mouseProc(int code, WPARAM wParam, LPARAM lParam);

    // Check if a mouse event at (x,y) should be treated as warp-induced.
    static bool isWarpEvent(int32_t x, int32_t y);

    // Per-instance state accessed from hook callbacks (single instance assumed).
    static WinInputCapturer* instance_;

    HHOOK kbHook_ = nullptr;
    HHOOK mouseHook_ = nullptr;
    std::thread hookThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> suppress_{false};

    // Warp filtering: we record the target coordinates and a "generation"
    // counter. Any mouse-move event that lands near the warp target within
    // a short window is considered synthetic and is skipped.
    struct WarpState {
        std::atomic<int32_t> x{0};
        std::atomic<int32_t> y{0};
        std::atomic<int64_t> untilMs{0};  // GetTickCount64() deadline
        std::atomic<int> remaining{0};    // max number of events to skip
    };
    WarpState warp_;

    // Track currently pressed mouse buttons (bitmask).
    std::atomic<uint32_t> pressedButtons_{0};

    // Cursor visibility ref-count.
    std::mutex cursorMtx_;
    int cursorHiddenCount_ = 0;

    // A 1x1 fully transparent cursor used to replace system cursors via
    // SetSystemCursor. ShowCursor(FALSE) is per-thread and does not hide the
    // cursor when it hovers over another process's window; SetSystemCursor
    // replaces the cursor globally and is reliable.
    HCURSOR invisibleCursor_ = nullptr;
    bool systemCursorsHidden_ = false;

    void createInvisibleCursor();
    void hideSystemCursor();
    void restoreSystemCursor();

    EventCallback callback_;
};

} // namespace zb
