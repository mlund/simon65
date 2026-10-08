// SPDX-License-Identifier: GPL-3.0-or-later

// The frame: each draw's sprites made into layers over the backdrop, and
// the display list built from them. Behind it, the figure cache and its
// decoding, the poses held for cels not decoded yet, the cut masks and the
// compositor that merges stacks.

#include "frame.hpp"

#include "banks.hpp"
#include "chipmap.hpp"
#include "composite.hpp"
#include "diagnostics.hpp"
#include "display.hpp"
#include "figures.hpp"
#include "masks.hpp"
#include "placed.hpp"
#include "room.hpp"
#include "rrb.hpp"
#include "speech.hpp"
#include "target_hooks.hpp"
#include "vga_vm.hpp"
#include "vmstate.hpp"

namespace {

/// The display list the VIC shows; the draw builds into the other.
uint8_t shown_list = 0;

/// The figures decoded so far, and where each one is in the Attic arena.
///
/// In .bss, which is ram_high and has the room: the near part of the index is
/// a kilobyte. And the crt zeroes .bss, so what is here at boot is nothing,
/// not whatever the last program left in the reserved low memory.
agos::FigureCache figures;

/// Which figure each layer is, so the compositor knows an unchanged stack.
uint8_t layer_zone[LAYERS_MAX];
uint16_t layer_image[LAYERS_MAX];
static_assert(LAYERS_MAX <= composite::MAX_LAYERS, "the compositor masks layers in 32 bits");

/// The whole placement each sprite was last drawn with.
///
/// If a sprite's next cel is not decoded, keeping the old pose holds the
/// figure in place. Without it, walking figures blink and scenes show dropped
/// frames, worst where a scene has many cels in a hurry (the fireworks).
/// The engine never meets this: every image is in memory, so a sprite always
/// has something to draw.
///
/// The placement and not merely the cel. Cels of a walk differ in height and
/// the script picks a sprite's y for the cel it just chose, so holding last
/// frame's art at the new y shifts the figure's bottom edge. Holding all of it
/// makes a late cel a pose held for a frame, which is what standing still
/// looks like.
///
/// Direct-mapped on the sprite's own id, because the list compacts when a
/// sprite halts and a slot number means nothing across that. The glyph is
/// kept to notice a tenant that has moved in the pool since; a move costs
/// no more than a miss, so it is let be.
///
/// Thirty-two, not sixteen: a zone's cast is a run of consecutive ids and
/// zone 8's is 802 to 823, so at sixteen the second half collides with the
/// first -- 818 on 802, 819 on 803 -- and each evicts the other's pose every
/// tick, and the figures blink. Thirty-two holds any cast this release has in
/// one scene without collisions.
constexpr uint8_t SHOWN_SLOTS = 32;
uint16_t shown_id[SHOWN_SLOTS];
uint16_t shown_image[SHOWN_SLOTS];
uint16_t shown_glyph[SHOWN_SLOTS];
Placed shown_at[SHOWN_SLOTS];

/// Bumped whenever the backdrop changes, so a mask's cut of it is cut again.
uint8_t backdrop_serial = 0;

/// Draws in a row that kept the last frame because a cel it needed was not
/// decoded, and the most there may be. The CD32 never shows part of a frame:
/// it decodes every cel on every draw into a hidden buffer, and a slow draw
/// is a late frame (runit2 0x22812, 0x1bd48). Holding is that late frame;
/// the cap is for a cel that never comes, which would freeze the picture.
uint8_t held_draws = 0;
constexpr uint8_t HOLD_MOST = 8;

/// Cels a drawn sprite will likely want next, for decoding in time nobody is
/// using. A sprite steps its image by one far more often than not: 74% were
/// the image before plus one, 86% within three. A ring, oldest overwritten:
/// a guess that waited too long is a guess about a past frame.
struct Ahead {
    uint8_t zone;
    uint16_t image;
};
constexpr uint8_t AHEAD = 3; //!< images past each drawn one
/// A power of two. A room changing many sprites at once, like the cart's,
/// would overwrite a smaller ring before it was decoded.
constexpr uint8_t AHEAD_RING = 32;
/// Out of zero page, where the allocator would put it and evict hotter
/// compositor variables, growing bank 3 by 108 bytes. Read only where written.
[[gnu::section(".noinit")]] Ahead ahead_ring[AHEAD_RING];
uint8_t ahead_put = 0, ahead_take = 0;

/// Guess at the cels after @p image of a sprite in @p zone.
[[gnu::always_inline]] inline void guess_ahead(uint8_t zone, uint16_t image) {
    for (uint8_t k = 1; k <= AHEAD; ++k) {
        ahead_ring[ahead_put & (AHEAD_RING - 1)] = {zone, static_cast<uint16_t>(image + k)};
        ++ahead_put;
    }
    if (static_cast<uint8_t>(ahead_put - ahead_take) > AHEAD_RING)
        ahead_take = static_cast<uint8_t>(ahead_put - AHEAD_RING);
}

/// One more layer for this frame, and which figure it is.
[[gnu::always_inline]] inline void lay(const Placed& layer, uint8_t zone, uint16_t image) {
    layer_zone[layer_count] = zone;
    layer_image[layer_count] = image;
    layers[layer_count++] = layer;
}

/// This frame's input to the compositor, summed in the display bank.
uint32_t composite_input = 0;

/// Merges stacked sprites, in its own bank with all its working memory.
COMPOSITE_DATA composite::Compositor compositor;

/// Merge this frame's stacks; the door into the compositor's bank.
extern "C" COMPOSITE_BANKED void composite_banked() {
    layer_count =
        compositor.run(figures, layers, layer_count, layer_zone, layer_image, composite_input);
    report::note_peak(report::COMPOSITES_PEAK, compositor.merged);
    report::counts[report::COMPOSITE_BUILDS] = compositor.builds;
    report::counts[report::COMPOSITE_REPLAYS] = compositor.replays;
}

/// Whether DMA reaches the compositor's buffers, asked once at start-up.
extern "C" COMPOSITE_BANKED void composite_check_banked() {
    if (!compositor.reaches())
        agos::note_fault(agos::Fault::COMPOSITE_SCRATCH, 0);
}

} // namespace

