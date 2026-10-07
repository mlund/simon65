// The mouse pointer: the one hardware sprite this display uses.
//
// Everything else that moves is an RRB token, which is why the map reserves a
// single sprite's worth of glyph and one pointer word (chipmap.hpp). A sprite
// is right for the pointer and wrong for an actor: it is free to place at any
// raster, and there are only eight of them.

#pragma once

#include "chipmap.hpp"
#include "far.hpp"

#ifdef __mos__
#include <mega65.h>
#endif

#include <stdint.h>

namespace cursor {

/// Sixteen-bit sprite pointers ($D06E.7), which let a sprite's glyph sit on
/// any 64-byte boundary in chip RAM rather than in the first 16 KB
/// (iomap.txt:267). The pointer holds the address in sixty-fourths.
inline constexpr uint8_t PTR16 = 0x80;

/// The pointer table, inside the cursor's own reservation and clear of the
/// 63 bytes of glyph below it.
inline constexpr uint32_t TABLE = chipmap::CURSOR + 64;

/// What a sprite Y is, against the physical raster the picture starts on.
/// Measured in twp65 and the same here: the sprite coordinate system is
/// offset from the raster the VIC reports.
inline constexpr uint16_t SPRITE_BIAS = 16;

/// Where the picture's left edge lands in sprite X. The classic origin, and
/// it agrees with the geometry: TEXTXPOS reads 80 physical columns, which is
/// 40 of this mode's pixels, less the same bias.
inline constexpr uint16_t LEFT_PIXEL = 24;

/// The picture's first raster, read from the VIC rather than written down
/// twice: the display sets the geometry and this follows it.
inline uint16_t top_raster = 0;

/// Two rasters a pixel row, as display_begin sets ($D048 chryscl). A picture
/// row is therefore two rasters down, which is why the Y below needs nine
/// bits: eight stop the pointer a little over half way down the screen, which
/// reads as a pointer that will not go lower rather than as a number that ran
/// out (measured in twp65).
inline constexpr uint8_t RASTERS_PER_ROW = 2;

/// An arrow, 24 by 21 as a sprite is: the classic shape, so that what is on
/// screen is unmistakably ours and not a leftover.
/// An arrow, eleven pixels across and fourteen down inside the 24 by 21 a
/// sprite always is. Small because a picture row is two rasters and the
/// sprite is expanded to match, so a pixel here is a pixel of the picture:
/// the 24-wide classic shape stood a seventh of the room's height.
///
/// The tip is the top left, which is where the pointer is said to be.
inline constexpr uint8_t ARROW[63] = {
    0x80,
    0x00,
    0x00,
    0xC0,
    0x00,
    0x00,
    0xE0,
    0x00,
    0x00,
    0xF0,
    0x00,
    0x00,
    0xF8,
    0x00,
    0x00,
    0xFC,
    0x00,
    0x00,
    0xFE,
    0x00,
    0x00,
    0xFF,
    0x00,
    0x00,
    0xF8,
    0x00,
    0x00,
    0xD8,
    0x00,
    0x00,
    0x8C,
    0x00,
    0x00,
    0x0C,
    0x00,
    0x00,
    0x06,
    0x00,
    0x00,
    0x06,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
};

#ifdef __mos__

/// Lay the glyph down, point the VIC at it, and turn sprite nought on.
inline void begin(uint8_t colour) {
    agos::far_write(chipmap::CURSOR, ARROW, sizeof ARROW);

    // Where the picture actually starts, as the VIC has it.
    const uint16_t textypos =
        static_cast<uint16_t>(VICIV.textypos_lsb | ((VICIV.textypos_msb & 0x0F) << 8));
    top_raster = static_cast<uint16_t>(textypos - SPRITE_BIAS);

    // Stated rather than inherited. Each of these survives a reset or an earlier
    // program and changes the picture without changing the data: sixteen-colour
    // mode reads these bytes as pairs of nybbles and paints a solid block,
    // expansion doubles a pixel, multicolour halves the resolution, alpha fades
    // it out, and V400 changes what a Y counts. mega65-freezer's sprite editor
    // clears the same set, and twp65 measured what each does.
    VICIV.spr_16en = 0; // one bit a pixel, which is what ARROW is
    VICIV.spr_x64en = 0;
    VICIV.spr_hgten = 0; // the classic twenty-one rows
    // X square with the picture, Y twice as tall, because the display draws two
    // rasters to a picture row and a sprite pixel is one raster. Unexpanded, the
    // pointer is half the height the same art has in ScummVM's 320x200; expanded
    // in X as well it would be twice as wide, since across we are already 1:1 at
    // forty cells of eight. The display decides this, not the caller.
    VICIV.spr_exp_x = 0;
    VICIV.spr_exp_y = 0x01;
    VICII.spr_mcolor = 0;
    VICIV.spr_enalpha = 0;
    VICIV.spr_env400 = 0;
    VICIV.spr_ymsbs = 0;
    VICIV.spr_ysmsbs = 0;
    VICII.spr_hi_x = 0;

    // The table holds sixty-fourths, which is what a 64-byte boundary is for.
    const uint16_t sixty_fourth = static_cast<uint16_t>(chipmap::CURSOR / 64);
    uint8_t entry[2] = {
        static_cast<uint8_t>(sixty_fourth), static_cast<uint8_t>(sixty_fourth >> 8)};
    agos::far_write(TABLE, entry, sizeof entry);

    VICIV.spr_ptradr_lsb = static_cast<uint8_t>(TABLE);
    VICIV.spr_ptradr_msb = static_cast<uint8_t>(TABLE >> 8);
    VICIV.spr_ptradr_bnk = static_cast<uint8_t>(((TABLE >> 16) & 0x7F) | PTR16);

    VICII.spr0_color = colour;
    VICII.spr_ena = 0x01;
}

/// Whether the pointer is on screen, read back from the VIC: the player may
/// act exactly while it is.
[[nodiscard]] inline bool visible() {
    return (VICII.spr_ena & 0x01) != 0;
}

/// Whether the pointer is on screen. The script hides it while it moves
/// things about (181) and shows it again when the player may act.
inline void shown(bool on) {
    if (on)
        VICII.spr_ena |= 0x01;
    else
        VICII.spr_ena &= static_cast<uint8_t>(~0x01);
}

/// Put the pointer at a place in the picture. X is a pixel across, y a row
/// down, which is what the game's own coordinates are.
inline void at(uint16_t x, uint16_t y) {
    const uint16_t across = static_cast<uint16_t>(LEFT_PIXEL + x);
    const uint16_t down = static_cast<uint16_t>(top_raster + y * RASTERS_PER_ROW);

    VICII.spr0_x = static_cast<uint8_t>(across);
    VICII.spr0_y = static_cast<uint8_t>(down);
    if ((across & 0x100) != 0)
        VICII.spr_hi_x |= 0x01;
    else
        VICII.spr_hi_x &= static_cast<uint8_t>(~0x01);
    if ((down & 0x100) != 0)
        VICIV.spr_ymsbs |= 0x01;
    else
        VICIV.spr_ymsbs &= static_cast<uint8_t>(~0x01);
}

#endif // __mos__

} // namespace cursor
