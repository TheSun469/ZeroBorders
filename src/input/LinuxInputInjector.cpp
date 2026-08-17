#include "LinuxInputInjector.h"
#include "../core/Log.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>

#include <cstring>
#include <map>

namespace zb {

// Windows WHEEL_DELTA：120 表示一格滚轮。
constexpr int32_t kWheelDelta = 120;

// vkCode → keysym 映射表（Capturer 的 keysym→vkCode 的反向表）。
// 仅包含常用键；未映射的 vkCode 将查不到对应 keysym，注入会失败。
static const std::map<uint16_t, KeySym>& vkToKeysymMap() {
    static const std::map<uint16_t, KeySym> m = {
        // 修饰键
        {0xA0, XK_Shift_L},    {0xA1, XK_Shift_R},
        {0xA2, XK_Control_L},  {0xA3, XK_Control_R},
        {0xA4, XK_Alt_L},      {0xA5, XK_Alt_R},
        {0x5B, XK_Super_L},    {0x5C, XK_Super_R},
        // 编辑/控制键
        {0x0D, XK_Return},    {0x1B, XK_Escape},
        {0x20, XK_space},     {0x09, XK_Tab},
        {0x08, XK_BackSpace}, {0x2E, XK_Delete},
        {0x2D, XK_Insert},    {0x24, XK_Home},
        {0x23, XK_End},       {0x21, XK_Page_Up},
        {0x22, XK_Page_Down}, {0x14, XK_Caps_Lock},
        {0x90, XK_Num_Lock},  {0x91, XK_Scroll_Lock},
        // 字母 A-Z(0x41-0x5A)
        {0x41, XK_a}, {0x42, XK_b}, {0x43, XK_c}, {0x44, XK_d},
        {0x45, XK_e}, {0x46, XK_f}, {0x47, XK_g}, {0x48, XK_h},
        {0x49, XK_i}, {0x4A, XK_j}, {0x4B, XK_k}, {0x4C, XK_l},
        {0x4D, XK_m}, {0x4E, XK_n}, {0x4F, XK_o}, {0x50, XK_p},
        {0x51, XK_q}, {0x52, XK_r}, {0x53, XK_s}, {0x54, XK_t},
        {0x55, XK_u}, {0x56, XK_v}, {0x57, XK_w}, {0x58, XK_x},
        {0x59, XK_y}, {0x5A, XK_z},
        // 数字 0-9(0x30-0x39)
        {0x30, XK_0}, {0x31, XK_1}, {0x32, XK_2}, {0x33, XK_3},
        {0x34, XK_4}, {0x35, XK_5}, {0x36, XK_6}, {0x37, XK_7},
        {0x38, XK_8}, {0x39, XK_9},
        // 方向键
        {0x25, XK_Left}, {0x26, XK_Up}, {0x27, XK_Right}, {0x28, XK_Down},
        // 功能键 F1-F12
        {0x70, XK_F1},  {0x71, XK_F2},  {0x72, XK_F3},  {0x73, XK_F4},
        {0x74, XK_F5},  {0x75, XK_F6},  {0x76, XK_F7},  {0x77, XK_F8},
        {0x78, XK_F9},  {0x79, XK_F10}, {0x7A, XK_F11}, {0x7B, XK_F12},
    };
    return m;
}

// MouseButton 枚举 → X11 button number
static int mouseButtonToX11(MouseButton b) {
    switch (b) {
        case MouseButton::Left:   return 1;
        case MouseButton::Middle: return 2;
        case MouseButton::Right:  return 3;
        case MouseButton::X1:     return 8;
        case MouseButton::X2:     return 9;
    }
    return 1;
}

// ---------------------------------------------------------------------------
// 构造/析构
// ---------------------------------------------------------------------------
LinuxInputInjector::LinuxInputInjector() {
    display_ = XOpenDisplay(nullptr);
    if (!display_) {
        ZB_LOG_ERROR("LinuxInputInjector: 无法打开 X display");
        return;
    }
    screenW_ = XDisplayWidth(display_, DefaultScreen(display_));
    screenH_ = XDisplayHeight(display_, DefaultScreen(display_));
    createInvisibleCursor();
    ZB_LOG_INFO("LinuxInputInjector: 已初始化，屏幕 {}x{}", screenW_, screenH_);
}

LinuxInputInjector::~LinuxInputInjector() {
    // 析构时恢复系统光标，防止光标永久隐藏
    setCursorVisible(true);

    std::lock_guard<std::mutex> lk(mtx_);
    if (invisibleCursor_ && display_) {
        XFreeCursor(display_, invisibleCursor_);
        invisibleCursor_ = 0;
    }
    if (display_) {
        XCloseDisplay(display_);
        display_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// IInputInjector 接口
// ---------------------------------------------------------------------------
bool LinuxInputInjector::inject(const InputEvent& ev) {
    switch (ev.type) {
        case EventType::MouseMove:
            return injectMouseMove(ev.mouseMove.x, ev.mouseMove.y);
        case EventType::MouseButton:
            return injectMouseButton(ev.mouseButton.button, ev.mouseButton.pressed,
                                      ev.mouseButton.x, ev.mouseButton.y);
        case EventType::MouseWheel:
            return injectMouseWheel(ev.mouseWheel.delta, ev.mouseWheel.horizontal);
        case EventType::KeyDown:
            return injectKey(ev.key.vkCode, true);
        case EventType::KeyUp:
            return injectKey(ev.key.vkCode, false);
        default:
            return false;
    }
}

void LinuxInputInjector::releaseAllKeys() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!display_) return;

    // 用 XQueryKeymap 查询当前真实按下的键（32 字节位图，每位对应一个 keycode）。
    // 仅对按下的修饰键发送 KeyUp，避免冗余注入。
    char keys[32] = {0};
    XQueryKeymap(display_, keys);

    // 修饰键 vkCode → keysym → keycode 映射
    struct ModKey { uint16_t vk; KeySym keysym; const char* name; };
    static const ModKey mods[] = {
        {0xA2, XK_Control_L, "LControl"},
        {0xA3, XK_Control_R, "RControl"},
        {0xA0, XK_Shift_L,   "LShift"},
        {0xA1, XK_Shift_R,   "RShift"},
        {0xA4, XK_Alt_L,     "LAlt"},
        {0xA5, XK_Alt_R,     "RAlt"},
        {0x5B, XK_Super_L,   "LWin"},
        {0x5C, XK_Super_R,   "RWin"},
    };

    for (const auto& m : mods) {
        KeyCode kc = XKeysymToKeycode(display_, m.keysym);
        if (kc == 0) continue;
        // 检查 keycode 对应位是否被按下
        if (keys[kc / 8] & (1 << (kc % 8))) {
            ZB_LOG_INFO("LinuxInputInjector: 释放残留修饰键 {}", m.name);
            XTestFakeKeyEvent(display_, kc, False, CurrentTime);
        }
    }
    XFlush(display_);
}

bool LinuxInputInjector::injectMouseMove(int32_t x, int32_t y) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!display_) return false;
    // XTestFakeMotionEvent：screen=-1 表示使用默认屏幕
    if (!XTestFakeMotionEvent(display_, -1, x, y, CurrentTime)) return false;
    XFlush(display_);
    return true;
}