/// The fewest pieces a decode slice does, whatever the clock says, and the
/// most.
///
/// The ceiling is not about speed: the floor and the frame test both read
/// `frames`, which only the raster interrupt advances, so a slice that
/// trusted the clock alone would spin for ever if the interrupt stalled,
/// wedging the machine.
constexpr uint8_t PIECES_LEAST = 24;
constexpr uint8_t PIECES_MOST = 96;

/// One piece of the figure in flight. Out of line so both the slice below and
/// the decode-ahead share the one copy of the decoder the step inlines.
[[gnu::noinline]] CODE_BANK(AGOS_ROOM_BANK) static void step_once() {
    figures.decode_step();
}

/// Take a figure on, demanded or guessed. Out of line for the same reason as
/// step_once: one copy of the start, the image table and the arena included.
[[gnu::noinline]] CODE_BANK(AGOS_ROOM_BANK) static void start_decode(
    uint8_t zone, uint16_t image, agos::PackedZones::Pixels from, bool ahead) {
    figures.decode_start(zone, image, from, ahead);
}

/// The decoder's doors for the decode-ahead, which runs in the world's bank:
/// one piece, and a guessed figure started from what decode_zone and its
/// fellows hold.
extern "C" CODE_BANK(AGOS_ROOM_BANK) void step_banked() {
    step_once();
}
extern "C" CODE_BANK(AGOS_ROOM_BANK) void start_ahead_banked() {
    start_decode(agos::decode_zone, agos::decode_wanted, agos::decode_from, true);
}

