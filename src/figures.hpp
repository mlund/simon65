// SPDX-License-Identifier: GPL-3.0-or-later

// A zone's figures: decoded into Attic when they are first drawn, copied
// from there into the glyph pool.
//
// Decoding is slow -- 11 frames for one image at worst -- so it happens once
// per image, and only for the images a scene actually draws; decoding a whole
// zone at its first sight costs 21 frames inside a tick that has 50. A figure
// is four-bit cells, and its palette block (vga.cpp:630) is only known when
// something draws it, so the block goes in the cell's colour byte at
// placement and the pixels never carry it.
//
// The directory lives in Attic as well. A zone has up to 255 images and near
// memory has no room for a table that size; what stays near is only the few
// figures resident in the pool.

#pragma once

#include "atticmap.hpp"
#include "chipmap.hpp"
#include "diagnostics.hpp"
#include "planar.hpp"
#include "zonepix.hpp"

#ifdef __mos__
#include "banks.hpp"
#else
#define ROOM_BANKED
#define DISPLAY_BANKED
#endif

namespace agos {

#ifdef __mos__
/// What the door below was asked to decode, and what to decode it from.
/// banked_call takes a function of no arguments, so the request travels in
/// these -- the pixels included, because the cache keeps no pointer to them.
inline uint8_t decode_zone = 0;
inline uint16_t decode_wanted = 0;
inline PackedZones::Pixels decode_from = {};
extern "C" void figures_decode_banked();
#endif

/// What a figure occupies, in glyphs: its art plus a blank above and below
/// every column, so the vertical offset has somewhere to read from.
[[nodiscard]] inline constexpr uint16_t glyph_span(uint8_t cells, uint8_t rows) {
    return static_cast<uint16_t>(cells * (rows + 2));
}

/// A figure resident in the glyph pool, for one image and one palette block.
struct Figure {
    uint16_t glyph = 0; //!< its first glyph in the pool
    uint8_t cells = 0;
    uint8_t rows = 0; //!< its art's height in glyphs

