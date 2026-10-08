// SPDX-License-Identifier: GPL-3.0-or-later

// The line an actor says over the room: where each speaker's line goes, the
// line resolved and laid out, the voice and the timer sprites that pace it.
// And the game's strings, global or the room's own, for whoever needs one.

#pragma once

#include "figures.hpp"
#include "localtext.hpp"
#include "vga_vm.hpp"

#include <stdint.h>

namespace speech {

/// The room strings' table and the fonts, once at start; false without them.
[[nodiscard]] bool begin();

/// The line the script asked for, if any: resolved, voiced, and its timer
/// sprites started. From the tick bank, once the frame is done with: a local
/// string can be a card read.
void pick_up();

/// The line as one more layer of this frame, while its timer sprite
/// @p sprites holds lives. From the display bank.
void lay(agos::FigureCache& figures, const agos::VgaSprite* sprites, uint8_t count);

/// Whether sprite @p id is a line's timer, which draws nothing itself.
[[nodiscard]] bool is_timer_sprite(uint16_t id);

/// Global string @p id copied into @p into, NUL-terminated within @p most;
/// its length, nought when there is none.
[[nodiscard]] uint8_t copy_global(uint16_t id, char* into, uint8_t most);

/// Any string, global or the room's own, the same way. In the tick bank.
[[nodiscard]] uint8_t resolve(uint16_t id, char* into, uint8_t most);

/// The line's own buffer, lent to a window's string as scratch. A line whose
/// pool slot is lost renders again from it, so a window string shown while it
/// stands can replace its text.
inline constexpr uint8_t LINE_CHARS = agos::LocalText::MOST_CHARS;
[[nodiscard]] char* lent_line();

/// Which line was last asked for, beside a sync wait that gave up.
void note_lost();

} // namespace speech