/// As much of one image as this frame has left, in the bank the decoders live
/// in: the draw asks for it the first time something wants that image.
///
/// The budget is the frame itself rather than a count. A piece is about 1.7
/// ms, so decoding stops within the frame's end and never overshoots by more;
/// atomic decoding of a large figure takes eight frames of stalling.
extern "C" CODE_BANK(AGOS_ROOM_BANK) void figures_decode_banked() {
    const Stamp began = stamp();
    start_decode(agos::decode_zone, agos::decode_wanted, agos::decode_from, false);
    // A floor as well as a ceiling. The draw has usually spent the frame by the
    // time it gets here, so "what is left" is often one piece -- and at one a
    // frame a title cel would take ninety-six of them. 24 and 96 are the knee
    // of the measured curve: from 8/32 to 128/240 the cel rate stays flat, so
    // the slice is not what limits it.
    uint8_t pieces = 0;
    do {
        if (!figures.decoding())
            break;
        step_once();
        ++pieces;
    } while (pieces < PIECES_LEAST || (frames_now() == began.frame && pieces < PIECES_MOST));
    // What a slice costs. One frame or less is the whole point of slicing.
    const uint16_t took = static_cast<uint16_t>(frames_now() - began.frame);
    report::count_add(report::DECODE_PIECES, pieces);
    count_since(report::SLICE_LINES, began);
    report::note_peak(report::WORST_DECODE, took);
}

/// The priority whose masks restore only Simon's colours, 0x20 to 0x2F,
/// rather than everything under them (runit2 0x1e5b0): a dithered
/// see-through Simon in rooms 85-87. A layer cannot ask what is under it, so
/// these are not drawn.
constexpr uint16_t SEE_THROUGH_PRIORITY = 49;

/// A mask this port does not draw: the see-through kind.
[[nodiscard, gnu::always_inline]] static inline bool draws_nothing(const agos::VgaSprite& one) {
    return (one.flags & agos::DRAW_MASKED) != 0 && one.priority == SEE_THROUGH_PRIORITY;
}

/// Whether a sprite's new cel is still not decoded, once asked for: the draw
/// then leaves the screen as it is. Decided before any want(), so a held
/// frame takes nothing from the pool. Two sprites in step -- Simon's limbs
/// and body on the ladder -- otherwise show one without the other. In the
/// tick's chip bank: the display bank has no room, and this runs every frame.
static uint8_t cel_missing = 0;
extern "C" CODE_BANK(AGOS_VGA_TICK_BANK) void cels_missing_banked() {
    const agos::VgaSprite* sprite = vmstate::animation.sprites();
    const uint8_t count = vmstate::animation.sprite_count();
    cel_missing = 0;
    // Only what the draw would lay: it stops at LAYERS_MAX and leaves out a
    // sprite with nothing on the picture, and a cel nobody sees must not stop
    // the clock. Off the left or the top depends on a size not yet decoded, so
    // only the right edge is known here; the count is at most the draw's.
    uint8_t laid = 0;
    for (uint8_t k = 0; k < count && laid < LAYERS_MAX; ++k) {
        const agos::VgaSprite& one = sprite[k];
        if (one.image == 0 || speech::is_timer_sprite(one.id) || one.x >= chipmap::CELLS_ACROSS ||
            draws_nothing(one))
            continue;
        const agos::PackedZones::Pixels pixels = agos::zone_pixels.find(one.zone);
        if (!pixels.valid())
            continue; // a zone not loaded is the draw's to ask for
        ++laid;
        const uint8_t seen = static_cast<uint8_t>(one.id & (SHOWN_SLOTS - 1));
        if ((shown_id[seen] == one.id && shown_image[seen] == one.image) ||
            figures.ready(one.zone, one.image, one.palette))
            continue;
        if (!figures.ask(one.zone, one.image, pixels) ||
            !figures.ready(one.zone, one.image, one.palette))
            cel_missing = 1;
    }
}