    /// A column is two glyphs taller than its art: one blank above and one
    /// below, so the vertical offset has somewhere to read from.
    [[nodiscard]] uint8_t stride() const {
        return static_cast<uint8_t>(rows + 2);
    }
    [[nodiscard]] bool valid() const {
        return rows != 0;
    }
};

/// The Attic arena of decoded glyphs, in 256-byte pages.
///
/// A room draws its own zone's sprites and the characters', and a character
/// lives in a zone of its own, so several zones' figures are kept at once. A
/// zone evicted and wanted again costs a card read and a whole decode,
/// measured at 420 milliseconds inside a tick that has 50; the intro draws
/// from five zones at once. A median zone's glyphs are 66 KiB and the worst
/// 814.
///
/// One arena, not a slot per zone. A bank is a construct for *code* -- 8 KB
/// the CPU maps into a window so instructions can be fetched -- and decoded
/// glyphs are data, reached by DMA at a 28-bit address, so they can lie
/// anywhere. Six fixed slots would buy one thing, O(1) addressing, at the
/// cost of a 682 KB ceiling per zone, only six zones at once, and re-letting
/// that puts one zone's name over another zone's pixels.
///
/// Pages, as PackedZones does (zonepix.hpp): four glyphs to a page, so a page
/// is glyph-aligned, an address is a shift rather than a 32-bit multiply, and
/// the bump is a uint16 over 16,384 of them.
inline constexpr uint16_t PAGE_BYTES = 256;
inline constexpr uint16_t ARENA_PAGES = static_cast<uint16_t>(atticmap::FIGURES_BYTES / PAGE_BYTES);
inline constexpr uint16_t NO_PAGE = 0xFFFF;

/// How many decoded figures the arena can name at once.
///
/// A power of two, so the ring wraps by a mask. The rows live in the Attic
/// (atticmap::FIGURE_INDEX), where they cost nothing: near memory is the
/// scarce thing, and five near arrays could name only 128 figures -- 6% of a
/// 4 MiB arena -- so figures would be re-decoded for want of a name, not of
/// bytes.
inline constexpr uint16_t RING_ROWS = 2048;
inline constexpr uint16_t NO_ROW = 0xFFFF;

/// Eight bytes a row: zone, image, page, cells, rows and a spare, so a row's
/// place is a shift rather than a multiply -- the shape the arena trace uses.
inline constexpr uint8_t ROW_BYTES = 8;

/// What near memory keeps: a hash of (zone, image) to a row, two rows a
/// bucket, one kilobyte.
///
/// Two ways rather than direct-mapped, measured rather than chosen.
/// Direct-mapped is nearly worthless -- 0.88 of the decodes at a 30% revisit
/// rate and 0.99 at 80% -- because two zones are live at once, their cel runs
/// collide systematically, and a collision *thrashes*: each insert throws out
/// the other. A second way absorbs that pair and reaches 0.81 / 0.51 / 0.51
/// against a perfect 2,048-row index's 0.60 / 0.41 / 0.45.
inline constexpr uint16_t HINT_BUCKETS = 256;
inline constexpr uint8_t HINT_WAYS = 2;

/// No zone at all. Zone 0 is Simon's own, so absence needs its own number.
inline constexpr uint8_t ROW_NO_ZONE = 0xFF;

/// What a mask's cut of the picture holds its place under (masks.hpp): not
/// a zone the game has, beside the pool's text and composite zones.
inline constexpr uint8_t MASK_ZONE = 0xFC;

/// One decoded figure, as the Attic holds it.
struct Row {
    uint8_t zone = ROW_NO_ZONE;
    uint16_t image = 0;
    uint16_t page = 0;
    uint8_t cells = 0;
    uint8_t rows = 0;
    uint8_t ahead = 0; //!< decoded ahead of being asked for, in the row's last byte
};

/// Where a page sits, and how many a figure wants.
[[nodiscard]] inline constexpr Place arena_at(uint16_t page) {
    return atticmap::FIGURES + (static_cast<Place>(page) << 8);
}

/// Where a row sits.
[[nodiscard]] inline constexpr Place row_at_place(uint16_t row) {
    return atticmap::FIGURE_INDEX + (static_cast<Place>(row) << 3);
}

static_assert(RING_ROWS * ROW_BYTES == atticmap::FIGURE_INDEX_BYTES,
    "the ring must fill the region the map reserves for it");

/// How many figures the pool holds at once.
///
/// Tenants, not slots: a figure takes the glyphs it needs and the pool is
/// packed end to end. Equal slots would make every figure fit an eleventh of
/// the pool, and 101 of the 624 images in the intro's zones do not -- its
/// title screen is 320 by 85, 33,280 bytes against a slot of 6,016.
///
/// Eleven bytes apiece, in the reserved low memory the VM tables use, which
/// has the room the fixed region has not. One scene wants eight figures a
/// frame and keeps them for many frames; twelve tenants left the table full
/// of the frame before. Thirty-two hold a frame's sprites and its merged
/// composites: about 22 sprites and 5 merged figures in the pot room.
inline constexpr uint8_t TENANTS = 32;

/// The pool measured in glyphs, which is what a tenant's place and span are
/// counted in: a glyph number is what the display asks for, and 16 bits hold
/// a thousand of them where the byte count of a figure does not fit.
inline constexpr uint16_t POOL_GLYPHS = static_cast<uint16_t>(chipmap::FIGURES_BYTES / GLYPH_BYTES);

static_assert(chipmap::FIGURES % GLYPH_BYTES == 0, "the pool's base must be a glyph number");

/// A figure's pages: its glyphs, rounded up to a page.
[[nodiscard]] inline constexpr uint16_t pages_for(uint8_t cells, uint8_t rows) {
    return static_cast<uint16_t>((glyph_span(cells, rows) + 3) >> 2);
}

class FigureCache {
  public:
    /// Everything the cache holds, emptied.
    ///
    /// Explicit rather than left to the crt's zeroing of .bss: a restart must
    /// empty it too. Three stores cost less than a loop over four arrays.
    void begin() {
        kept_first_ = kept_count_ = 0;
        next_page_ = 0;
        forget_pool();
        decodes_this_frame_ = 0;
        decoded_ = too_big_ = undecoded_ = hint_miss_ = given_up_ = wraps_ = 0;
        ahead_decoded_ = ahead_used_ = ahead_abandoned_ = 0;
        too_wide_ = crowded_ = 0;
        for (uint16_t at = 0; at < HINT_BUCKETS * HINT_WAYS; ++at)
            hint_[at] = NO_ROW;
    }

    /// Begin a frame's requests.
    ///
    /// What the pool already holds stays; what this frame has asked for cannot
    /// be taken by a later request in the same frame. Without that, the tenth
    /// layer could evict the figure the first was placed from, and the row
    /// would be built from another figure's pixels.
    ///
    /// Nor can what the list on screen names, which the beam is painting from:
    /// once a frame's list goes up its claims become SHOWN, and the list before
    /// it is off screen by the time this runs (the draw waits for the swap). A
    /// held draw puts nothing up, so what shows is still shown.
    void begin_frame() {
        for (uint8_t at = 0; at < TENANTS; ++at)
            flags_[at] = built_ != 0 ? ((flags_[at] & CLAIMED) != 0 ? SHOWN : 0)
                                     : static_cast<uint8_t>(flags_[at] & SHOWN);
        built_ = 0;
        decodes_this_frame_ = 0;
    }

    /// This frame's claims are what the list just built names.
    void list_built() {
        built_ = 1;
    }

