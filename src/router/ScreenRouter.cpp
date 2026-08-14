#include "ScreenRouter.h"
#include "../core/Log.h"

#include <algorithm>

namespace zb {

ScreenRouter::ScreenRouter() = default;
ScreenRouter::~ScreenRouter() = default;

void ScreenRouter::configure(uint32_t serverW, uint32_t serverH,
                              uint32_t clientW, uint32_t clientH,
                              ScreenLayout layout) {
    std::lock_guard<std::mutex> lk(mutex_);
    serverW_ = serverW;
    serverH_ = serverH;
    clientW_ = clientW;
    clientH_ = clientH;
    layout_ = layout;
    remoteControl_ = false;
    keyboardControl_ = false;
    clientCursorX_ = -1;
    clientCursorY_ = -1;
    clientCursorAccumX_ = 0.0;
    clientCursorAccumY_ = 0.0;
    ZB_LOG_INFO("ScreenRouter configured: server {}x{}, client {}x{}, layout={}",
                serverW, serverH, clientW, clientH, layoutToString(layout));
}

void ScreenRouter::setLayout(ScreenLayout layout) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (layout_ == layout) return;
    layout_ = layout;
    // Return control to local so the new edge is used immediately.
    if (remoteControl_.load()) {
        remoteControl_ = false;
        if (releaseKeysCb_) releaseKeysCb_();
        if (suppressCb_) suppressCb_(false);
        if (cursorVisibleCb_) cursorVisibleCb_(true);
    }
    keyboardControl_ = false;
    clientCursorX_ = -1;
    clientCursorY_ = -1;
    clientCursorAccumX_ = 0.0;
    clientCursorAccumY_ = 0.0;
    ZB_LOG_INFO("ScreenRouter layout changed to {}", layoutToString(layout));
}

bool ScreenRouter::processEvent(const InputEvent& ev) {
    std::lock_guard<std::mutex> lk(mutex_);

    // 尚未配置分辨率时（连接建立前或配置异常），拒绝处理所有事件，
    // 避免 isAtCrossEdge 误判（serverW_=0 时 x>=-1 恒真）和
    // calcEntryPoint 除零崩溃。
    if (serverW_ == 0 || serverH_ == 0 || clientW_ == 0 || clientH_ == 0) {
        return false;
    }

    if (remoteControl_.load()) {
        // 鼠标控制权在对端：鼠标移动和点击始终转发给对端。
        InputEvent mapped = ev;

        if (ev.type == EventType::MouseMove) {
            // Warp-to-center relative mode: calculate delta from screen center,
            // apply to client cursor (scaled), then warp back to center.
            int32_t centerX = static_cast<int32_t>(serverW_) / 2;
            int32_t centerY = static_cast<int32_t>(serverH_) / 2;
            int32_t dx = ev.mouseMove.x - centerX;
            int32_t dy = ev.mouseMove.y - centerY;

            if (serverW_ > 0) {
                clientCursorAccumX_ += static_cast<double>(dx) *
                    static_cast<double>(clientW_) / static_cast<double>(serverW_);
            }
            if (serverH_ > 0) {
                clientCursorAccumY_ += static_cast<double>(dy) *
                    static_cast<double>(clientH_) / static_cast<double>(serverH_);
            }

            int32_t stepX = static_cast<int32_t>(clientCursorAccumX_);
            int32_t stepY = static_cast<int32_t>(clientCursorAccumY_);
            clientCursorAccumX_ -= static_cast<double>(stepX);
            clientCursorAccumY_ -= static_cast<double>(stepY);

            clientCursorX_ += stepX;
            clientCursorY_ += stepY;

            clientCursorX_ = std::max(0, std::min(clientCursorX_,
                static_cast<int32_t>(clientW_) - 1));
            clientCursorY_ = std::max(0, std::min(clientCursorY_,
                static_cast<int32_t>(clientH_) - 1));

            // 鼠标碰到返回边缘：立即切换鼠标控制权回本地（与原行为一致），
            // 键盘控制权也一并释放。
            if (isAtReturnEdge(clientCursorX_, clientCursorY_)) {
                Edge retEdge = clientReturnEdge(layout_);
                returnToLocalControl(retEdge);
                return true;
            }

            if (warpCb_) warpCb_(centerX, centerY);

            mapped.mouseMove.x = clientCursorX_;
            mapped.mouseMove.y = clientCursorY_;
            if (sendCb_) sendCb_(mapped);
            return true;
        }

        if (ev.type == EventType::MouseButton) {
            // 左键按下时切换键盘控制权到对端（鼠标已在对端）。
            if (ev.mouseButton.button == MouseButton::Left && ev.mouseButton.pressed) {
                keyboardControl_ = true;
            }
            mapped.mouseButton.x = clientCursorX_;
            mapped.mouseButton.y = clientCursorY_;
            if (sendCb_) sendCb_(mapped);
            return true;
        }

        // 键盘事件：只有 keyboardControl_ 为 true 时才转发给对端，
        // 否则让按键在本机生效（不拦截）。
        if (ev.type == EventType::KeyDown || ev.type == EventType::KeyUp) {
            if (keyboardControl_) {
                if (sendCb_) sendCb_(mapped);
                return true;   // 拦截，不传给本机
            }
            return false;       // 不拦截，本机正常处理
        }

        // 滚轮跟随鼠标，始终转发给对端。
        if (sendCb_) sendCb_(mapped);
        return true;
    }

    // 本地鼠标控制模式：鼠标移动检测边缘穿越（原行为）。
    if (ev.type == EventType::MouseMove) {
        lastMouseX_ = ev.mouseMove.x;
        lastMouseY_ = ev.mouseMove.y;

        if (isAtCrossEdge(ev.mouseMove.x, ev.mouseMove.y)) {
            enterRemoteControl(ev.mouseMove.x, ev.mouseMove.y);
            return true;
        }
        return false;
    }

    // 本地模式下，键盘和鼠标点击不拦截（本机正常处理）。
    return false;
}

