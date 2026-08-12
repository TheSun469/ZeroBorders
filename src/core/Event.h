#pragma once

#include <cstdint>

namespace zb {

enum class EventType : uint8_t {
    MouseMove,
    MouseButton,
    MouseWheel,
    KeyDown,
    KeyUp,
    CursorEnter,
    CursorLeave,
};

enum class MouseButton : uint8_t { Left, Right, Middle, X1, X2 };

struct MouseMoveEvent {
    int32_t x;
    int32_t y;
};

struct MouseButtonEvent {
    MouseButton button;
    bool pressed;
    int32_t x;
    int32_t y;
};

struct MouseWheelEvent {
    int32_t delta;
    bool horizontal;
};

struct KeyEvent {
    uint16_t vkCode;
    uint16_t scanCode;
    bool extended;
};

struct InputEvent {
    EventType type;
    uint64_t timestamp;
    union {
        MouseMoveEvent   mouseMove;
        MouseButtonEvent mouseButton;
        MouseWheelEvent  mouseWheel;
        KeyEvent         key;
    };
};

} // namespace zb