    /// What the pool already holds and nothing else: no hint, no decode, no
    /// budget. A caller with somewhere to fall back to wants this rather than
    /// want(), which is always_inline and would emit the whole decode path a
    /// second time for a lookup that never reaches it. Claimed like any other
    /// hit, so the frame cannot evict what it is about to draw.
    [[nodiscard]] [[gnu::noinline]] Figure held(uint8_t zone, uint16_t image, uint8_t block) {
        const uint8_t at = find(zone, image, block);
        if (at == TENANTS)
            return {};
        flags_[at] = static_cast<uint8_t>(flags_[at] | CLAIMED);
        return {glyph_of(at), holds_cells_[at], holds_rows_[at]};
    }

    /// Whether a cel can be had without a decode: in the pool, or decoded in
    /// the Attic -- or known not to be there, which no wait would change.
    [[nodiscard]] bool ready(uint8_t zone, uint16_t image, uint8_t block) const {
        Row row;
        return find(zone, image, block) != TENANTS || hinted(zone, image, &row);
    }

    /// Decode a cel that is not in the Attic, or carry on with it, taking
    /// nothing from the pool: want()'s miss, and the draw's when it holds a
    /// frame whose glyphs must stay where the screen shows them. False when the
    /// frame's budget is spent and nothing ran.
    [[gnu::noinline]] bool ask(uint8_t zone, uint16_t image, PackedZones::Pixels from) {
        // One a frame: a decode is 11 frames at worst and a scene can want a
        // dozen new images at once, so the sprites that miss the budget are
        // drawn a frame or two later, which is what the eye does not see.
        if (!from.valid() || decodes_this_frame_ == DECODES_A_FRAME)
            return false;
        ++decodes_this_frame_;
        // Demand before guesses: decode_start ignores a cel while anything is
        // in flight, so a guess left would be stepped instead of it. A guess
        // at this very cel, the likeliest, is kept and becomes demand.
        if (decoding() && doing_ahead_ != 0) {
            if (doing_zone_ == zone && doing_image_ == image) {
                doing_ahead_ = 0;
            } else {
                figure_decode.abandon();
                ++ahead_abandoned_;
            }
        }
        decode_one(zone, image, from);
        return true;
    }

    /// The figure for this image and block, copied into the pool if it is not
    /// there already, decoded into the arena if it is not there either.
    ///
    /// @p from is the zone's packed pixels, looked up by the caller, which has
    /// to ask anyway to know whether the zone is here at all. The cache keeps
    /// no pointer into the pixel arena: a remembered pointer goes stale, and
    /// that was every fault this class has had.
    ///
    /// Null if the image has none, if there are no pixels to decode from, or if
    /// the pool is full of figures this frame has already placed.
    [[nodiscard]] [[gnu::always_inline]] Figure want(
        uint8_t zone, uint16_t image, uint8_t block, PackedZones::Pixels from) {
        // What the pool already holds, answered without touching the Attic. A
        // tenant carries its own shape, and the draw asks for every sprite every
        // frame.
        if (const uint8_t held = find(zone, image, block); held != TENANTS) {
            flags_[held] = static_cast<uint8_t>(flags_[held] | CLAIMED);
            return {glyph_of(held), holds_cells_[held], holds_rows_[held]};
        }
        Row row;
        if (!hinted(zone, image, &row)) {
            ++hint_miss_;
            if (!ask(zone, image, from) || !hinted(zone, image, &row))
                return {};
        }
        const uint8_t cells = row.cells, rows = row.rows;
        if (rows == 0)
            return {}; // asked for and not there

        const uint8_t at = take(glyph_span(cells, rows));
        if (at == TENANTS)
            return {};
        copy_in(arena_at(row.page), at);
        if (row.ahead != 0)
            ++ahead_used_; // a guess used, again on each re-copy
        holds_zone_[at] = zone;
        holds_image_[at] = image;
        holds_block_[at] = block;
        holds_cells_[at] = cells;
        holds_rows_[at] = rows;
        return {glyph_of(at), cells, rows};
    }

    /// A place for a line of text, cleared and ready to be drawn into.
    ///
    /// Text is not an image: it has no zone and no directory row, so it cannot
    /// come through want(). What it shares is the pool, the placement and the
    /// layering, which is the whole of what a figure is once it is glyphs.
    /// @p fresh says the glyphs are new and the caller must render into them; a
    /// line already there is kept, so a speech that stands for a hundred frames
    /// is drawn once.
    ///
    /// Keyed by @p id in a zone of nought, which is not a zone (main.cpp) and
    /// so cannot collide with an image's key.
    [[nodiscard]] Figure text_slot(uint16_t id, uint8_t cells, uint8_t rows, bool* fresh) {
        return blank_slot(TEXT_ZONE, id, cells, rows, fresh);
    }

