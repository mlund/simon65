// The VIC-IV in full colour mode, showing one room.

#pragma once

#include "chipmap.hpp"
#include "figures.hpp"
#include "placed.hpp"
#include "rrb.hpp"

#include <mega65.h>

namespace {
namespace display_detail {

/// The row under construction. In ram_low because ram_fixed is where the code
/// lives and has no 488 bytes to spare, and because this is working memory the
/// image never carries -- rows are rebuilt once an actor moves.
[[gnu::section(".vmstate"), gnu::used]] uint8_t screen_row[rrb::ROW_BYTES];
[[gnu::section(".vmstate"), gnu::used]] uint8_t colour_row[rrb::ROW_BYTES];

constexpr uint8_t DEN = 0x10; // $D011 bit 4; the SDK names no mask

/// $D011 bit 7 reads as the live raster's bit 8 but writes as the raster
/// compare's. Read-modify-write it while the beam is past line 255 and the
/// compare becomes line+256, which a 312-line raster never reaches -- the
/// music's interrupt then stops for good. Never write this bit back.
constexpr uint8_t RASTER_MSB = 0x80;
constexpr uint8_t RSEL_YSCROLL3 = 0x0B;
constexpr uint8_t CSEL = 0x08;

constexpr uint16_t CELLS = uint16_t(CELLS_ACROSS) * SCREEN_ROWS;

volatile uint8_t* const D051 = reinterpret_cast<volatile uint8_t*>(0xD051);
/// $D065, COLPTRMSB (iomap.txt): the overlay has only the 16-bit COLPTR.
volatile uint8_t* const COLPTR_MSB = reinterpret_cast<volatile uint8_t*>(0xD065);

/// The physical raster's top three bits, in $D053.0-2 and in its compare's
/// $D07A.0-2 (iomap.txt:220, :286).
constexpr uint8_t RASTER_MSB_BITS = 0x07;
/// $D07A's SPTRCONT, CHARY16 and errata bit, kept as read: bit 5 reads back
/// as the state it sets, so writing it back changes nothing
/// (viciv.vhdl:2015, :2944-2950). Bit 7, FNRSTCMP, is left clear: the compare
/// is in physical rasters.
constexpr uint8_t RASCMP_KEEP = 0x38;

} // namespace display_detail
} // namespace

using namespace display_detail;

/// How many figures a frame may lay. Past this the frame stops reading the
/// sprite list, so whichever sprites sort last -- the front-most -- are not
/// drawn; LAYERS_CAPPED counts those frames.
///
/// Twenty-four, all the pool can hold at once (TENANTS). Measured over the
/// intro on the machine with four-bit figures: the pot room has 40 sprites
/// live, lays all 24 and is still capped in 4 frames, its widest row is 126
/// cells of 128, and no row refused a layer. So the cap and the row bind
/// together here, and raising one only trades LAYERS_CAPPED for
/// LAYERS_DROPPED. Beyond this, layers must get cheaper, not more numerous.
inline constexpr uint8_t LAYERS_MAX = 24;
static_assert(LAYERS_MAX <= agos::TENANTS, "a frame must be able to place every layer it draws");

/// What a row cost at its worst, and how many layers would not fit.
///
/// The peak is what the row consumes, close included, so it is the number
/// ROW_CELLS has to cover. The other counts *layers* and not rows -- one
/// crowded row can refuse several -- and neither is reset per frame: both are
/// the worst a run reached, which is what sizing wants.
inline uint8_t row_peak = 0;
inline uint16_t layers_dropped = 0;
inline uint16_t closes_failed = 0;

/// Where row @p row of display list @p list starts, in SCREEN and in COLOUR
/// alike.
[[nodiscard]] inline uint16_t row_at(uint8_t list, uint8_t row) {
    return static_cast<uint16_t>(
        (list != 0 ? rrb::LIST_BYTES : 0) + uint16_t{row} * rrb::ROW_BYTES);
}

/// Write every row's prefix -- its opening token and backdrop cells -- into
/// both display lists, where it stays. Inline, for the door in the Attic bank
/// (main.cpp): it runs once, at start.
[[gnu::always_inline]] inline void display_prefixes() {
    for (uint8_t row = 0; row < SCREEN_ROWS; ++row) {
        rrb::RowList cells(screen_row, colour_row);
        (void)cells.at(0, 0, false);
        if (row < PICTURE_ROWS)
            (void)cells.run(uint16_t(FIRST_GLYPH + uint16_t(row) * CELLS_ACROSS), CELLS_ACROSS);
        else
            // Below the picture is the panel, which the text windows write into: a
            // glyph of nought paints nothing, so an empty panel shows the border as a
            // blank row did.
            (void)cells.run(
                uint16_t(PANEL_GLYPH + uint16_t(row - PICTURE_ROWS) * CELLS_ACROSS), CELLS_ACROSS);
        for (uint8_t list = 0; list < rrb::LISTS; ++list) {
            agos::far_write(SCREEN + row_at(list, row), screen_row, rrb::PREFIX_CELLS * 2);
            agos::far_write(COLOUR + row_at(list, row), colour_row, rrb::PREFIX_CELLS * 2);
        }
    }
}

