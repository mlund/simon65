// What a run leaves for a monitor to read over serial, so it can be judged without a
// screen: counters of what the interpreter asked for and could not have, a ring of
// what wrote each arena row, and a snapshot of a frame's sprite list.

#pragma once

#include "atticmap.hpp"
#include "far.hpp"
#include "vga_vm.hpp"

#include <stdint.h>

// --- counters -----------------------------------------------------------------------------

namespace report {

/// Read back at the address the link map gives, never at one written here.
///
/// ZONES_ASKED is one bit per zone the script asked to be loaded. A zone that
/// was never asked for is a scene whose setup never ran, which is a different
/// fault from a zone that was asked for and did not arrive.
///
/// SYNC_LOST holds the first four idents a wait gave up on. The count says
/// how many waits timed out; which ones names the scene that never signalled,
/// and a scene that never signalled is one the script then runs straight past.
///
/// EXTRA_BANK is what the Attic bank answered when asked: a bank that did not
/// load runs whatever bytes are at that address, so an answer only it knows is
/// the only proof it is there.
///
/// VGA_SEEN and SCRIPT_SEEN are one bit per opcode a run met and did not
/// implement -- four words of video opcodes and twelve of script. The counts
/// say how often and VGA_LAST which was last; neither names the set, and the
/// set is what decides what to write next.
enum Slot : uint8_t {
    MAGIC,
    SCRIPT_MISSING,
    VGA_MISSING,
    FAULTS,
    FAULT_VALUE,
    ITEMS_MOVED,
    TICKS,
    TABLES,
    ZONES,
    SPRITES,
    DRAWN,
    DECODE_FRAMES,
    PAINTED,
    PALETTES,
    LAST_BLOCK,
    LAYERS,
    FRAME_RASTERS,
    RESIDENT,
    SKIPPED_ZONE,
    ZONES_DECODED,
    TOO_WIDE,
    CROWDED,
    WORST_FRAME,
    SAID,
    SAID_LEN,
    SAID_STRING,
    SHOWN,
    SHOWN_LEN,
    SHOWN_WINDOW,
    TIMEOUTS_RUN,
    TIMEOUTS_HELD,
    BOXES,
    BOXES_SPILLED,
    MOUSE_OFF,
    CLICKED,
    CLICKED_BOX,
    CLICKED_VERB,
    CLICKS_RUN,
    PICTURES,
    SYNC_GAVE_UP,
    SYNC_WANTED,
    SYNC_SENT,
    TUNE_MISSING,
    TUNE_WANTED,
    VGA_LAST,
    FAULT_KIND,
    ZONES_FORCED,
    IN_SUB,
    STOPPED_ON,
    TICK_RASTERS,
    WORST_TICK,
    WORST_DECODE,
    ZONES_HELD,
    ZONES_EMPTIED,
    PAINT_MISSED,
    TOO_BIG,
    UNDECODED,
    DECODED,
    HINT_MISS,
    WORST_LOAD,
    LOADS,
    STACK_GUARD,
    ROW_PEAK,
    LAYERS_DROPPED,
    CLOSES_FAILED,
    EXTRA_BANK,
    SYNC_LOST,
    VGA_SEEN = SYNC_LOST + 4,
    SCRIPT_SEEN = VGA_SEEN + 4,
    ZONES_ASKED = SCRIPT_SEEN + 12,
    LOST_ID = ZONES_ASKED + 16,
    LOST_ZONE,
    LOST_PC_LO,
    LOST_PC_HI,
    // Appended, never inserted: a slot's number is what a readback shows.
    DECODES_GIVEN_UP = LOST_PC_HI + 1,
    /// What the animation VM asks the screen for, against what it gets.
    ///
    /// CELS_WANTED counts a sprite presenting a cel it was not drawn with last
    /// tick -- the animation's own demand, set by the sprite scripts and the
    /// tick rate, and nothing to do with how fast a figure decodes. CELS_HELD
    /// counts the ones answered with the pose it held instead. Against DECODED
    /// and TICKS these say whether the decoder is behind the demand or the
    /// demand itself is slow, which nothing here could tell apart before.
    CELS_WANTED,
    CELS_HELD,
    /// The frame's layer count at its worst, and how many frames ran out of
    /// layers before they ran out of sprites. LAYERS alone is whatever the last
    /// frame happened to hold, which says nothing about a busy room going past
    /// the cap; LAYERS_CAPPED above nought is a figure somewhere not drawn.
    LAYERS_PEAK,
    LAYERS_CAPPED,
    /// Stacked sprites merged into one layer: the most in one frame, and how
    /// many merges were painted rather than found unchanged.
    COMPOSITES_PEAK,
    COMPOSITE_BUILDS,
    /// Figure-decode pieces stepped, running.
    DECODE_PIECES,
    /// Slices' length in physical raster lines, 32 us apiece, running: wraps
    /// counted piece by piece, 624 lines a frame (pixel_driver.vhdl:580).
    SLICE_LINES,
    /// Raster lines spent merging stacked sprites, running, counted the same way;
    /// how many times it ran, and how many of those replayed the frame before.
    COMPOSITE_LINES,
    COMPOSITE_CALLS,
    COMPOSITE_REPLAYS,
    /// Sprites a frame could not draw at all: no figure yet and no pose of
    /// theirs left to hold. Each is a blink.
    NOT_DRAWN,
    /// Raster lines in the animation VM's ticks, and in the draw with the decode
    /// it finishes, both running: with SLICE_LINES and COMPOSITE_LINES, the frame.
    TICK_LINES,
    DRAW_LINES,
    /// The draw split further, in raster lines, running: the sprite loop (with
    /// the decode slices want() runs), the row rebuild and how many rows it
    /// built, the decode finished after the draw, and the main loop's script
    /// work. And frames past the catch-up limit, each a frame of game time
    /// lost.
    SPRITE_LINES,
    ROWS_LINES,
    ROWS_BUILT,
    AFTER_DRAW_LINES,
    SCRIPT_LINES,
    LOST_FRAMES,
    /// Figures decoded ahead of being asked for, in idle time; copy-ins of
    /// them, re-copies included; and guesses dropped for a cel asked for.
    AHEAD_DECODED,
    AHEAD_USED,
    AHEAD_ABANDONED,
    /// Draws that kept the last frame while a cel was decoded, and holds that
    /// reached the cap and drew the frame without it.
    HELD_DRAWS,
    HOLDS_GIVEN_UP,
    /// Draws that found the last list not yet on screen and waited for it.
    SWAP_WAITS,
    /// Masks laid as a cut of the picture, running; and masks of priority 49,
    /// which only see Simon's colours through and are not drawn.
    MASKS_LAID,
    MASKS_SEE_THROUGH,
    /// The line of speech in force when a wait first gave up: its speaker and
    /// its voice, which say why nothing sent the sync it waited for.
    LOST_SAY_WHICH,
    LOST_SAY_SPEECH,
    /// Speech: SPEECH.BIN's runs, the voice last spoken, the frames until it
    /// was all off the card, and the card time its reading took a turn.
    SPEECH_RUNS,
    VOICE_SPOKEN,
    VOICE_LOAD_FRAMES,
    PUMP_LINES,
    WORST_PUMP,
    /// Times the Attic figure arena came round (figures.hpp).
    ARENA_WRAPS,
    SLOTS
};

/// Set once the run has got as far as it is going to get.
inline constexpr uint16_t RUNNING = 0xB0FF;

extern volatile uint16_t counts[SLOTS];

} // namespace report