/// What a mask's cut was cut for, and the key its pool slot is held under. A
/// new cut takes a new key, as a merge does (composite_slot), so a slot found
/// is always the cut wanted. Direct-mapped on the sprite id: a zone's masks
/// are consecutive ids, at most about nine live (zone 21). An unwritten entry
/// has id nought, which no sprite has.
namespace {
struct MaskCut {
    uint16_t id, image, key;
    int16_t x, y;
    uint8_t zone, serial;
};
} // namespace
constexpr uint8_t MASK_CUTS = 16;
EXTRA_DATA static MaskCut mask_cuts[MASK_CUTS];
EXTRA_DATA static uint16_t mask_keys;

/// Which sprite the draw hands the door below.
static uint8_t mask_index = 0;

static_assert(sizeof display_detail::screen_row >= masks::SCRATCH_BYTES,
    "a cut works in the row buffer, free while sprites are placed");

/// A masked sprite's layer: the backdrop cut to its image, laid where the
/// sprite is in the list. In the Attic: a cut is made once and then found,
/// and the frame that makes one is a scene change or a mask that moved.
extern "C" CODE_BANK(AGOS_EXTRA_BANK) void mask_banked() {
    const agos::VgaSprite& one = vmstate::animation.sprites()[mask_index];
    if (draws_nothing(one)) {
        report::count_one(report::MASKS_SEE_THROUGH);
        return;
    }
    // Decoded like any cel, asked for by cels_missing_banked; nothing is laid
    // until then.
    agos::Row art;
    if (!figures.decoded_row(one.zone, one.image, &art) || art.rows == 0)
        return;

    // Over the backdrop alone a cut changes nothing, and it costs every row it
    // covers: lay it only where something laid before it this frame lies under.
    const int16_t left = static_cast<int16_t>(one.x * chipmap::CELL_LINES);
    const int16_t right = static_cast<int16_t>(left + art.cells * 2 * chipmap::CELL_LINES);
    const int16_t bottom = static_cast<int16_t>(one.y + art.rows * chipmap::CELL_LINES);
    bool under = false;
    for (uint8_t k = 0; k < layer_count && !under; ++k) {
        const Placed& p = layers[k];
        const auto x0 = static_cast<int16_t>(p.at);
        const int16_t y0 = y_of(p);
        under = x0 < right && left < x0 + p.cells * cell_px(p.flags) && y0 < bottom &&
            one.y < y0 + p.rows * chipmap::CELL_LINES;
    }
    if (!under)
        return;

    MaskCut& was = mask_cuts[one.id & (MASK_CUTS - 1)];
    if (was.id != one.id || was.image != one.image || was.zone != one.zone || was.x != one.x ||
        was.y != one.y || was.serial != backdrop_serial)
        was = {one.id, one.image, ++mask_keys, one.x, one.y, one.zone, backdrop_serial};
    bool fresh = false;
    const agos::Figure fig =
        figures.mask_slot(was.key, static_cast<uint8_t>(art.cells * 2), art.rows, &fresh);
    if (!fig.valid())
        return;
    if (fresh)
        masks::cut({agos::arena_at(art.page),
                       agos::Place{fig.glyph} * chipmap::GLYPH_BYTES,
                       art.cells,
                       art.rows,
                       one.x,
                       one.y},
            chipmap::BACKDROP,
            masks::stand_in(agos::shadow_palette, agos::PALETTE_ENTRIES),
            display_detail::screen_row);
    // Full colour and transparent: a byte of nought lets the layers under it
    // show, and the backdrop's own nought became a colour in the cut.
    const Placed on = placed(fig, one.x, one.y);
    if (on.cells == 0)
        return;
    lay(on, agos::MASK_ZONE, was.key);
    report::count_one(report::MASKS_LAID);
}

