// SPDX-License-Identifier: GPL-3.0-or-later

// The game's text: its own two fonts, rendered into full-colour glyphs.
//
// A line of text is a figure laid out like decode_figure: columns of
// glyphs with art one down and blanks on either end. Placement, layering
// and pool slot all exist; none is written twice. The original renders
// speech into a sprite image and animates it (string.cpp:545-556).

#pragma once

#include "atticmap.hpp"
#include "chipmap.hpp"
#include "far.hpp"

namespace text {

using agos::far_fill;
using agos::far_read;
using agos::far_write;
using agos::Place;

// --- the windows' font -------------------------------------------------------
//
// Fixed six by eight, one bit per pixel, one byte per row. 98 glyphs from
// character 32 (`windowDrawChar`, charset-fontdata.cpp:2917). Inventory, verbs, parchment
// use it; speech does not.

inline constexpr uint8_t WIN_LINES = 8;
inline constexpr uint8_t WIN_ADVANCE = 6;
inline constexpr uint8_t WIN_FIRST_CHAR = 32;
inline constexpr uint8_t WIN_GLYPHS = 98;
inline constexpr const char* WIN_FILE = "FONTWIN.BIN";
inline constexpr uint16_t WIN_BYTES = WIN_GLYPHS * WIN_LINES;

// --- the font an actor talks in ----------------------------------------------
//
// Ninety characters from '!', 41 bytes each: ten rows of four masks, then
// width (`renderStringAmiga`, charset-fontdata.cpp:1101). Three masks are
// shades drawn at the script's colour and the two above; the fourth is an
// outline in every bitplane (colour 15), so words read over whatever is behind. A character
// steps its width minus one, joining neighbouring outlines.

inline constexpr uint8_t SAY_ROWS = 10;
inline constexpr uint8_t SAY_MASKS = 4;
inline constexpr uint8_t SAY_BYTES = SAY_ROWS * SAY_MASKS + 1;
inline constexpr uint8_t SAY_FIRST_CHAR = '!';
inline constexpr uint8_t SAY_CHARS = 90;
inline constexpr uint8_t SAY_SHADES = 3;
inline constexpr uint8_t SAY_OUTLINE = 15;
inline constexpr uint8_t SAY_SPACE = 7; // what anything below '!' steps
inline constexpr const char* SAY_FILE = "FONTSAY.BIN";
inline constexpr uint16_t SAY_FONT_BYTES = SAY_CHARS * SAY_BYTES;

/// Whether the speech font has this character. Space and control codes step without drawing.
[[nodiscard]] inline bool draws(uint8_t ch) {
    return ch >= SAY_FIRST_CHAR && ch < SAY_FIRST_CHAR + SAY_CHARS;
}

[[nodiscard]] inline Place say_at(uint8_t ch) {
    return atticmap::SAYFONT + Place{uint8_t(ch - SAY_FIRST_CHAR)} * SAY_BYTES;
}

/// What a character steps: width minus one, so the next's outline lands
/// on this one's (charset-fontdata.cpp:1196).
[[nodiscard]] inline uint8_t step_of(uint8_t ch) {
    const uint8_t width = draws(ch) ? agos::far_read8(say_at(ch) + SAY_BYTES - 1) : SAY_SPACE;
    return uint8_t(width - 1);
}

/// How wide a run of characters draws, in pixels.
[[nodiscard]] inline uint16_t say_width(const char* s, uint8_t n) {
    uint16_t wide = 0;
    for (uint8_t i = 0; i < n; ++i)
        wide = uint16_t(wide + step_of(uint8_t(s[i])));
    return wide;
}

/// Where a word ends: the next space, or the end of the string.
[[nodiscard]] inline uint8_t word_end(const char* s, uint8_t from) {
    uint8_t at = from;
    while (s[at] != '\0' && s[at] != ' ')
        ++at;
    return at;
}

/// How many characters of @p s fit in @p pixels, broken at a space.
/// A word longer than the line takes it and overhangs, as the original does
/// rather than hyphenate.
[[nodiscard]] inline uint8_t say_break(const char* s, uint16_t pixels) {
    uint8_t took = 0, at = 0;
    for (;;) {
        const uint8_t end = word_end(s, at);
        if (say_width(s, end) > pixels && took != 0)
            return took;
        took = end;
        if (s[end] == '\0')
            return took;
        at = uint8_t(end + 1); // past the space, which the line keeps
    }
}

/// Cells a run of @p pixels covers.
[[nodiscard]] inline uint8_t cells_for(uint16_t pixels) {
    return uint8_t((pixels + chipmap::CELL_LINES - 1) / chipmap::CELL_LINES);
}

/// Glyph rows a block of @p lines takes, each ten pixels deep.
[[nodiscard]] inline uint8_t rows_for(uint8_t lines) {
    return uint8_t((uint16_t(lines) * SAY_ROWS + chipmap::CELL_LINES - 1) / chipmap::CELL_LINES);
}

namespace detail {

/// One mask's eight pixels into a tile, in @p colour, clipped to the cell.
///
/// Tested at the top and shifted left, never indexed by a runtime distance: a
/// variable shift is a called loop on this CPU.
inline void paint(
    uint8_t* tile, uint8_t line, uint16_t x, uint16_t left, uint8_t mask, uint8_t colour) {
    uint8_t bits = mask;
    for (uint8_t b = 0; b < 8; ++b) {
        const uint16_t at = uint16_t(x + b);
        if ((bits & 0x80) != 0 && at >= left && at < left + chipmap::CELL_LINES)
            tile[line * chipmap::CELL_LINES + uint8_t(at - left)] = colour;
        bits = uint8_t(bits << 1);
    }
}

} // namespace detail

/// Draw @p n characters of the windows' font into the glyphs at @p glyphs.
/// @p x, @p y are pixels within that grid, @p across its width in cells.
/// Fixed at six pixels a character, as `windowDrawChar` advances, and one
/// colour: this font has no shades and no outline.
/// Read-modify-write, a tile at a time: text lands where its pixels fall,
/// not on a cell boundary.
inline void render_window(
    const char* s, uint8_t n, uint8_t ink, Place glyphs, uint8_t across, uint16_t x, uint16_t y) {
    uint8_t rows[WIN_LINES];
    uint8_t tile[chipmap::GLYPH_BYTES];

    const uint8_t first_cell = uint8_t(x / chipmap::CELL_LINES);
    const uint8_t last_cell = uint8_t((x + uint16_t(n) * WIN_ADVANCE) / chipmap::CELL_LINES);

    for (uint8_t cell = first_cell; cell <= last_cell && cell < across; ++cell) {
        const uint16_t left = uint16_t(cell) * chipmap::CELL_LINES;
        const uint8_t first_row = uint8_t(y / chipmap::CELL_LINES);
        const uint8_t last_row = uint8_t((y + WIN_LINES - 1) / chipmap::CELL_LINES);

        for (uint8_t row = first_row; row <= last_row; ++row) {
            const uint16_t index = uint16_t(uint16_t(row) * across + cell);
            const Place at = glyphs + Place{index} * chipmap::GLYPH_BYTES;
            far_read(at, tile, chipmap::GLYPH_BYTES);

            for (uint8_t i = 0; i < n; ++i) {
                const uint16_t where = uint16_t(x + uint16_t(i) * WIN_ADVANCE);
                if (where + WIN_ADVANCE <= left || where >= left + chipmap::CELL_LINES)
                    continue;
                const uint8_t ch = uint8_t(s[i]);
                const uint8_t index_of = ch < WIN_FIRST_CHAR || ch >= WIN_FIRST_CHAR + WIN_GLYPHS
                    ? 0
                    : uint8_t(ch - WIN_FIRST_CHAR);
                far_read(atticmap::WINFONT + Place{index_of} * WIN_LINES, rows, WIN_LINES);
                for (uint8_t r = 0; r < WIN_LINES; ++r) {
                    const uint16_t line = uint16_t(y + r);
                    if (line / chipmap::CELL_LINES != row)
                        continue;
                    detail::paint(
                        tile, uint8_t(line % chipmap::CELL_LINES), where, left, rows[r], ink);
                }
            }
            far_write(at, tile, chipmap::GLYPH_BYTES);
        }
    }
}

/// The colour render_say takes for a script's @p colour drawn in palette
/// @p block: three shades from `color * 3 + 1` (string.cpp:550-551), four
/// bits as the Amiga's four planes hold them, under the block.
[[nodiscard]] constexpr uint8_t say_ink(uint8_t block, uint8_t colour) {
    return uint8_t(block << 4 | ((colour * 3 + 1) & 0x0F));
}

/// Render one line of speech into the glyphs at @p glyphs.
/// @p top is the line's first pixel row within the figure, so blocks of
/// lines render ten pixels apart. @p left_margin is the starting pixel
/// (centring); @p stride is column height (art starts one glyph down).
/// @p colour is the script's; the caller, which has the script's number,
/// makes the original's three shades of it (`color * 3 + 1`, string.cpp:551).
/// Its high nybble is the palette block: the original renders four planes
/// (renderStringAmiga, charset-fontdata.cpp:1101), so shades and outline stay
/// within four bits and the line's sprite lays its block over them
/// (draw.cpp:107). Read-modify-write, a tile at a time: ten rows do not
/// divide eight, so two lines can share a glyph and the second must not wipe
/// the first.
inline void render_say(const char* s,
    uint8_t n,
    uint8_t colour,
    Place glyphs,
    uint8_t stride,
    uint8_t cells,
    uint8_t top,
    uint8_t left_margin = 0) {
    uint8_t chr[SAY_BYTES];
    uint8_t tile[chipmap::GLYPH_BYTES];
    constexpr uint8_t NYBBLE = 0x0F;
    const uint8_t block = colour & uint8_t(~NYBBLE);

    for (uint8_t cell = 0; cell < cells; ++cell) {
        const uint16_t left = uint16_t(cell) * chipmap::CELL_LINES;
        const uint8_t first = uint8_t(top / chipmap::CELL_LINES);
        const uint8_t last = uint8_t((top + SAY_ROWS - 1) / chipmap::CELL_LINES);

        for (uint8_t row = first; row <= last && row + 2u <= stride; ++row) {
            const uint16_t index = uint16_t(uint16_t(cell) * stride + row + 1);
            const Place at = glyphs + Place{index} * chipmap::GLYPH_BYTES;
            far_read(at, tile, chipmap::GLYPH_BYTES);

            uint16_t x = left_margin;
            for (uint8_t i = 0; i < n; ++i) {
                const uint8_t ch = uint8_t(s[i]);
                // A character is nine pixels at the widest, so it reaches this cell
                // or the next one and nothing further.
                if (draws(ch) && x < left + chipmap::CELL_LINES && x + 9u > left) {
                    far_read(say_at(ch), chr, SAY_BYTES);
                    for (uint8_t r = 0; r < SAY_ROWS; ++r) {
                        const uint8_t y = uint8_t(top + r);
                        if (y / chipmap::CELL_LINES != row)
                            continue;
                        const uint8_t line = uint8_t(y % chipmap::CELL_LINES);
                        // Shades first and the outline over them: where a glyph is both,
                        // the original's planes come out as the outline's colour.
                        for (uint8_t m = 0; m < SAY_SHADES; ++m)
                            // A shade of nought sets no plane, so it paints nothing.
                            if (const uint8_t shade = uint8_t((colour + m) & NYBBLE); shade != 0)
                                detail::paint(tile,
                                    line,
                                    x,
                                    left,
                                    chr[r * SAY_MASKS + m],
                                    uint8_t(block | shade));
                        detail::paint(tile,
                            line,
                            x,
                            left,
                            chr[r * SAY_MASKS + SAY_SHADES],
                            uint8_t(block | SAY_OUTLINE));
                    }
                }
                x = uint16_t(x + step_of(ch));
            }
            far_write(at, tile, chipmap::GLYPH_BYTES);
        }
    }
}

/// Clear the glyphs a text figure of @p cells by @p stride occupies.
/// Every glyph, blanks included: a slot holds the last figure's leftovers,
/// so a column that did not write a cell would wear them.
inline void clear(Place glyphs, uint8_t cells, uint8_t stride) {
    far_fill(glyphs, 0, uint16_t(uint16_t(cells) * stride * chipmap::GLYPH_BYTES));
}

} // namespace text
