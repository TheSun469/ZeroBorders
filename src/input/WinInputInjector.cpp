#include "WinInputInjector.h"
#include "../core/Log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// OCR_* cursor IDs may not be exposed when WIN32_LEAN_AND_MEAN is defined.
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

WinInputInjector::WinInputInjector() {
    screenW_ = GetSystemMetrics(SM_CXSCREEN);
    screenH_ = GetSystemMetrics(SM_CYSCREEN);
    createInvisibleCursor();
}

WinInputInjector::~WinInputInjector() {
    // 析构时恢复系统光标，防止光标永久隐藏。
    setCursorVisible(true);
    if (invisibleCursor_) {
        DestroyCursor(invisibleCursor_);
        invisibleCursor_ = nullptr;
    }
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

void WinInputInjector::releaseAllKeys() {
    // 释放所有修饰键，防止跨屏切换时修饰键状态在对端残留。
    // 用 GetAsyncKeyState 检查实际状态，只释放真正按下的键。
    struct { uint16_t vk; const char* name; } mods[] = {
        {VK_LCONTROL,  "LControl"},
        {VK_RCONTROL,  "RControl"},
        {VK_LSHIFT,    "LShift"},
        {VK_RSHIFT,    "RShift"},
        {VK_LMENU,     "LAlt"},
        {VK_RMENU,     "RAlt"},
        {VK_LWIN,      "LWin"},
        {VK_RWIN,      "RWin"},
    };

    bool any = false;
    INPUT inputs[8]{};
    int count = 0;
    for (auto& m : mods) {
        if (GetAsyncKeyState(m.vk) & 0x8000) {
            ZB_LOG_INFO("Releasing stuck key: {}", m.name);
            inputs[count].type = INPUT_KEYBOARD;
            inputs[count].ki.wVk = m.vk;
            inputs[count].ki.dwFlags = KEYEVENTF_KEYUP;
            ++count;
            any = true;
            // 重置 Win 键状态（Win+L 检测用）
            if (m.vk == VK_LWIN || m.vk == VK_RWIN) winPressed_ = false;
        }
    }
    if (any && count > 0) {
        SendInput(count, inputs, sizeof(INPUT));
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

void WinInputInjector::setCursorVisible(bool visible) {
    if (visible) {
        restoreSystemCursor();
    } else {
        hideSystemCursor();
    }
}

void WinInputInjector::createInvisibleCursor() {
    // 1x1 全透明光标：AND mask 全 1（保留屏幕像素），XOR mask 全 0（无颜色）。
    static const WORD andMask = 0xFFFF;
    static const WORD xorMask = 0x0000;
    invisibleCursor_ = CreateCursor(nullptr, 0, 0, 1, 1, &andMask, &xorMask);
}

void WinInputInjector::hideSystemCursor() {
    if (systemCursorsHidden_) return;
    if (!invisibleCursor_) return;

    // 替换所有标准系统光标为透明光标，确保无论鼠标悬停在什么窗口上
    // （文本框、链接、调整大小等）光标都不可见。
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

void WinInputInjector::restoreSystemCursor() {
    if (!systemCursorsHidden_) return;
    // SPI_SETCURSORS 从注册表重新加载默认光标，撤销所有 SetSystemCursor 替换。
    SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0);
    systemCursorsHidden_ = false;
}

} // namespace zb
