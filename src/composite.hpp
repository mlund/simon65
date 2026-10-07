// Stacked sprites merged into one figure, so a row pays for their union once.
//
// Busy rooms stack sprites in one place -- the pot room's gnome is five -- and
// each costs a token and its own cells on every row it covers. Painted into
// one figure in the order they would have been laid, they cost one token and
// the cells of their union. The picture does not change: the VM's sprite list
// is untouched, and painting every sprite onto one buffer in priority order is
// what ScummVM and the CD32 original do for the whole screen.
//
// The merge is the CPU's. DMA's transparency is a whole byte, and half the
// painted bytes of a four-bit figure have one transparent nybble.

#pragma once

#include "figures.hpp"
#include "placed.hpp"
#include "rrb.hpp"

#ifdef __mos__
#include "banks.hpp"
#else
#define COMPOSITE_BANKED
#endif

namespace composite {

using agos::Place;

/// Layers the grouping can take, one bit apiece in a mask.
inline constexpr uint8_t MAX_LAYERS = 24;
/// Merged figures remembered from one frame to the next: the pot room's peak
/// is 7 at once. A frame with more is merged again rather than replayed.
inline constexpr uint8_t MAX_KEPT = 7;
/// Past these a group is left as laid: a sprite partly above the picture has
/// a top row that wraps, and its union would claim the whole pool.
inline constexpr uint8_t MAX_CELLS = 20;
inline constexpr uint8_t MAX_ROWS = 25;
/// What one pass of the merge moves through near memory.
inline constexpr uint16_t CHUNK = 128;
/// One pixel line of a glyph, and a four-bit cell's width in pixels. A
/// member half a cell across lands half of each line on each of two columns.
inline constexpr uint8_t LINE_BYTES = chipmap::GLYPH_BYTES / chipmap::CELL_LINES;
inline constexpr uint8_t HALF_LINE = LINE_BYTES / 2;
inline constexpr uint8_t FOUR_BIT_PX = 2 * chipmap::CELL_LINES;

/// A layer's extent in pixels, its art only: glyph rows, so a little generous,
/// which only ever errs towards not merging.
struct Box {
    int16_t x0, x1, y0, y1;
};

[[nodiscard, gnu::always_inline]] inline Box box_of(const Placed& p) {
    const int16_t y0 = y_of(p);
    // Every cell sixteen pixels: the compositor runs before the line of speech
    // is added, so it only ever sees four-bit figures.
    return {static_cast<int16_t>(p.at),
        static_cast<int16_t>(p.at + p.cells * FOUR_BIT_PX),
        y0,
        static_cast<int16_t>(y0 + (p.rows - 1) * chipmap::CELL_LINES)};
}

[[nodiscard, gnu::always_inline]] inline bool overlaps(const Box& a, const Box& b) {
    return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}

[[nodiscard, gnu::always_inline]] inline Box joined(const Box& a, const Box& b) {
    return {a.x0 < b.x0 ? a.x0 : b.x0,
        a.x1 > b.x1 ? a.x1 : b.x1,
        a.y0 < b.y0 ? a.y0 : b.y0,
        a.y1 > b.y1 ? a.y1 : b.y1};
}

[[nodiscard, gnu::always_inline]] inline uint8_t cells_of(const Box& b) {
    return static_cast<uint8_t>(static_cast<uint16_t>(b.x1 - b.x0 + FOUR_BIT_PX - 1) / FOUR_BIT_PX);
}

[[nodiscard, gnu::always_inline]] inline uint8_t art_rows_of(const Box& b) {
    return static_cast<uint8_t>(
        static_cast<uint16_t>(b.y1 - b.y0 + chipmap::CELL_LINES - 1) / chipmap::CELL_LINES);
}

/// What a layer of this extent costs the display list: a token and its cells
/// on every screen row it reaches. Rows count, not just width: a tall thin
/// sprite merged with a short wide one would pay the width all the way down.
[[nodiscard, gnu::always_inline]] inline uint16_t cost_of(const Box& b) {
    // Shifts, not division: they floor, which counts a row a layer above the
    // picture straddles rather than rounding it away. CELL_LINES is eight.
    static_assert(chipmap::CELL_LINES == 8);
    const auto rows = static_cast<uint8_t>(((b.y1 - 1) >> 3) - (b.y0 >> 3) + 1);
    return static_cast<uint16_t>(rows * (1 + cells_of(b)));
}

/// Four-bit and unmirrored. Text is full colour, and a mirrored member would
/// need its nybbles swapped in software.
[[nodiscard, gnu::always_inline]] inline bool joinable(const Placed& p) {
    return (p.flags & (rrb::FOUR_BIT | rrb::FLIP_HORIZONTAL)) == rrb::FOUR_BIT;
}

/// Keep the nybbles of @p dst that @p src leaves transparent.
[[nodiscard, gnu::always_inline]] inline uint8_t over(uint8_t dst, uint8_t src) {
    const uint8_t keep =
        static_cast<uint8_t>(((src & 0x0F) != 0 ? 0 : 0x0F) | ((src & 0xF0) != 0 ? 0 : 0xF0));
    return static_cast<uint8_t>((dst & keep) | src);
}

/// A merged figure kept from the frame before, known by a checksum of its
/// members' keys. Two running sums, so a change to any one field always shows
/// in the first; a multiply is a library call here, and full copies of the
/// members did not fit the bank.
struct Kept {
    uint32_t sum;
    uint16_t id;
    Placed layer; //!< the merged layer as laid, for a frame that replays it
    uint8_t head; //!< which layer of the input it stood in for
};

/// Two running sums over the input -- layers, images, zones -- by which a
/// frame the same as the last is known. Inline, so it runs in the caller's
/// bank and not in the compositor's, which has no room for it.
[[nodiscard, gnu::always_inline]] inline uint32_t checksum(
    const Placed* layers, uint8_t count, const uint8_t* zone, const uint16_t* image) {
    uint16_t first = 0, second = 0;
    const auto feed = [&](const void* at, uint8_t n) {
        const auto* bytes = static_cast<const uint8_t*>(at);
        for (uint8_t i = 0; i < n; ++i) {
            first = static_cast<uint16_t>(first + bytes[i]);
            second = static_cast<uint16_t>(second + first);
        }
    };
    for (uint8_t i = 0; i < count; ++i) {
        feed(&layers[i], sizeof(Placed));
        feed(&image[i], sizeof image[i]);
        feed(&zone[i], 1);
    }
    return uint32_t{second} << 16 | first;
}

/// Merges a frame's stacked layers. On the machine it lives in its own bank,
/// work and all, so it costs no near memory; nothing outside that bank may
/// read it.
class Compositor {
  public:
    /// Rework @p layers in place, merging stacks into one layer apiece, and
    /// say how many layers are left. @p zone and @p image say which figure
    /// each layer is, so an unchanged stack is found rather than merged again.
    /// A group the pool has no room for is left as laid. @p input is
    /// checksum() of the same three, worked out by the caller, outside this bank.
    [[nodiscard]] COMPOSITE_BANKED uint8_t run(agos::FigureCache& figures,
        Placed* layers,
        uint8_t count,
        const uint8_t* zone,
        const uint16_t* image,
        uint32_t input) {
        merged = 0;
        // Not dead: the caller's static_assert already keeps count in range, but
        // the bound is what lets the compiler narrow group()'s loops and masks --
        // 546 bytes of this bank without it.
        if (count > MAX_LAYERS)
            count = MAX_LAYERS;

        // Most frames lay what the last one laid: the sprite list changes about
        // eight times a second and the draw runs at twenty. The same input gives
        // the same output, so replay it and skip the grouping, which is the cost.
        if (input == last_input_ && count == last_count_) {
            if (const uint8_t out = replay(figures, layers, count); out != 0) {
                ++replays;
                return out;
            }
        }

        group(layers, count);

        Kept* const now = kept_[1 - side_];
        uint8_t now_count = 0;
        bool complete = true;
        for (uint8_t h = 0; h < count; ++h) {
            if (head_[h] != h || (members_[h] & (members_[h] - 1)) == 0)
                continue; // alone, or not a head
            const Box u = union_[h];
            const uint8_t cells = cells_of(u), rows = art_rows_of(u);
            if (cells > MAX_CELLS || rows > MAX_ROWS)
                continue;

            // Each member's figure and layer, which decide its pixels and place.
            // The layer as it stands, so a stack that walks is merged again; keying
            // on places within the group saved those, but cost about 500 bytes of a
            // bank with 500 free, and walking was in the 0.2% measured anyway.
            uint16_t first = 0, second = 0;
            for (uint8_t i = h; i < count; ++i)
                if (head_[i] == h) {
                    sum(&first,
                        &second,
                        reinterpret_cast<const uint8_t*>(&layers[i]),
                        sizeof(Placed));
                    sum(&first, &second, reinterpret_cast<const uint8_t*>(&image[i]), 2);
                    sum(&first, &second, &zone[i], 1);
                }
            const uint32_t checksum = uint32_t{second} << 16 | first;
            const uint16_t id = find(checksum);

            bool fresh = false;
            const agos::Figure slot = figures.composite_slot(id, cells, rows, &fresh);
            if (!slot.valid()) {
                complete = false; // the pool was full: try again next frame
                continue;
            }
            if (fresh) {
                paint(layers, count, h, u, slot);
                ++builds;
            }
            // Over the first member, which nothing reads again: every later group
            // starts past it and has its members past its own start. The union is
            // inside the picture already, so laid() and not placed(): the same
            // rounding without the clipping.
            layers[h] = laid(static_cast<uint16_t>(slot.glyph + 1),
                static_cast<uint16_t>(u.x0),
                u.y0,
                cells,
                rows,
                rrb::FOUR_BIT,
                layers[h].colour);
            if (now_count < MAX_KEPT)
                now[now_count++] = {checksum, id, layers[h], h};
            else
                complete = false; // one not kept, so this frame cannot be replayed
            members_[h] = 0;      // built: drop its members
            ++merged;
        }
        side_ = static_cast<uint8_t>(1 - side_);
        kept_count_ = now_count;

        uint8_t out = 0;
        uint32_t bit = 1;
        dropped_ = 0;
        for (uint8_t i = 0; i < count; ++i, bit <<= 1) {
            if (head_[i] == i || members_[head_[i]] != 0)
                layers[out++] = layers[i];
            else
                dropped_ |= bit;
        }
        last_input_ = complete ? input : 0;
        last_count_ = count;
        return out;
    }