// ---------------------------------------------------------------------------
// 私有实现
// ---------------------------------------------------------------------------
bool LinuxInputInjector::injectMouseButton(MouseButton button, bool pressed,
                                            int32_t x, int32_t y) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!display_) return false;

    // 先把光标移动到目标位置（与 Windows 实现保持一致：注入按钮前先定位）
    XTestFakeMotionEvent(display_, -1, x, y, CurrentTime);

    int x11Btn = mouseButtonToX11(button);
    if (!XTestFakeButtonEvent(display_, x11Btn, pressed ? True : False, CurrentTime)) {
        return false;
    }
    XFlush(display_);
    return true;
}

bool LinuxInputInjector::injectMouseWheel(int32_t delta, bool horizontal) {
    if (delta == 0) return true;

    std::lock_guard<std::mutex> lk(mtx_);
    if (!display_) return false;

    // 计算滚动格数：每 120 delta 为一格
    int clicks = (std::abs(delta) + kWheelDelta / 2) / kWheelDelta;
    if (clicks <= 0) clicks = 1;

    // X11 滚轮按钮：
    //   垂直：4=上(delta>0), 5=下(delta<0)
    //   水平：7=右(delta>0), 6=左(delta<0)
    int button;
    if (horizontal) {
        button = (delta > 0) ? 7 : 6;
    } else {
        button = (delta > 0) ? 4 : 5;
    }

    for (int i = 0; i < clicks; ++i) {
        // 滚轮一格 = ButtonPress + ButtonRelease 配对
        XTestFakeButtonEvent(display_, button, True, CurrentTime);
        XTestFakeButtonEvent(display_, button, False, CurrentTime);
    }
    XFlush(display_);
    return true;
}