void ScreenRouter::enterRemoteControl(int32_t mouseX, int32_t mouseY) {
    remoteControl_ = true;
    // 每次进入对端时重置键盘控制权，确保必须再次左键点击才能切换键盘。
    // 不重置的话，上一次左键点击设置的 keyboardControl_=true 会残留，
    // 导致鼠标再次进入客户端时键盘直接转发到对端。
    keyboardControl_ = false;

    CursorEnterMsg enter = calcEntryPoint(mouseX, mouseY);
    clientCursorX_ = enter.x;
    clientCursorY_ = enter.y;
    clientCursorAccumX_ = 0.0;
    clientCursorAccumY_ = 0.0;

    ZB_LOG_INFO("Cursor crossing edge -> client at ({}, {})", enter.x, enter.y);

    // Suppress local input FIRST so no hardware events leak.
    if (suppressCb_) suppressCb_(true);

    // Release any mouse buttons that are physically held. Without this,
    // a drag started locally would leave a stuck button-down on the server
    // while the button-up is forwarded to the client, causing accidental
    // selection/drag when the cursor returns.
    if (releaseButtonsCb_) releaseButtonsCb_();

    // 释放本地可能卡住的修饰键（Ctrl/Shift/Alt/Win），防止跨屏后
    // 本地状态残留导致对端误判组合键。同时被控端收到 CursorEnter 后
    // 也会调用 releaseAllKeys 清理自身状态。
    if (releaseKeysCb_) releaseKeysCb_();

    // Hide the local cursor so the user only sees the remote cursor.
    if (cursorVisibleCb_) cursorVisibleCb_(false);

    // Warp cursor to center of server screen for relative tracking.
    int32_t centerX = static_cast<int32_t>(serverW_) / 2;
    int32_t centerY = static_cast<int32_t>(serverH_) / 2;
    if (warpCb_) warpCb_(centerX, centerY);

    if (enterCb_) enterCb_(enter);
}

void ScreenRouter::returnToLocalControl(Edge clientEdge) {
    remoteControl_ = false;
    // 返回本地时重置键盘控制权，下次进入对端需要重新左键点击切换。
    keyboardControl_ = false;

    int32_t x = 0, y = 0;
    calcReturnPosition(clientEdge, x, y);

    ZB_LOG_INFO("Cursor returning -> server at ({}, {})", x, y);

    // 通知被控端释放所有修饰键，防止 Ctrl/Shift/Alt/Win 在对端残留。
    if (releaseKeysCb_) releaseKeysCb_();

    // Update last known position so immediate re-entry calculates the
    // correct entry Y coordinate.
    lastMouseX_ = x;
    lastMouseY_ = y;

    // Warp while still suppressed and cursor hidden (no visible jump).
    if (warpCb_) warpCb_(x, y);

    // Now show the cursor at the return position and release suppression.
    // Warp-induced events are filtered by the capturer regardless of the
    // suppress flag, so it is safe to release immediately after warp.
    if (cursorVisibleCb_) cursorVisibleCb_(true);
    if (suppressCb_) suppressCb_(false);
}

void ScreenRouter::handleCursorLeave(Edge clientEdge) {
    std::lock_guard<std::mutex> lk(mutex_);

    if (!remoteControl_.load()) {
        ZB_LOG_WARN("Received CursorLeave but control is already local");
        return;
    }

    returnToLocalControl(clientEdge);
}

