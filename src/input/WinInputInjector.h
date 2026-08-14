#pragma once

#include "IInputInjector.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace zb {

class WinInputInjector : public IInputInjector {
public:
    WinInputInjector();
    ~WinInputInjector() override = default;

    bool inject(const InputEvent& ev) override;
    void releaseAllKeys() override;

    // Move cursor to absolute position (public for CursorEnter handling).
    bool injectMouseMove(int32_t x, int32_t y);

private:
    bool injectMouseButton(MouseButton button, bool pressed, int32_t x, int32_t y);
    bool injectMouseWheel(int32_t delta, bool horizontal);
    bool injectKey(uint16_t vkCode, uint16_t scanCode, bool extended, bool down);

    int screenW_ = 0;
    int screenH_ = 0;
    int lastX_ = 0;
    int lastY_ = 0;

    // Win+L 检测：SendInput 注入的 Win+L 无法锁屏，检测到组合时
    // 直接调用 LockWorkStation()。
    bool winPressed_ = false;
};

} // namespace zb