/// Build one screen row of display list @p list after its prefix and move it
/// out: every figure that reaches this row, then the close.
///
/// Figures are laid in the order given, which is the sprite list's order, and
/// that is priority order -- so a later one covers an earlier one, which is
/// what the engine's own back-to-front walk does.
///
/// In the bank both its callers are in. CODE_BANK banks only what carries the
/// attribute, so this had landed back in the fixed region -- a kilobyte of it,
/// for a function draw_frame_banked and display_begin_banked are the only two
/// ways into. A chip bank runs at full speed; only the Attic one is slower.
///
/// Only what follows the prefix is built, and the park tokens after the close
/// go out as DMA fills: the backdrop cells never change between frames, and
/// the tail is one value repeated, which a fill moves faster than a loop.
DISPLAY_BANKED static void display_row_over(
    uint8_t list, uint8_t row, const Placed* over, uint8_t count) {
    rrb::RowList cells(screen_row, colour_row, rrb::PREFIX_CELLS);
    for (uint8_t i = 0; i < count; ++i) {
        const Placed& one = over[i];
        // One byte compare for both edges: above the figure, row - top wraps to
        // 129 or more, so this holds while a figure covers at most 128 rows. The
        // screen has 25.
        if (uint8_t(row - uint8_t(one.top)) >= one.rows)
            continue;
        // The column step is its art's height plus the two blanks, which is one
        // more than the screen rows it covers.
        if (!cells.layer(one.at,
                one.down,
                uint16_t(one.first + (row - one.top)),
                one.cells,
                uint8_t(one.rows + 1),
                one.flags,
                one.colour))
            // Refused, not started: a narrower later layer can still be laid.
            ++layers_dropped;
    }

    // Plus what close() is about to take, since layer() reserved it: the peak
    // is then what the row consumes rather than two short of it.
    const uint8_t cost = uint8_t(cells.used() + rrb::RowList::CLOSE_CELLS);
    if (cost > row_peak)
        row_peak = cost;
    // Not counted with the dropped layers: layer() reserves what this needs, so
    // a close that fails is a broken reservation and not a crowded row.
    if (!cells.close_edge(CELLS_ACROSS * CELL_LINES, BLANK_GLYPH))
        ++closes_failed;
    constexpr uint16_t FROM = rrb::PREFIX_CELLS * 2;
    const uint16_t end = cells.bytes();
    const uint16_t at = row_at(list, row);
    agos::far_write(SCREEN + at + FROM, screen_row + FROM, uint16_t(end - FROM));
    agos::far_write(COLOUR + at + FROM, colour_row + FROM, uint16_t(end - FROM));
    const rrb::Cell park = rrb::tail();
    agos::far_fill(SCREEN + at + end, park.screen[0], uint16_t(rrb::ROW_BYTES - end));
    agos::far_fill(COLOUR + at + end, park.colour[0], uint16_t(rrb::ROW_BYTES - end));
}

/// Show display list @p list. Not hot registers (viciv.vhdl:1403-1411), so
/// nothing else is recomputed; SCRNPTR is $D060-$D063 and COLPTR $D064-$D065
/// (iomap.txt). Both are read afresh for every row, so this belongs outside
/// the picture (beam_outside_picture).
///
/// The lists are whole pages apart in one bank, so only the page bytes
/// change: the interrupt this runs in plays the music too.
[[gnu::always_inline]] inline void display_point(uint8_t list) {
    static_assert(rrb::LIST_BYTES % 256 == 0 &&
            ((SCREEN + rrb::LIST_BYTES) >> 16) == (SCREEN >> 16) &&
            uint8_t(SCREEN >> 8) + (rrb::LIST_BYTES >> 8) < 256,
        "the second list must differ from the first in page alone");
    const uint8_t page = list != 0 ? uint8_t(rrb::LIST_BYTES >> 8) : 0;
    VICIV.scrnptr_msb = uint8_t(uint8_t(SCREEN >> 8) + page);
    *COLPTR_MSB = page;
}

/// The picture in physical rasters, as the ROM leaves TBDRPOS and BBDRPOS
/// ($D048-$D04B, read on the machine): 25 rows of 16 rasters.
inline constexpr uint16_t PICTURE_TOP_LINE = 104;
inline constexpr uint16_t PICTURE_END_LINE = 504;

/// Rasters before the picture that already read it: the RRB builds a line a
/// raster ahead into the other half of the line buffer (NORRDEL clear,
/// viciv.vhdl:2124), and one more as margin, not measured.
inline constexpr uint8_t FETCH_LEAD_LINES = 2;