    /// A place for stacked sprites merged into one figure (composite.hpp),
    /// keyed by @p id under a zone of its own. The caller takes a new id for
    /// every merge, so a held slot always has the shape it was asked for.
    [[nodiscard]] Figure composite_slot(uint16_t id, uint8_t cells, uint8_t rows, bool* fresh) {
        return blank_slot(COMPOSITE_ZONE, id, cells, rows, fresh);
    }

    /// A place for a mask's cut of the picture (masks.hpp), keyed by @p id
    /// under a zone of its own; full colour, so twice a figure's cells.
    [[nodiscard]] Figure mask_slot(uint16_t id, uint8_t cells, uint8_t rows, bool* fresh) {
        return blank_slot(MASK_ZONE, id, cells, rows, fresh);
    }

    /// Every figure of @p zone forgotten, and every merge, as one may hold
    /// such a figure: the zone's pixels changed under its name (the beard,
    /// 182/183). Cold, so a walk of the ring is fine. A tenant keeps its
    /// place, as one on screen must, under a block no request asks for.
    void forget_zone(uint8_t zone) {
        for (uint16_t n = 0; n < kept_count_; ++n) {
            const Place at = row_at_place(row_at(n));
            if (far_read8(at) == zone)
                far_write8(at, ROW_NO_ZONE);
        }
        for (uint8_t at = 0; at < TENANTS; ++at)
            if (holds_zone_[at] == zone || holds_zone_[at] == COMPOSITE_ZONE)
                holds_block_[at] = NO_BLOCK;
        // A decode in flight would record the old pixels under the zone's name.
        if (decoding() && doing_zone_ == zone) {
            figure_decode.abandon();
            ++given_up_;
        }
    }

    /// The Attic row of a decoded image, read rather than placed: a mask's
    /// pixels only shape its cut. False if the arena has no answer yet; an
    /// answer of no rows is an image that will not decode.
    [[nodiscard]] bool decoded_row(uint8_t zone, uint16_t image, Row* into) const {
        return hinted(zone, image, into);
    }

    /// Where the merged figure @p id sits, claimed for the frame, or nought if
    /// the pool no longer holds it. Finds only: a replayed frame must not take
    /// a slot it will not paint.
    [[nodiscard]] uint16_t claim_composite(uint16_t id) {
        return held(COMPOSITE_ZONE, id, 0).glyph;
    }

  private:
    /// Copy a figure in from Attic to its place in the pool.
    ///
    /// Straight, with no tint: a figure cell is four-bit, and its palette block
    /// travels in the cell's colour byte, not in the pixels. In steps, because
    /// a DMA job counts bytes in 16 bits and a figure's height is the data's.
    void copy_in(Place from, uint8_t at) {
        const Place to = chipmap::FIGURES + Place{place_[at]} * GLYPH_BYTES;
        for (uint16_t done = 0; done < span_[at]; done += COPY_GLYPHS) {
            const uint16_t glyphs = static_cast<uint16_t>(
                span_[at] - done < COPY_GLYPHS ? span_[at] - done : COPY_GLYPHS);
            const Place off = Place{done} * GLYPH_BYTES;
            far_copy(from + off, to + off, static_cast<uint16_t>(glyphs * GLYPH_BYTES));
        }
    }

    /// Decode one image, through the door into the bank the decoder lives in.
    void decode_one(uint8_t zone, uint16_t image, PackedZones::Pixels from) {
#ifdef __mos__
        decode_zone = zone;
        decode_wanted = image;
        decode_from = from;
        banked_call(AGOS_ROOM_BANK, figures_decode_banked);
#else
        decode_now(zone, image, from);
#endif
    }

  public:
    /// Decode one image into the arena and remember where it went.
    ///
    /// Whatever the new run treads on is forgotten first. Every outcome records
    /// something: an image the zone has not got, or one that will not decode,
    /// leaves a row of no rows -- asked for and not there -- so the draw does
    /// not ask again every frame.
    void decode_now(uint8_t zone, uint16_t image, PackedZones::Pixels from) {
        decode_start(zone, image, from);
        while (decoding())
            decode_step();
    }

    /// Take on an image, or record straight away what will never decode.
    ///
    /// Nothing happens if a decode is already in flight: one at a time, and the
    /// figure that missed asks again next frame.
    [[gnu::always_inline]] void decode_start(
        uint8_t zone, uint16_t image, PackedZones::Pixels from, bool ahead = false) {
        if (decoding())
            return;
        doing_ahead_ = ahead ? 1 : 0;
        if (from.valid()) {
            const ImageEntry entry = image_entry(from.at, image);
            if (image < image_table_rows(from.at, from.bytes) && entry.plausible()) {
                const uint8_t cells = static_cast<uint8_t>(entry.width_px / 16);
                const uint8_t rows =
                    static_cast<uint8_t>((entry.height + GLYPH_SIDE - 1) / GLYPH_SIDE);
                const uint16_t page = take_pages(pages_for(cells, rows));
                if (page != NO_PAGE) {
                    if (figure_decode.begin(from.at, from.bytes, entry, arena_at(page))) {
                        doing_emptied_ = zone_pixels.emptied();
                        doing_zone_ = zone;
                        doing_image_ = image;
                        doing_page_ = page;
                        doing_cells_ = cells;
                        doing_rows_ = rows;
                        return;
                    }
                    ++undecoded_; // a refusal, and it would be invisible
                }
                // The pages stay taken either way: rolling the bump back would break
                // the order the eviction walks in, and they die at the next wrap.
            }
        }
        record(zone, image, NO_PAGE, 0, 0);
    }