/// One frame's worth of sprites: their figures into the pool the VIC reads,
/// then every row rebuilt with the layers that reach it.
///
/// The figures are decoded once and live in Attic; this moves the ones on
/// screen down by DMA, which is what makes a frame affordable at all.
extern "C" CODE_BANK(AGOS_DISPLAY_BANK) void draw_frame_banked() {
    const agos::VgaSprite* sprite = vmstate::animation.sprites();
    const uint8_t count = vmstate::animation.sprite_count();

    layer_count = 0;
    // The list built last is not on screen until the interrupt swaps it, and
    // this frame builds into the one it replaces -- and evicts what that names.
    if (swap_to != 0) {
        report::count_one(report::SWAP_WAITS);
        while (swap_to != 0) {
        }
    }
    // Nothing this frame places, nor the list on screen, may be evicted by
    // something later in it.
    figures.begin_frame();

    // A new cel not decoded yet: leave the screen as it is (cels_missing_banked).
    banked_call(AGOS_VGA_TICK_BANK, cels_missing_banked);
    if (cel_missing != 0) {
        if (held_draws < HOLD_MOST) {
            ++held_draws;
            report::count_one(report::HELD_DRAWS);
            return;
        }
        report::count_one(report::HOLDS_GIVEN_UP);
    }
    held_draws = 0;
    // What the frame is about to decide about each sprite, when a monitor has
    // asked for it. After the hold, which ends no census it began.
    census::begin();

    // What the room's script painted is in the backdrop, under every row, so
    // the display list carries only what the scripts animate.
    const Stamp sprites_began = stamp();
    uint8_t i = 0;
    for (; i < count && layer_count < LAYERS_MAX; ++i) {
        const agos::VgaSprite& one = sprite[i];
        // A line's timer sprite shows the engine's rendered text, which here is
        // drawn as a line of its own (speech.cpp).
        if (one.image == 0 || speech::is_timer_sprite(one.id)) {
            census::note(i, census::NO_IMAGE);
            continue; // a sprite with no frame set yet draws nothing
        }
        // A zone the store has never seen: asked for here and fetched between
        // frames, never inside one. The fetch is a card read and a whole-zone
        // decode, about eighty frames of it, and a draw that waited for it would
        // stand still that long -- with the pointer, the music's timing and
        // every other sprite waiting with it. The sprite is left out until its
        // figures arrive, which is a tick or two.
        //
        // One a frame, the first that asks: the next frame asks again for
        // whatever is still missing.
        const agos::PackedZones::Pixels pixels = agos::zone_pixels.find(one.zone);
        if (!pixels.valid()) {
            // A zone the card has not got is an answer too: ask for one it has not
            // been asked for, and let the rest alone.
            room::want_zone(one.zone);
            report::count_one(report::SKIPPED_ZONE);
            census::note(i, census::ZONE_ABSENT);
            continue;
        }
        // A mask draws nothing of its own: it puts the clean picture back over
        // what came before it (DRAW_MASKED).
        if ((one.flags & agos::DRAW_MASKED) != 0) {
            mask_index = i;
            banked_call(AGOS_EXTRA_BANK, mask_banked);
            continue;
        }
        const uint8_t seen = static_cast<uint8_t>(one.id & (SHOWN_SLOTS - 1));
        // A cel this sprite was not drawn with last tick: the animation asking,
        // before anything here has had a say in whether it can be answered.
        const bool new_cel = shown_id[seen] != one.id || shown_image[seen] != one.image;
        if (new_cel)
            report::count_one(report::CELS_WANTED);
        const agos::Figure fig = figures.want(one.zone, one.image, one.palette, pixels);
        if (!fig.valid()) {
            // The pose it held last frame, if the pool still has that cel where it
            // had it. Laid as it was laid then, position and all.
            if (shown_id[seen] == one.id &&
                figures.held(one.zone, shown_image[seen], one.palette).glyph == shown_glyph[seen] &&
                layer_count < LAYERS_MAX) {
                census::note(i, census::DRAWN);
                report::count_one(report::CELS_HELD);
                lay(shown_at[seen], one.zone, shown_image[seen]);
                continue;
            }
            census::note(i, census::NO_FIGURE);
            report::count_one(report::NOT_DRAWN);
            continue;
        }
        census::note(i, census::DRAWN);
        // A sprite's x is in eight-pixel units and its y in pixels:
        // xoffs = (vlut[0] * 2 + state->x) * 8, yoffs = vlut[1] + state->y
        // (gfx.cpp:940), and window 4 -- the room -- sits at 0, 0
        // (initialVideoWindows_Simon, agos.cpp:723). The script's own steps say
        // the same: SET_SPRITE_XY moves by one, which is eight pixels.
        const Placed on = placed(fig,
            one.x,
            one.y,
            static_cast<uint8_t>(
                rrb::FOUR_BIT | ((one.flags & agos::DRAW_FLIP) != 0 ? rrb::FLIP_HORIZONTAL : 0u)),
            rrb::four_bit_colour(one.palette));
        if (on.cells == 0)
            continue; // wholly outside the picture; the slot is worth more
        shown_id[seen] = one.id;
        shown_image[seen] = one.image;
        shown_glyph[seen] = fig.glyph;
        shown_at[seen] = on;
        lay(on, one.zone, one.image);
        // On a new cel only: a sprite holding a pose would refill the ring with
        // the same guesses every draw, each then refused by a lookup.
        if (new_cel)
            guess_ahead(one.zone, one.image);
    }

    // Stacks into one layer apiece, before the line of speech: text is never
    // merged, and it goes on top. One layer is no stack.
    if (layer_count > 1) {
        // Timed from here, bank switch and all: the compositor's bank has no room.
        const Stamp began = stamp();
        composite_input = composite::checksum(layers, layer_count, layer_zone, layer_image);
        banked_call(AGOS_COMPOSITE_BANK, composite_banked);
        count_since(report::COMPOSITE_LINES, began);
        report::count_one(report::COMPOSITE_CALLS);
    }

    count_since(report::SPRITE_LINES, sprites_began);

    // Sprites the frame never reached, because it had no layers left for them.
    if (i < count)
        report::count_one(report::LAYERS_CAPPED);

    speech::lay(figures, sprite, count);

    // The list as it stands, now every entry's fate is known and every layer
    // is laid -- a line of speech is one, so counting before it would disagree
    // with report::LAYERS for no reason but call order. A sprite past the last
    // one examined stays UNSEEN, which is what a full display list looks like
    // from outside.
    census::end(sprite, count, vmstate::animation.ticks(), layer_count, room::wanted());

    report::counts[report::LAYERS] = layer_count;
    report::note_peak(report::LAYERS_PEAK, layer_count);
    report::counts[report::RESIDENT] = figures.held();
    report::counts[report::TOO_WIDE] = figures.too_wide();
    // Why a figure is not on screen, which is otherwise a thing only eyes can
    // report: too big for the arena, refused by the decoder, or not decoded
    // yet.
    report::counts[report::TOO_BIG] = figures.too_big();
    report::counts[report::UNDECODED] = figures.undecoded();
    report::counts[report::DECODED] = figures.decoded();
    report::counts[report::ARENA_WRAPS] = figures.wraps();
    report::counts[report::AHEAD_DECODED] = figures.ahead_decoded();
    report::counts[report::AHEAD_USED] = figures.ahead_used();
    report::counts[report::AHEAD_ABANDONED] = figures.ahead_abandoned();
    report::counts[report::HINT_MISS] = figures.hint_miss();
    report::counts[report::DECODES_GIVEN_UP] = figures.given_up();
    report::counts[report::CROWDED] = figures.crowded();
    // What the display list cost, and whether any row could not hold it.
    report::counts[report::ROW_PEAK] = row_peak;
    report::counts[report::LAYERS_DROPPED] = layers_dropped;
    report::counts[report::CLOSES_FAILED] = closes_failed;

    // The rows below the picture, when a window has written to them.
    if (panel_changed != 0) {
        panel_changed = 0;
        rows_owed(chipmap::PICTURE_ROWS, chipmap::SCREEN_ROWS);
    }

    // Only the rows a figure touches, this frame or when the back list was last
    // built, two frames ago. The backdrop's own rows never change, and
    // rebuilding all twenty-five costs two frames where the figures cover about
    // ten.
    uint8_t touched[chipmap::SCREEN_ROWS] = {};
    for (uint8_t i = 0; i < layer_count; ++i)
        for (uint8_t row = layers[i].top < 0 ? 0 : uint8_t(layers[i].top);
            int{row} < layers[i].top + layers[i].rows && row < chipmap::SCREEN_ROWS;
            ++row)
            touched[row] = 1;

    const Stamp rows_began = stamp();
    const uint8_t back = static_cast<uint8_t>(shown_list ^ 1);
    for (uint8_t row = 0; row < chipmap::SCREEN_ROWS; ++row) {
        if (!touched[row] && !was_touched[back][row])
            continue;
        display_row_over(back, row, layers, layer_count);
        was_touched[back][row] = touched[row];
        report::count_one(report::ROWS_BUILT);
    }
    count_since(report::ROWS_LINES, rows_began);
    figures.list_built();
    shown_list = back;
    swap_to = static_cast<uint8_t>(back + 1);
}

