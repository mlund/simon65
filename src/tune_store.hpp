// Moving tunes: card to an Attic slot when first asked for, then its samples
// to chip RAM. Also the only place that knows how to reach the MOD driver.

#pragma once

#include "atticmap.hpp"
#include "far.hpp"
#include "game_store.hpp"
#include "move.hpp"

// mapper.h declares banked_call.
#include <mapper.h>
#include <mega65.h>

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
