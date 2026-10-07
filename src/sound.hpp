// The game's audio: tunes staged for the MOD driver on channels 0-2, and speech and
// effects on channel 3.
//
// --- speech and effects ---
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

#include "atticmap.hpp"
#include "banks.hpp"
#include "chipmap.hpp"
#include "far.hpp"
#include "game_store.hpp"

// mapper.h declares banked_call.
#include <mapper.h>
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

// --- tunes --------------------------------------------------------------------------------
//
// Moving tunes: card to an Attic slot when first asked for, then its samples
// to chip RAM. Also the only place that knows how to reach the MOD driver.

extern "C" {
void pep_init();
void pep_play();
void pep_stop();
uint8_t pep_bp_works();

/// The sample block's home in chip RAM, as the linker placed it. A 28-bit
/// address does not fit this target's 16-bit pointer, so it travels as bytes.

/// The driver's 32-bit module address, four contiguous bytes.
extern uint8_t pep_mod_addr[4];
}

namespace {
namespace tune_detail {

// The wrappers stay in the fixed region and jump into the body, so the bank
// only has to be mapped across the call.
#ifdef PEP_ATTIC_BANK
#define PEP_CALL(fn) banked_call(PEP_BANK, fn)
#define PEP_CALL_R(fn) banked_call_r(PEP_BANK, fn)
#else
#define PEP_CALL(fn) fn()
#define PEP_CALL_R(fn) fn()
#endif

/// What pep_bp_works stores and reads back through the driver's own base page.
/// If this does not come back, the base page is not where the driver thinks and
/// nothing it does afterwards means anything.
constexpr uint8_t BASE_PAGE_PROBE = 0x5a;

/// Where a module states what it is, and the three forms the driver plays.
/// Hyppo reports only whether a load started, so this is what says the bytes
/// arrived: a dead HyperRAM loads successfully and stores nothing.
constexpr uint16_t MOD_MAGIC = 1080;
constexpr char MOD_MAGICS[3][5] = {"M.K.", "M!K!", "FLT4"};

[[nodiscard]] inline bool module_landed(uint32_t base) {
    for (const auto& magic : MOD_MAGICS) {
        uint8_t same = 0;
        while (same < 4 &&
            agos::far_read8(base + MOD_MAGIC + same) == static_cast<uint8_t>(magic[same]))
            ++same;
        if (same == 4)
            return true;
    }
    return false;
}

} // namespace tune_detail
} // namespace

using namespace tune_detail;

/// Whether the driver answers. The tunes come later, each when the game
/// first asks for it.
[[nodiscard]] static bool tune_store_works() {
    return PEP_CALL_R(pep_bp_works) == BASE_PAGE_PROBE;
}

/// Every audio-DMA channel off, and the master switch with them.
///
/// A reset does not clear these: a program that left a channel running leaves
/// it running, and the next one hears it as noise the moment audio DMA is
/// enabled. Four channels of sixteen bytes from $D720 (modplay.S:25-31), and
/// the flags byte is where the enable is.
static void tune_store_silence() {
    auto* const enable = reinterpret_cast<volatile uint8_t*>(0xD711);
    *enable = 0;
    auto* const channel = reinterpret_cast<volatile uint8_t*>(0xD720);
    for (uint8_t at = 0; at < 4 * 0x10; at += 0x10) {
        channel[at] = 0;     // CHXFLAGS: enable, loop and the rest
        channel[at + 9] = 0; // CHXVOLUME
    }
}

/// Whether a module has been loaded. Nothing plays until the script says so
/// (127 PLAY_TUNE), and the driver must not be ticked over a module that was
/// never read: pep_play would walk whatever Attic holds.
inline bool tune_loaded = false;

/// The module format: big-endian offset and length in the first four bytes,
/// which the driver does not read. Samples are stored separately.
constexpr uint8_t SAMPLES_AT_BYTES = 4;

/// Play the module at @p base.
static void tune_store_select(uint32_t base) {
    PEP_CALL(pep_stop);

    // Header and patterns are read in place from Attic; only the samples have to
    // come down, because audio DMA's base address is 24 bits (iomap.txt:1103).
    uint8_t at[SAMPLES_AT_BYTES];
    agos::far_read(base, at, SAMPLES_AT_BYTES);
    copy(base + agos::be16(at), SAMPLES, agos::be16(at + 2));

    pep_mod_addr[0] = base & 0xff;
    pep_mod_addr[1] = (base >> 8) & 0xff;
    pep_mod_addr[2] = (base >> 16) & 0xff;
    pep_mod_addr[3] = (base >> 24) & 0xff;

    PEP_CALL(pep_init);
    tune_loaded = true;
}

/// One driver tick, from the raster interrupt.
static void tune_store_tick() {
    if (tune_loaded)
        PEP_CALL(pep_play);
}