/// Decode the guessed cels while the frame lasts: what is in flight first,
/// then the next guess not decoded already. Stops the moment a frame passes,
/// so it never holds up a period; a piece is short (FigureDecode::step). Here,
/// in the world's bank, because it only schedules: the work is through the
/// decoder's doors.
CODE_BANK(AGOS_TICK_BANK) void frame::decode_ahead() {
    const uint16_t began = frames_now();
    while (frames_now() == began) {
        if (figures.decoding()) {
            banked_call(AGOS_ROOM_BANK, step_banked);
            continue;
        }
        if (ahead_take == ahead_put)
            return;
        const Ahead next = ahead_ring[ahead_take & (AHEAD_RING - 1)];
        ++ahead_take;
        if (!figures.worth_ahead(next.zone, next.image))
            continue;
        const agos::PackedZones::Pixels from = agos::zone_pixels.find(next.zone);
        if (!from.valid())
            continue;
        agos::decode_zone = next.zone;
        agos::decode_wanted = next.image;
        agos::decode_from = from;
        banked_call(AGOS_ROOM_BANK, start_ahead_banked);
    }
}

namespace frame {

void begin() {
    figures.begin();
}

void check_composite() {
    banked_call(AGOS_COMPOSITE_BANK, composite_check_banked);
}

// always_inline, into the tick bank's turn.
[[gnu::always_inline]] void draw() {
    // A door between them: the display bank holds the drawing and the row
    // compositor and has no room for the tick as well, and this is twenty
    // crossings a second against a bank that has to stay in chip RAM. Checked
    // by eye: no tearing.
    banked_call(AGOS_DISPLAY_BANK, draw_frame_banked);
    // A decode in flight is finished by the frames, not by whichever sprite
    // happens to miss next: it is started from want(), and once every drawn
    // figure is resident nothing would step it again -- it would hold its
    // arena pages, unnamed by any row, until some image missed the hash.
    // Through the door: the draw is in the display bank and the decoder is in
    // the room bank, and a direct call would run whatever sits at that address
    // in this one.
    if (figures.decoding()) { // the start inside it is a no-op here
        const Stamp after_began = stamp();
        banked_call(AGOS_ROOM_BANK, figures_decode_banked);
        count_since(report::AFTER_DRAW_LINES, after_began);
    }
}

[[gnu::always_inline]] bool held() {
    return held_draws != 0;
}

void backdrop_changed() {
    ++backdrop_serial;
}

[[gnu::always_inline]] void forget_zone(uint8_t zone) {
    figures.forget_zone(zone);
}

} // namespace frame
