// SPDX-License-Identifier: GPL-3.0-or-later

// Amiga bitplanes to one byte of colour index per pixel.
//
// The pixel data is planar: `depth` separate bitmaps, one per bit of the
// colour index, each word-RLE compressed on its own. ScummVM decompresses
// every plane into a buffer and then merges them (res_ami.cpp:102-133). That
// buffer is up to 160 KiB for the widest image in the release, which this
// machine does not have to spare -- but every plane is read in the same word
// order, so they can be advanced together and never materialised at all.
//
// Format references, all engines/agos in the ScummVM tree:
//   vga.cpp:637     the 8-byte image table entry
//   vga.cpp:667     bit 7 of the flags byte means compressed, for the Amiga
//   res_ami.cpp:36  uncompressPlane -- the word-oriented RLE
//   res_ami.cpp:56  bitplaneToChunky
//   res_ami.cpp:117 column-major for compressed, row-major otherwise
//   res_ami.cpp:167 four planes stored consecutively; anything else interleaved

#pragma once

#include "far.hpp"

namespace agos {

/// One row of the image table at the start of a type-2 zone file. The table
/// has no count: the animation scripts index it by image id (vga.cpp:618).
struct ImageEntry {
    Place offset;      //!< pixel data, relative to the start of the file
    uint16_t width_px; //!< always a multiple of 16
    uint8_t height;
    uint8_t flags;

    /// Amiga only. The script flag kDFCompressed (0x08) is a different field.
    [[nodiscard]] bool compressed() const {
        return (flags & 0x80) != 0;
    }

    /// Whether this reads as a table row at all, which is how the end of the
    /// table is found: deliberately conservative, since nothing marks it.
    [[nodiscard]] bool plausible() const {
        return width_px != 0 && width_px <= 1024 && (width_px & 15) == 0 && height != 0 &&
            offset != 0;
    }
};

inline constexpr uint8_t IMAGE_ENTRY_BYTES = 8;

/// Image @p index of the table at the start of the type-2 file.
[[nodiscard]] inline ImageEntry image_entry(Place file2, uint16_t index) {
    const Place at = file2 + Place{index} * IMAGE_ENTRY_BYTES;
    return {far_read32(at), far_read16(at + 6), far_read8(at + 5), far_read8(at + 4)};
}

/// How many rows the image table has.
///
/// It ends where image 1's data begins, which is that row's own offset
/// (ScummVM walks it this way, debug.cpp:659). Row 0 is reserved and reads as
/// zeros in every zone of this release, so ids start at 1. Stopping instead at
/// the first row that does not read as an image -- which is what the table's
/// lack of a count invites -- loses 3,373 real images across the release,
/// since a gap inside the table reads exactly like its end.
[[nodiscard]] inline uint16_t image_table_rows(Place file2, uint32_t file_bytes) {
    // The count itself is in the file, so a file too short to hold row 1 has no
    // table; and a count longer than the file is a count of rows that are not
    // there -- whatever the last zone left at this address, read as a table.
    if (file_bytes < 2 * IMAGE_ENTRY_BYTES + 4)
        return 0;
    const uint32_t stated = far_read32(file2 + IMAGE_ENTRY_BYTES);
    const uint32_t fits = stated < file_bytes ? stated : file_bytes;
    const uint32_t rows = fits / IMAGE_ENTRY_BYTES;
    return rows > 0xFFFFu ? uint16_t{0xFFFF} : static_cast<uint16_t>(rows);
}

/// One bitplane as a stream of 16-bit words, compressed or not.
///
/// The whole point of the class: `next()` is all the merge below needs, so the
/// RLE never has to produce a plane, only the next word of one.
///
/// Every read is bounded. Nothing marks the end of the image table, so a row
/// past it can read as a perfectly plausible image whose pixels are not in the
/// file; unbounded, that decodes whatever happens to sit next in memory and
/// looks like a picture. The bound costs one compare per sixteen pixels.
class PlaneReader {
  public:
    /// A compressed plane running from @p at to @p end.
    ///
    /// over_ is cleared here because a reader outlives one image: the decoder
    /// keeps its readers between pieces, and a plane that overran on one image
    /// would otherwise condemn every image after it.
    void open_packed(Place at, Place end) {
        at_ = at;
        end_ = end;
        left_ = 0;
        packed_ = true;
        over_ = false;
    }

