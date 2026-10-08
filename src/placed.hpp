// SPDX-License-Identifier: GPL-3.0-or-later

// A figure as the row builder lays it: glyphs, place and shape, and the one
// function that makes it from where the game says a figure goes.
//
// Its own header so the host can build and inspect layers without the VIC,
// and so everything that lays a figure lays it by the same rounding.

#pragma once

#include "chipmap.hpp"
#include "figures.hpp"
#include "rrb.hpp"

#include <stdint.h>

/// A figure on the screen: which glyphs, where, and its shape in cells. The
/// rows count is also the step between columns, because a figure is numbered
/// down them.
/// Clipped when it is built, so a row lays it without arithmetic: `at` is a
/// position a GOTOX can express, and `cells` is what the picture can show.
/// `cells` of nought is a figure wholly outside it, which is not laid at all.
struct Placed {
    uint16_t first; //!< the glyph for screen row `top`, its leftmost visible column
    uint16_t at;    //!< left edge in pixels, inside the picture
    int8_t top;     //!< first screen row it reaches, negative above the picture
    uint8_t cells;  //!< visible width in glyphs
    uint8_t rows;   //!< screen rows it covers, one more than its art when shifted
    int8_t down;    //!< pixel rows below the top of `top`
    uint8_t flags;  //!< the first colour byte: rrb::FLIP_HORIZONTAL, rrb::FOUR_BIT
    uint8_t colour; //!< the second: a four-bit figure's palette block
};

/// A cell's width in pixels: sixteen four-bit, eight full colour.
[[nodiscard, gnu::always_inline]] inline uint8_t cell_px(uint8_t flags) {
    return (flags & rrb::FOUR_BIT) != 0 ? 2 * chipmap::CELL_LINES : chipmap::CELL_LINES;
}

/// Where a layer's art starts, in pixels down the picture.
[[nodiscard, gnu::always_inline]] inline int16_t y_of(const Placed& p) {
    return static_cast<int16_t>(p.top * chipmap::CELL_LINES + p.down);
}

/// A layer from its first glyph, its left edge in pixels and its top in
/// pixels: the one place a pixel row becomes a screen row and a shift.
/// Signed: a figure rising out of the top keeps its true row and its whole
/// height, so the rows still inside it show and the column step stays what
/// its glyphs are numbered by. Unsigned, a row of -2 would read 254 and every
/// row would skip the figure: Simon's body at the top of the ladder.
[[nodiscard, gnu::always_inline]] inline Placed laid(uint16_t first,
    uint16_t at,
    int16_t y,
    uint8_t cells,
    uint8_t art_rows,
    uint8_t flags,
    uint8_t colour) {
    return {first,
        at,
        static_cast<int8_t>(y / chipmap::CELL_LINES),
        cells,
        static_cast<uint8_t>(art_rows + 1),
        static_cast<int8_t>(y % chipmap::CELL_LINES),
        flags,
        colour};
}

/// Where a figure goes on the screen, from where the game says it goes.
///
/// A sprite's x is in eight-pixel units and its y in pixels:
/// xoffs = (vlut[0] * 2 + state->x) * 8, yoffs = vlut[1] + state->y
/// (gfx.cpp:940), and window 4 -- the room -- sits at 0, 0
/// (initialVideoWindows_Simon, agos.cpp:723). The art starts one glyph down
/// its column, and covers one screen row more than its art when it is shifted.
///
/// Clipped here, not where a row is laid, because none of it depends on the
/// row: a figure spans up to seventeen rows and the row builder runs
/// twenty-five times a frame, where this runs once a figure. Eight-pixel units
/// make it free: x arrives in them, and a cell is one (text) or two (a
/// four-bit figure).
///
/// Clipping is by whole cells, and a four-bit cell straddling either edge is
/// kept, so nothing on the picture is lost. Its position is exact either way:
/// GOTOX is in pixels.
///
/// `cells` of nought means nothing of it is on screen; the caller drops it
/// rather than spend one of the twelve layer slots on a figure every row
/// would refuse.
///
/// @p flags and @p colour are the cells' two colour bytes, as rrb::layer()
/// lays them.
[[nodiscard, gnu::always_inline]] inline Placed placed(const agos::Figure& fig,
    int16_t x,
    int16_t y,
    uint8_t flags = 0,
    uint8_t colour = rrb::FULL_COLOUR_FF) {
    const bool flip = (flags & rrb::FLIP_HORIZONTAL) != 0;
    const bool four = (flags & rrb::FOUR_BIT) != 0;
    // Cells wholly off the left are dropped from the front. A four-bit cell
    // straddling the edge is kept and placed at -8: GOTOX is ten bits and the
    // write address wraps (rrb.hpp), so its left half lands past the buffer's
    // end and its right half on column 0, which would otherwise show the
    // backdrop over the figure. xemu does not wrap, so there the half is lost.
    uint8_t gone = 0;
    if (x < 0)
        gone = four ? static_cast<uint8_t>((-x) >> 1) : static_cast<uint8_t>(-x);
    const int16_t column = x + (four ? gone * 2 : gone);
    uint8_t wide = fig.cells > gone ? static_cast<uint8_t>(fig.cells - gone) : 0;
    // Rounded up: a four-bit cell straddling the right edge is laid, its right
    // half past the picture under the border, or the figure's last column shows
    // the backdrop over it. The write address is ten bits, far past the edge.
    uint8_t room =
        column < chipmap::CELLS_ACROSS ? static_cast<uint8_t>(chipmap::CELLS_ACROSS - column) : 0;
    if (four)
        room = static_cast<uint8_t>((room + 1) >> 1);
    if (wide > room)
        wide = room;
    // A figure is numbered down its columns, so the first visible column is
    // `gone` steps along -- a step being its art's height plus the two blanks,
    // one more than the screen rows it covers. Folded in here, the row adds
    // only its own offset.
    //
    // Mirrored, the screen's leftmost cell is the art's *last* column, so the
    // columns trimmed off the left come off the far end and the row starts from
    // the lowest-numbered one still visible, walking back from the other end.
    //
    // Wholly above the picture is as gone as wholly off one side.
    if (y + fig.rows * chipmap::CELL_LINES <= 0)
        wide = 0;
    const uint8_t step = fig.stride();
    const uint8_t lowest = flip ? static_cast<uint8_t>(fig.cells - gone - wide) : gone;
    return laid(static_cast<uint16_t>(fig.glyph + 1 + uint16_t{lowest} * step),
        static_cast<uint16_t>(column * chipmap::CELL_LINES),
        y,
        wide,
        fig.rows,
        flags,
        colour);
}
