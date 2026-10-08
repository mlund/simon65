// SPDX-License-Identifier: GPL-3.0-or-later

// What is heard besides the music: the voice streamed off SPEECH.BIN, and the
// effects of the sound set in. Both play on channel 3 (sound.hpp).

#pragma once

#include <stdint.h>

namespace talk {

/// SPEECH.BIN mapped and channel 3 set up, once at start. A card without
/// speech says nothing, and its lines hold for their text alone.
void begin();

/// Start voice @p asked: a new voice cuts the old, an effect plays on. A cue past
/// the table (9999) leaves the old one playing.
void speak(uint16_t asked);

/// Cut the voice, as a skipped cutscene does; an effect plays on.
void stop_voice();

/// One frame's share of the voice off the card, while any is left.
void pump();

} // namespace talk