    /// A plane of plain words at @p at, stepping @p stride words each time --
    /// one where the planes are consecutive, `depth` where they interleave.
    void open_plain(Place at, uint8_t stride, Place end) {
        at_ = at;
        end_ = end;
        stride_ = stride;
        packed_ = false;
        over_ = false;
    }

    /// Whether this plane ever asked for a byte the caller did not promise.
    [[nodiscard]] bool overran() const {
        return over_;
    }

    [[nodiscard]] uint16_t next() {
        if (!packed_) {
            if (at_ + 2 > end_)
                return over_ = true, uint16_t{0};
            const uint16_t word = far_read16(at_);
            at_ += Place{stride_} * 2;
            return word;
        }
        if (left_ == 0) {
            if (at_ >= end_)
                return over_ = true, uint16_t{0};
            // A signed control byte: n >= 0 repeats the next word n+1 times, n < 0
            // copies -n literal words. Counted in words, not bytes.
            const uint8_t control = far_read8(at_++);
            repeat_ = control < 0x80;
            left_ = repeat_ ? static_cast<uint16_t>(control + 1)
                            : static_cast<uint16_t>(0x100 - control);
            if (repeat_) {
                if (at_ + 2 > end_)
                    return over_ = true, uint16_t{0};
                word_ = far_read16(at_);
                at_ += 2;
            }
        }
        --left_;
        if (repeat_)
            return word_;
        if (at_ + 2 > end_)
            return over_ = true, uint16_t{0};
        const uint16_t word = far_read16(at_);
        at_ += 2;
        return word;
    }

