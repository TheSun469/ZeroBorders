#pragma once

#include "../core/Event.h"
#include <cstdint>
#include <functional>

namespace zb {

class IInputCapturer {
public:
    // Callback returns true if the event should be suppressed (swallowed,
    // not passed to the OS). This gives the consumer per-event control,
    // which is essential when state transitions happen inside the callback
    // itself (e.g. releasing suppression during a return-edge event would
    // otherwise leak that event to the OS).
    using EventCallback = std::function<bool(const InputEvent&)>;
    virtual ~IInputCapturer() = default;
    virtual bool start(EventCallback cb) = 0;
    virtual void stop() = 0;
    // When suppress is true, captured events are swallowed (not passed to the OS).
    virtual void setSuppress(bool suppress) = 0;
    // Warp the cursor to an absolute position. The resulting synthetic mouse
    // event(s) will be filtered out and never delivered to the callback.
    virtual void warpCursor(int32_t x, int32_t y) = 0;
    // Inject button-up for any mouse button currently tracked as pressed.
    // Used to clear stuck button state when crossing the screen edge mid-drag.
    virtual void releaseAllButtons() = 0;
    // Show or hide the local system cursor.
    virtual void setCursorVisible(bool visible) = 0;
    // Returns a bitmask of currently pressed mouse buttons (bit 0=Left, 1=Right, 2=Middle).
    virtual uint32_t pressedButtons() const = 0;
};

} // namespace zb
