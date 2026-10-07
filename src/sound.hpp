// Speech and effects on audio channel 3: starting and stopping them.
//
// The refill is sound.S, from the raster interrupt: each ring page is the
// speech's next page, or silence, with the effect's next page added into it,
// clipped (sound.S). This is the main loop's half: set a
// stream's position, prime the ring, program the channel. Music keeps
// channels 0-2 (modplay.S), as the CD32's does.
//
// The arbitration is the CD32's (runit2 0x1cd4a, 0x1f36a): new speech cuts
// the old and leaves an effect playing; a new effect cuts the old effect and
// never speech.

#pragma once

#include "banks.hpp"
#include "chipmap.hpp"
#include "far.hpp"
#include "move.hpp"

#include <mega65.h>
#include <stdint.h>

extern "C" {
/// One refill pass: every page the play position has left (sound.S).
void sound_tick();
/// Fill ring page sound_page and step it on, while sound_live is nought.
void sound_fill();
extern volatile uint8_t sound_live, sound_page, sound_hushed, sound_loading, sound_quiet,
    sound_playing;
extern volatile uint16_t sound_left, sound_effect_left;
extern volatile uint8_t sound_src_megabyte, sound_src_bank, sound_src_page;
/// sound.S's two 32-bit pointers, for lda [ptr],z: the effect's next page
/// and the ring page it is added into, low bytes nought. Zero page, declared
/// here so the compiler's own allocation knows of them (main.cpp).
extern __zp volatile uint8_t sound_effect[4], sound_ring[4];
}