  private:
    Place at_ = NOWHERE;
    Place end_ = NOWHERE;
    uint16_t left_ = 0; // words still to come from the current run
    uint16_t word_ = 0; // what a repeat run repeats
    /// Zero, not one: every path sets it before a read, and a non-zero
    /// default would put the decoder's 280 bytes of state in .data rather
    /// than .bss -- initialised bytes ram_fixed has not got.
    uint8_t stride_ = 0;
    bool packed_ = false;
    bool repeat_ = false;
    bool over_ = false;
};

/// The most planes an image can have: the colour index is one byte.
inline constexpr uint8_t MAX_PLANES = 8;

/// Open a compressed image's planes, whose offsets are two big-endian words
/// apiece, summed (res_ami.cpp:102). False if the table itself is not there.
[[nodiscard]] inline bool open_planes(PlaneReader* plane, Place data, Place end, uint8_t depth) {
    if (data + Place{depth} * 4 > end)
        return false;
    for (uint8_t p = 0; p < depth; ++p) {
        const Place at = data + Place{p} * 4;
        plane[p].open_packed(data + far_read16(at) + far_read16(at + 2), end);
    }
    return true;
}

/// How many bitplanes an image has -- 4 for 16 colours, 5 for 32.
///
/// ScummVM takes this from engine state rather than from the file
/// (res_ami.cpp:145), but the file says it anyway: the plane offset table is
/// four bytes per plane and plane 0 starts immediately after it, so the first
/// offset is the table's own size. Uncompressed images carry no such table and
/// answer 4, which is what ScummVM defaults to.
[[nodiscard]] inline uint8_t image_depth(
    Place file2, uint32_t file_bytes, const ImageEntry& entry) {
    if (!entry.compressed())
        return 4;
    if (entry.offset + 4 > file_bytes)
        return 0;
    const Place at = file2 + entry.offset;
    const uint16_t table_bytes = static_cast<uint16_t>(far_read16(at) + far_read16(at + 2));
    const uint8_t depth = static_cast<uint8_t>(table_bytes / 4);
    if ((table_bytes & 3) != 0 || depth < 1 || depth > MAX_PLANES)
        return 0;
    return depth;
}

/// Two planes' pixels, looked up by the two nibbles they contribute.
///
/// The transpose is the decode's largest part: stubbing it took a 32-cell
/// title image from 12 frames to 5, measured on hardware. Bit by bit it costs
/// a test, a shift, a load and a store per pixel per plane; a table gives two
/// planes at one load, indexed by one plane's nibble over the other's.
struct PairBits {
    uint8_t at[4][256];
};

consteval PairBits pair_bits() {
    PairBits made{};
    for (uint16_t i = 0; i < 256; ++i)
        for (uint8_t k = 0; k < 4; ++k) {
            // Leftmost pixel first, as the planes are most significant bit first.
            const uint8_t shift = static_cast<uint8_t>(3 - k);
            const uint8_t low = (i >> 4) >> shift & 1;
            const uint8_t high = (i & 0x0F) >> shift & 1;
            made.at[k][i] = static_cast<uint8_t>(low | high << 1);
        }
    return made;
}

/// One table, not two: the other two planes' bits are shifted two places up
/// from it. The fixed region has no room for another kilobyte.
inline constexpr PairBits PAIR = pair_bits();

/// Eight pixels from the top bits of each plane word, consuming them.
///
/// Plane-major, not pixel-major. A plane's word is loaded once and stays in
/// registers for all eight pixels it feeds, shifting left a bit at a time;
/// asking for bit 15-x per pixel instead reloads every plane for every pixel
/// *and* costs a variable-distance 16-bit shift, which on this CPU is a called
/// loop rather than an instruction. Measured on hardware over one 320x136
/// room: bit-indexed 95 frames, shift-and-test but pixel-major 37, this 29.
/// res_ami.cpp:56 is the same shape, two pixels at a time.
///
/// @p seed is what a pixel starts as, which lets a caller that paints every
/// pixel write straight into its destination with the palette block already
/// on: decoding into a scratch and copying costs 21 cycles a pixel, about a
/// frame over a full-screen backdrop.
inline void chunky8(uint16_t* word, uint8_t depth, uint8_t* out, uint8_t seed = 0) {
    // Four planes is what a figure has and nearly every room: worth the one
    // special case, since it is the whole of the transpose in eight lines.
    if (depth == 4) {
        const uint8_t plane0 = static_cast<uint8_t>(word[0] >> 8);
        const uint8_t plane1 = static_cast<uint8_t>(word[1] >> 8);
        const uint8_t plane2 = static_cast<uint8_t>(word[2] >> 8);
        const uint8_t plane3 = static_cast<uint8_t>(word[3] >> 8);
        const uint8_t left01 = static_cast<uint8_t>((plane0 & 0xF0) | plane1 >> 4);
        const uint8_t left23 = static_cast<uint8_t>((plane2 & 0xF0) | plane3 >> 4);
        const uint8_t right01 = static_cast<uint8_t>(plane0 << 4 | (plane1 & 0x0F));
        const uint8_t right23 = static_cast<uint8_t>(plane2 << 4 | (plane3 & 0x0F));
        out[0] = static_cast<uint8_t>(seed | PAIR.at[0][left01] | PAIR.at[0][left23] << 2);
        out[1] = static_cast<uint8_t>(seed | PAIR.at[1][left01] | PAIR.at[1][left23] << 2);
        out[2] = static_cast<uint8_t>(seed | PAIR.at[2][left01] | PAIR.at[2][left23] << 2);
        out[3] = static_cast<uint8_t>(seed | PAIR.at[3][left01] | PAIR.at[3][left23] << 2);
        out[4] = static_cast<uint8_t>(seed | PAIR.at[0][right01] | PAIR.at[0][right23] << 2);
        out[5] = static_cast<uint8_t>(seed | PAIR.at[1][right01] | PAIR.at[1][right23] << 2);
        out[6] = static_cast<uint8_t>(seed | PAIR.at[2][right01] | PAIR.at[2][right23] << 2);
        out[7] = static_cast<uint8_t>(seed | PAIR.at[3][right01] | PAIR.at[3][right23] << 2);
        for (uint8_t p = 0; p < 4; ++p)
            word[p] = static_cast<uint16_t>(word[p] << 8);
        return;
    }
    for (uint8_t x = 0; x < 8; ++x)
        out[x] = seed;
    uint8_t bit = 1;
    for (uint8_t p = 0; p < depth; ++p) {
        uint16_t w = word[p];
        for (uint8_t x = 0; x < 8; ++x) {
            if ((w & 0x8000) != 0)
                out[x] = static_cast<uint8_t>(out[x] | bit);
            w = static_cast<uint16_t>(w << 1);
        }
        word[p] = w;
        bit = static_cast<uint8_t>(bit << 1);
    }
}

/// Sixteen pixels from one word of each plane, most significant bit leftmost.
inline void chunky16(uint16_t* word, uint8_t depth, Place into) {
    uint8_t eight[8];
    chunky8(word, depth, eight);
    far_write(into, eight, 8);
    chunky8(word, depth, eight);
    far_write(into + 8, eight, 8);
}

/// Two pixels to a byte, which is what a four-bit cell holds.
///
/// PAIR's shape, one step further: where that gives two planes' bits for one
/// pixel, this gives them for two pixels at once, so a 16-pixel strip costs
/// eight lookups against chunky8's sixteen. The low nybble is the LEFT pixel
/// -- the core paints bits 3..0 and then shifts right by four, which is
/// the opposite of the sprite convention.
struct PackBits {
    uint8_t at[2][256];
};

consteval PackBits pack_bits() {
    PackBits made{};
    for (uint16_t i = 0; i < 256; ++i)
        for (uint8_t j = 0; j < 2; ++j) {
            uint8_t both = 0;
            for (uint8_t half = 0; half < 2; ++half) {
                const uint8_t shift = static_cast<uint8_t>(3 - (2 * j + half));
                const uint8_t low = (i >> 4) >> shift & 1;
                const uint8_t high = (i & 0x0F) >> shift & 1;
                both = static_cast<uint8_t>(both | (low | high << 1) << (half * 4));
            }
            made.at[j][i] = both;
        }
    return made;
}

/// Half a kilobyte, against the transpose being 58% of a figure's decode.
inline constexpr PackBits PACK = pack_bits();

#ifdef __mos__
} // namespace agos
/// PACK under a C name, for the assembly transpose (transpose.S), which reads
/// it from here; defined once, in main.cpp.
extern "C" const agos::PackBits PACK_TABLE;
/// The four-plane chunky16_packed in 45GS02 assembly (transpose.S).
extern "C" void chunky16_packed4(const uint16_t* word, uint8_t* out);
namespace agos {
#endif

/// Sixteen pixels packed two to a byte, consuming a whole word of each plane.
///
/// The figures' transpose and not the room's: a backdrop cell is full colour
/// and keeps chunky8, which this leaves alone. Four planes is what every
/// figure in the release has -- 15,162 sprite image references, none with an
/// index above 15 -- so the fast path is the one that runs.
inline void chunky16_packed(uint16_t* word, uint8_t depth, uint8_t* out) {
    if (depth == 4) {
        for (uint8_t half = 0; half < 2; ++half) {
            const uint8_t plane0 = static_cast<uint8_t>(word[0] >> 8);
            const uint8_t plane1 = static_cast<uint8_t>(word[1] >> 8);
            const uint8_t plane2 = static_cast<uint8_t>(word[2] >> 8);
            const uint8_t plane3 = static_cast<uint8_t>(word[3] >> 8);
            const uint8_t left01 = static_cast<uint8_t>((plane0 & 0xF0) | plane1 >> 4);
            const uint8_t left23 = static_cast<uint8_t>((plane2 & 0xF0) | plane3 >> 4);
            const uint8_t right01 = static_cast<uint8_t>(plane0 << 4 | (plane1 & 0x0F));
            const uint8_t right23 = static_cast<uint8_t>(plane2 << 4 | (plane3 & 0x0F));
            out[0] = static_cast<uint8_t>(PACK.at[0][left01] | PACK.at[0][left23] << 2);
            out[1] = static_cast<uint8_t>(PACK.at[1][left01] | PACK.at[1][left23] << 2);
            out[2] = static_cast<uint8_t>(PACK.at[0][right01] | PACK.at[0][right23] << 2);
            out[3] = static_cast<uint8_t>(PACK.at[1][right01] | PACK.at[1][right23] << 2);
            for (uint8_t p = 0; p < 4; ++p)
                word[p] = static_cast<uint16_t>(word[p] << 8);
            out += 4;
        }
        return;
    }
    for (uint8_t x = 0; x < 8; ++x)
        out[x] = 0;
    uint8_t bit = 1;
    for (uint8_t p = 0; p < depth; ++p) {
        uint16_t w = word[p];
        // Only four planes fit a nybble. Nothing reaching here has more: a
        // five-plane image is a room's ground, and that goes through chunky8.
        if (p < 4)
            for (uint8_t x = 0; x < 16; ++x) {
                if ((w & 0x8000) != 0)
                    out[x / 2] = static_cast<uint8_t>(out[x / 2] | bit << ((x & 1) * 4));
                w = static_cast<uint16_t>(w << 1);
            }
        word[p] = 0; // a whole word at a time, so nothing of it is left
        bit = static_cast<uint8_t>(bit << 1);
    }
}

/// Decode one image to @p into: one byte of colour index per pixel,
/// `width_px` bytes to a row. False if it does not read as an image, or if its
/// pixels are not all within the @p file_bytes the caller promises at
/// @p file2 -- in which case @p into has been partly written and means
/// nothing.
///
/// Compressed images are stored column-major -- every row of the leftmost
/// 16-pixel strip, then every row of the next. Walking strip-then-row rather
/// than dividing a running index keeps a division off the 6502 entirely.
[[nodiscard]] inline bool decode_image(
    Place file2, uint32_t file_bytes, const ImageEntry& entry, Place into) {
    if (!entry.plausible() || entry.offset >= file_bytes)
        return false;
    const uint8_t depth = image_depth(file2, file_bytes, entry);
    if (depth == 0)
        return false;

    const Place data = file2 + entry.offset;
    const Place end = file2 + file_bytes;
    const uint16_t strips = static_cast<uint16_t>(entry.width_px / 16u);
    const uint16_t words = static_cast<uint16_t>(strips * entry.height);

    PlaneReader plane[MAX_PLANES];
    if (entry.compressed()) {
        if (!open_planes(plane, data, end, depth))
            return false;
    } else if (depth == 4)
        for (uint8_t p = 0; p < depth; ++p)
            plane[p].open_plain(data + Place{p} * words * 2, 1, end);
    else
        for (uint8_t p = 0; p < depth; ++p)
            plane[p].open_plain(data + Place{p} * 2, depth, end);

    uint16_t word[MAX_PLANES];
    const auto strip_at = [&](uint16_t strip, uint8_t row) {
        for (uint8_t p = 0; p < depth; ++p)
            word[p] = plane[p].next();
        chunky16(word, depth, into + Place{row} * entry.width_px + Place{strip} * 16);
    };

    if (entry.compressed())
        for (uint16_t strip = 0; strip < strips; ++strip)
            for (uint8_t row = 0; row < entry.height; ++row)
                strip_at(strip, row);
    else
        for (uint8_t row = 0; row < entry.height; ++row)
            for (uint16_t strip = 0; strip < strips; ++strip)
                strip_at(strip, row);

    for (uint8_t p = 0; p < depth; ++p)
        if (plane[p].overran())
            return false;
    return true;
}

/// A full-colour cell is 8x8 pixels, one byte each.
inline constexpr uint8_t GLYPH_SIDE = 8;
inline constexpr uint8_t GLYPH_BYTES = GLYPH_SIDE * GLYPH_SIDE;

/// Two adjacent glyphs, which is what eight rows of one strip come to.
inline constexpr uint16_t TILE_BYTES = 2 * GLYPH_BYTES;

/// Where plane @p p's word @p index is in an image stored plain: four planes
/// one after another, any other depth interleaved word by word -- the two
/// layouts open_plain() is given in decode_image().
[[nodiscard, gnu::always_inline]] inline Place plain_word(
    Place data, uint16_t words, uint8_t depth, uint16_t index, uint8_t p) {
    return depth == 4 ? data + Place{p} * words * 2 + Place{index} * 2
                      : data + (Place{index} * depth + p) * 2;
}

/// Composite one piece of a room into the backdrop's glyphs.
///
/// A room is not one picture. Zone 64's image script paints twenty-two: the
/// base, then its scenery, each over what is already there. They belong in the
/// backdrop, not the figure pool: they are ground, not actors, and the pool
/// holds one painted figure at a time, so every piece but the last would be
/// lost.
///
/// Transparent, because that is what the pieces are: colour nought is left
/// alone (gfx.cpp:793), so a destination tile is read, merged and written back
/// rather than overwritten. A four-plane piece's index reaches the display
/// palette OR'd with its block, which is why the tint travels with it.
///
/// The geometry is kind in one direction only: a DRAW's x is in eight-pixel
/// units and an image's width is a multiple of sixteen, so a piece always
/// starts on a cell boundary across. Its y is in pixels and usually does not,
/// so a strip's eight-line run straddles two glyph rows and the walk steps by
/// what is left of the current one.
///
/// Anything outside the picture is consumed and dropped rather than refused:
/// the stream has to be read to reach the next strip either way, and the
/// engine clips a piece to its window.
///
/// @p transparent says which kind this is. A room's base is opaque -- every
/// pixel is written, colour nought included -- and scenery is not: it is
/// merged into what is there.
///
/// Neither clears the ground. Fourteen of the release's backdrops are
/// narrower than the picture and more are shorter, so what the art misses has
/// to be cleared by whoever knows a room is starting; doing it here as well
/// would write the same bytes twice. The zeroing below is a different thing:
/// the near tile started clean before a part row goes into it, since every
/// strip reuses that scratch.
///
/// @p glyphs is the backdrop's first glyph, @p across the cells in a screen
/// row, @p rows the glyph rows the picture has, @p x_cells and @p y_px where
/// the script puts it, and @p block its palette block.
[[nodiscard]] [[gnu::always_inline]] inline bool decode_piece(Place file2,
    uint32_t file_bytes,
    const ImageEntry& entry,
    Place glyphs,
    uint8_t across,
    uint8_t rows,
    int16_t x_cells,
    int16_t y_px,
    uint8_t block,
    bool transparent) {
    if (!entry.plausible() || entry.offset >= file_bytes)
        return false;
    const uint8_t depth = image_depth(file2, file_bytes, entry);
    if (depth == 0)
        return false;

    const Place data = file2 + entry.offset;
    const Place end = file2 + file_bytes;
    const uint16_t strips = static_cast<uint16_t>(entry.width_px / 16u);
    const uint16_t words = static_cast<uint16_t>(strips * entry.height);
    const uint8_t tint = static_cast<uint8_t>(block * 16);

    PlaneReader plane[MAX_PLANES];
    if (entry.compressed()) {
        if (!open_planes(plane, data, end, depth))
            return false;
    } else if (static_cast<uint32_t>(static_cast<uint16_t>(words * 2u)) * depth > end - data) {
        return false; // addressed rather than streamed, so it must all be there
    }

    // Hoisted: the storage kind cannot change between lines, and this is the
    // innermost loop the decoder has.
    const bool streamed = entry.compressed();

    uint8_t tile[TILE_BYTES];
    uint8_t half[GLYPH_SIDE];
    uint16_t word[MAX_PLANES];
    for (uint16_t strip = 0; strip < strips; ++strip) {
        // A strip is two cells and leaves as one move, so a strip hanging over
        // either edge is dropped whole rather than half drawn. x is the script's
        // own and is signed: a piece placed partly off the left edge would
        // otherwise wrap to the far side of the row.
        const int16_t left_cell = static_cast<int16_t>(x_cells + static_cast<int16_t>(strip * 2u));
        const bool across_screen = left_cell >= 0 && left_cell + 1 < across;
        // An addressed image is not a stream, so a strip that lands nowhere need
        // not be walked at all. A compressed one must be, to reach the next.
        if (!streamed && !across_screen)
            continue;
        int16_t at = y_px; // where this strip's next line lands
        uint8_t line = 0;
        while (line < entry.height) {
            const uint8_t inside = static_cast<uint8_t>(at >= 0 ? at % GLYPH_SIDE : 0);
            // Above the picture, the run is what it takes to reach it -- not a whole
            // cell, or a piece starting at a y that is not a multiple of eight lands
            // misaligned once it comes into view.
            const uint16_t room_here = static_cast<uint16_t>(at < 0 ? -at : GLYPH_SIDE - inside);
            const uint8_t left_in_image = static_cast<uint8_t>(entry.height - line);
            const uint8_t lines =
                static_cast<uint8_t>(room_here < left_in_image ? room_here : left_in_image);
            const int16_t row = static_cast<int16_t>(at >= 0 ? at / GLYPH_SIDE : -1);
            const bool shown = across_screen && row >= 0 && row < rows;

            // Only where it lands: off the picture the row is negative, and the
            // multiply would be signed.
            const uint16_t cell = shown
                ? static_cast<uint16_t>(static_cast<uint16_t>(row) * static_cast<uint16_t>(across) +
                      static_cast<uint16_t>(left_cell))
                : 0;
            const Place tile_at = glyphs + Place{cell} * GLYPH_BYTES;
            if (shown) {
                if (transparent)
                    far_read(tile_at, tile, TILE_BYTES);
                else if (lines != GLYPH_SIDE)
                    for (uint16_t i = 0; i < TILE_BYTES; ++i)
                        tile[i] = 0; // a part row, so what the art misses is cleared
            }
            for (uint8_t i = 0; i < lines; ++i) {
                if (streamed)
                    // Read to reach the next strip, whether this one lands or not.
                    for (uint8_t p = 0; p < depth; ++p)
                        word[p] = plane[p].next();
                else if (shown) {
                    const uint16_t index = static_cast<uint16_t>((line + i) * strips + strip);
                    for (uint8_t p = 0; p < depth; ++p)
                        word[p] = far_read16(plain_word(data, words, depth, index, p));
                }
                if (!shown)
                    continue;
                // Two halves of sixteen pixels, landing in cells side by side.
                for (uint8_t side = 0; side < 2; ++side) {
                    uint8_t* const into = &tile[side * GLYPH_BYTES + (inside + i) * GLYPH_SIDE];
                    if (!transparent) {
                        chunky8(word, depth, into, tint); // every pixel, no copy
                        continue;
                    }
                    // Merging has to see which pixels are nought before the block goes
                    // on them, so this one decodes aside and then chooses.
                    chunky8(word, depth, half);
                    for (uint8_t px = 0; px < GLYPH_SIDE; ++px)
                        if (half[px] != 0)
                            into[px] = static_cast<uint8_t>(half[px] | tint);
                }
            }
            if (shown)
                far_write(tile_at, tile, TILE_BYTES);
            line = static_cast<uint8_t>(line + lines);
            at = static_cast<int16_t>(at + lines);
        }
    }

    for (uint8_t p = 0; p < depth; ++p)
        if (plane[p].overran())
            return false;
    return true;
}

/// A figure decoded a piece at a time, so no frame stops for a whole one,
/// into glyphs numbered down its columns.
///
/// Figures, as against decode_piece's ground, and three things differ.
///
/// The numbering: a figure moves vertically by the GOTOX offset, which carries
/// into the next glyph number (viciv.vhdl:4486), so the glyph below one of its
/// glyphs must be the next number up, and a figure is numbered down its
/// columns.
///
/// The height: only 11.5% of the release's images are a multiple of eight
/// tall, so the last glyph row is usually partial and its remainder has to be
/// cleared -- a figure that left it alone would carry the previous tenant's
/// pixels in its bottom strip.
///
/// The storage: 36% are uncompressed, and those are row-major. Their planes
/// are not a stream but a block, so a word can be addressed directly and the
/// walk stays column-major whatever the file does.
///
/// Each column carries two blank glyphs, one above its art and one below, and
/// so is `rows + 2` glyphs tall. That is what makes the vertical offset usable:
/// shifting down by d makes the top d lines of a cell come from the *previous*
/// glyph number (`(glyph & chargen_y) - offset`, viciv.vhdl:4486), and the
/// bottom 8-d lines of the last cell from the next. Without the blanks a
/// shifted figure wears its neighbour's pixels at top and bottom.
///
/// Whole, a big decode is eight frames: 160 milliseconds in which the game
/// does nothing, landing on the title's cross-fade. Sliced, the work is the
/// same but stops between pieces and comes back.
///
/// The state is here rather than on the stack because of the plane readers:
/// a packed plane is a stream consumed strictly in order, so re-entering an
/// image halfway is impossible without them. About 280 bytes, which is what
/// resumability costs.
class FigureDecode {
  public:
    /// Take on an image. False if it will not decode at all, in which case
    /// nothing is begun.
    [[gnu::always_inline]] [[nodiscard]] bool begin(
        Place file2, uint32_t file_bytes, const ImageEntry& entry, Place glyphs) {
        if (!entry.plausible() || entry.offset >= file_bytes)
            return false;
        depth_ = image_depth(file2, file_bytes, entry);
        if (depth_ == 0)
            return false;

        data_ = file2 + entry.offset;
        const Place end = file2 + file_bytes;
        glyphs_ = glyphs;
        height_ = entry.height;
        packed_ = entry.compressed();
        strips_ = static_cast<uint16_t>(entry.width_px / 16u);
        words_ = static_cast<uint16_t>(strips_ * entry.height);
        glyph_rows_ = static_cast<uint8_t>((entry.height + GLYPH_SIDE - 1) / GLYPH_SIDE);
        stride_ = static_cast<uint8_t>(glyph_rows_ + 2);

        if (packed_) {
            if (!open_planes(plane_, data_, end, depth_))
                return false;
            // The planes below are read by address rather than as a stream, so
            // overran() says nothing about them and the whole block has to be in
            // the file before the first read. Two images in the release end past
            // it; unchecked, they decode from whatever sits next in Attic and
            // report success. In 16 bits for the product -- 32 is a call to
            // __mulsi3, 326 bytes of it.
        } else if (static_cast<uint32_t>(static_cast<uint16_t>(words_ * 2u)) * depth_ >
            end - data_) {
            return false;
        }
        strip_ = 0;
        glyph_row_ = 0;
        column_ = 0;
        good_ = true;
        busy_ = true;
        return true;
    }

