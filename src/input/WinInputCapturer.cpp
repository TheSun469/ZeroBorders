#include "WinInputCapturer.h"
#include "../core/Log.h"

#include <cstdlib>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// OCR_* cursor IDs may not be exposed when WIN32_LEAN_AND_MEAN is defined.
// They are stable resource IDs defined by the system.
#ifndef OCR_NORMAL
#define OCR_NORMAL         MAKEINTRESOURCEW(32512)
#define OCR_IBEAM          MAKEINTRESOURCEW(32513)
#define OCR_WAIT           MAKEINTRESOURCEW(32514)
#define OCR_CROSS          MAKEINTRESOURCEW(32515)
#define OCR_UP             MAKEINTRESOURCEW(32516)
#define OCR_SIZENWSE       MAKEINTRESOURCEW(32642)
#define OCR_SIZENESW       MAKEINTRESOURCEW(32643)
#define OCR_SIZEWE         MAKEINTRESOURCEW(32644)
#define OCR_SIZENS         MAKEINTRESOURCEW(32645)
#define OCR_SIZEALL        MAKEINTRESOURCEW(32646)
#define OCR_NO             MAKEINTRESOURCEW(32648)
#define OCR_HAND           MAKEINTRESOURCEW(32649)
#define OCR_APPSTARTING    MAKEINTRESOURCEW(32650)
#endif

namespace zb {

WinInputCapturer* WinInputCapturer::instance_ = nullptr;

WinInputCapturer::WinInputCapturer() {
    instance_ = this;
    createInvisibleCursor();
}

WinInputCapturer::~WinInputCapturer() {
    stop();
    if (invisibleCursor_) {
        DestroyCursor(invisibleCursor_);
        invisibleCursor_ = nullptr;
    }
    if (instance_ == this) instance_ = nullptr;
}

bool WinInputCapturer::start(EventCallback cb) {
    if (running_.load()) return false;
    callback_ = std::move(cb);
    running_ = true;
    hookThread_ = std::thread([this] { hookThreadFunc(); });
    return true;
}

void WinInputCapturer::stop() {
    if (!running_.exchange(false)) return;
    // Show cursor if we hid it.
    setCursorVisible(true);
    restoreSystemCursor();
    if (hookThread_.joinable()) {
        hookThread_.join();
    }
    kbHook_ = nullptr;
    mouseHook_ = nullptr;
}

void WinInputCapturer::warpCursor(int32_t x, int32_t y) {
    // Record the exact warp target. SetCursorPos produces exactly ONE
    // synthetic WM_MOUSEMOVE at the target coordinates. We use exact
    // coordinate matching (0 tolerance) so that real slow movements near
    // the target are never incorrectly swallowed.
    warp_.x.store(x);
    warp_.y.store(y);
    warp_.untilMs.store(static_cast<int64_t>(GetTickCount64()) + 15);
    warp_.remaining.store(1);
    SetCursorPos(x, y);
}

void WinInputCapturer::releaseAllButtons() {
    uint32_t btns = pressedButtons_.exchange(0);
    if (!btns) return;

    ZB_LOG_INFO("Releasing stuck mouse buttons: mask={:#x}", btns);

    // Send button-up for each pressed button.
    INPUT inputs[3]{};
    int count = 0;

    if (btns & 0x1) {
        inputs[count].type = INPUT_MOUSE;
        inputs[count].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        ++count;
    }
    if (btns & 0x2) {
        inputs[count].type = INPUT_MOUSE;
        inputs[count].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
        ++count;
    }
    if (btns & 0x4) {
        inputs[count].type = INPUT_MOUSE;
        inputs[count].mi.dwFlags = MOUSEEVENTF_MIDDLEUP;
        ++count;
    }

    if (count > 0) {
        SendInput(count, inputs, sizeof(INPUT));
    }
}

void WinInputCapturer::setCursorVisible(bool visible) {
    std::lock_guard<std::mutex> lk(cursorMtx_);
    if (visible) {
        while (cursorHiddenCount_ > 0) {
            ShowCursor(TRUE);
            --cursorHiddenCount_;
        }
        restoreSystemCursor();
    } else {
        if (cursorHiddenCount_ == 0) {
            ShowCursor(FALSE);
            hideSystemCursor();
        }
        ++cursorHiddenCount_;
    }
}

void WinInputCapturer::createInvisibleCursor() {
    // Create a 1x1 monochrome bitmap with all-zero mask bits. A cursor with
    // AND mask all 1s and XOR mask all 0s is fully transparent.
    static const WORD andMask = 0xFFFF; // all 1s = keep screen pixels
    static const WORD xorMask = 0x0000; // all 0s = no color
    invisibleCursor_ = CreateCursor(nullptr, 0, 0, 1, 1, &andMask, &xorMask);
}

void WinInputCapturer::hideSystemCursor() {
    if (systemCursorsHidden_) return;
    if (!invisibleCursor_) return;

    // Replace every standard system cursor with a copy of our invisible
    // cursor. SetSystemCursor takes ownership of the handle passed in, so
    // we must CopyCursor for each ID. We do not use OCR_NORMAL alone
    // because different windows (text editors, hyperlinks, etc.) switch to
    // other system cursors that would remain visible.
    static const LPCWSTR cursorIds[] = {
        OCR_NORMAL, OCR_IBEAM, OCR_WAIT, OCR_CROSS,
        OCR_UP, OCR_SIZENWSE, OCR_SIZENESW, OCR_SIZEWE,
        OCR_SIZENS, OCR_SIZEALL, OCR_NO, OCR_HAND,
        OCR_APPSTARTING,
    };
    for (auto id : cursorIds) {
        HCURSOR copy = CopyCursor(invisibleCursor_);
        if (copy) {
            SetSystemCursor(copy, reinterpret_cast<DWORD>(id));
        }
    }
    systemCursorsHidden_ = true;
}

void WinInputCapturer::restoreSystemCursor() {
    if (!systemCursorsHidden_) return;
    // SPI_SETCURSORS reloads the default cursors from the registry,
    // undoing all SetSystemCursor replacements.
    SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0);
    systemCursorsHidden_ = false;
}