    /// One piece of the figure in flight. The row is recorded only when the
    /// figure is whole, so a lookup finds nothing until then and the sprite is
    /// skipped by the usual refusal path.
    [[gnu::always_inline]] void decode_step() {
        // The packed pixels this is reading can be thrown out between frames
        // (zonepix.hpp, hold). The planes cannot see it -- their bounds are still
        // in range -- so the figure would be recorded as whole and built from
        // another zone's bytes. Emptying is already counted, so noticing is one
        // compare.
        if (zone_pixels.emptied() != doing_emptied_) {
            figure_decode.abandon();
            ++given_up_;
            return; // nothing recorded: it is asked for again
        }
        figure_decode.step();
        if (figure_decode.busy())
            return;
        if (figure_decode.good()) {
            ++decoded_;
            if (doing_ahead_ != 0)
                ++ahead_decoded_;
            record(doing_zone_, doing_image_, doing_page_, doing_cells_, doing_rows_, doing_ahead_);
        } else {
            ++undecoded_;
            record(doing_zone_, doing_image_, NO_PAGE, 0, 0);
        }
    }

    /// Whether a figure nobody has asked for yet is worth decoding ahead
    /// (main.cpp): not decoded already, nor known not to be there.
    [[nodiscard]] bool worth_ahead(uint8_t zone, uint16_t image) const {
        Row row;
        return !hinted(zone, image, &row);
    }

    [[nodiscard]] uint16_t ahead_decoded() const {
        return ahead_decoded_;
    }
    [[nodiscard]] uint16_t ahead_used() const {
        return ahead_used_;
    }
    [[nodiscard]] uint16_t ahead_abandoned() const {
        return ahead_abandoned_;
    }

    [[nodiscard]] [[gnu::always_inline]] bool decoding() const {
        return figure_decode.busy();
    }

  private:
    [[nodiscard]] uint8_t find(uint8_t zone, uint16_t image, uint8_t block) const {
        for (uint8_t at = 0; at < TENANTS; ++at)
            if (holds(at) && holds_zone_[at] == zone && holds_image_[at] == image &&
                holds_block_[at] == block)
                return at;
        return TENANTS;
    }

    /// Whether a row holds anything. A row that has never been used has a zone
    /// of nought, which is a real zone number -- it is the span that says the
    /// row was ever given glyphs.
    [[nodiscard]] bool holds(uint8_t at) const {
        return holds_zone_[at] != NO_ZONE && span_[at] != 0;
    }

    /// Room for @p span glyphs, as a tenant holding it.
    ///
    /// Round the pool rather than into a slot: the free pointer walks forward,
    /// whatever it lands on that this frame has not spoken for is forgotten,
    /// and anything it has is stepped over -- which is how the engine's own
    /// arena places a zone (allocBlock, zones.cpp:120). Wrapping is what makes
    /// the pool a cache rather than a queue: a figure that has sat there since
    /// the room was painted is reached again before a fresh one displaces it.
    [[nodiscard]] uint8_t take(uint16_t span) {
        if (span > POOL_GLYPHS) {
            ++too_wide_; // larger than the whole pool: nothing can hold it
            return TENANTS;
        }
        for (uint8_t tries = 0; tries <= TENANTS; ++tries) {
            if (next_ + span > POOL_GLYPHS)
                next_ = 0;
            const uint16_t end = static_cast<uint16_t>(next_ + span);
            const uint8_t busy = spoken_for(next_, end);
            if (busy != TENANTS) {
                next_ = static_cast<uint16_t>(place_[busy] + span_[busy]);
                continue;
            }
            for (uint8_t at = 0; at < TENANTS; ++at)
                if (holds(at) && overlaps(at, next_, end))
                    holds_zone_[at] = NO_ZONE;
            uint8_t at = spare();
            if (at == TENANTS) {
                // Room in the pool but no row to record it in: the figures the table
                // holds lie elsewhere. Forget one that this frame has not placed and
                // come round again. Without this the table fills once and every
                // figure after it is refused -- 130 a second, an actor drawn as
                // whichever of its parts got in first.
                at = unclaimed();
                if (at == TENANTS)
                    break;
                holds_zone_[at] = NO_ZONE;
                continue;
            }
            place_[at] = next_;
            span_[at] = span;
            flags_[at] = CLAIMED;
            next_ = end;
            return at;
        }
        ++crowded_; // more figures in one frame than the pool holds at once
        return TENANTS;
    }