bool LinuxInputInjector::injectKey(uint16_t vkCode, bool down) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!display_) return false;

    KeyCode kc = vkToX11KeyCode(vkCode);
    if (kc == 0) {
        ZB_LOG_WARN("LinuxInputInjector: vkCode={:#x} 无对应 X11 keycode，跳过", vkCode);
        return false;
    }
    if (!XTestFakeKeyEvent(display_, kc, down ? True : False, CurrentTime)) {
        return false;
    }
    XFlush(display_);
    return true;
}

uint16_t LinuxInputInjector::vkToX11KeyCode(uint16_t vkCode) {
    if (!display_) return 0;

    // vkCode → keysym → keycode
    auto it = vkToKeysymMap().find(vkCode);
    if (it == vkToKeysymMap().end()) return 0;

    KeyCode kc = XKeysymToKeycode(display_, it->second);
    return static_cast<uint16_t>(kc);
}

void LinuxInputInjector::setCursorVisible(bool visible) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!display_) return;
    if (visible) {
        restoreSystemCursor();
    } else {
        hideSystemCursor();
    }
}

// ---------------------------------------------------------------------------
// 光标隐藏
// ---------------------------------------------------------------------------
void LinuxInputInjector::createInvisibleCursor() {
    if (!display_ || invisibleCursor_) return;

    // 1x1 全透明 Pixmap 光标：source 与 mask 都用全 0 的 1x1 位图，
    // mask 全 0 表示所有像素都"穿透"（透明），实现完全不可见的光标。
    char zeroData = 0;
    Pixmap src = XCreatePixmapFromBitmapData(display_,
        DefaultRootWindow(display_), &zeroData, 1, 1, 0, 0, 1);
    Pixmap msk = XCreatePixmapFromBitmapData(display_,
        DefaultRootWindow(display_), &zeroData, 1, 1, 0, 0, 1);
    if (src && msk) {
        XColor fg{}, bg{};  // 全 0 颜色
        fg.flags = DoRed | DoGreen | DoBlue;
        bg.flags = DoRed | DoGreen | DoBlue;
        invisibleCursor_ = XCreatePixmapCursor(display_, src, msk, &fg, &bg, 0, 0);
        XFreePixmap(display_, src);
        XFreePixmap(display_, msk);
    }
}

void LinuxInputInjector::hideSystemCursor() {
    if (!display_ || systemCursorsHidden_) return;
    if (!invisibleCursor_) createInvisibleCursor();
    if (!invisibleCursor_) return;

    // 为根窗口定义透明光标，子窗口继承后实现全局隐藏
    XDefineCursor(display_, DefaultRootWindow(display_), invisibleCursor_);
    XFlush(display_);
    systemCursorsHidden_ = true;
}

void LinuxInputInjector::restoreSystemCursor() {
    if (!display_ || !systemCursorsHidden_) return;
    XUndefineCursor(display_, DefaultRootWindow(display_));
    XFlush(display_);
    systemCursorsHidden_ = false;
}

} // namespace zb