    uint8_t merged = 0;   //!< figures merged this frame
    uint16_t builds = 0;  //!< merges painted, ever; the rest were found
    uint16_t replays = 0; //!< frames that laid the last one's output again

#ifdef __mos__
    /// Whether DMA can reach the CPU's buffers. If not, merges corrupt
    /// silently; the host lacks banks and cannot verify, so the machine tests
    /// at startup.
    [[nodiscard]] COMPOSITE_BANKED bool reaches() {
        for (uint8_t i = 0; i < 4; ++i)
            source_[i] = static_cast<uint8_t>(0xA5 ^ (i * 0x3C));
        for (uint8_t i = 0; i < 4; ++i)
            if (agos::far_read8(physical(source_) + i) != source_[i])
                return false;
        return true;
    }
#endif

  private:
    /// Which layers merge, into head_, members_ and union_.
    ///
    /// Greedy, in priority order: a layer joins the group whose rows it saves
    /// most, given that it touches a member, shares its palette block -- a
    /// four-bit cell has one colour byte -- and that nothing between the
    /// group's first member and it in the list touches any member. The merged
    /// figure is laid where the first member was, so a layer in between that
    /// touched a member would change sides; one that touches none can be passed
    /// freely, since the merged figure is transparent wherever no member is.
    COMPOSITE_BANKED void group(const Placed* layers, uint8_t count) {
        uint32_t bit = 1;
        for (uint8_t i = 0; i < count; ++i, bit <<= 1) {
            box_[i] = box_of(layers[i]);
            touch_[i] = 0;
            uint32_t other = 1;
            for (uint8_t j = 0; j < i; ++j, other <<= 1)
                if (overlaps(box_[i], box_[j])) {
                    touch_[i] |= other;
                    touch_[j] |= bit;
                }
        }

        bit = 1;
        for (uint8_t i = 0; i < count; ++i, bit <<= 1) {
            head_[i] = i;
            members_[i] = bit;
            union_[i] = box_[i];
            if (!joinable(layers[i]))
                continue;
            const uint8_t block = layers[i].colour >> 4;
            const uint16_t own = cost_of(box_[i]);
            uint8_t best = i;
            uint16_t best_gain = 0;
            // Downwards, so the layers strictly between h and i build up as a mask
            // without a shift by a variable count, which is a loop on this target.
            uint32_t between = 0, at = bit >> 1;
            for (uint8_t h = i; h-- > 0; between |= at, at >>= 1) {
                if (head_[h] != h || !joinable(layers[h]) || (layers[h].colour >> 4) != block ||
                    (touch_[i] & members_[h]) == 0 ||
                    ((between & ~members_[h]) & (touch_[h] | touch_[i])) != 0)
                    continue;
                const Box u = joined(union_[h], box_[i]);
                const uint16_t before = cost_of(union_[h]) + own;
                const uint16_t after = cost_of(u);
                if (before > after && before - after > best_gain) {
                    best_gain = static_cast<uint16_t>(before - after);
                    best = h;
                }
            }
            if (best == i)
                continue;
            head_[i] = best;
            members_[best] |= bit;
            union_[best] = joined(union_[best], box_[i]);
            touch_[best] |= touch_[i]; // a head's touch is now its whole group's
        }
    }