    [[nodiscard]] bool overlaps(uint8_t at, uint16_t start, uint16_t end) const {
        return place_[at] < end && start < place_[at] + span_[at];
    }

    /// A tenant in the way of [start, end) that may not be moved: one this
    /// frame has placed a layer from, or the list on screen names.
    [[nodiscard]] uint8_t spoken_for(uint16_t start, uint16_t end) const {
        for (uint8_t at = 0; at < TENANTS; ++at)
            if (holds(at) && flags_[at] != 0 && overlaps(at, start, end))
                return at;
        return TENANTS;
    }

    [[nodiscard]] uint8_t spare() const {
        for (uint8_t at = 0; at < TENANTS; ++at)
            if (!holds(at))
                return at; // never used, or forgotten
        return TENANTS;
    }

    /// A tenant neither this frame nor the screen holds, to make room in the
    /// table rather than in the pool.
    [[nodiscard]] uint8_t unclaimed() const {
        for (uint8_t at = 0; at < TENANTS; ++at)
            if (flags_[at] == 0)
                return at;
        return TENANTS;
    }

    /// Where a tenant's glyphs begin, as the display counts them.
    [[nodiscard]] uint16_t glyph_of(uint8_t at) const {
        return static_cast<uint16_t>(chipmap::FIGURES / GLYPH_BYTES + place_[at]);
    }

  public:
    /// How many tenants the pool holds, counted rather than tallied: a slot is
    /// never re-let, so nothing would reset a tally and it would drift into a
    /// count of copies in.
    [[nodiscard]] uint16_t held() const {
        uint16_t held = 0;
        for (uint8_t at = 0; at < TENANTS; ++at)
            if (holds(at))
                ++held;
        return held;
    }
    /// The arena's live rows, oldest first. What a test asserts over, and what
    /// a reader walks straight out of the Attic.
    [[nodiscard]] uint16_t entries() const {
        return kept_count_;
    }
    [[nodiscard]] Row kept(uint16_t n) const {
        return read_row(row_at(n));
    }

    /// Lookups the hash could not answer, which is what a re-decode costs. The
    /// table is two-way and never maintained, so this is the price of both.
    [[nodiscard]] uint16_t hint_miss() const {
        return hint_miss_;
    }

    /// Decodes given up because the packed pixels went out under them.
    [[nodiscard]] uint16_t given_up() const {
        return given_up_;
    }

    [[nodiscard]] uint16_t decoded() const {
        return decoded_;
    }
    /// Times the arena came round to its start: until it does, nothing decoded
    /// has been forgotten for want of room.
    [[nodiscard]] uint16_t wraps() const {
        return wraps_;
    }

    /// Images this store could not take, by reason: an image outran the whole
    /// arena, would not decode, or outran the pool.
    /// Each is a drop that otherwise looks like an image that never existed.
    [[nodiscard]] uint16_t too_big() const {
        return too_big_;
    }
    [[nodiscard]] uint16_t undecoded() const {
        return undecoded_;
    }
    [[nodiscard]] uint16_t too_wide() const {
        return too_wide_;
    }
    [[nodiscard]] uint16_t crowded() const {
        return crowded_;
    }

  private:
    /// What text holds its place under. Not a zone the game has: zone 0 is
    /// Simon's own, and 0xFF marks a row holding nothing at all.
    static constexpr uint8_t TEXT_ZONE = 0xFE;
    static constexpr uint8_t COMPOSITE_ZONE = 0xFD;
    /// A palette block past the sixteen, so a tenant under it is never found.
    static constexpr uint8_t NO_BLOCK = 0xFF;

    /// A run of the pool for glyphs that are not an image, keyed by @p id under
    /// @p zone, and claimed for the frame either way.
    [[nodiscard]] Figure blank_slot(
        uint8_t zone, uint16_t id, uint8_t cells, uint8_t rows, bool* fresh) {
        const uint8_t held = find(zone, id, 0);
        if (held != TENANTS) {
            flags_[held] = static_cast<uint8_t>(flags_[held] | CLAIMED);
            *fresh = false;
            return {glyph_of(held), cells, rows};
        }
        const uint8_t at = take(glyph_span(cells, rows));
        if (at == TENANTS)
            return {};
        holds_zone_[at] = zone;
        holds_image_[at] = id;
        holds_block_[at] = 0;
        *fresh = true;
        return {glyph_of(at), cells, rows};
    }

    void forget_pool() {
        for (uint8_t at = 0; at < TENANTS; ++at) {
            holds_zone_[at] = NO_ZONE;
            holds_image_[at] = 0xFFFF;
            holds_block_[at] = 0;
            flags_[at] = 0;
        }
        built_ = 0;
        next_ = 0;
    }