    /// One piece: a glyph pair of the art, or a column's two blanks. The unit
    /// is about 1.7 ms, so a frame holds a dozen of them.
    [[gnu::always_inline]] void step() {
        if (strip_ < strips_) {
            art();
            if (++glyph_row_ == glyph_rows_) {
                glyph_row_ = 0;
                ++strip_;
            }
            return;
        }
        if (column_ < strips_) {
            blanks();
            ++column_;
            return;
        }
        if (packed_)
            for (uint8_t p = 0; p < depth_; ++p)
                if (plane_[p].overran())
                    good_ = false;
        busy_ = false;
    }

    [[nodiscard]] bool busy() const {
        return busy_;
    }

    /// Give up on the figure in flight, whole or not. The caller records
    /// nothing, so the image is simply asked for again.
    void abandon() {
        busy_ = false;
        good_ = false;
    }

    /// Whether the figure that just finished is whole. Only meaningful once
    /// busy() has gone false.
    [[nodiscard]] bool good() const {
        return good_;
    }

  private:
    /// One glyph row of one strip: its lines, and the two glyphs they make.
    [[gnu::always_inline]] void art() {
        const uint8_t lines = static_cast<uint8_t>(height_ - glyph_row_ * GLYPH_SIDE < GLYPH_SIDE
                ? height_ - glyph_row_ * GLYPH_SIDE
                : GLYPH_SIDE);
        if (lines != GLYPH_SIDE)
            for (uint16_t i = 0; i < GLYPH_BYTES; ++i)
                tile_[i] = 0; // a part row leaves the rest of its glyph

        uint16_t word[MAX_PLANES];
        for (uint8_t line = 0; line < lines; ++line) {
            if (packed_)
                for (uint8_t p = 0; p < depth_; ++p)
                    word[p] = plane_[p].next();
            else {
                // Row-major on disk, so word (row, strip) is at a known place and the
                // walk need not follow the file's order.
                const uint16_t row = static_cast<uint16_t>(glyph_row_ * GLYPH_SIDE + line);
                const uint16_t index = static_cast<uint16_t>(row * strips_ + strip_);
                for (uint8_t p = 0; p < depth_; ++p)
                    word[p] = far_read16(plain_word(data_, words_, depth_, index, p));
            }
#ifdef __mos__
            if (depth_ == 4)
                chunky16_packed4(word, &tile_[line * GLYPH_SIDE]);
            else
#endif
                chunky16_packed(word, depth_, &tile_[line * GLYPH_SIDE]);
        }

        // One four-bit cell holds the strip, so it leaves as one move. Art sits
        // one glyph down its column, leaving index 0 blank.
        const uint16_t cell = static_cast<uint16_t>(strip_ * stride_ + glyph_row_ + 1);
        far_write(glyphs_ + Place{cell} * GLYPH_BYTES, tile_, GLYPH_BYTES);
    }

