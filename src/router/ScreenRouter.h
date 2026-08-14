#pragma once

#include "../core/Event.h"
#include "../core/ScreenLayout.h"
#include "../core/Protocol.h"

#include <atomic>
#include <functional>
#include <mutex>

namespace zb {

// ScreenRouter runs on the Server side. It intercepts mouse-move events,
// detects edge crossing, and manages control handoff to/from the Client.
//
// When control is local (server): mouse moves are inspected for edge crossing.
// When control is remote (client): all input events are forwarded to client
// with coordinates mapped to the client screen. The router also tracks the
// virtual cursor position on the client screen to detect return edge crossing.
class ScreenRouter {
public:
    using CursorEnterCallback = std::function<void(const CursorEnterMsg&)>;
    using SuppressCallback = std::function<void(bool suppress)>;
    using WarpCursorCallback = std::function<void(int32_t x, int32_t y)>;
    using SendEventCallback = std::function<void(const InputEvent&)>;
    using ReleaseButtonsCallback = std::function<void()>;
    using CursorVisibleCallback = std::function<void(bool visible)>;
    using ReleaseKeysCallback = std::function<void()>;

    ScreenRouter();
    ~ScreenRouter();

    void configure(uint32_t serverW, uint32_t serverH,
                   uint32_t clientW, uint32_t clientH,
                   ScreenLayout layout);

    // Dynamically change the screen layout while connected. Returns control
    // to local and resets cursor tracking so the new edge takes effect.
    void setLayout(ScreenLayout layout);

    void onCursorEnter(CursorEnterCallback cb) { enterCb_ = std::move(cb); }
    void onSuppress(SuppressCallback cb) { suppressCb_ = std::move(cb); }
    void onWarpCursor(WarpCursorCallback cb) { warpCb_ = std::move(cb); }
    void onSendEvent(SendEventCallback cb) { sendCb_ = std::move(cb); }
    void onReleaseButtons(ReleaseButtonsCallback cb) { releaseButtonsCb_ = std::move(cb); }
    void onCursorVisible(CursorVisibleCallback cb) { cursorVisibleCb_ = std::move(cb); }
    void onReleaseKeys(ReleaseKeysCallback cb) { releaseKeysCb_ = std::move(cb); }

    bool processEvent(const InputEvent& ev);

    // Called when client sends CursorLeave (mouse hit the return edge on client side).
    void handleCursorLeave(Edge clientEdge);

    void forceLocalControl();

    bool isRemoteControl() const { return remoteControl_.load(); }

private:
    bool isAtCrossEdge(int32_t x, int32_t y) const;
    CursorEnterMsg calcEntryPoint(int32_t mouseX, int32_t mouseY) const;
    void calcReturnPosition(Edge clientEdge, int32_t& outX, int32_t& outY) const;
    int32_t mapXToClient(int32_t serverX) const;
    int32_t mapYToClient(int32_t serverY) const;
    bool isAtReturnEdge(int32_t clientX, int32_t clientY) const;

    void enterRemoteControl(int32_t mouseX, int32_t mouseY);
    void returnToLocalControl(Edge clientEdge);

    uint32_t serverW_ = 0;
    uint32_t serverH_ = 0;
    uint32_t clientW_ = 0;
    uint32_t clientH_ = 0;
    ScreenLayout layout_ = ScreenLayout::LeftOf;

    std::atomic<bool> remoteControl_{false};
    std::mutex mutex_;

    CursorEnterCallback enterCb_;
    SuppressCallback suppressCb_;
    WarpCursorCallback warpCb_;
    SendEventCallback sendCb_;
    ReleaseButtonsCallback releaseButtonsCb_;
    CursorVisibleCallback cursorVisibleCb_;
    ReleaseKeysCallback releaseKeysCb_;

    int32_t lastMouseX_ = -1;
    int32_t lastMouseY_ = -1;
    // Virtual cursor position on client screen (for return edge detection).
    int32_t clientCursorX_ = -1;
    int32_t clientCursorY_ = -1;
    // Fractional accumulators for sub-pixel delta scaling. Without these,
    // integer truncation causes small mouse movements to be lost (feels
    // like the cursor is stuck or dragging slowly).
    double clientCursorAccumX_ = 0.0;
    double clientCursorAccumY_ = 0.0;
};

} // namespace zb
