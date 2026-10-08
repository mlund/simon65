// SPDX-License-Identifier: GPL-3.0-or-later

// Raster Rewrite Buffer cells: what one screen row is made of.
//
// A row is a list of cells the VIC walks left to right, each either a glyph to
// paint or a token that moves where the next glyph lands. Two bytes go to
// screen RAM and two to colour RAM; they live in different memories but mean
// nothing apart, so they are built together and moved out separately.
//
// The encodings are hardware facts, read from the core rather than trusted
// from twp65, which established them on a real machine (core v920413):
// `viciv.vhdl` line numbers throughout.

#pragma once

#include "chipmap.hpp"
#include "far.hpp"

#ifdef __mos__
/// The cells of layer() in 45GS02 assembly (laycells.S), written from the
/// pointers given; the C below is the reference the host runs.
extern "C" void lay_cells(uint8_t* screen,
    uint8_t* colour,
    uint8_t cells,
    uint8_t flags,
    uint16_t first,
    uint8_t step,
    uint8_t colour_byte);
#endif

namespace rrb {

/// The colour byte's flags, at the bits the core reads them from. The top two
/// mean one thing on a glyph and another on a token; bit 4 chooses which.
inline constexpr uint8_t FLIP_VERTICAL = 0x80;   // viciv.vhdl:4399
inline constexpr uint8_t FLIP_HORIZONTAL = 0x40; // :4400, an actor facing left
inline constexpr uint8_t ALPHA = 0x20;           // :4401
inline constexpr uint8_t GOTO_X = 0x10;          // :4404

/// On a token, the bit that flips a glyph instead makes what follows leave the
/// buffer alone. Clear, what follows paints colour zero as background, which
/// is how a row's first pass covers what the last row left there.
inline constexpr uint8_t TRANSPARENT = FLIP_VERTICAL;

/// On a glyph, 16 pixels of four bits, painted through the full-colour
/// pipeline (viciv.vhdl:4406-4411): a figure's four planes as they are, at
/// half the cells and half the bytes. The low nybble is the left pixel, the
/// reverse of a sprite's, and the core swaps them itself when the glyph is
/// mirrored (:4741-4748).
inline constexpr uint8_t FOUR_BIT = 0x08; // viciv.vhdl:4408

/// On a token, the bit that makes a glyph four-bit is instead the row mask's
/// enable (viciv.vhdl:4830-4832): set, the token's second colour byte says
/// which of the eight pixel rows the cells after it draw, a set bit meaning
/// drawn (:4458). Left clear here, and the second byte with it, because a
/// figure's column is padded with a blank glyph above and below
/// (figures.hpp:28-31) -- so a shifted figure reads blank where the mask would
/// have blanked, and its own art where the mask would have let it through.
/// The mask costs a token 16 raster cycles against 8, so the padding is also
/// the cheaper of the two.
inline constexpr uint8_t ROWMASK_ENABLE = FOUR_BIT;

/// The second colour byte of a four-bit glyph drawn in palette @p block.
///
/// A nybble of 1-14 paints the byte's high nybble over itself and 15 paints
/// the whole byte (viciv.vhdl:5283-5290), so a low nybble of 15 makes both
/// land in the block: entry block*16 + n, which is where an AGOS figure's
/// index goes (vga.cpp:630, gfx.cpp:793). Nought is background either way.
[[nodiscard]] constexpr uint8_t four_bit_colour(uint8_t block) {
    return static_cast<uint8_t>(unsigned{block} << 4u | 0x0Fu);
}

/// The second colour byte of a full-colour glyph, and every cell's unless a
/// four-bit one names its block. A pixel of $FF paints this byte, not palette
/// entry 255 (viciv.vhdl:5345-5348), so it is 255 to keep that pixel entry
/// 255: an icon's colour 15 is one. ATTR is off (display.hpp), so the top
/// nybble means nothing more.
inline constexpr uint8_t FULL_COLOUR_FF = 0xFF;

/// One cell, as the two memories want it.
struct Cell {
    uint8_t screen[2];
    uint8_t colour[2];
};

/// A glyph to paint. Thirteen bits of number: the top three of the high byte
/// are a width trim this does not use. @p colour is the second colour byte,
/// which only a four-bit glyph reads here.
[[nodiscard]] constexpr Cell glyph(
    uint16_t number, uint8_t flags = 0, uint8_t colour = FULL_COLOUR_FF) {
    return {{static_cast<uint8_t>(number), static_cast<uint8_t>((unsigned{number} >> 8) & 0x1Fu)},
        {flags, colour}};
}

/// Put the next glyph at pixel @p x, and shift what follows by @p lines pixel
/// rows, up to seven either way.
///
/// The shift adds to `glyph_number & chargen_y` (viciv.vhdl:4480), so it runs
/// into the *next glyph number* once it passes the bottom of a glyph -- free
/// vertical placement only where the glyph below is the next number up.
[[nodiscard]] constexpr Cell go_to(uint16_t x, int8_t down = 0, bool transparent = true) {
    // Unsigned literals throughout: `int` is 16 bits on the target, so uint16_t
    // is unsigned int and stops promoting to int, and a plain 0x10 would be a
    // signed operand in an unsigned expression. The host, where the promotion
    // does happen, never sees it.
    const unsigned magnitude = static_cast<unsigned>(down < 0 ? -int{down} : int{down});
    return {{static_cast<uint8_t>(x),
                static_cast<uint8_t>(((unsigned{x} >> 8) & 0x03u) | (down > 0 ? 0x10u : 0u) |
                    ((magnitude & 0x07u) << 5u))},
        {static_cast<uint8_t>(GOTO_X | (transparent ? TRANSPARENT : 0u)), 0}};
}

/// What a row's unused cells must hold, and why it cannot be a blank glyph.
///
/// Every glyph advances the write address by its own width; that address is
/// ten bits, so a blank tail wraps and paints over the row's left. A token
/// does not advance, so a tail of them stays in place.
///
/// **xemu does not wrap the address**, so a row that overruns looks right
/// there and fails only on hardware.
///
/// $0303: the write position is ten bits, so only $0000, $0101, $0202 and
/// $0303 have matching screen bytes; a tail of repeating bytes fills by block
/// move instead of strided write. $0303 is 771, past anything a row paints.
[[nodiscard]] constexpr Cell tail() {
    Cell out = go_to(0x0303);
    out.colour[1] = out.colour[0];
    return out;
}

/// Cells a row holds, fetched whether used or not -- a row cannot stop early.
///
/// **A row's cost is the fetch, not the paint.** CHRCOUNT is global, so park
/// tokens are fetched at 8 cycles each. Measured on hardware, row 0 is not
/// ready in time at 144 cells; at 128 and below it is. DBLRR at V200 smears
/// instead.
///
/// Demand through the intro reaches at least 124 including the close.
inline constexpr uint8_t ROW_CELLS = 128;

/// What a row takes of each memory: two bytes a cell.
inline constexpr uint16_t ROW_BYTES = static_cast<uint16_t>(ROW_CELLS) * 2;

/// Two lists, because the next frame's row is built while this one shows. Only
/// the screen halves live here; the colour halves are their own memory.
inline constexpr uint8_t LISTS = 2;
inline constexpr uint16_t LIST_BYTES = ROW_BYTES * chipmap::SCREEN_ROWS;

static_assert(uint32_t{LIST_BYTES} * LISTS <= chipmap::SCREEN_BYTES,
    "two display lists of this many cells do not fit SCREEN");
static_assert(uint32_t{LIST_BYTES} * LISTS <= chipmap::COLOUR_BYTES,
    "their colour halves do not fit colour RAM");
static_assert(ROW_CELLS >= chipmap::CELLS_ACROSS + 2,
    "a row must hold the backdrop, a closing token and its cell");

/// A row's opening token and its backdrop cells: the same every frame, so the
/// display list keeps them and a row is rebuilt from here on.
inline constexpr uint8_t PREFIX_CELLS = 1 + chipmap::CELLS_ACROSS;

/// One row under construction, in near memory.
///
/// Screen and colour are gathered separately because that is how they leave:
/// two block moves to two different memories. The row is not valid until
/// close() has run, which is what makes it reach the right-hand edge.
class RowList {
  public:
    /// Cells before @p first are taken as already there.
    RowList(uint8_t* screen, uint8_t* colour, uint8_t first = 0)
        : screen_(screen), colour_(colour), at_(uint16_t{first} * 2), used_(first) {
    }

