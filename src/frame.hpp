// SPDX-License-Identifier: GPL-3.0-or-later

// The frame as the rest of the game drives it: drawn once a turn, decoding
// ahead in the time left, and told when the backdrop or the beard changes.

#pragma once

#include <stdint.h>

namespace frame {

/// The figure cache emptied, once at start: Attic keeps what the last run
/// left.
void begin();

/// Whether DMA reaches the compositor's buffers, once at start; a fault
/// says it does not.
void check_composite();

/// This turn's sprites drawn into the back display list, which the
/// interrupt then shows, and a decode in flight taken on. From the tick bank.
void draw();

/// Whether the last draw kept the frame on screen, waiting on a cel: the
/// world waits with it.
[[nodiscard]] bool held();

/// Guessed cels decoded while the frame lasts. In the tick bank: call it
/// from there.
void decode_ahead();

/// The backdrop has changed, so a mask's cut of it is cut again.
void backdrop_changed();

/// Zone @p zone's figures forgotten, as the beard changes them.
void forget_zone(uint8_t zone);

} // namespace frame
