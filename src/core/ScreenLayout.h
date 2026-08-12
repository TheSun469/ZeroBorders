#pragma once

#include <cstdint>
#include <string>

namespace zb {

enum class ScreenLayout : uint8_t {
    RightOf,  // Client screen is to the right of Server
    LeftOf,   // Client screen is to the left of Server
    Above,    // Client screen is above Server
    Below,    // Client screen is below Server
};

// Which edge of a screen the cursor crossed.
enum class Edge : uint8_t {
    Left,
    Right,
    Top,
    Bottom,
};

inline std::string layoutToString(ScreenLayout l) {
    switch (l) {
        case ScreenLayout::RightOf: return "right_of";
        case ScreenLayout::LeftOf:  return "left_of";
        case ScreenLayout::Above:   return "above";
        case ScreenLayout::Below:   return "below";
    }
    return "left_of";
}

inline ScreenLayout layoutFromString(const std::string& s) {
    if (s == "right_of") return ScreenLayout::RightOf;
    if (s == "above")    return ScreenLayout::Above;
    if (s == "below")    return ScreenLayout::Below;
    return ScreenLayout::LeftOf; // default
}

// The edge on the server screen that, when crossed, hands control to the client.
inline Edge serverCrossEdge(ScreenLayout l) {
    switch (l) {
        case ScreenLayout::RightOf: return Edge::Right;
        case ScreenLayout::LeftOf:  return Edge::Left;
        case ScreenLayout::Above:   return Edge::Top;
        case ScreenLayout::Below:   return Edge::Bottom;
    }
    return Edge::Right;
}

// The edge on the client screen that, when crossed, returns control to the server.
inline Edge clientReturnEdge(ScreenLayout l) {
    switch (l) {
        case ScreenLayout::RightOf: return Edge::Left;
        case ScreenLayout::LeftOf:  return Edge::Right;
        case ScreenLayout::Above:   return Edge::Bottom;
        case ScreenLayout::Below:   return Edge::Top;
    }
    return Edge::Left;
}

} // namespace zb