    [[nodiscard]] uint8_t used() const {
        return used_;
    }
    [[nodiscard]] uint16_t bytes() const {
        return at_;
    } //!< of each half

    [[nodiscard]] bool put(const Cell& one) {
        if (used_ >= ROW_CELLS)
            return false;
        // One running index instead of four multiplies per put() call.
        uint8_t* const screen = &screen_[at_];
        uint8_t* const colour = &colour_[at_];
        screen[0] = one.screen[0];
        screen[1] = one.screen[1];
        colour[0] = one.colour[0];
        colour[1] = one.colour[1];
        at_ = static_cast<uint16_t>(at_ + 2);
        ++used_;
        return true;
    }

    /// @p count cells of consecutive glyph numbers, which is how a backdrop row
    /// is stored: the decoder numbers them left to right. @p colour is each
    /// cell's second colour byte.
    [[nodiscard]] bool run(
        uint16_t first, uint8_t count, uint8_t flags = 0, uint8_t colour = FULL_COLOUR_FF) {
        for (uint8_t i = 0; i < count; ++i)
            if (!put(glyph(static_cast<uint16_t>(first + i), flags, colour)))
                return false;
        return true;
    }

    /// @p count cells of one glyph. A blank row must paint blank glyphs: a
    /// row that paints nothing shows what the row before left in the line
    /// buffer, and only glyphs cover.
    [[nodiscard]] bool fill(uint16_t number, uint8_t count, uint8_t flags = 0) {
        for (uint8_t i = 0; i < count; ++i)
            if (!put(glyph(number, flags)))
                return false;
        return true;
    }