// --- arena row trace ----------------------------------------------------------------------
//
// A ring of what wrote each arena row, for reading back later.
//
// A row goes wrong while a zone is live and nothing is watching -- it ends up
// describing an image it does not hold -- and by the time a check notices,
// whatever did it is several rooms in the past. Sampling cannot catch it --
// a monitor read is a second and a decode is a frame -- so every write records
// itself here instead and the ring is read whole afterwards.
//
// It sits at the top of the diagnostics region, above the sprite census.

namespace trace {

/// zone, image and page (two bytes each, little-endian), cells, rows, spare.
/// Eight, so a record's place in the ring is a shift rather than a multiply.
inline constexpr uint8_t RECORD = 8;

/// A power of two, so the wrap is a mask rather than a division: a modulo by
/// anything else is a call on this target. Thirty-two of them is what fits
/// above the sprite census in the same region.
inline constexpr uint16_t RECORDS = 32;
inline constexpr uint16_t COUNT_BYTES = 2;
inline constexpr uint16_t RING_BYTES = COUNT_BYTES + RECORDS * RECORD;

inline constexpr agos::Place AT = atticmap::DIAGNOSTICS + atticmap::DIAGNOSTICS_BYTES - RING_BYTES;

// The census below checks the two do not overlap: it knows its own size, and it
// is compiled for the target only.

#ifdef __mos__
/// One entry as the arena records it, taking the row the caller has already
/// packed: the ring's record and the index's row are the same eight bytes, and
/// packing them twice cost the fixed region -- which is the region with least
/// to spare -- a second copy of the shifts and stores.
///
/// Out of line and in the fixed region on purpose: the decoder that calls it
/// is in a bank with little to spare.
void log_entry(const uint8_t (&row)[RECORD]);
#else
inline void log_entry(const uint8_t (&)[RECORD]) {
}
#endif

} // namespace trace

