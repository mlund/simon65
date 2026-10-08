// SPDX-License-Identifier: GPL-3.0-or-later

// The room as the rest of the game asks for it: its zones, its picture and
// its sprites.

#pragma once

#include <stdint.h>

namespace room {

/// Zone @p zone's scripts and figures in, if they are not.
void load_zone(uint8_t zone);

/// The display on, showing the room as its scripts have painted it.
void show();

/// Every sprite stopped, as KILL_ANIMATE does: the screen keeps them until
/// the next tick.
void kill_all();

/// Zone @p zone asked for, from the draw, which met a figure of it without
/// its pixels: fetched between frames by fetch_wanted(). One at a time, and
/// none the card has answered already.
void want_zone(uint8_t zone);

/// The zone asked for and not yet fetched, 0xFF for none.
[[nodiscard]] uint8_t wanted();

/// The zone the draw asked for, fetched now that the frame is done with.
void fetch_wanted();

/// Sprite @p sprite stopped if it runs, then started afresh in @p window at
/// @p x, @p y with palette block @p palette. In the tick bank: call it from
/// there.
void start_sprite(uint16_t sprite, uint8_t window, int16_t x, uint8_t y, uint8_t palette);

} // namespace room