    [[nodiscard]] bool at(uint16_t x, int8_t down = 0, bool transparent = true) {
        return put(go_to(x, down, transparent));
    }

    /// A figure's cells for this screen row, all or none: the token placing
    /// them, then @p cells glyphs @p step apart, from lowest-numbered visible
    /// column (backwards when @p flags mirrors). Each cell carries @p flags and
    /// @p colour (how a four-bit figure gets its palette block).
    ///
    /// Stepped because the vertical offset carries into the next glyph number;
    /// a figure is numbered down its columns, so a row's cells are a column's
    /// height apart.
    ///
    /// All or none: a partial figure leaves it cut off, and the row cannot close
    /// either, so it never reaches the right edge.
    [[nodiscard]] bool layer(uint16_t x,
        uint8_t down,
        uint16_t first,
        uint8_t cells,
        uint8_t step,
        uint8_t flags = 0,
        uint8_t colour = FULL_COLOUR_FF) {
        if (used_ + 1u + cells + CLOSE_CELLS > ROW_CELLS)
            return false;
        if (!at(x, static_cast<int8_t>(down)))
            return false;
#ifdef __mos__
        lay_cells(&screen_[at_], &colour_[at_], cells, flags, first, step, colour);
        at_ = static_cast<uint16_t>(at_ + uint16_t{cells} * 2);
        used_ = static_cast<uint8_t>(used_ + cells);
        return true;
#else
        // A mirrored figure needs its columns in the other order as well as its
        // pixels: the bit mirrors a glyph's own, and starting at the last
        // column and stepping back mirrors the figure over them.
        uint16_t number = first;
        int16_t stride = step;
        if ((flags & FLIP_HORIZONTAL) != 0) {
            number = static_cast<uint16_t>(first + uint16_t{cells} * step - step);
            stride = static_cast<int16_t>(-stride);
        }
        for (uint8_t i = 0; i < cells; ++i) {
            if (!put(glyph(number, flags, colour)))
                return false;
            number = static_cast<uint16_t>(int16_t(number) + stride);
        }
        return true;
#endif
    }

    /// The token and the cell a close costs, which layer() leaves room for.
    static constexpr uint8_t CLOSE_CELLS = 2;

    /// Reach @p width without painting, then fill the rest with tokens.
    ///
    /// A transparent glyph advances the write address but paints nothing
    /// (viciv.vhdl:5310, :5316), so a blank cell at the edge sets the row's
    /// width without covering what is under it. A token cannot: scan-out stops
    /// at the furthest written address (:3354-3358).

    [[nodiscard]] bool close(uint16_t width, uint16_t blank) {
        if (!close_edge(width, blank))
            return false;

        // Every byte of a parked token repeats, so the tail fills as block
        // moves instead of strided writes: rebuilding the screen measured 2x
        // faster.
        const Cell park = tail();
        for (uint16_t i = at_; i < ROW_BYTES; ++i) {
            screen_[i] = park.screen[0];
            colour_[i] = park.colour[0];
        }
        used_ = ROW_CELLS;
        at_ = ROW_BYTES;
        return true;
    }

    /// Reach @p width without painting, as close() does, and leave the rest
    /// for the caller to park: a DMA fill does it faster than this loop.
    [[nodiscard]] bool close_edge(uint16_t width, uint16_t blank) {
        return at(static_cast<uint16_t>(width - 8)) && put(glyph(blank));
    }

  private:
    uint8_t* screen_;
    uint8_t* colour_;
    uint16_t at_; // byte offset of the next cell, kept rather than derived
    uint8_t used_;
};

} // namespace rrb