    /// Lay last frame's output again over the same input, if every merged
    /// figure it used is still in the pool where it was. Claimed, so this frame
    /// cannot evict it. One gone and the ids are forgotten, so the full pass
    /// takes new ones and paints them. The layers laid, or nought for that.
    [[nodiscard]] COMPOSITE_BANKED uint8_t replay(
        agos::FigureCache& figures, Placed* layers, uint8_t count) {
        const Kept* const before = kept_[side_];
        for (uint8_t n = 0; n < kept_count_; ++n)
            if (figures.claim_composite(before[n].id) + 1 != before[n].layer.first) {
                kept_count_ = 0;
                return 0;
            }
        uint8_t out = 0;
        uint32_t bit = 1;
        for (uint8_t i = 0; i < count; ++i, bit <<= 1) {
            if ((dropped_ & bit) != 0)
                continue;
            layers[out] = layers[i];
            for (uint8_t n = 0; n < kept_count_; ++n)
                if (before[n].head == i)
                    layers[out] = before[n].layer;
            ++out;
        }
        merged = kept_count_;
        return out;
    }

    /// The id of the merged figure last frame with exactly these members, or a
    /// new one. New for every merge, so a slot the pool still holds always has
    /// the shape it was taken for.
    [[nodiscard]] COMPOSITE_BANKED uint16_t find(uint32_t sum) {
        const Kept* const before = kept_[side_];
        for (uint8_t n = 0; n < kept_count_; ++n)
            if (before[n].sum == sum)
                return before[n].id;
        return ++serial_;
    }

