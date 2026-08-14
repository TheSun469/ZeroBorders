#pragma once

#include "../core/Event.h"

namespace zb {

class IInputInjector {
public:
    virtual ~IInputInjector() = default;
    virtual bool inject(const InputEvent& ev) = 0;
    // 释放所有可能卡住的修饰键（Ctrl/Shift/Alt/Win）。
    // 跨屏切换控制权时调用，防止修饰键状态在对端残留。
    virtual void releaseAllKeys() = 0;
};

} // namespace zb
