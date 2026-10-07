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

#pragma once

#include "atticmap.hpp"
#include "far.hpp"
#include "trace.hpp"
#include "vga_vm.hpp"

#include <stdint.h>

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