/// Whether the beam is outside the picture, where neither list pointer is
/// being read: the core takes both afresh for every row (viciv.vhdl:3450,
/// :3537), so a swap inside the picture splits a row between the lists.
///
/// The two bytes are read apart, so a read straddling line 255 to 256 can
/// misjudge it. Left so: that takes an interrupt some 250 rasters late landing
/// on those few cycles, and reading again needs a temporary this interrupt
/// cannot have (irq_tick).
[[nodiscard, gnu::always_inline]] inline bool beam_outside_picture() {
    const uint16_t line =
        static_cast<uint16_t>(VICIV.fn_raster_lsb | (VICIV.fn_raster_msb & RASTER_MSB_BITS) << 8);
    return line >= PICTURE_END_LINE || line < PICTURE_TOP_LINE - FETCH_LEAD_LINES;
}

/// The frame's interrupt, on the first physical raster below the picture.
///
/// Physical because the VIC-II raster cannot get there: on the machine it
/// counts one a physical raster from about 45 and holds at 311 from about
/// 356, so a compare of 250 fires with the beam on picture row 10. Any write to
/// $D011 or $D012 puts the compare back to VIC-II rasters
/// (viciv.vhdl:2327-2328, :2343-2344), so this follows every one.
[[gnu::always_inline]] inline void display_arm_tick() {
    static_assert(PICTURE_END_LINE >> 8 <= RASTER_MSB_BITS);
    VICIV.rstcmp = uint8_t(PICTURE_END_LINE);
    VICIV.rstcmp_msb = uint8_t((VICIV.rstcmp_msb & RASCMP_KEEP) | uint8_t(PICTURE_END_LINE >> 8));
}

/// Put the VIC into full colour mode and lay out the screen. Leaves the
/// display off; call display_show() once there is something to look at.
static void display_begin() {
    // Without this the VIC-IV registers are SID mirrors and every write below
    // goes somewhere harmless and wrong.
    VICIV.key = VIC4_KEY_VICIV_A;
    VICIV.key = VIC4_KEY_VICIV_B;

    // Hot registers first: writing $D011/$D016/$D018/$D031 while they are live
    // recomputes and clobbers LINESTEP, CHRCOUNT, SCRNPTR and COLPTR.
    VICIV.sdbdrwd_msb &= ~VIC4_HOTREG_MASK;

    // 40 columns, 200 rows, and ATTR off -- with ATTR set the top nybble of a
    // colour byte becomes blink, reverse, bold and underline instead of colour.
    VICIV.ctrlb =
        (VICIV.ctrlb | VIC3_FAST_MASK) & ~(VIC3_H640_MASK | VIC3_V400_MASK | VIC3_ATTR_MASK);

    // 16-bit cells, and full colour only above glyph $FF. FCLRLO is deliberately
    // clear: with it set every GOTOX costs sixteen cycles of a raster instead of
    // eight (viciv.vhdl:4351-4355), and nothing here is numbered under 256 --
    // the picture starts at FIRST_GLYPH, which is BACKDROP over 64.
    VICIV.ctrlc = VIC4_CHR16_MASK | VIC4_FCLRHI_MASK | VIC4_VFAST_MASK;

    VICIV.scrnptr_lsb = uint8_t(SCREEN);
    VICIV.scrnptr_bnk = uint8_t(SCREEN >> 16);
    VICIV.scrnptr_mb = 0;
    VICIV.colptr = 0;
    display_point(0);

    VICIV.linestep = rrb::ROW_BYTES; // bytes, whatever the SDK comment says
    // More rows than the picture has, or the VIC draws rows out of a buffer
    // nothing ever filled.
    VICIV.disp_rows = SCREEN_ROWS;
    VICIV.chryscl = 1;        // two rasters a pixel row, V200
    VICIV.alphadelay &= 0x0F; // one raster a row; a room player leaves this set

    VICIV.ctrl1 = (VICIV.ctrl1 & 0x40) | RSEL_YSCROLL3; // DEN deliberately off
    VICIV.ctrl2 = (VICIV.ctrl2 & 0xE0) | CSEL;
    display_arm_tick();

    // One pass over each line: no raster-rewrite double buffering here.
    *D051 &= ~(VIC4_DBLRR_MASK | VIC4_NORRDEL_MASK);

    for (uint8_t list = 0; list < rrb::LISTS; ++list)
        for (uint8_t row = 0; row < SCREEN_ROWS; ++row)
            display_row_over(list, row, nullptr, 0); // the prefixes are the caller's

    VICIV.chrcount = rrb::ROW_CELLS;
    VICIV.scrnptr_mb &= ~VIC4_CHRCOUNT_MASK;
}

/// Both halves of blanking: the border over everything, and a character count
/// of nought. Clearing DEN alone leaves the raster buffer showing the last
/// picture drawn.
static void display_blank() {
    VICIV.ctrl1 = VICIV.ctrl1 & uint8_t(~(DEN | RASTER_MSB));
    display_arm_tick();
    VICIV.chrcount = 0;
    VICIV.scrnptr_mb &= ~VIC4_CHRCOUNT_MASK;
}

static void display_show() {
    VICIV.chrcount = rrb::ROW_CELLS;
    VICIV.scrnptr_mb &= ~VIC4_CHRCOUNT_MASK;
    VICIV.ctrl1 = (VICIV.ctrl1 & ~RASTER_MSB) | DEN;
    display_arm_tick();
}
