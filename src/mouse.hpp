// SPDX-License-Identifier: MIT OR Apache-2.0
// Copyright 2026 Mikael Lund aka Wombat
//
// The pointer: two pot readings, turned into a place on the screen.
//
// Ported from twp65's src/mouse.cpp and src/mouse.h. The six-bit convention
// and registers follow mega65-libc; the arithmetic is custom. A reading wraps
// at sixty-four, so a step is the difference sign-extended from six bits.
// Differenced in eight and thresholded, the wrap falls in neither accepted
// window and the movement is dropped, once every sixty-four counts. That shows
// as a pointer that sticks, never as one that jumps.
//
// The arithmetic is here and host-tested; only begin and poll want the machine.

#pragma once

#include <stdint.h>

#ifdef __mos__
#include <mega65.h>
#endif

namespace mouse {

/// Readings a pot counts before it comes round.
inline constexpr uint8_t WRAP = 64;

/// The position a pot register carries. Bit nought is noise on a 1351, so the
/// six above it are the reading.
[[nodiscard]] constexpr uint8_t reading(uint8_t pot) {
    return static_cast<uint8_t>((static_cast<unsigned>(pot) >> 1U) & (WRAP - 1U));
}

/// How far the pointer moved between two readings, either way.
[[nodiscard]] constexpr int8_t step(uint8_t previous, uint8_t now) {
    const auto forward =
        static_cast<uint8_t>((static_cast<unsigned>(now) - previous) & (WRAP - 1U));
    return static_cast<int8_t>(forward < WRAP / 2 ? forward : forward - WRAP);
}

/// `value` moved `by`, held within `low`..`high`, where it already sits.
///
/// The distance to the edge is compared against the step rather than the moved
/// value against the edge: both are unsigned, and a 6502 signed compare is a
/// cmp/sbc pair with an overflow branch either side. This saves most of poll()'s cost.
[[nodiscard]] constexpr uint16_t nudge(uint16_t value, int8_t by, uint16_t low, uint16_t high) {
    if (by < 0) {
        const auto back = static_cast<uint16_t>(-static_cast<int>(by));
        return static_cast<uint16_t>(value - low) < back ? low
                                                         : static_cast<uint16_t>(value - back);
    }
    const auto forward = static_cast<uint16_t>(static_cast<uint8_t>(by));
    return static_cast<uint16_t>(high - value) < forward ? high
                                                         : static_cast<uint16_t>(value + forward);
}

/// Where the pointer is, in the picture's own coordinates.
struct Point {
    uint16_t x = 0;
    uint16_t y = 0;
};

#ifdef __mos__

/// The pots, and the button the two ports share.
inline constexpr uint16_t POT_X = 0xD620;
inline constexpr uint16_t POT_Y = 0xD621;
inline constexpr uint8_t BUTTON = 0x10;

inline uint16_t left_edge, top_edge, right_edge, bottom_edge;
inline uint8_t was_x, was_y;
inline Point at;
inline Point pressed_at;
inline bool button_seen, button_was_down;

[[nodiscard]] inline uint8_t pot(uint16_t which) {
    return *reinterpret_cast<volatile uint8_t*>(which);
}

/// Hold the pointer inside the box, and start it in the middle.
inline void begin(uint16_t left, uint16_t top, uint16_t right, uint16_t bottom) {
    left_edge = left;
    top_edge = top;
    right_edge = right;
    bottom_edge = bottom;
    at.x = static_cast<uint16_t>(left + (right - left) / 2);
    at.y = static_cast<uint16_t>(top + (bottom - top) / 2);
    // Read once without acting, so the first poll reports movement since now
    // rather than since whatever the counter happened to hold.
    was_x = reading(pot(POT_X));
    was_y = reading(pot(POT_Y));
    button_seen = false;
    button_was_down = false;
}

/// Where the pointer is, having taken the movement since the last call.
inline Point poll() {
    const uint8_t now_x = reading(pot(POT_X));
    const uint8_t now_y = reading(pot(POT_Y));
    at.x = nudge(at.x, step(was_x, now_x), left_edge, right_edge);
    // Down the screen is up the counter, so y is taken the other way about.
    at.y = nudge(at.y, static_cast<int8_t>(-step(was_y, now_y)), top_edge, bottom_edge);
    was_x = now_x;
    was_y = now_y;

    // The edge, not the level: held for a second at fifty frames a second, a
    // level would be fifty clicks. Latched, because a press between two asks is
    // still a press. A bit is clear in either port exactly when it is clear in
    // their AND, so the two ports are one test.
    const bool now_down = ((CIA1.pra & CIA1.prb) & BUTTON) == 0;
    if (now_down && !button_was_down) {
        button_seen = true;
        pressed_at = at;
    }
    button_was_down = now_down;
    return at;
}

/// Whether the button has gone down since the last ask.
[[nodiscard]] inline bool clicked() {
    const bool went_down = button_seen;
    button_seen = false;
    return went_down;
}

/// Whether the button is down now, as the last poll found it.
[[nodiscard]] inline bool down() {
    return button_was_down;
}

/// Where the pointer was when the button went down, not where it is now: a
/// press is acted on a redraw later, by which time the pointer has moved, and
/// an object twenty pixels across does not forgive that.
[[nodiscard]] inline Point where() {
    return pressed_at;
}

#endif // __mos__

} // namespace mouse