void ScreenRouter::forceLocalControl() {
    std::lock_guard<std::mutex> lk(mutex_);
    keyboardControl_ = false;
    if (remoteControl_.load()) {
        remoteControl_ = false;
        if (releaseKeysCb_) releaseKeysCb_();
        if (suppressCb_) suppressCb_(false);
        if (cursorVisibleCb_) cursorVisibleCb_(true);
        ZB_LOG_INFO("Forced local control (emergency release)");
    }
}

bool ScreenRouter::isAtCrossEdge(int32_t x, int32_t y) const {
    if (serverW_ == 0 || serverH_ == 0) return false;
    Edge e = serverCrossEdge(layout_);
    switch (e) {
        case Edge::Right:  return x >= static_cast<int32_t>(serverW_) - 1;
        case Edge::Left:   return x <= 0;
        case Edge::Top:    return y <= 0;
        case Edge::Bottom: return y >= static_cast<int32_t>(serverH_) - 1;
    }
    return false;
}

CursorEnterMsg ScreenRouter::calcEntryPoint(int32_t mouseX, int32_t mouseY) const {
    CursorEnterMsg msg{};
    // 除零兜底：processEvent 已有防护，这里再防御一层确保万无一失。
    uint32_t sW = serverW_ ? serverW_ : 1;
    uint32_t sH = serverH_ ? serverH_ : 1;
    switch (layout_) {
        case ScreenLayout::RightOf:
            msg.x = 0;
            msg.y = static_cast<int32_t>(static_cast<int64_t>(mouseY) * clientH_ / sH);
            break;
        case ScreenLayout::LeftOf:
            msg.x = static_cast<int32_t>(clientW_) - 1;
            msg.y = static_cast<int32_t>(static_cast<int64_t>(mouseY) * clientH_ / sH);
            break;
        case ScreenLayout::Above:
            msg.x = static_cast<int32_t>(static_cast<int64_t>(mouseX) * clientW_ / sW);
            msg.y = static_cast<int32_t>(clientH_) - 1;
            break;
        case ScreenLayout::Below:
            msg.x = static_cast<int32_t>(static_cast<int64_t>(mouseX) * clientW_ / sW);
            msg.y = 0;
            break;
    }
    return msg;
}

void ScreenRouter::calcReturnPosition(Edge /*clientEdge*/, int32_t& outX, int32_t& outY) const {
    Edge serverEdge = serverCrossEdge(layout_);

    // Map the current client cursor position back to server coordinates so
    // the cursor appears at the correct vertical/horizontal position on the
    // return edge, not at the stale position from before entering remote.
    int32_t mappedX = (clientW_ > 0)
        ? static_cast<int32_t>(static_cast<int64_t>(clientCursorX_) * serverW_ / clientW_)
        : serverW_ / 2;
    int32_t mappedY = (clientH_ > 0)
        ? static_cast<int32_t>(static_cast<int64_t>(clientCursorY_) * serverH_ / clientH_)
        : serverH_ / 2;

    switch (serverEdge) {
        case Edge::Right:
            outX = static_cast<int32_t>(serverW_) - 2;
            outY = std::max(0, std::min(mappedY, static_cast<int32_t>(serverH_) - 1));
            break;
        case Edge::Left:
            outX = 1;
            outY = std::max(0, std::min(mappedY, static_cast<int32_t>(serverH_) - 1));
            break;
        case Edge::Top:
            outX = std::max(0, std::min(mappedX, static_cast<int32_t>(serverW_) - 1));
            outY = 1;
            break;
        case Edge::Bottom:
            outX = std::max(0, std::min(mappedX, static_cast<int32_t>(serverW_) - 1));
            outY = static_cast<int32_t>(serverH_) - 2;
            break;
    }
}

int32_t ScreenRouter::mapXToClient(int32_t serverX) const {
    if (serverW_ == 0) return serverX;
    return static_cast<int32_t>(static_cast<int64_t>(serverX) * clientW_ / serverW_);
}

int32_t ScreenRouter::mapYToClient(int32_t serverY) const {
    if (serverH_ == 0) return serverY;
    return static_cast<int32_t>(static_cast<int64_t>(serverY) * clientH_ / serverH_);
}

bool ScreenRouter::isAtReturnEdge(int32_t clientX, int32_t clientY) const {
    Edge e = clientReturnEdge(layout_);
    switch (e) {
        case Edge::Left:   return clientX <= 0;
        case Edge::Right:  return clientX >= static_cast<int32_t>(clientW_) - 1;
        case Edge::Top:    return clientY <= 0;
        case Edge::Bottom: return clientY >= static_cast<int32_t>(clientH_) - 1;
    }
    return false;
}

} // namespace zb
