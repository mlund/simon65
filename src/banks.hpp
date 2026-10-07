// The code banks, checked against the region the map set aside for them.
//
// The platform's defaults are a 24 KB stride, which puts bank 3 at $20000 and
// bank 8 at $40000 -- on top of the load area and on top of the glyphs. CMake
// bases them by hand on an 8 KB stride instead, and these say so, so that a
// bank the map has no room for is a link-time failure rather than a backdrop
// that comes out wrong for no visible reason.

#pragma once

#include "atticmap.hpp"
#include "banknumbers.hpp"
#include "chipmap.hpp"

#include <mapper.h>

/// Never true, and the branch it guards never runs.
///
/// Holds the call graph so the compiler sees nested banked calls. banked_call
/// is assembly, hiding the re-entry from the optimiser: without this, static
/// stack frames overlap and nested calls corrupt each other's locals. The SDK's
/// callback(2) attribute should cover this (mega65-common/include/_mapper.h:
/// 83-86) but does not.
inline volatile bool recursion_is_real = false;

/// banked_call, for a door that can be re-entered through the bank it calls.
///
/// A template rather than a pointer argument so the never-taken branch is a
/// direct call to @p Fn, which is the edge; taking it as a parameter would
/// leave the graph exactly as opaque as before.
template <void (*Fn)()> inline void banked_reenter(uint8_t bank) {
    if (recursion_is_real)
        Fn();
    banked_call(bank, Fn);
}

/// A function pinned into a bank. Empty off the machine, so one definition
/// serves the host with no call-site change.
///
/// CODE_BANK banks only what carries the attribute: anything it calls that the
/// optimiser declines to inline lands back in the fixed region, which is the
/// trap these are here to avoid. Hence the attribute on the worker rather than
/// on the door alone.
#define STORE_BANKED CODE_BANK(AGOS_STORE_BANK)
#define CARD_BANKED CODE_BANK(AGOS_CARD_BANK)
#define ROOM_BANKED CODE_BANK(AGOS_ROOM_BANK)
#define DISPLAY_BANKED CODE_BANK(AGOS_DISPLAY_BANK)
#define COMPOSITE_BANKED CODE_BANK(AGOS_COMPOSITE_BANK)
#define VERB_BANKED CODE_BANK(AGOS_VERB_BANK)
#define SAVE_BANKED CODE_BANK(AGOS_SAVE_BANK)
#define SOUND_BANKED CODE_BANK(AGOS_SOUND_BANK)

/// Data that lives in the compositor's bank: reachable only while it is
/// mapped, and costing no near memory. Zeros in the bank image, written at
/// run time -- the bank is RAM.
#define COMPOSITE_DATA __attribute__((section(_BANK_SECTION(AGOS_COMPOSITE_BANK) ".scratch")))

/// The same for the Attic bank the verb bar runs in: its state, which the
/// rule puts in Attic, and its names, which would otherwise cost the fixed
/// region. Constants apart, since a section is either written or not.
#define VERB_DATA __attribute__((section(_BANK_SECTION(AGOS_VERB_BANK) ".scratch")))
#define VERB_CONST __attribute__((section(_BANK_SECTION(AGOS_VERB_BANK) ".names")))

/// And for the Attic bank of the world's turn: the inventory's icon in the
/// unpacking (inventory.hpp).
#define TICK_BANKED CODE_BANK(AGOS_TICK_BANK)
#define TICK_DATA __attribute__((section(_BANK_SECTION(AGOS_TICK_BANK) ".scratch")))

/// And for the Attic bank of scene changes: the masks' keys (main.cpp).
#define EXTRA_DATA __attribute__((section(_BANK_SECTION(AGOS_EXTRA_BANK) ".scratch")))

/// And for the save bank: the image, worked on with a pointer, which near
/// memory has no 3.5 KB for.
#define SAVE_DATA __attribute__((section(_BANK_SECTION(AGOS_SAVE_BANK) ".scratch")))