uint32_t WinInputCapturer::pressedButtons() const {
    return pressedButtons_.load();
}

void WinInputCapturer::hookThreadFunc() {
    kbHook_ = SetWindowsHookEx(WH_KEYBOARD_LL, keyboardProc, nullptr, 0);
    if (!kbHook_) {
        ZB_LOG_ERROR("Failed to set keyboard hook: {}", GetLastError());
        running_ = false;
        return;
    }
    mouseHook_ = SetWindowsHookEx(WH_MOUSE_LL, mouseProc, nullptr, 0);
    if (!mouseHook_) {
        ZB_LOG_ERROR("Failed to set mouse hook: {}", GetLastError());
        UnhookWindowsHookEx(kbHook_);
        kbHook_ = nullptr;
        running_ = false;
        return;
    }

    ZB_LOG_INFO("Input hooks installed (keyboard + mouse)");

    MSG msg;
    while (running_.load()) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) break;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        } else {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
        }
    }

    if (kbHook_) { UnhookWindowsHookEx(kbHook_); kbHook_ = nullptr; }
    if (mouseHook_) { UnhookWindowsHookEx(mouseHook_); mouseHook_ = nullptr; }
    ZB_LOG_INFO("Input hooks removed");
}

LRESULT CALLBACK WinInputCapturer::keyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && instance_ && instance_->running_.load()) {
        const auto* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);

        EventType type;
        switch (wParam) {
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
                type = EventType::KeyDown;
                break;
            case WM_KEYUP:
            case WM_SYSKEYUP:
                type = EventType::KeyUp;
                break;
            default:
                return CallNextHookEx(nullptr, code, wParam, lParam);
        }

        InputEvent ev{};
        ev.type = type;
        ev.timestamp = static_cast<uint64_t>(info->time);
        ev.key.vkCode = static_cast<uint16_t>(info->vkCode);
        ev.key.scanCode = static_cast<uint16_t>(info->scanCode);
        ev.key.extended = (info->flags & LLKHF_EXTENDED) != 0;

        // Skip injected events (e.g. synthetic button release from releaseAllButtons)
        // so they are not forwarded to the remote. They still pass to the OS.
        bool injected = (info->flags & LLKHF_INJECTED) != 0;
        bool suppressEvent = false;
        if (!injected && instance_->callback_) {
            try {
                suppressEvent = instance_->callback_(ev);
            } catch (const std::exception& e) {
                ZB_LOG_ERROR("Keyboard hook callback exception: {}", e.what());
            } catch (...) {
                ZB_LOG_ERROR("Keyboard hook callback unknown exception");
            }
        }

        // 键盘事件：只根据 callback 返回值决定是否拦截，不受全局 suppress_ 影响。
        // suppress_ 仅用于鼠标（隐藏光标时压制本地鼠标移动），键盘控制权由
        // ScreenRouter 的 keyboardControl_ 标志精确控制。
        if (suppressEvent && !injected) {
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

LRESULT CALLBACK WinInputCapturer::mouseProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && instance_ && instance_->running_.load()) {
        const auto* info = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        int32_t mx = static_cast<int32_t>(info->pt.x);
        int32_t my = static_cast<int32_t>(info->pt.y);

        // Events injected via SendInput (e.g. our synthetic button-release)
        // must NOT be forwarded to the remote, but MUST reach the OS so the
        // physical button state is cleared. We still let warp filtering below
        // suppress synthetic move events from SetCursorPos.
        bool injected = (info->flags & LLMHF_INJECTED) != 0;

        // Robust warp-event filtering: skip synthetic WM_MOUSEMOVE events
        // produced by SetCursorPos. We use exact coordinate matching because
        // SetCursorPos produces exactly one move event at the target point.
        // A tolerance window would incorrectly swallow real slow movements
        // that happen to land near the warp target.
        bool isWarpMove = false;
        if (wParam == WM_MOUSEMOVE) {
            int remaining = instance_->warp_.remaining.load();
            if (remaining > 0) {
                int64_t now = static_cast<int64_t>(GetTickCount64());
                int64_t until = instance_->warp_.untilMs.load();
                if (now < until) {
                    int32_t tx = instance_->warp_.x.load();
                    int32_t ty = instance_->warp_.y.load();
                    if (mx == tx && my == ty) {
                        instance_->warp_.remaining.fetch_sub(1);
                        isWarpMove = true;
                    }
                } else {
                    instance_->warp_.remaining.store(0);
                }
            }
        }

        // Warp-induced move events are swallowed entirely.
        if (isWarpMove) {
            return 1;
        }

        InputEvent ev{};
        ev.timestamp = static_cast<uint64_t>(info->time);

        bool handled = true;
        bool isButtonDown = false;
        bool isButtonUp = false;
        uint32_t btnBit = 0;

        switch (wParam) {
            case WM_MOUSEMOVE:
                ev.type = EventType::MouseMove;
                ev.mouseMove.x = mx;
                ev.mouseMove.y = my;
                break;
            case WM_LBUTTONDOWN:
                ev.type = EventType::MouseButton;
                ev.mouseButton = {MouseButton::Left, true, mx, my};
                isButtonDown = true; btnBit = 0x1;
                break;
            case WM_LBUTTONUP:
                ev.type = EventType::MouseButton;
                ev.mouseButton = {MouseButton::Left, false, mx, my};
                isButtonUp = true; btnBit = 0x1;
                break;
            case WM_RBUTTONDOWN:
                ev.type = EventType::MouseButton;
                ev.mouseButton = {MouseButton::Right, true, mx, my};
                isButtonDown = true; btnBit = 0x2;
                break;
            case WM_RBUTTONUP:
                ev.type = EventType::MouseButton;
                ev.mouseButton = {MouseButton::Right, false, mx, my};
                isButtonUp = true; btnBit = 0x2;
                break;
            case WM_MBUTTONDOWN:
                ev.type = EventType::MouseButton;
                ev.mouseButton = {MouseButton::Middle, true, mx, my};
                isButtonDown = true; btnBit = 0x4;
                break;
            case WM_MBUTTONUP:
                ev.type = EventType::MouseButton;
                ev.mouseButton = {MouseButton::Middle, false, mx, my};
                isButtonUp = true; btnBit = 0x4;
                break;
            case WM_XBUTTONDOWN:
                ev.type = EventType::MouseButton;
                ev.mouseButton.button = (HIWORD(info->mouseData) == XBUTTON1)
                    ? MouseButton::X1 : MouseButton::X2;
                ev.mouseButton.pressed = true;
                ev.mouseButton.x = mx;
                ev.mouseButton.y = my;
                isButtonDown = true;
                break;
            case WM_XBUTTONUP:
                ev.type = EventType::MouseButton;
                ev.mouseButton.button = (HIWORD(info->mouseData) == XBUTTON1)
                    ? MouseButton::X1 : MouseButton::X2;
                ev.mouseButton.pressed = false;
                ev.mouseButton.x = mx;
                ev.mouseButton.y = my;
                isButtonUp = true;
                break;
            case WM_MOUSEWHEEL:
                ev.type = EventType::MouseWheel;
                ev.mouseWheel.delta = GET_WHEEL_DELTA_WPARAM(info->mouseData);
                ev.mouseWheel.horizontal = false;
                break;
            case WM_MOUSEHWHEEL:
                ev.type = EventType::MouseWheel;
                ev.mouseWheel.delta = GET_WHEEL_DELTA_WPARAM(info->mouseData);
                ev.mouseWheel.horizontal = true;
                break;
            default:
                handled = false;
                break;
        }

        // Track button state only for real (non-injected) events.
        if (!injected) {
            if (isButtonDown) {
                instance_->pressedButtons_.fetch_or(btnBit);
            } else if (isButtonUp) {
                instance_->pressedButtons_.fetch_and(~btnBit);
            }
        }

        if (handled) {
            // Do not forward injected events to the remote.
            bool suppressEvent = false;
            if (!injected && instance_->callback_) {
                try {
                    suppressEvent = instance_->callback_(ev);
                } catch (const std::exception& e) {
                    ZB_LOG_ERROR("Mouse hook callback exception: {}", e.what());
                } catch (...) {
                    ZB_LOG_ERROR("Mouse hook callback unknown exception");
                }
            }
            // Suppress if the global suppress flag is on OR the callback
            // explicitly requested it. The callback return value is critical
            // for return-edge events where suppression is released inside the
            // callback but the current event must still be swallowed.
            if ((instance_->suppress_.load() || suppressEvent) && !injected) {
                return 1;
            }
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

} // namespace zb