    [[gnu::always_inline]] static void sum(
        uint16_t* first, uint16_t* second, const uint8_t* bytes, uint8_t n) {
        for (uint8_t i = 0; i < n; ++i) {
            *first = static_cast<uint16_t>(*first + bytes[i]);
            *second = static_cast<uint16_t>(*second + *first);
        }
    }

    /// Paint group @p h into the glyphs at @p glyph, laid out as a figure is:
    /// columns of art with a blank glyph above and below, numbered down. A
    /// column is then one run of eight-byte pixel rows, and a member's column
    /// lands on one at a pixel offset -- or, half a cell across, half of each
    /// of its rows on two.
    COMPOSITE_BANKED void paint(
        const Placed* layers, uint8_t count, uint8_t h, const Box& u, const agos::Figure& into) {
        agos::far_fill(Place{into.glyph} * chipmap::GLYPH_BYTES,
            0,
            static_cast<uint16_t>(agos::glyph_span(into.cells, into.rows) * chipmap::GLYPH_BYTES));
        bool bottom = true;
        for (uint8_t i = h; i < count; ++i) {
            if (head_[i] != h)
                continue;
            const Placed& m = layers[i];
            const auto across = static_cast<uint16_t>(m.at - u.x0);
            const auto k = static_cast<uint8_t>(across / FOUR_BIT_PX);
            const bool half = across % FOUR_BIT_PX != 0;
            const auto lines = static_cast<uint16_t>(box_[i].y0 - u.y0);
            const auto bytes = static_cast<uint16_t>((m.rows - 1) * chipmap::GLYPH_BYTES);
            for (uint8_t c = 0; c < m.cells; ++c) {
                const Place src =
                    Place{static_cast<uint16_t>(m.first + c * (m.rows + 1))} * chipmap::GLYPH_BYTES;
                const Place dst =
                    column(into, static_cast<uint8_t>(k + c)) + Place{lines} * LINE_BYTES;
                if (!half) {
                    if (bottom)
                        move(src, dst, bytes); // exact: nothing under it yet
                    else
                        merge(src, dst, bytes, 0, 0, LINE_BYTES);
                } else {
                    merge(src, dst, bytes, 0, HALF_LINE, HALF_LINE);
                    merge(src,
                        column(into, static_cast<uint8_t>(k + c + 1)) + Place{lines} * LINE_BYTES,
                        bytes,
                        HALF_LINE,
                        0,
                        HALF_LINE);
                }
            }
            bottom = false;
        }
    }

