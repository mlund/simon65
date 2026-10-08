// SPDX-License-Identifier: GPL-3.0-or-later

// The four save slots on the F keys and the game's own SAVE/LOAD_USER_GAME:
// the slot files on the card, the image and its check, and the border flash
// that says how a key went.

#pragma once

#include <stdint.h>

namespace saves {

/// A key off the queue, in the tick. True for a load key whose slot will
/// load: the caller unwinds the running script, and act() loads once it has.
/// A slot key is refused while the pointer is hidden, and that is decided
/// here, once, for act() as well.
[[nodiscard]] bool press(uint8_t key);

/// The key press() last took, acted on once from the main loop with no
/// script running: an F key saves or loads its slot, anything else nothing.
void act();

/// The border back to black once a key's flash has run its time.
void flash_over(uint16_t now);

/// What a load key's load must do after it that the game's script would have.
/// Defined by the game, always_inline: its one caller is in the save bank,
/// and CODE_BANK would make it noinline.
void reloaded();

} // namespace saves
