// A zone's NNN1.out: the palettes, and the two tables that locate a sprite's
// animation or image script.

#pragma once

#include "far.hpp"
#include "gamedb.hpp"

#include <stdint.h>

namespace agos {

/// Where the header sits: a big-endian pointer at offset 4 (gfx.cpp:1265), not the
/// offset 10 of vc19_loop, which this game never installs.
inline constexpr uint8_t ZONE_HEADER_POINTER = 4;

/// VgaFile1Header_Common, nine big-endian words (vga.h:85).
inline constexpr uint8_t ZONE_HEADER_WORDS = 9;

/// ImageHeader_Simon is {id, colour, x2, scriptOffs}; AnimationHeader_Simon
/// drops the colour (vga.h:55, :62).
inline constexpr uint8_t IMAGE_HEADER = 8;
inline constexpr uint8_t ANIMATION_HEADER = 6;

/// 32 colours of three 6-bit components, scaled by four on load
/// (vga_s1.cpp:115) -- the DOS layout, not the Amiga 0RGB words.
inline constexpr uint8_t PALETTE_COLOURS = 32;
inline constexpr uint16_t PALETTE_BYTES = PALETTE_COLOURS * 3;
inline constexpr uint8_t PALETTE_FIRST = 6;

/// A sprite id names its own zone (gfx.cpp:1225).
inline constexpr uint16_t SPRITES_PER_ZONE = 100;

/// One zone's script file, kept writable: vc20_setRepeat stores its loop
/// counter inside the script it is running (vga.cpp:885), so the stream is
/// data as well as code and cannot be run from anywhere read-only.
class VgaZone {
  public:
    void reset(Place file1, uint32_t size) {
        base_ = file1;
        size_ = size;
        const uint16_t header = far_read16(file1 + ZONE_HEADER_POINTER);
        image_count_ = far_read16(file1 + header + 2);
        animation_count_ = far_read16(file1 + header + 6);
        image_table_ = far_read16(file1 + header + 10);
        animation_table_ = far_read16(file1 + header + 14);
    }

    [[nodiscard]] bool valid() const {
        return base_ != NOWHERE;
    }
    [[nodiscard]] uint16_t image_count() const {
        return image_count_;
    }
    [[nodiscard]] uint16_t animation_count() const {
        return animation_count_;
    }

    /// Where a sprite's animation script starts, or nullptr. Both tables are
    /// linear-searched by id, as the engine searches them.
    [[nodiscard]] Place animation_script(uint16_t sprite_id) const {
        return script(animation_table_, animation_count_, ANIMATION_HEADER, 4, sprite_id);
    }
    [[nodiscard]] Place image_script(uint16_t sprite_id) const {
        return script(image_table_, image_count_, IMAGE_HEADER, 6, sprite_id);
    }

    /// One 32-colour block, or nullptr past the end.
    [[nodiscard]] Place palette(uint16_t block) const {
        const uint32_t at = PALETTE_FIRST + static_cast<uint32_t>(block) * PALETTE_BYTES;
        return at + PALETTE_BYTES <= size_ ? base_ + at : NOWHERE;
    }

    [[nodiscard]] Place base() const {
        return base_;
    }

  private:
    [[nodiscard]] Place script(uint16_t table,
        uint16_t count,
        uint8_t stride,
        uint8_t offset_field,
        uint16_t sprite_id) const {
        // If base_ is NOWHERE, the bare offset is a small near address that the
        // caller would then run as a script.
        if (base_ == NOWHERE)
            return NOWHERE;
        // A cursor rather than table + i * stride: the multiply is a library call
        // on this CPU, and this search runs on every CALL the animation VM makes.
        for (Place entry = base_ + table; count != 0; --count, entry += stride)
            if (far_read16(entry) == sprite_id) {
                // And the offset must land inside the file: a table read from a zone
                // that is not the one it describes points anywhere.
                const uint16_t offset = far_read16(entry + offset_field);
                return offset < size_ ? base_ + offset : NOWHERE;
            }
        return NOWHERE;
    }

    Place base_ = NOWHERE;
    uint32_t size_ = 0;
    uint16_t image_count_ = 0;
    uint16_t animation_count_ = 0;
    uint16_t image_table_ = 0;
    uint16_t animation_table_ = 0;
};

} // namespace agos