    /// The row holding this (zone, image), if the hash still points at it.
    ///
    /// The hint is never required to be correct, and that is what makes it
    /// cheap: a stale entry points at a row that has since been recycled or
    /// dropped, the row's own zone and image say so, and the cost is one
    /// re-decode rather than another figure's pixels. Nothing maintains the
    /// table on eviction.
    ///
    /// Two far reads at worst, on a pool miss only -- the pool answers the
    /// common case above.
    [[nodiscard]] [[gnu::noinline]] bool hinted(uint8_t zone, uint16_t image, Row* into) const {
        const uint16_t bucket = bucket_of(zone, image);
        for (uint8_t way = 0; way < HINT_WAYS; ++way) {
            const uint16_t row = hint_[bucket + way];
            if (row == NO_ROW || !live(row))
                continue;
            *into = read_row(row);
            if (into->zone == zone && into->image == image)
                return true;
        }
        return false;
    }

    /// Which pair of ways a key falls in.
    ///
    /// Cels are consecutive within a zone, so the low bits of the image spread
    /// a scene's own figures perfectly; the zone is folded in because two zones
    /// are live at once and their cel runs would otherwise land on top of each
    /// other. An exclusive-or keeps a run collision-free, being a bijection.
    [[nodiscard]] static uint16_t bucket_of(uint8_t zone, uint16_t image) {
        const uint16_t at = static_cast<uint16_t>(
            (image ^ (static_cast<uint16_t>(zone) << 4)) & (HINT_BUCKETS - 1));
        return static_cast<uint16_t>(at * HINT_WAYS);
    }

    /// Whether a row is still inside the ring's live window.
    ///
    /// A row recycled by record() is caught by the zone and image it now holds,
    /// which is what t_figures tests. This covers the other way a row goes
    /// stale, which that test cannot cheaply provoke: drop_over lets a row go
    /// while its bytes stand, and the bump then hands its pages to something
    /// else. The shape would still match and the pixels would not be its own.
    /// Near arithmetic, no Attic.
    [[nodiscard]] bool live(uint16_t row) const {
        const uint16_t since = static_cast<uint16_t>((row - kept_first_) & (RING_ROWS - 1));
        return since < kept_count_;
    }

    /// One row. Five far calls rather than a DMA job: eight bytes is well under
    /// the 23-byte threshold where DMA setup pays for itself, and measured here
    /// the job is 100 bytes of code larger.
    [[nodiscard]] static Row read_row(uint16_t row) {
        const Place at = row_at_place(row);
        return {far_read8(at),
            far_read16_le(at + 1),
            far_read16_le(at + 3),
            far_read8(at + 5),
            far_read8(at + 6),
            far_read8(at + 7)};
    }

    /// Room for @p pages, as the page it starts at.
    ///
    /// Bump and wrap, forgetting whatever the new run lands on -- which is how
    /// take() places a tenant in the chip pool and how the engine's own arena
    /// places a zone. Nothing here is pinned: the only reader of a decoded
    /// figure is copy_in(), inside the want() that looked it up, and what the
    /// display reads is the copy. So the rows are in bump order, and
    /// what a run overlaps is always the oldest of them.
    [[nodiscard]] uint16_t take_pages(uint16_t pages) {
        if (pages == 0 || pages > ARENA_PAGES) {
            ++too_big_; // larger than the whole arena
            return NO_PAGE;
        }
        if (pages > ARENA_PAGES - next_page_) {
            drop_over(next_page_, ARENA_PAGES); // the stub at the top is abandoned
            next_page_ = 0;
            ++wraps_;
        }
        const uint16_t end = static_cast<uint16_t>(next_page_ + pages);
        drop_over(next_page_, end);
        const uint16_t at = next_page_;
        if (end == ARENA_PAGES) {
            next_page_ = 0;
            ++wraps_;
        } else {
            next_page_ = end;
        }
        return at;
    }

    /// The oldest rows, while they lie in [from, to).
    ///
    /// A far read an iteration, but only on a decode, and each row is dropped
    /// once.
    void drop_over(uint16_t from, uint16_t to) {
        while (kept_count_ != 0) {
            const Row head = read_row(kept_first_);
            const uint16_t at = head.page;
            const uint16_t pages = pages_for(head.cells, head.rows);
            // A row of no pages is bookkeeping, not storage: it would stand at the
            // head for ever, so it goes whenever the head is reached.
            if (pages != 0 && !(at < to && from < at + pages))
                break;
            drop_oldest();
        }
    }

    void drop_oldest() {
        kept_first_ = static_cast<uint16_t>((kept_first_ + 1) & (RING_ROWS - 1));
        --kept_count_;
    }