/// Hot code in chip, cold code in the Attic. Code runs from an Attic bank about
/// six times slower: the decoder's piece took 4.51 ms there and 0.70 ms from
/// chip, the same build with two bank numbers swapped, timed in raster lines.
/// So the Attic is for what runs rarely -- loading, resets, scene changes --
/// or does little; and the display bank, which composites the raster-rewrite
/// row against the beam, is chip whatever else moves.
///
/// An Attic bank is based at $8000000 (CMakeLists.txt), so the address says
/// which kind it is.
#define BANK_BASE(n) MAPPER_BANK_##n
#define BANK_BASE_OF(n) BANK_BASE(n)
static_assert(BANK_BASE_OF(AGOS_DISPLAY_BANK) < 0x8000000UL,
    "the display bank composites the RRB row and must be chip RAM");

/// Where DMA finds a buffer in the window while the bank based at @p base is
/// mapped: the bank's home, not the window the CPU sees it through. Bank 0
/// is the window's own RAM, so its base is where the window starts.
[[nodiscard, gnu::always_inline]] inline uint32_t in_bank(uint32_t base, const void* p) {
    return base + (reinterpret_cast<uint16_t>(p) - BANK_PHYS_BASE_0);
}

namespace bankmap {

[[nodiscard]] constexpr bool within(uint32_t base, uint32_t size, uint32_t from, uint32_t bytes) {
    return base >= from && base + size <= from + bytes;
}

/// Whether a bank sits in a region some map set aside for banks.
///
/// Both maps, because a bank may be in either: chip RAM is the fast one and
/// the Attic costs no chip RAM at all. Naming the two regions rather than the
/// two bank numbers is what lets the assertions below be a list of banks
/// instead of a list of special cases -- the previous form let any address
/// outside chip RAM pass, so an Attic bank landing on the zone pixels was
/// legal by construction.
[[nodiscard]] constexpr bool placed(uint32_t base, uint32_t size) {
    return within(base, size, chipmap::BANKS, chipmap::BANKS_BYTES) ||
        within(base, size, atticmap::BANKS, atticmap::BANKS_BYTES);
}

static_assert(
    BANK_SIZE_1 == BANK_BYTES, "the window size and the map disagree about how big a bank is");

/// Every bank the program declares, whichever map it lives in.
#define SIMON_BANK_PLACED(n)                                                                       \
    static_assert(placed(BANK_PHYS_BASE_##n, BANK_SIZE_##n),                                       \
        "bank " #n " is outside both maps' bank regions")

#if MAPPER_BANK_COUNT >= 1
SIMON_BANK_PLACED(1);
#endif
#if MAPPER_BANK_COUNT >= 2
SIMON_BANK_PLACED(2);
#endif
#if MAPPER_BANK_COUNT >= 3
SIMON_BANK_PLACED(3);
#endif
#if MAPPER_BANK_COUNT >= 4
SIMON_BANK_PLACED(4);
#endif
#if MAPPER_BANK_COUNT >= 5
SIMON_BANK_PLACED(5);
#endif
#if MAPPER_BANK_COUNT >= 6
SIMON_BANK_PLACED(6);
#endif
#if MAPPER_BANK_COUNT >= 7
SIMON_BANK_PLACED(7);
#endif
#if MAPPER_BANK_COUNT >= 8
SIMON_BANK_PLACED(8);
#endif
#if MAPPER_BANK_COUNT >= 9
SIMON_BANK_PLACED(9);
#endif
#if MAPPER_BANK_COUNT >= 10
SIMON_BANK_PLACED(10);
#endif
#if MAPPER_BANK_COUNT >= 11
SIMON_BANK_PLACED(11);
#endif
#if MAPPER_BANK_COUNT >= 12
SIMON_BANK_PLACED(12);
#endif
// A ceiling of the expansion above and not of the design: raising it is adding
// lines here, which is why it may say so.
#if defined(MAPPER_BANK_COUNT)
static_assert(MAPPER_BANK_COUNT <= 12, "more banks than this header expands");
static_assert(MAPPER_BANK_COUNT >= chipmap::BANK_COUNT,
    "the chip map reserves room for banks the program never declares");
#endif

} // namespace bankmap