namespace sound {

inline constexpr uint16_t PAGE = 256;
inline constexpr uint8_t RING_PAGES = static_cast<uint8_t>(chipmap::SPEECH_BYTES / PAGE);
static_assert(RING_PAGES == 16, "sound.S masks the ring's pages with 15");
inline constexpr uint8_t CHANNEL = 3;

/// The rate register counts CPU cycles: it is added to a 24-bit counter each
/// one, and a sample is fetched when that overflows (twp65 twpsnd.py).
inline constexpr uint32_t CLOCK = 40500000;
[[nodiscard]] constexpr uint32_t rate_register(uint32_t hertz) {
    return static_cast<uint32_t>(((uint64_t{hertz} << 24) + CLOCK / 2) / CLOCK);
}
/// Speech's rate (sound.cpp:249), and effects', which the CD32 adds into
/// speech's buffer as they are.
inline constexpr uint32_t SPEECH_RATE = rate_register(22050);
static_assert(SPEECH_RATE == 9134, "twp65 twpsnd.py freq_of(22050)");

/// Looped by the hardware, signed eight-bit samples.
inline constexpr uint8_t ENABLED = DMA_CHENABLE | DMA_CHLOOP | DMA_CHSBITS_8;

/// Speech's level on both sides: full, over music at a module's own 0-64
/// (modplay.S). A side sums its channels and clips (gs4510.vhdl:4211-4227).
inline constexpr uint8_t VOLUME = 0xFF;

/// What never changes about channel 3, once at startup: the ring's place and
/// top, the rate, both volumes, and the constant bytes of sound.S's pointers.
/// Music leaves channel 3 alone (modplay.S), so they stay set.
inline void begin() {
    auto& channel = DMA.channel[CHANNEL];
    channel.enable = 0;
    constexpr uint32_t RING = chipmap::SPEECH;
    channel.baddr_lsb = static_cast<uint8_t>(RING);
    channel.baddr_msb = static_cast<uint8_t>(RING >> 8);
    channel.baddr_mb = static_cast<uint8_t>(RING >> 16);
    constexpr uint16_t TOP = static_cast<uint16_t>(RING + chipmap::SPEECH_BYTES);
    channel.taddr_lsb = static_cast<uint8_t>(TOP);
    channel.taddr_msb = static_cast<uint8_t>(TOP >> 8);
    channel.freq_lsb = static_cast<uint8_t>(SPEECH_RATE);
    channel.freq_mb = static_cast<uint8_t>(SPEECH_RATE >> 8);
    channel.freq_msb = static_cast<uint8_t>(SPEECH_RATE >> 16);
    // A mono channel wants both sides: channel 3's own volume is the right,
    // ch3lvol the left (gs4510.vhdl:4212-4235).
    channel.volume = VOLUME;
    DMA.ch3lvol = VOLUME;
    sound_ring[0] = 0;
    sound_ring[2] = static_cast<uint8_t>(RING >> 16);
    sound_ring[3] = 0;
    sound_effect[0] = 0;
}

/// Silence channel 3 and let go of both streams.
inline void stop() {
    sound_live = 0;
    DMA.channel[CHANNEL].enable = 0;
    sound_loading = 0;
    sound_left = 0;
    sound_effect_left = 0;
}

/// Whether speech is still playing: IF_SPEECH. A ringful filled without it
/// is a ringful played without it, the refill running a ring behind.
[[nodiscard]] inline bool speaking() {
    return sound_live != 0 && sound_quiet < RING_PAGES;
}

/// Prime the whole ring as the refill would, and set the channel going --
/// unless there was nothing to prime it with. sound_live must be nought.
/// In the sound bank with its callers, not left to land in the fixed region.
SOUND_BANKED inline void start() {
    sound_page = 0;
    sound_hushed = 0;
    for (uint8_t page = 0; page < RING_PAGES; ++page)
        sound_fill();
    if (sound_hushed >= RING_PAGES)
        return;
    // Bits 8-15 are baddr_msb in begin() but curaddr_mb here: the SDK's names
    // swap (twp65 audio.cpp:100-104). Not cleared by disabling the channel.
    auto& channel = DMA.channel[CHANNEL];
    constexpr uint32_t RING = chipmap::SPEECH;
    channel.curaddr_lsb = static_cast<uint8_t>(RING);
    channel.curaddr_mb = static_cast<uint8_t>(RING >> 8);
    channel.curaddr_msb = static_cast<uint8_t>(RING >> 16);
    channel.enable = ENABLED;
    sound_live = 1;
}

/// Pages of the effect playing, as `effect` was given them.
inline uint16_t effect_pages = 0;

/// Step a playing effect back over the pages it has in the ring that the
/// channel has not played yet, so re-priming the ring loses none of it.
inline void rewind_effect() {
    if (sound_effect_left == 0)
        return;
    uint16_t back = static_cast<uint8_t>((sound_page - sound_playing) & (RING_PAGES - 1));
    const uint16_t added = static_cast<uint16_t>(effect_pages - sound_effect_left);
    if (back > added)
        back = added;
    // A page is the pointer's bytes 1 to 3, the low byte nought.
    const uint32_t at =
        (uint32_t{sound_effect[3]} << 16 | uint32_t{sound_effect[2]} << 8 | sound_effect[1]) - back;
    sound_effect[1] = static_cast<uint8_t>(at);
    sound_effect[2] = static_cast<uint8_t>(at >> 8);
    sound_effect[3] = static_cast<uint8_t>(at >> 16);
    sound_effect_left = static_cast<uint16_t>(sound_effect_left + back);
}

/// Speak @p pages pages from Attic @p from, page-aligned; with @p more, the
/// rest is still being read and arrives through `arrived`. A playing effect
/// goes on from the last page the channel played.
inline void speak(agos::Place from, uint16_t pages, bool more) {
    sound_live = 0;
    DMA.channel[CHANNEL].enable = 0;
    rewind_effect();
    sound_src_page = static_cast<uint8_t>(from >> 8);
    sound_src_bank = static_cast<uint8_t>((from >> 16) & 0x0F);
    sound_src_megabyte = static_cast<uint8_t>(from >> 20);
    sound_left = pages;
    sound_loading = more ? 1 : 0;
    sound_quiet = 0;
    start();
}

/// Play the effect of @p pages pages at Attic @p from, page-aligned, over
/// whatever speech is playing. Joined to a ring already going, it is added
/// from the next page the refill fills.
SOUND_BANKED inline void effect(agos::Place from, uint16_t pages) {
    bool live = false;
    {
        const Masked masked;
        live = sound_live != 0; // under the mask: it may end meanwhile
        sound_effect[1] = static_cast<uint8_t>(from >> 8);
        sound_effect[2] = static_cast<uint8_t>(from >> 16);
        sound_effect[3] = static_cast<uint8_t>(from >> 24);
        sound_effect_left = pages;
    }
    effect_pages = pages;
    if (!live) {
        sound_quiet = RING_PAGES; // nothing said, so IF_SPEECH stays false
        start();
    }
}

/// Stop the effect and leave speech playing. What of it is already in the
/// ring plays out.
inline void stop_effect() {
    const Masked masked;
    sound_effect_left = 0;
}

/// Cut the speech, as new speech does, and leave an effect playing. What of
/// it is already in the ring plays out.
inline void hush() {
    const Masked masked;
    sound_left = 0;
    sound_loading = 0;
}

/// @p pages more speech has landed after what the refill had. Masked, as
/// the refill counts the same word down.
inline void arrived(uint8_t pages) {
    const Masked masked;
    sound_left = static_cast<uint16_t>(sound_left + pages);
}

/// The speech has all landed: pages the refill finds missing now are its end.
inline void all_arrived() {
    sound_loading = 0;
}

} // namespace sound