// --- sprite census ------------------------------------------------------------------------
//
// A frame's sprite list, and what became of every entry in it.
//
// The list says what is alive; only the draw knows what was dropped, and a
// dropped sprite looks on screen exactly like one the scripts never created.
//
// Taken on request rather than every frame: a monitor read of the whole thing
// is a second long and a frame is twenty milliseconds, so a snapshot written
// as it is read is several frames spliced together -- which showed up at once
// as a list of sprites that were drawn and outcomes that said they were never
// reached. The reader arms it and waits; the frame that answers writes the
// list, then clears the arm and leaves it alone until asked again. It also
// means the draw pays nothing for this between reads.

// Target only: a sprite is fifteen bytes there and sixteen on the host.
#ifdef __mos__

namespace census {

/// What became of a sprite this frame. UNSEEN is what the outcomes are filled
/// with, so a sprite the draw never reached -- the display list filled first
/// -- reports itself without costing a write.
enum Outcome : uint8_t { UNSEEN, DRAWN, NO_IMAGE, ZONE_ABSENT, NO_FIGURE };

/// Written with the rest of the header, once the snapshot is whole.
inline constexpr uint16_t MAGIC = 0x5350;

/// The header: the arm, then what the frame as a whole did. These are the one
/// statement of it -- end() below writes by these names, and the reader takes
/// them off this enum.
enum : uint8_t {
    ARMED_AT = 0,
    ZONE_WANTED_AT = 1,
    MAGIC_AT = 2, // two bytes
    TICK_AT = 4,  // two bytes
    COUNT_AT = 6,
    LAYERS_AT = 7
};

inline constexpr uint16_t HEADER_BYTES = 8;
inline constexpr uint16_t OUTCOMES_AT = HEADER_BYTES;
inline constexpr uint16_t SPRITES_AT = OUTCOMES_AT + agos::MAX_SPRITES;
inline constexpr uint16_t SPRITE_BYTES = sizeof(agos::VgaSprite);

/// The reader decodes the sprites by this stride, so a field added to the
/// struct must fail here rather than silently shift every record.
static_assert(SPRITE_BYTES == 15, "a sprite is no longer fifteen bytes");
static_assert(atticmap::DIAGNOSTICS + SPRITES_AT + agos::MAX_SPRITES * SPRITE_BYTES <= trace::AT,
    "the census and the arena-row ring overlap");

/// Whether this frame is being watched, asked once a frame and remembered
/// here rather than carried through every call below: the arm is a far read,
/// and a byte of near memory is cheaper than passing it five times.
inline bool armed = false;

/// Whether anyone is waiting for a snapshot, and every outcome back to UNSEEN
/// if so. One far read a frame, which is what the census costs when nobody is.
[[gnu::always_inline]] inline void begin() {
    armed = agos::far_read8(atticmap::DIAGNOSTICS + ARMED_AT) != 0;
    if (armed)
        agos::far_fill(atticmap::DIAGNOSTICS + OUTCOMES_AT, UNSEEN, agos::MAX_SPRITES);
}

/// What became of one sprite. A byte apiece rather than gathered near first:
/// there is no near memory left to gather into, and a frame examines a few
/// dozen sprites, not a few thousand bytes.
[[gnu::always_inline]] inline void note(uint8_t at, Outcome why) {
    if (armed)
        agos::far_write8(atticmap::DIAGNOSTICS + OUTCOMES_AT + at, static_cast<uint8_t>(why));
}

/// The list itself, in one DMA, and then the header -- which disarms, so what
/// the reader goes on to read stands still.
[[gnu::always_inline]] inline void end(const agos::VgaSprite* sprites,
    uint8_t count,
    uint16_t tick,
    uint8_t layers,
    uint8_t zone_wanted) {
    if (!armed)
        return;
    agos::far_write(atticmap::DIAGNOSTICS + SPRITES_AT,
        reinterpret_cast<const uint8_t*>(sprites),
        static_cast<uint16_t>(count * SPRITE_BYTES));
    uint8_t header[HEADER_BYTES] = {}; // ARMED_AT stays nought: it disarms
    header[ZONE_WANTED_AT] = zone_wanted;
    header[MAGIC_AT] = static_cast<uint8_t>(MAGIC);
    header[MAGIC_AT + 1] = static_cast<uint8_t>(MAGIC >> 8);
    header[TICK_AT] = static_cast<uint8_t>(tick);
    header[TICK_AT + 1] = static_cast<uint8_t>(tick >> 8);
    header[COUNT_AT] = count;
    header[LAYERS_AT] = layers;
    agos::far_write(atticmap::DIAGNOSTICS, header, HEADER_BYTES);
}

} // namespace census

#endif // __mos__
