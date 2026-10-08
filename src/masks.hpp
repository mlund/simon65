// SPDX-License-Identifier: GPL-3.0-or-later

// Masked sprites (video opcode 61): the room's clean picture cut to a mask's
// shape, as full-colour glyphs a layer can lay.
//
// The engine draws a mask by copying its clean background over the frame
// wherever the mask's image is set (runit2 0x1e5b0; vc61, vga_s1.cpp:225).
// The backdrop here is that clean background -- sprites never write into it --
// so the cut, laid at the mask's place in the sprite list, covers exactly the
// sprites drawn before it.

#pragma once

#include "chipmap.hpp"
#include "far.hpp"

#include <stdint.h>

namespace masks {

using agos::Place;

/// A mask image's four-bit figure in the Attic, and where its cut goes.
struct Cut {
    Place art;     //!< the decoded figure: a blank, then art, down each column
    Place out;     //!< the cut, laid out the same way, one byte a pixel
    uint8_t cells; //!< the figure's four-bit cells; the cut has twice as many
    uint8_t rows;  //!< glyph rows of art
    int16_t x;     //!< eight-pixel units, as a sprite's x is
    int16_t y;     //!< pixels
};

/// Near memory cut() works in: a mask glyph, the two backdrop glyphs a line
/// can straddle, and the glyph being built.
inline constexpr uint16_t SCRATCH_BYTES = 4 * chipmap::GLYPH_BYTES;

/// The palette entry other than nought nearest entry nought, by summed
/// difference over the three components of @p palette.
///
/// A layer's byte of nought is transparent, so a cut cannot show the
/// backdrop's nought as it is: it would let the sprite under it through.
[[nodiscard]] inline uint8_t stand_in(const uint8_t* palette, uint16_t entries) {
    uint8_t best = 1;
    uint16_t best_distance = UINT16_MAX;
    for (uint16_t entry = 1; entry < entries; ++entry) {
        uint16_t distance = 0;
        for (uint8_t c = 0; c < 3; ++c) {
            const uint8_t have = palette[entry * 3 + c], want = palette[c];
            distance = static_cast<uint16_t>(distance + (have > want ? have - want : want - have));
        }
        if (distance < best_distance) {
            best_distance = distance;
            best = static_cast<uint8_t>(entry);
        }
    }
    return best;
}

/// Cut the backdrop at @p backdrop (FCM, a picture of glyph rows) to the mask
/// in @p cut: a pixel the mask sets takes the backdrop's byte, nought turned
/// into @p nought_as; every other pixel is nought, which lets what is under
/// the layer show.
///
/// The mask's y need not be on a glyph row, so each built glyph reads the two
/// backdrop glyphs its lines straddle. Lines above or below the picture stay
/// transparent.
inline void cut(const Cut& cut, Place backdrop, uint8_t nought_as, uint8_t* scratch) {
    constexpr uint8_t SIDE = chipmap::CELL_LINES;
    constexpr uint8_t HALF = SIDE / 2; // bytes of a four-bit line in a cut cell
    constexpr uint16_t GLYPH = chipmap::GLYPH_BYTES;
    constexpr uint16_t PICTURE_ROW = chipmap::CELLS_ACROSS * GLYPH;
    static_assert(SIDE == 8 && GLYPH == 64);
    uint8_t* const mask = scratch;
    uint8_t* const pair = scratch + GLYPH;
    uint8_t* const out = scratch + 3 * GLYPH;

    // Addresses are stepped, not multiplied: a Place is 32 bits.
    const uint16_t column_bytes = static_cast<uint16_t>((cut.rows + 2) * GLYPH);
    // Every art row starts the same lines into a glyph row, as rows are eight
    // lines apart. Floored, so a mask rising out of the top still lines up.
    const int16_t first_row = static_cast<int16_t>(cut.y >> 3);
    const uint8_t inside = static_cast<uint8_t>((cut.y & (SIDE - 1)) * SIDE);
    Place to = cut.out;
    Place art = cut.art + GLYPH;
    const uint8_t across = static_cast<uint8_t>(cut.cells * 2);
    // Cleared whole in one job: the blanks above and below each column, which
    // a shifted layer reads into, and columns off the picture, never laid.
    agos::far_fill(cut.out, 0, static_cast<uint16_t>(across * column_bytes));
    for (uint8_t column = 0; column < across;
        ++column, to += column_bytes, art += (column & 1) == 0 ? column_bytes : 0) {
        const int16_t screen_column = static_cast<int16_t>(cut.x + column);
        if (screen_column < 0 || screen_column >= chipmap::CELLS_ACROSS)
            continue;
        // Which half of the four-bit cell this column is.
        const uint8_t half = (column & 1) != 0 ? HALF : 0;
        Place from = art, into = to + GLYPH;
        int16_t glyph_row = first_row;
        for (uint8_t row = 0; row < cut.rows; ++row, ++glyph_row, from += GLYPH, into += GLYPH) {
            agos::far_read(from, mask, GLYPH);
            // Which of the two are on the picture, a bit apiece: an array would
            // live on the soft stack.
            uint8_t shown = 0;
            for (uint8_t k = 0; k < 2; ++k) {
                const int16_t at = static_cast<int16_t>(glyph_row + k);
                if (at < 0 || at >= chipmap::PICTURE_ROWS)
                    continue;
                shown = static_cast<uint8_t>(shown | (k + 1));
                agos::far_read(backdrop +
                        static_cast<uint16_t>(static_cast<uint16_t>(at) * PICTURE_ROW +
                            static_cast<uint16_t>(screen_column) * GLYPH),
                    pair + k * GLYPH,
                    GLYPH);
            }
            for (uint8_t at = 0; at < GLYPH; ++at) {
                const uint8_t px = at & (SIDE - 1);
                // The low nybble is the left pixel (planar.hpp, PackBits).
                const uint8_t two = mask[(at & ~(SIDE - 1)) + half + (px >> 1)];
                const uint8_t set = (px & 1) != 0 ? two >> 4 : two & 0x0F;
                // Where this byte's line is in the two backdrop glyphs read.
                const uint8_t in_pair = static_cast<uint8_t>(at + inside);
                uint8_t value = 0;
                if (set != 0 && (shown & ((in_pair & GLYPH) != 0 ? 2 : 1)) != 0) {
                    value = pair[in_pair];
                    if (value == 0)
                        value = nought_as;
                }
                out[at] = value;
            }
            agos::far_write(into, out, GLYPH);
        }
    }
}

} // namespace masks
