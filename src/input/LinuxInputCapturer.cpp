#include "LinuxInputCapturer.h"
#include "../core/Log.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <X11/extensions/record.h>
#include <X11/extensions/XTest.h>

#include <chrono>
#include <cstring>
#include <map>

namespace zb {

// ---------------------------------------------------------------------------
// X 协议线缆事件格式
// KeyPress/KeyRelease/ButtonPress/ButtonRelease/MotionNotify 共享同一布局，
// 严格按 X 协议线缆格式打包（每事件 32 字节）。注意：XRecord 返回的原始
// 字节流采用服务器本机字节序，x86/x64 Linux 上与本机字节序一致，可直接强转。
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct XWireEvent {
    uint8_t  type;          // 1 字节：事件类型
    uint8_t  detail;       // 1 字节：keycode 或 button number
    uint16_t seqnum;       // 2 字节：序号
    uint32_t timestamp;    // 4 字节：服务器时间戳
    uint32_t root;         // 4 字节：根窗口 ID
    uint32_t event;        // 4 字节：事件窗口 ID
    uint32_t child;        // 4 字节：子窗口 ID
    int16_t  root_x;       // 2 字节：根窗口坐标 X
    int16_t  root_y;       // 2 字节：根窗口坐标 Y
    int16_t  event_x;      // 2 字节：事件窗口坐标 X
    int16_t  event_y;      // 2 字节：事件窗口坐标 Y
    uint16_t state;        // 2 字节：修饰键/按钮掩码
    uint8_t  same_screen;  // 1 字节
    uint8_t  pad;          // 1 字节
};
#pragma pack(pop)
static_assert(sizeof(XWireEvent) == 32, "XWireEvent 必须为 32 字节");

// X 协议核心事件类型常量
constexpr uint8_t kKeyPress       = 2;
constexpr uint8_t kKeyRelease     = 3;
constexpr uint8_t kButtonPress    = 4;
constexpr uint8_t kButtonRelease  = 5;
constexpr uint8_t kMotionNotify   = 6;

// keysym → Windows vkCode 映射表。仅包含常用键，未覆盖的按键
// 会回退到用 keysym 低 16 位作为 vkCode（保证按键不丢失）。
static const std::map<KeySym, uint16_t>& keysymToVkMap() {
    static const std::map<KeySym, uint16_t> m = {
        // 修饰键
        {XK_Shift_L,    0xA0}, {XK_Shift_R,    0xA1},
        {XK_Control_L,  0xA2}, {XK_Control_R,  0xA3},
        {XK_Alt_L,      0xA4}, {XK_Alt_R,      0xA5},
        {XK_Super_L,    0x5B}, {XK_Super_R,    0x5C},
        // 编辑/控制键
        {XK_Return,     0x0D}, {XK_Escape,     0x1B},
        {XK_space,      0x20}, {XK_Tab,        0x09},
        {XK_BackSpace,  0x08}, {XK_Delete,     0x2E},
        {XK_Insert,     0x2D}, {XK_Home,       0x24},
        {XK_End,        0x23}, {XK_Page_Up,    0x21},
        {XK_Page_Down,  0x22}, {XK_Caps_Lock,  0x14},
        {XK_Num_Lock,   0x90}, {XK_Scroll_Lock,0x91},
        // 字母 A-Z(0x41-0x5A) → XK_a-XK_z
        {XK_a, 0x41}, {XK_b, 0x42}, {XK_c, 0x43}, {XK_d, 0x44},
        {XK_e, 0x45}, {XK_f, 0x46}, {XK_g, 0x47}, {XK_h, 0x48},
        {XK_i, 0x49}, {XK_j, 0x4A}, {XK_k, 0x4B}, {XK_l, 0x4C},
        {XK_m, 0x4D}, {XK_n, 0x4E}, {XK_o, 0x4F}, {XK_p, 0x50},
        {XK_q, 0x51}, {XK_r, 0x52}, {XK_s, 0x53}, {XK_t, 0x54},
        {XK_u, 0x55}, {XK_v, 0x56}, {XK_w, 0x57}, {XK_x, 0x58},
        {XK_y, 0x59}, {XK_z, 0x5A},
        // 数字 0-9(0x30-0x39) → XK_0-XK_9
        {XK_0, 0x30}, {XK_1, 0x31}, {XK_2, 0x32}, {XK_3, 0x33},
        {XK_4, 0x34}, {XK_5, 0x35}, {XK_6, 0x36}, {XK_7, 0x37},
        {XK_8, 0x38}, {XK_9, 0x39},
        // 方向键
        {XK_Left, 0x25}, {XK_Up, 0x26}, {XK_Right, 0x27}, {XK_Down, 0x28},
        // 功能键 F1-F12
        {XK_F1,  0x70}, {XK_F2,  0x71}, {XK_F3,  0x72}, {XK_F4,  0x73},
        {XK_F5,  0x74}, {XK_F6,  0x75}, {XK_F7,  0x76}, {XK_F8,  0x77},
        {XK_F9,  0x78}, {XK_F10, 0x79}, {XK_F11, 0x7A}, {XK_F12, 0x7B},
    };
    return m;
}

// X11 button number → 按钮位索引（对应 pressedButtons_ 的 bit 位置）。
// 返回 -1 表示不可映射（滚轮 4/5/6/7 由 handleButtonEvent 单独处理）。
static int x11ButtonToBit(int button) {
    switch (button) {
        case 1: return 0;  // Left
        case 3: return 1;  // Right
        case 2: return 2;  // Middle
        case 8: return 3;  // X1
        case 9: return 4;  // X2
        default: return -1;
    }
}

// X11 button number → MouseButton 枚举
static MouseButton x11ButtonToMouseButton(int button) {
    switch (button) {
        case 1: return MouseButton::Left;
        case 2: return MouseButton::Middle;
        case 3: return MouseButton::Right;
        case 8: return MouseButton::X1;
        case 9: return MouseButton::X2;
        default: return MouseButton::Left;
    }
}

static uint64_t nowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

// ---------------------------------------------------------------------------
// 构造/析构
// ---------------------------------------------------------------------------
LinuxInputCapturer::LinuxInputCapturer() = default;

LinuxInputCapturer::~LinuxInputCapturer() {
    stop();
    // invisibleCursor_ 在 stop() 中已通过 restoreSystemCursor 释放（仅控制端 display 有效）。
    // 此处再做一次保险释放。
    std::lock_guard<std::mutex> lk(controlMtx_);
    if (invisibleCursor_ && controlDisplay_) {
        XFreeCursor(controlDisplay_, invisibleCursor_);
    }
}

bool LinuxInputCapturer::start(EventCallback cb) {
    if (running_.load()) return false;
    callback_ = std::move(cb);

    // 控制连接：用于 XWarpPointer、XDefineCursor、XTest 等命令性操作。
    // 与 XRecord 的 display 必须分开，因为 XRecord 会阻塞在自己的连接上。
    controlDisplay_ = XOpenDisplay(nullptr);
    if (!controlDisplay_) {
        ZB_LOG_ERROR("LinuxInputCapturer: 无法打开控制 X display");
        return false;
    }

    // 验证 XRecord 扩展可用
    int recordMajor = 0, recordMinor = 0;
    if (!XRecordQueryVersion(controlDisplay_, &recordMajor, &recordMinor)) {
        ZB_LOG_ERROR("LinuxInputCapturer: XRecord 扩展不可用");
        XCloseDisplay(controlDisplay_);
        controlDisplay_ = nullptr;
        return false;
    }

    // 提前创建透明光标，避免 hideSystemCursor 时再分配
    createInvisibleCursor();

    running_ = true;
    recordThread_ = std::thread([this] { recordThreadFunc(); });
    ZB_LOG_INFO("LinuxInputCapturer: 已启动（XRecord {}.{})", recordMajor, recordMinor);
    return true;
}

void LinuxInputCapturer::stop() {
    if (!running_.exchange(false)) return;

    // 恢复系统光标
    setCursorVisible(true);

    // 退出 XRecord 异步循环：在控制连接上禁用上下文，唤醒 recordDisplay 阻塞
    {
        std::lock_guard<std::mutex> lk(controlMtx_);
        if (controlDisplay_ && recordContext_) {
            XRecordDisableContext(controlDisplay_, recordContext_);
            XFlush(controlDisplay_);
        }
    }

    if (recordThread_.joinable()) {
        recordThread_.join();
    }

    // recordContext_ 的最终释放在 recordThreadFunc 内（用 recordDisplay）
    // 这里仅清理控制端引用
    {
        std::lock_guard<std::mutex> lk(controlMtx_);
        if (controlDisplay_) {
            if (invisibleCursor_) {
                XFreeCursor(controlDisplay_, invisibleCursor_);
                invisibleCursor_ = 0;
            }
            XCloseDisplay(controlDisplay_);
            controlDisplay_ = nullptr;
        }
    }
    recordContext_ = 0;
    callback_ = {};
    ZB_LOG_INFO("LinuxInputCapturer: 已停止");
}

// ---------------------------------------------------------------------------
// 公有接口
// ---------------------------------------------------------------------------
void LinuxInputCapturer::setSuppress(bool suppress) {
    // 注意：XRecord 是只读被动监听，无法真正拦截事件投递给本地应用。
    // 此标志由调用方（ScreenRouter）配合更高层逻辑使用，例如决定是否
    // 转发到远端。需要真正压制本地事件时，应额外使用 XGrabPointer。
    suppress_.store(suppress);
}

void LinuxInputCapturer::warpCursor(int32_t x, int32_t y) {
    std::lock_guard<std::mutex> lk(controlMtx_);
    if (!controlDisplay_) return;

    // 记录 warp 目标，用于在 XRecord 回调中过滤掉合成的 MotionNotify。
    // XWarpPointer 会产生一个位置精确等于目标坐标的 MotionNotify 事件。
    // 使用精确坐标匹配（容差为 0）以避免误吞真实慢速移动。
    warp_.x.store(x);
    warp_.y.store(y);
    warp_.untilMs.store(static_cast<int64_t>(nowMs()) + 15);
    warp_.remaining.store(1);

    XWarpPointer(controlDisplay_, None, DefaultRootWindow(controlDisplay_),
                 0, 0, 0, 0, x, y);
    XFlush(controlDisplay_);
}

void LinuxInputCapturer::releaseAllButtons() {
    uint32_t btns = pressedButtons_.exchange(0);
    if (!btns) return;

    std::lock_guard<std::mutex> lk(controlMtx_);
    if (!controlDisplay_) return;

    ZB_LOG_INFO("LinuxInputCapturer: 释放卡住的鼠标按钮 mask={:#x}", btns);

    // X11 button number：1=左, 2=中, 3=右, 8=X1, 9=X2
    // pressedButtons_ 位定义：bit0=Left, bit1=Right, bit2=Middle, bit3=X1, bit4=X2
    static const int x11Btns[5] = {1, 3, 2, 8, 9};
    for (int i = 0; i < 5; ++i) {
        if (btns & (1u << i)) {
            XTestFakeButtonEvent(controlDisplay_, x11Btns[i], False, CurrentTime);
        }
    }
    XFlush(controlDisplay_);
}

void LinuxInputCapturer::setCursorVisible(bool visible) {
    std::lock_guard<std::mutex> lk(controlMtx_);
    if (!controlDisplay_) return;
    if (visible) {
        restoreSystemCursor();
    } else {
        hideSystemCursor();
    }
}

uint32_t LinuxInputCapturer::pressedButtons() const {
    return pressedButtons_.load();
}

// ---------------------------------------------------------------------------
// XRecord 线程与回调
// ---------------------------------------------------------------------------
void LinuxInputCapturer::recordThreadFunc() {
    // XRecord 必须在独立 display 上运行，且会阻塞 XRecordEnableContext。
    // 此连接与 controlDisplay_ 完全隔离。
    Display* recordDisplay = XOpenDisplay(nullptr);
    if (!recordDisplay) {
        ZB_LOG_ERROR("LinuxInputCapturer: 无法打开 record X display");
        running_ = false;
        return;
    }

    // 配置捕获范围：所有客户端的核心设备事件
    //   KeyPress(2) ~ MotionNotify(6) 覆盖键盘按下/释放、鼠标按下/释放、移动
    XRecordRange* range = XRecordAllocRange();
    if (!range) {
        ZB_LOG_ERROR("LinuxInputCapturer: XRecordAllocRange 失败");
        XCloseDisplay(recordDisplay);
        running_ = false;
        return;
    }
    std::memset(range, 0, sizeof(XRecordRange));
    range->device_events.first = kKeyPress;
    range->device_events.last  = kMotionNotify;

    XRecordClientSpec client = XRecordAllClients;
    XRecordRange* ranges[1] = {range};
    XRecordClientSpec clients[1] = {client};

    // 用 recordDisplay 创建上下文（context 与 display 绑定）
    recordContext_ = XRecordCreateContext(recordDisplay, 0,
                                           clients, 1, ranges, 1);
    XFree(range);
    if (!recordContext_) {
        ZB_LOG_ERROR("LinuxInputCapturer: XRecordCreateContext 失败");
        XCloseDisplay(recordDisplay);
        running_ = false;
        return;
    }

    ZB_LOG_INFO("LinuxInputCapturer: XRecord 上下文已创建，进入捕获循环");

    // 阻塞调用：直到 stop() 中 XRecordDisableContext 唤醒
    XRecordEnableContext(recordDisplay, recordContext_,
                         &LinuxInputCapturer::recordCallback,
                         reinterpret_cast<XPointer>(this));

    // 清理：上下文释放必须用 recordDisplay
    XRecordFreeContext(recordDisplay, recordContext_);
    recordContext_ = 0;
    XCloseDisplay(recordDisplay);

    ZB_LOG_INFO("LinuxInputCapturer: XRecord 捕获循环已退出");
}

void LinuxInputCapturer::recordCallback(XPointer closure, XRecordInterceptData* data) {
    auto* self = reinterpret_cast<LinuxInputCapturer*>(closure);
    if (!self || !self->running_.load()) {
        XRecordFreeData(data);
        return;
    }

    // 仅处理来自服务器的事件（核心设备事件）
    if (data->category != XRecordFromServer || data->data_len < 32) {
        XRecordFreeData(data);
        return;
    }

    // data->data 是一组 32 字节的 X 协议线缆事件
    const int count = data->data_len / 32;
    const auto* events = reinterpret_cast<const XWireEvent*>(data->data);
    for (int i = 0; i < count; ++i) {
        const XWireEvent& ev = events[i];
        switch (ev.type) {
            case kKeyPress:
                self->handleKeyEvent(ev.detail, true);
                break;
            case kKeyRelease:
                self->handleKeyEvent(ev.detail, false);
                break;
            case kButtonPress:
                self->handleButtonEvent(ev.detail, true, ev.root_x, ev.root_y);
                break;
            case kButtonRelease:
                self->handleButtonEvent(ev.detail, false, ev.root_x, ev.root_y);
                break;
            case kMotionNotify:
                self->handleMotionEvent(ev.root_x, ev.root_y);
                break;
            default:
                break;
        }
    }

    XRecordFreeData(data);
}

void LinuxInputCapturer::handleKeyEvent(int keyCode, bool pressed) {
    if (!callback_) return;

    // keycode → keysym（XKeycodeToKeysym 虽然已标记 deprecated，但语义明确，
    // 仍可用；index=0 取第一组 keysym，对未加 Shift 的字母键返回小写）。
    KeySym keysym = NoSymbol;
    {
        std::lock_guard<std::mutex> lk(controlMtx_);
        if (controlDisplay_) {
            keysym = XkbKeycodeToKeysym(controlDisplay_,
                                        static_cast<KeyCode>(keyCode), 0, 0);
        }
    }

    uint16_t vkCode = 0;
    if (keysym != NoSymbol) {
        auto it = keysymToVkMap().find(keysym);
        if (it != keysymToVkMap().end()) {
            vkCode = it->second;
        } else {
            // 未映射的键：用 keysym 低 16 位兜底，保证不丢键
            vkCode = static_cast<uint16_t>(static_cast<unsigned long>(keysym) & 0xFFFF);
        }
    }

    InputEvent ev{};
    ev.type = pressed ? EventType::KeyDown : EventType::KeyUp;
    ev.timestamp = nowMs();
    ev.key.vkCode = vkCode;
    ev.key.scanCode = static_cast<uint16_t>(keyCode);
    ev.key.extended = false;

    // XRecord 无法真正拦截事件，回调返回值仅用于上层决策是否转发远端。
    try {
        (void)callback_(ev);
    } catch (const std::exception& e) {
        ZB_LOG_ERROR("LinuxInputCapturer: 键盘回调异常 {}", e.what());
    } catch (...) {
        ZB_LOG_ERROR("LinuxInputCapturer: 键盘回调未知异常");
    }
}

void LinuxInputCapturer::handleButtonEvent(int button, bool pressed, int x, int y) {
    if (!callback_) return;

    // 滚轮：X11 把滚轮表示为 button 4/5（垂直）、6/7（水平），
    // 按下/释放成对出现。这里在 Press 阶段构造一次 Wheel 事件，
    // delta 取 Windows WHEEL_DELTA(120) 的倍数。
    if (button == 4 || button == 5 || button == 6 || button == 7) {
        if (!pressed) return;  // 只在 Press 阶段触发一次
        InputEvent ev{};
        ev.type = EventType::MouseWheel;
        ev.timestamp = nowMs();
        switch (button) {
            case 4: ev.mouseWheel.delta = 120;  ev.mouseWheel.horizontal = false; break;  // 上
            case 5: ev.mouseWheel.delta = -120; ev.mouseWheel.horizontal = false; break; // 下
            case 6: ev.mouseWheel.delta = -120; ev.mouseWheel.horizontal = true;  break; // 左
            case 7: ev.mouseWheel.delta = 120;  ev.mouseWheel.horizontal = true;  break; // 右
        }
        try {
            (void)callback_(ev);
        } catch (const std::exception& e) {
            ZB_LOG_ERROR("LinuxInputCapturer: 滚轮回调异常 {}", e.what());
        } catch (...) {
            ZB_LOG_ERROR("LinuxInputCapturer: 滚轮回调未知异常");
        }
        return;
    }

    int bit = x11ButtonToBit(button);
    if (bit < 0) return;  // 未知按钮号，忽略

    // 维护 pressedButtons_ 掩码（仅真实事件）
    if (pressed) {
        pressedButtons_.fetch_or(1u << bit);
    } else {
        pressedButtons_.fetch_and(~(1u << bit));
    }

    InputEvent ev{};
    ev.type = EventType::MouseButton;
    ev.timestamp = nowMs();
    ev.mouseButton.button = x11ButtonToMouseButton(button);
    ev.mouseButton.pressed = pressed;
    ev.mouseButton.x = x;
    ev.mouseButton.y = y;

    try {
        (void)callback_(ev);
    } catch (const std::exception& e) {
        ZB_LOG_ERROR("LinuxInputCapturer: 按钮回调异常 {}", e.what());
    } catch (...) {
        ZB_LOG_ERROR("LinuxInputCapturer: 按钮回调未知异常");
    }
}

void LinuxInputCapturer::handleMotionEvent(int x, int y) {
    if (!callback_) return;

    // Warp 过滤：XWarpPointer 产生的合成 MotionNotify 位置精确等于 warp 目标，
    // 用精确坐标匹配（容差 0）避免误吞真实慢速移动。
    int remaining = warp_.remaining.load();
    if (remaining > 0) {
        int64_t until = warp_.untilMs.load();
        if (static_cast<int64_t>(nowMs()) < until) {
            if (warp_.x.load() == x && warp_.y.load() == y) {
                warp_.remaining.fetch_sub(1);
                return;  // 吞掉合成事件，不投递给回调
            }
        } else {
            warp_.remaining.store(0);
        }
    }

    InputEvent ev{};
    ev.type = EventType::MouseMove;
    ev.timestamp = nowMs();
    ev.mouseMove.x = x;
    ev.mouseMove.y = y;

    try {
        (void)callback_(ev);
    } catch (const std::exception& e) {
        ZB_LOG_ERROR("LinuxInputCapturer: 移动回调异常 {}", e.what());
    } catch (...) {
        ZB_LOG_ERROR("LinuxInputCapturer: 移动回调未知异常");
    }
}

// ---------------------------------------------------------------------------
// 光标隐藏
// ---------------------------------------------------------------------------
void LinuxInputCapturer::createInvisibleCursor() {
    if (!controlDisplay_ || invisibleCursor_) return;

    // 创建 1x1 全透明 Pixmap 光标：
    //   source 和 mask 都用同一张全 0 的 1x1 Pixmap，
    //   mask 全 0 表示光标所有像素都"穿透"（透明）。
    char zeroData = 0;
    Pixmap src = XCreatePixmapFromBitmapData(controlDisplay_,
        DefaultRootWindow(controlDisplay_), &zeroData, 1, 1, 0, 0, 1);
    Pixmap msk = XCreatePixmapFromBitmapData(controlDisplay_,
        DefaultRootWindow(controlDisplay_), &zeroData, 1, 1, 0, 0, 1);
    if (src && msk) {
        XColor fg{}, bg{};  // 全 0 颜色
        fg.flags = DoRed | DoGreen | DoBlue;
        bg.flags = DoRed | DoGreen | DoBlue;
        invisibleCursor_ = XCreatePixmapCursor(controlDisplay_, src, msk, &fg, &bg, 0, 0);
        XFreePixmap(controlDisplay_, src);
        XFreePixmap(controlDisplay_, msk);
    }
}

void LinuxInputCapturer::hideSystemCursor() {
    if (!controlDisplay_ || cursorHidden_) return;
    if (!invisibleCursor_) createInvisibleCursor();
    if (!invisibleCursor_) return;

    // 为根窗口定义透明光标。子窗口若未自定义光标会继承根窗口的光标，
    // 大多数应用窗口都会继承，从而实现全局隐藏效果。
    XDefineCursor(controlDisplay_, DefaultRootWindow(controlDisplay_), invisibleCursor_);
    XFlush(controlDisplay_);
    cursorHidden_ = true;
}

void LinuxInputCapturer::restoreSystemCursor() {
    if (!controlDisplay_ || !cursorHidden_) return;
    // XUndefineCursor 恢复根窗口默认光标
    XUndefineCursor(controlDisplay_, DefaultRootWindow(controlDisplay_));
    XFlush(controlDisplay_);
    cursorHidden_ = false;
}

} // namespace zb
