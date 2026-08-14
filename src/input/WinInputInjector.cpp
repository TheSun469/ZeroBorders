#include "WinInputInjector.h"
#include "../core/Log.h"

namespace zb {

WinInputInjector::WinInputInjector() {
    screenW_ = GetSystemMetrics(SM_CXSCREEN);
    screenH_ = GetSystemMetrics(SM_CYSCREEN);
}

bool WinInputInjector::inject(const InputEvent& ev) {
    switch (ev.type) {
        case EventType::MouseMove:
            return injectMouseMove(ev.mouseMove.x, ev.mouseMove.y);
        case EventType::MouseButton:
            return injectMouseButton(ev.mouseButton.button, ev.mouseButton.pressed,
                                     ev.mouseButton.x, ev.mouseButton.y);
        case EventType::MouseWheel:
            return injectMouseWheel(ev.mouseWheel.delta, ev.mouseWheel.horizontal);
        case EventType::KeyDown:
            return injectKey(ev.key.vkCode, ev.key.scanCode, ev.key.extended, true);
        case EventType::KeyUp:
            return injectKey(ev.key.vkCode, ev.key.scanCode, ev.key.extended, false);
        default:
            return false;
    }
}

bool WinInputInjector::injectMouseMove(int32_t x, int32_t y) {
    lastX_ = x;
    lastY_ = y;
    INPUT input{};
    input.type = INPUT_MOUSE;
    // Absolute coordinates require mapping to 0..65535 range.
    input.mi.dx = (screenW_ > 0) ? MulDiv(x, 65535, screenW_ - 1) : 0;
    input.mi.dy = (screenH_ > 0) ? MulDiv(y, 65535, screenH_ - 1) : 0;
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    return SendInput(1, &input, sizeof(INPUT)) == 1;
}

bool WinInputInjector::injectMouseButton(MouseButton button, bool pressed, int32_t x, int32_t y) {
    // Move to position first.
    injectMouseMove(x, y);

    INPUT input{};
    input.type = INPUT_MOUSE;
    DWORD flag = 0;
    switch (button) {
        case MouseButton::Left:
            flag = pressed ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
            break;
        case MouseButton::Right:
            flag = pressed ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
            break;
        case MouseButton::Middle:
            flag = pressed ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
            break;
        case MouseButton::X1:
            flag = pressed ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
            input.mi.mouseData = XBUTTON1;
            break;
        case MouseButton::X2:
            flag = pressed ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
            input.mi.mouseData = XBUTTON2;
            break;
    }
    input.mi.dwFlags = flag;
    return SendInput(1, &input, sizeof(INPUT)) == 1;
}

bool WinInputInjector::injectMouseWheel(int32_t delta, bool horizontal) {
    // SendInput routes WM_MOUSEWHEEL to the foreground (focused) window,
    // which may be our own window rather than the application under the
    // cursor. We instead locate the window under the cursor and post the
    // wheel message directly to it, matching real hardware behaviour.
    POINT pt{lastX_, lastY_};
    HWND hwnd = WindowFromPoint(pt);
    if (hwnd) {
        // Screen coordinates for lParam.
        LPARAM lParam = MAKELPARAM(static_cast<USHORT>(pt.x),
                                   static_cast<USHORT>(pt.y));
        UINT msg = horizontal ? WM_MOUSEHWHEEL : WM_MOUSEWHEEL;
        WPARAM wParam = MAKEWPARAM(0, static_cast<SHORT>(delta));
        PostMessage(hwnd, msg, wParam, lParam);
        return true;
    }

    // Fallback to SendInput if WindowFromPoint fails.
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = horizontal ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
    input.mi.mouseData = static_cast<DWORD>(delta);
    return SendInput(1, &input, sizeof(INPUT)) == 1;
}

bool WinInputInjector::injectKey(uint16_t vkCode, uint16_t scanCode, bool extended, bool down) {
    // 跟踪 Win 键状态用于 Win+L 检测。
    if (vkCode == VK_LWIN || vkCode == VK_RWIN) {
        winPressed_ = down;
        // Win 键正常注入，让其他 Win 组合键也能工作。
    } else if (winPressed_ && vkCode == 'L' && down) {
        // 命中 Win+L：SendInput 注入的 Win+L 无法锁屏，
        // 直接调用 LockWorkStation()。
        // 先释放 Win 键（避免锁屏后键状态残留）。
        INPUT winUp{};
        winUp.type = INPUT_KEYBOARD;
        winUp.ki.wVk = VK_LWIN;
        winUp.ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(1, &winUp, sizeof(INPUT));
        winPressed_ = false;
        LockWorkStation();
        return true;
    }

    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vkCode;
    input.ki.wScan = scanCode;
    input.ki.dwFlags = 0;
    if (extended) input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    if (!down) input.ki.dwFlags |= KEYEVENTF_KEYUP;
    return SendInput(1, &input, sizeof(INPUT)) == 1;
}

} // namespace zb
