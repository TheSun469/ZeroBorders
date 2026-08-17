#pragma once

#include "../core/Event.h"

namespace zb {

class IInputInjector {
public:
    virtual ~IInputInjector() = default;
    virtual bool inject(const InputEvent& ev) = 0;
    // 直接移动光标到绝对坐标（用于 CursorEnter 时 warp 到入口位置）。
    virtual bool injectMouseMove(int32_t x, int32_t y) = 0;
    // 释放所有可能卡住的修饰键（Ctrl/Shift/Alt/Win）。
    // 跨屏切换控制权时调用，防止修饰键状态在对端残留。
    virtual void releaseAllKeys() = 0;
    // 显示/隐藏本地光标。被控端在失去控制权时隐藏光标，
    // 避免用户在控制端操作时被控端屏幕上残留静止光标。
    virtual void setCursorVisible(bool visible) = 0;
};

} // namespace zb
