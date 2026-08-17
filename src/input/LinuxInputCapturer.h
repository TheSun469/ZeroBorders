#pragma once
#include "IInputCapturer.h"
#include <X11/Xlib.h>
#include <X11/extensions/record.h>
#include <atomic>
#include <thread>
#include <mutex>

namespace zb {
class LinuxInputCapturer : public IInputCapturer {
public:
    LinuxInputCapturer();
    ~LinuxInputCapturer() override;
    bool start(EventCallback cb) override;
    void stop() override;
    void setSuppress(bool suppress) override;
    void warpCursor(int32_t x, int32_t y) override;
    void releaseAllButtons() override;
    void setCursorVisible(bool visible) override;
    uint32_t pressedButtons() const override;

private:
    void recordThreadFunc();
    static void recordCallback(XPointer closure, XRecordInterceptData* data);
    void handleKeyEvent(int keyCode, bool pressed);
    void handleButtonEvent(int button, bool pressed, int x, int y);
    void handleMotionEvent(int x, int y);

    Display* controlDisplay_ = nullptr;  // 控制连接（XWarpPointer/XDefineCursor/XTest）
    XRecordContext recordContext_ = 0;
    std::thread recordThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> suppress_{false};
    std::atomic<uint32_t> pressedButtons_{0};
    EventCallback callback_;

    // 光标隐藏
    Cursor invisibleCursor_ = 0;
    bool cursorHidden_ = false;
    void createInvisibleCursor();
    void hideSystemCursor();
    void restoreSystemCursor();

    // XWarpPointer 产生的合成 MotionNotify 会回流到 XRecord，需过滤。
    struct WarpState {
        std::atomic<int32_t> x{0};
        std::atomic<int32_t> y{0};
        std::atomic<int64_t> untilMs{0};  // 截止时间（steady_clock 毫秒）
        std::atomic<int> remaining{0};    // 剩余需过滤的事件数
    };
    WarpState warp_;

    // X11 display 非线程安全，controlDisplay_ 可能被多个线程访问
    // （XRecord 线程通过回调间接访问、主线程直接调用），需序列化。
    std::mutex controlMtx_;
};
} // namespace zb