    /// Where art row nought of column @p c of @p f starts.
    [[nodiscard, gnu::always_inline]] static Place column(const agos::Figure& f, uint8_t c) {
        return Place{static_cast<uint16_t>(f.glyph + 1 + c * f.stride())} * chipmap::GLYPH_BYTES;
    }

    /// @p width bytes of each eight-byte row of @p src, from byte @p from,
    /// painted over @p dst's row at byte @p to; @p bytes is whole rows.
    COMPOSITE_BANKED void merge(
        Place src, Place dst, uint16_t bytes, uint8_t from, uint8_t to, uint8_t width) {
        for (uint16_t done = 0; done < bytes; done += CHUNK) {
            const uint16_t n = bytes - done < CHUNK ? static_cast<uint16_t>(bytes - done) : CHUNK;
            fetch(source_, src + done, n);
            fetch(target_, dst + done, n);
            // Most painted bytes are all transparent or all opaque; only the mixed
            // ones need the mask.
            for (uint16_t row = 0; row < n; row += LINE_BYTES) {
                const uint8_t* in = source_ + row + from;
                uint8_t* out = target_ + row + to;
                for (uint8_t b = 0; b < width; ++b) {
                    const uint8_t v = in[b];
                    if (v == 0)
                        continue;
                    out[b] = (v & 0x0F) != 0 && (v & 0xF0) != 0 ? v : over(out[b], v);
                }
            }
            store(dst + done, target_, n);
        }
    }

    /// Every DMA move the merge makes, in one place: far_copy is inline, and
    /// five copies of its megabyte split did not fit the bank.
    COMPOSITE_BANKED static void move(Place from, Place to, uint16_t n) {
        agos::far_copy(from, to, n);
    }

    // Near-memory hops for the merge. On the machine the buffers are in this
    // bank's window, which DMA does not see (in_bank).
#ifdef __mos__
    [[nodiscard, gnu::always_inline]] static Place physical(const uint8_t* p) {
        return in_bank(BANK_BASE_OF(AGOS_COMPOSITE_BANK), p);
    }
    [[gnu::always_inline]] static void fetch(uint8_t* into, Place from, uint16_t n) {
        move(from, physical(into), n);
    }
    [[gnu::always_inline]] static void store(Place to, const uint8_t* from, uint16_t n) {
        move(physical(from), to, n);
    }
#else
    static void fetch(uint8_t* into, Place from, uint16_t n) {
        agos::far_read(from, into, n);
    }
    static void store(Place to, const uint8_t* from, uint16_t n) {
        agos::far_write(to, from, n);
    }
#endif

    uint8_t head_[MAX_LAYERS] = {};
    uint32_t members_[MAX_LAYERS] = {};
    uint32_t touch_[MAX_LAYERS] = {};
    Box box_[MAX_LAYERS] = {};
    Box union_[MAX_LAYERS] = {};
    Kept kept_[2][MAX_KEPT] = {};
    uint8_t kept_count_ = 0;
    uint8_t side_ = 0;
    uint16_t serial_ = 0;
    uint32_t last_input_ = 0; //!< last frame's input checksum, nought if unreplayable
    uint32_t dropped_ = 0;    //!< which of its layers the merge took away
    uint8_t last_count_ = 0;
    uint8_t source_[CHUNK] = {};
    uint8_t target_[CHUNK] = {};
};

} // namespace composite
