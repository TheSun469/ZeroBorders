#pragma once
#include "IInputInjector.h"
#include <X11/Xlib.h>
#include <mutex>

namespace zb {
class LinuxInputInjector : public IInputInjector {
public:
    LinuxInputInjector();
    ~LinuxInputInjector() override;
    bool inject(const InputEvent& ev) override;
    void releaseAllKeys() override;
    void setCursorVisible(bool visible) override;
    bool injectMouseMove(int32_t x, int32_t y);

private:
    bool injectMouseButton(MouseButton button, bool pressed, int32_t x, int32_t y);
    bool injectMouseWheel(int32_t delta, bool horizontal);
    bool injectKey(uint16_t vkCode, bool down);
    uint16_t vkToX11KeyCode(uint16_t vkCode);  // vkCode → X11 keycode

    Display* display_ = nullptr;
    int screenW_ = 0;
    int screenH_ = 0;

    // 光标隐藏
    Cursor invisibleCursor_ = 0;
    bool systemCursorsHidden_ = false;
    void createInvisibleCursor();
    void hideSystemCursor();
    void restoreSystemCursor();

    // X11 display 非线程安全；inject 等方法可能被多线程调用
    // （主线程 + 接收线程），需序列化所有 display_ 调用。
    std::mutex mtx_;
};
} // namespace zb
