// SPDX-License-Identifier: GPL-3.0-or-later

// Where everything lives in chip RAM.
//
// Said once, so no file restates it and no restatement drifts. Each region is
// a tenant with a ceiling of its own, and every ceiling is silent when broken:
// a glyph fetched from the wrong number, a sample that plays nothing, a display
// list the VIC reads past.
//
// The order is fixed and the regions are contiguous from $10000 up, so the
// figure pool is what is left rather than a number chosen, and a region that
// grows is a compile error instead of a room nobody can draw.

#pragma once

/// Where the MOD sample block lands. A macro and not just the constant below,
/// because the driver needs it at assembly time and an .S file sees only this
/// much of the file. Audio DMA's base address is 24 bits (iomap.txt:1103), so
/// it cannot reach Attic; chip RAM ends at $60000, so its top byte is zero.
#define SIMON_SAMPLES 0x20000
/// The speech ring (sound.hpp): a 4 KB boundary, so one byte of the play
/// address names the page and the ring's pages wrap with a mask.
#define SIMON_SPEECH 0x1E000

#ifndef __ASSEMBLER__

#include <stdint.h>

namespace chipmap {

/// One tenant: where it starts and how much it holds.
struct Region {
    uint32_t at = 0;
    uint32_t bytes = 0;
};

/// Whether `count` regions fit below `end` without treading on each other.
///
/// Takes the regions rather than reading the map below, so a test can hand it a
/// broken one. Empty counts as broken: a region of nothing is a tenant that was
/// forgotten, not one that needs no room.
[[nodiscard]] constexpr bool sound(const Region* regions, uint8_t count, uint32_t end) {
    uint32_t reached = 0;
    for (uint8_t i = 0; i < count; ++i) {
        if (regions[i].bytes == 0 || regions[i].at < reached)
            return false;
        reached = regions[i].at + regions[i].bytes;
        if (reached > end)
            return false;
    }
    return true;
}

/// Whether the regions leave no gap between `from` and `end`.
///
/// Sound is not enough. A gap is bytes nothing is named for, and bytes nothing
/// is named for get taken by whoever notices them first -- at which point two
/// tenants hold the same memory and neither declared it.
[[nodiscard]] constexpr bool snug(
    const Region* regions, uint8_t count, uint32_t from, uint32_t end) {
    uint32_t reached = from;
    for (uint8_t i = 0; i < count; ++i) {
        if (regions[i].at != reached)
            return false;
        reached = regions[i].at + regions[i].bytes;
    }
    return reached == end;
}

/// All of it. The audio DMA forces the top address bits to zero and drops
/// anything at or above this, so a sample above it is silence with every
/// channel register correct.
inline constexpr uint32_t CHIP_BYTES = 384UL * 1024;

/// The VIC fetches glyphs only from the first 512 KB.
inline constexpr uint32_t GLYPH_REACH = 512UL * 1024;

// ---------------------------------------------------------------- the picture

/// Nothing in the game is wider than 320, so nothing scrolls. The picture takes
/// the top of the screen and the rows below it are Simon's inventory panel.
inline constexpr uint8_t CELLS_ACROSS = 40;
inline constexpr uint8_t PICTURE_ROWS = 17;
inline constexpr uint8_t SCREEN_ROWS = 25; // the VIC fetches past the picture

inline constexpr uint16_t GLYPH_BYTES = 64;
inline constexpr uint8_t CELL_LINES = 8;
inline constexpr uint16_t PICTURE_CELLS = static_cast<uint16_t>(CELLS_ACROSS) * PICTURE_ROWS;
inline constexpr uint16_t PICTURE_BYTES = PICTURE_CELLS * GLYPH_BYTES;
inline constexpr uint16_t PICTURE_LINES = PICTURE_ROWS * CELL_LINES;

// ------------------------------------------------------------------- tenants
//
// Everything from $10000 up, in order and without a gap. The first 64 KB is the
// program: zero page, stack, Hyppo's name page, the near regions the linker
// hands out, the bank window at $2000 and I/O.

/// Hyppo's own workspace. Named and left empty because the bank converter
/// refuses to place a bank here, and an unnamed hole gets taken by whoever
/// notices it first.
inline constexpr uint32_t DOSWORK = 0x10000;
inline constexpr uint32_t DOSWORK_BYTES = 0x2000;

/// Code banks, 8 KB each. The one chip region hemmed in by the DOS area below
/// and the colour mirror above, so it is useless for bulk buffers and exactly
/// right for code slices.
inline constexpr uint32_t BANKS = DOSWORK + DOSWORK_BYTES;
inline constexpr uint32_t BANK_BYTES = 0x2000;
inline constexpr uint8_t BANK_COUNT = 6;
inline constexpr uint32_t BANKS_BYTES = BANK_BYTES * BANK_COUNT;

/// Streaming speech. Audio DMA cannot see Attic, so this has to be chip RAM;
/// inside one page, so no ring straddles a page boundary.
inline constexpr uint32_t SPEECH = BANKS + BANKS_BYTES;
inline constexpr uint32_t SPEECH_BYTES = 4096;
static_assert(SPEECH == SIMON_SPEECH && SPEECH % SPEECH_BYTES == 0,
    "the macro and the map disagree about the speech ring");

/// A spare page: DMA job lists are built on the stack, and the only sprite
/// pointer is the cursor's, inside CURSOR. Named and empty so the bytes are not
/// taken by whoever notices them first. Chip RAM is scarce -- the figure pool
/// is what is left of it -- so what needs neither the VIC nor audio DMA goes to
/// the Attic, as the sprite census does (atticmap::DIAGNOSTICS).
inline constexpr uint32_t SCRATCH = SPEECH + SPEECH_BYTES;
inline constexpr uint32_t SCRATCH_BYTES = 2048;

/// The colour-RAM mirror. Named and left empty: the VIC rewrites it, silently.
inline constexpr uint32_t SHADOW = SCRATCH + SCRATCH_BYTES;
inline constexpr uint32_t SHADOW_BYTES = 2048;

/// The MOD sample block, shared with everything else the audio DMA reads on
/// page 3. Stated at the top of this file, where the driver can also see it.
///
/// First above the colour mirror, and the anchor the rest of the map hangs
/// from: 36,864 bytes that may not cross a 64 KB boundary, which pins it to a
/// page start. Everything after it is free to be wherever the sizes put it.
inline constexpr uint32_t SAMPLES = SIMON_SAMPLES;
inline constexpr uint32_t SAMPLES_BYTES = 36864;
static_assert(
    SAMPLES == SHADOW + SHADOW_BYTES, "the macro and the map disagree about where the anchor is");

/// The boxes a click is tested against (107 ADD_BOX), unwritten. Sixteen bytes
/// a box -- x, y, width and height as words, then flags, verb, id and an item
/// -- and 192 of them, against the 250 ScummVM keeps for every AGOS game at
/// once (agos.h:581). Chip rather than near: near is where the VMs and the
/// card layer live, and a box is read once a click.
inline constexpr uint32_t HITAREAS = SAMPLES + SAMPLES_BYTES;
inline constexpr uint32_t HITAREAS_BYTES = 3072;

/// The transcoded subroutine heap: the resident block plus one TABLES file,
/// measured at 30,298 bytes peak over the whole release. Chip rather than Attic
/// because it is the hottest byte stream the VM has.
inline constexpr uint32_t SCRIPTS = HITAREAS + HITAREAS_BYTES;
inline constexpr uint32_t SCRIPTS_BYTES = 30720;

/// Unused, must not be removed: without it every region above shifts down 16 KB
/// and the display garbles for an unknown reason. All addresses and glyph
/// numbers check out. Effects stream from Attic and need none of it.
inline constexpr uint32_t SPACER = SCRIPTS + SCRIPTS_BYTES;
inline constexpr uint32_t SPACER_BYTES = 16384;

/// Two RRB display lists, so one is built while the other is shown.
///
/// ROW_CELLS * SCREEN_ROWS * 2 bytes a list, and rrb.hpp asserts it: this is
/// the number that decides how much a row may hold, and the pool above pays
/// for it.
inline constexpr uint32_t SCREEN = SPACER + SPACER_BYTES;
inline constexpr uint32_t SCREEN_BYTES = 12800;

/// Full-colour glyphs are 64 bytes at 64 times their number, so a glyph's
/// number is its address. Chip RAM, because the VIC cannot see Attic.
inline constexpr uint32_t BACKDROP = SCREEN + SCREEN_BYTES;
inline constexpr uint32_t BACKDROP_BYTES = PICTURE_BYTES;

/// The rows below the picture: the text windows write here, and Simon's
/// inventory strip will. One glyph more than the grid, which is the blank
/// every row ends with -- see BLANK_GLYPH.
inline constexpr uint32_t PANEL = BACKDROP + BACKDROP_BYTES;
inline constexpr uint16_t PANEL_CELLS =
    static_cast<uint16_t>(CELLS_ACROSS) * (SCREEN_ROWS - PICTURE_ROWS);
inline constexpr uint32_t PANEL_BYTES = (static_cast<uint32_t>(PANEL_CELLS) + 1) * GLYPH_BYTES;

/// The mouse pointer, the one hardware sprite. Last in the map, so it is the
/// ceiling the figure pool grows up to.
inline constexpr uint32_t CURSOR_BYTES = 256;
inline constexpr uint32_t CURSOR = CHIP_BYTES - CURSOR_BYTES;

/// The figures on screen now, refilled as animation frames change.
///
/// Speech text is a figure: cells of glyphs placed and layered like any other,
/// keyed by zone zero (not a real zone), so it needs no region of its own.
///
/// Not decoded per zone: one zone holds 66,304 bytes median, 814,080 worst.
/// Sized by the fifteen largest live at once: 18,432 median, 52,864 worst
/// (zone 6, full-colour cells). Figures are four-bit cells, widths multiples
/// of 16, so each is exactly half: 9,216 and 26,432.
inline constexpr uint32_t FIGURES = PANEL + PANEL_BYTES;
inline constexpr uint32_t FIGURES_BYTES = CURSOR - FIGURES;

/// The item records of gameamiga, which measure 7,522 bytes. Near, because
/// the game writes item state back into them and they are walked at random;
/// the subroutine block behind them stays where the file landed. A map fact,
/// though the linker reserves it. The strings live far (atticmap::GAMETEXT);
/// near holds only GameDb's index of offsets into that block.
inline constexpr uint16_t DATABASE_BYTES = 7680;
static_assert(DATABASE_BYTES >= 7522, "gameamiga's item records do not fit");

/// Colour RAM has memory of its own; this is not chip RAM and not in the map.
inline constexpr uint32_t COLOUR = 0xFF80000;
inline constexpr uint32_t COLOUR_BYTES = 32768;

/// Glyph 0 is unusable: its pixels are the zero page.
inline constexpr uint16_t FIRST_GLYPH = BACKDROP / GLYPH_BYTES;

// Nothing may be numbered under 256: FCLRLO is left clear so that a GOTOX
// costs eight raster cycles rather than sixteen, and glyphs below $100 are
// one bit per pixel while it is (viciv.vhdl:4351-4355).
static_assert(FIRST_GLYPH > 0xFF, "a glyph under $100 would not be full colour");

/// The panel's first glyph, for the rows below the picture.
inline constexpr uint16_t PANEL_GLYPH = PANEL / GLYPH_BYTES;

/// The blank every row ends with, left at colour nought: in full colour a
/// pixel of nought is transparent and shows $D021.
///
/// One past the panel's grid, not one past the picture: that glyph is the
/// panel's first cell, so a window writing at its top left would show the
/// character down the right edge of all twenty-five rows. Nothing writes this
/// one.
inline constexpr uint16_t BLANK_GLYPH = static_cast<uint16_t>(PANEL_GLYPH + PANEL_CELLS);

/// Where the map starts naming things: below this is the program.
inline constexpr uint32_t ARENA = DOSWORK;

inline constexpr Region MAP[] = {
    {DOSWORK, DOSWORK_BYTES},
    {BANKS, BANKS_BYTES},
    {SPEECH, SPEECH_BYTES},
    {SCRATCH, SCRATCH_BYTES},
    {SHADOW, SHADOW_BYTES},
    {SAMPLES, SAMPLES_BYTES},
    {HITAREAS, HITAREAS_BYTES},
    {SCRIPTS, SCRIPTS_BYTES},
    {SPACER, SPACER_BYTES},
    {SCREEN, SCREEN_BYTES},
    {BACKDROP, BACKDROP_BYTES},
    {PANEL, PANEL_BYTES},
    {FIGURES, FIGURES_BYTES},
    {CURSOR, CURSOR_BYTES},
};
inline constexpr uint8_t REGIONS = sizeof MAP / sizeof MAP[0];

static_assert(sound(MAP, REGIONS, CHIP_BYTES), "two tenants hold the same bytes");
static_assert(snug(MAP, REGIONS, ARENA, CHIP_BYTES), "a byte nothing is named for");

// Pinned by the hardware rather than derived, so they cannot slide with a
// region above them.
static_assert(SHADOW == 0x1F800 && SHADOW_BYTES == 2048, "the colour mirror moved");
static_assert(DOSWORK == 0x10000 && DOSWORK_BYTES == 0x2000, "Hyppo's workspace moved");

// Audio DMA forces the top address bits to zero and drops anything at or above
// the chip RAM size, so a buffer above this is silence with every register set.
static_assert(SAMPLES + SAMPLES_BYTES <= CHIP_BYTES, "samples out of the DMA's reach");
static_assert(SPEECH + SPEECH_BYTES <= CHIP_BYTES, "speech out of the DMA's reach");
static_assert(SAMPLES / 0x10000 == (SAMPLES + SAMPLES_BYTES - 1) / 0x10000,
    "the sample block straddles a 64 KB page");
static_assert(SPEECH / 0x10000 == (SPEECH + SPEECH_BYTES - 1) / 0x10000,
    "the speech ring straddles a 64 KB page");

// Glyphs: a number is an address, and the VIC fetches them only from the first
// 512 KB.
static_assert(BACKDROP % GLYPH_BYTES == 0 && PANEL % GLYPH_BYTES == 0 && FIGURES % GLYPH_BYTES == 0,
    "a glyph's number is its address");
static_assert(
    FIGURES + FIGURES_BYTES <= GLYPH_REACH, "the VIC fetches glyphs only from the first 512 KB");

// What the data needs, measured over the whole release.
static_assert(HITAREAS_BYTES >= 100 * 16, "too few boxes to play a room");
static_assert(SCRIPTS_BYTES >= 30298, "the subroutine heap peak does not fit");
static_assert(FIGURES_BYTES >= 26432, "zone 6's fifteen largest figures do not fit");

} // namespace chipmap

using namespace chipmap;

#endif // __ASSEMBLER__