    /// The blanks at either end of one column.
    ///
    /// A fill rather than a buffer of zeros written out: the job carries the
    /// value, so nothing has to be cleared first.
    [[gnu::always_inline]] void blanks() {
        const Place at = glyphs_ + Place{static_cast<uint16_t>(column_ * stride_)} * GLYPH_BYTES;
        far_fill(at, 0, GLYPH_BYTES);
        far_fill(at + Place{static_cast<uint8_t>(stride_ - 1)} * GLYPH_BYTES, 0, GLYPH_BYTES);
    }

    PlaneReader plane_[MAX_PLANES];
    /// A member, not a local in art(): a local that far_write takes the address
    /// of is addressed through a frame, and absolute beats that by 500 bytes of
    /// text here, measured when ram_fixed had 268 to give.
    uint8_t tile_[GLYPH_BYTES] = {};
    Place data_ = NOWHERE, glyphs_ = NOWHERE;
    uint16_t strips_ = 0, words_ = 0, strip_ = 0, column_ = 0;
    uint8_t glyph_rows_ = 0, stride_ = 0, depth_ = 0, height_ = 0;
    uint8_t glyph_row_ = 0;
    bool packed_ = false, busy_ = false, good_ = false;
};

/// The one decode in flight. One at a time: a second would need a second set
/// of plane readers, and the draw asks for one.
inline FigureDecode figure_decode;

#ifndef __mos__
/// One whole figure in one call, for callers with no frame to protect.
///
/// The host tests want a figure finished when the call returns; the target
/// slices it instead, and is fenced out of here so a blocking call cannot be
/// added there by accident. See FigureDecode for the layout and why.
[[nodiscard]] inline bool decode_figure(
    Place file2, uint32_t file_bytes, const ImageEntry& entry, Place glyphs) {
    if (!figure_decode.begin(file2, file_bytes, entry, glyphs))
        return false;
    while (figure_decode.busy())
        figure_decode.step();
    return figure_decode.good();
}
#endif

} // namespace agos