    /// One decoded figure, at the young end of the ring.
    void record(uint8_t zone,
        uint16_t image,
        uint16_t page,
        uint8_t cells,
        uint8_t rows,
        uint8_t ahead = 0) {
        if (kept_count_ == RING_ROWS)
            drop_oldest(); // its pages wait for the bump to reach them
        const uint16_t row = row_at(kept_count_);
        const Place put = row_at_place(row);
        const uint8_t bytes[ROW_BYTES] = {zone,
            static_cast<uint8_t>(image),
            static_cast<uint8_t>(image >> 8),
            static_cast<uint8_t>(page),
            static_cast<uint8_t>(page >> 8),
            cells,
            rows,
            ahead};
        far_write(put, bytes, ROW_BYTES);
        ++kept_count_;
        // The older way falls out. Nothing checks what it held: whatever points
        // at it is caught by the row's own zone and image.
        const uint16_t bucket = bucket_of(zone, image);
        hint_[bucket] = hint_[bucket + 1];
        hint_[bucket + 1] = row;
        trace::log_entry(bytes);
    }

    [[nodiscard]] uint16_t row_at(uint16_t n) const {
        return static_cast<uint16_t>((kept_first_ + n) & (RING_ROWS - 1));
    }

    /// The most one DMA job moves: 512 glyphs, 32 KB, well inside its 16 bits.
    static constexpr uint16_t COPY_GLYPHS = 512;

    /// A tenant this frame has placed a layer from, which a later request in
    /// the same frame may not take.
    static constexpr uint8_t CLAIMED = 1;
    /// A tenant the list on screen names, which no request may take until the
    /// next list goes up.
    static constexpr uint8_t SHOWN = 2;

    /// No zone is numbered this, so it marks a slot that holds none.
    static constexpr uint8_t NO_ZONE = 0xFF;

    /// How many images one frame may decode.
    ///
    /// A backdrop takes 580 ms, but a cel is smaller and wanted far more
    /// often: the intro's actors step a new one every eight ticks, and with
    /// one decode a frame the first magic act decodes 5 to 20 images a second
    /// while only one or two layers reach the screen.
    ///
    /// Measured with the demand counters, the animation asks for about one cel
    /// a tick that the pool has not got. With one decode in flight the decoder
    /// returned 0.41 of them (43%), because a decode spans a couple of ticks, and
    /// widening the slice did not move it: the limit is how many are in flight.
    static constexpr uint8_t DECODES_A_FRAME = 6;
    /// Counted up from nought, not down from the cap: a non-zero default puts
    /// the whole cache in .data, whose image ram_fixed has no room for.
    uint8_t decodes_this_frame_ = 0;

    /// Which row the hash points at, two ways to a bucket, youngest last.
    ///
    /// The only part of the index in near memory. A cel number needs sixteen
    /// bits: zone 8's image table has 1,093 rows and the intro's actors step
    /// cels in the hundreds. A byte key refuses every cel above 255, and the
    /// scene plays with nothing in it.
    uint16_t hint_[HINT_BUCKETS * HINT_WAYS] = {};

    /// The ring over the Attic rows, in bump order -- see take_pages -- so the
    /// oldest row is always the first the bump will tread on.
    uint16_t kept_first_ = 0, kept_count_ = 0;
    uint16_t next_page_ = 0; //!< the bump, in pages
    uint8_t holds_zone_[TENANTS] = {};
    uint16_t holds_image_[TENANTS] = {};
    uint8_t holds_block_[TENANTS] = {};
    /// What each tenant is, so a hit answers without reading the arena.
    uint8_t holds_cells_[TENANTS] = {};
    uint8_t holds_rows_[TENANTS] = {};

    /// Where each tenant sits in the pool and how much of it it holds, in
    /// glyphs, and where the next figure goes.
    uint16_t place_[TENANTS] = {};
    uint16_t span_[TENANTS] = {};
    uint16_t next_ = 0;
    uint16_t decoded_ = 0;
    uint16_t wraps_ = 0;
    uint16_t too_big_ = 0;
    uint16_t undecoded_ = 0;
    uint16_t too_wide_ = 0;
    uint16_t crowded_ = 0;
    uint16_t hint_miss_ = 0;
    uint16_t given_up_ = 0;

    /// The figure in flight, kept until its last piece is done.
    uint16_t doing_image_ = 0, doing_page_ = 0, doing_emptied_ = 0;
    uint8_t doing_zone_ = 0, doing_cells_ = 0, doing_rows_ = 0;
    uint8_t doing_ahead_ = 0;
    uint16_t ahead_decoded_ = 0;   //!< figures decoded ahead of being asked for
    uint16_t ahead_used_ = 0;      //!< copy-ins from guessed rows, re-copies too
    uint16_t ahead_abandoned_ = 0; //!< guesses given up for a cel asked for

    uint8_t flags_[TENANTS] = {};
    uint8_t built_ = 0; //!< the last draw put its list up
};

} // namespace agos
