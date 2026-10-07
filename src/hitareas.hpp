// The boxes a click is tested against (107 ADD_BOX, 65 ADD_TEXT_BOX).
//
// A box is a rectangle on screen, an item, and the verb that item answers
// to. The release defines 99 of them for 45 distinct ids, so a box is
// replaced far more often than it is added -- which is why defining one
// forgets any box already holding that id, as the engine does
// (defineBox, verb.cpp:521).
//
// Sixteen bytes apiece in chipmap::HITAREAS, and 192 of them: chip rather
// than near because near is where the VMs and the card layer live, and a box
// is read once a click.

#pragma once

#include "chipmap.hpp"
#include "far.hpp"

#ifdef __mos__
#include "banks.hpp"
#else
#ifndef VERB_BANKED
#define VERB_BANKED
#endif
#endif

namespace agos {

class HitAreas {
  public:
    /// What a box occupies, and how many the region holds.
    static constexpr uint8_t BYTES = 16;
    static constexpr uint8_t COUNT = static_cast<uint8_t>(chipmap::HITAREAS_BYTES / BYTES);

    /// The engine's box flags (intern.h:206-214).
    enum Flag : uint8_t {
        TEXT_BOX = 0x01,      ///< a conversation's choice; `text` is its slot
        LIT = 0x02,           ///< highlighted now
        NO_TOUCH_NAME = 0x04, ///< hovering shows the verb, not the item
        INVERT_TOUCH = 0x08,  ///< hovering highlights it
        DRAG = 0x10,
        BOX_ITEM = 0x80, ///< a click names its item even with a verb set
    };
    /// Dead is kept apart from the flags above, which are the script's: a
    /// disabled box is still defined, but nothing hits it (verb.cpp:467).
    static constexpr uint8_t DEAD = 0x40;

    /// The engine's own boxes: one per inventory icon, all sharing an id, and
    /// the two scroll arrows (runit2 1adc8, 1b57c; icons.cpp:599-612, 747-774).
    /// Every engine id is SCROLL_UP or above, which priority() relies on.
    static constexpr uint16_t SCROLL_UP = 0x7FFB, SCROLL_DOWN = 0x7FFC, ICON = 0x7FFD;
    /// Their priority. A script's box is its own id (verb.cpp:518), so these
    /// lose to box 200, which covers the screen while commands are refused.
    static constexpr uint16_t ENGINE_PRIORITY = 100;

    /// ADD_BOX packs flags into the id, a thousand to each (o_addBox,
    /// script.cpp:661-676): 1 highlight, 2 no name, 4 item, 8 text, 16 drag.
    [[nodiscard]] static constexpr uint8_t from_script(uint8_t packed) {
        return static_cast<uint8_t>(((packed & 1) != 0 ? INVERT_TOUCH : 0) |
            ((packed & 2) != 0 ? NO_TOUCH_NAME : 0) | ((packed & 4) != 0 ? BOX_ITEM : 0) |
            ((packed & 8) != 0 ? TEXT_BOX : 0) | ((packed & 16) != 0 ? DRAG : 0));
    }

    /// A box the release can ask for. Ids run to 999 before the flags the
    /// script packs above them (o_addBox, script.cpp:661).
    struct Box {
        uint16_t id = 0;
        uint16_t x = 0, y = 0, w = 0, h = 0;
        uint16_t verb = 0;
        uint16_t item = 0;
        uint8_t flags = 0;
        uint8_t text = 0;

        [[nodiscard]] bool live() const {
            return w != 0 && h != 0;
        }
        [[nodiscard]] bool dead() const {
            return (flags & DEAD) != 0;
        }
        /// Defined and not disabled: what a click can hit.
        [[nodiscard]] bool active() const {
            return live() && !dead();
        }
        /// Half-open on both axes, as runit2 tests it (FUN_0001a050).
        [[nodiscard]] bool holds(uint16_t px, uint16_t py) const {
            return active() && px >= x && px < x + w && py >= y && py < y + h;
        }
    };

    static_assert(sizeof(Box) == BYTES, "a box is stored as it is laid out");

    /// Forget every box, which is what a room change does.
    [[gnu::always_inline]] void clear() {
        far_fill(chipmap::HITAREAS, 0, uint16_t(uint16_t(COUNT) * BYTES));
        held_ = 0;
    }

    /// Define a box, replacing whatever held its id.
    VERB_BANKED void define(const Box& box) {
        undefine(box.id);
        add(box);
    }

    /// Add a box beside any holding its id, as the icons are.
    VERB_BANKED void add(const Box& box) {
        for (uint8_t slot = 0; slot < COUNT; ++slot)
            if (!read(slot).live()) {
                write(slot, box);
                if (slot >= held_)
                    held_ = static_cast<uint8_t>(slot + 1);
                return;
            }
        ++spilled_; // more boxes at once than the region holds
    }

    /// Remove every box with @p id (108). A script's id holds one slot at
    /// most, since define undefines first; the icons share theirs.
    VERB_BANKED void undefine(uint16_t id) {
        for (uint8_t slot = 0; slot < held_; ++slot)
            if (read(slot).id == id)
                write(slot, Box{});
    }

    /// Disable (110) or enable (109) a box: still defined, but a dead one is
    /// never hit, and it loses its highlight with its life (verb.cpp:467).
    /// False when there is no such box.
    VERB_BANKED bool kill(uint16_t id, bool dead) {
        const uint8_t slot = find(id);
        if (slot == COUNT)
            return false;
        Box one = read(slot);
        one.flags = static_cast<uint8_t>(dead ? one.flags | DEAD : one.flags & ~DEAD);
        write(slot, one);
        return true;
    }

    /// Shift the box with @p id by a step (vc55, vga_e2.cpp:333-349).
    VERB_BANKED void move(uint16_t id, int16_t dx, int16_t dy) {
        const uint8_t slot = find(id);
        if (slot == COUNT)
            return;
        Box one = read(slot);
        one.x = static_cast<uint16_t>(one.x + dx);
        one.y = static_cast<uint16_t>(one.y + dy);
        write(slot, one);
    }

    /// The box with this id, or one whose id is nought.
    [[nodiscard]] VERB_BANKED Box with_id(uint16_t id) const {
        const uint8_t slot = find(id);
        return slot == COUNT ? Box{} : read(slot);
    }

    /// The box under a point, or an id of nought if there is none. On a tie
    /// of priority the later slot wins (FUN_0001a050: `bgt` skips, so equal
    /// replaces). Dead boxes are never hit.
    [[nodiscard]] VERB_BANKED Box at(uint16_t x, uint16_t y) const {
        Box best{}; // priority nought, so the first hit always takes it
        for (uint8_t slot = 0; slot < held_; ++slot) {
            const Box one = read(slot);
            if (one.holds(x, y) && priority(one.id) >= priority(best.id))
                best = one;
        }
        return best;
    }

    [[nodiscard]] uint8_t held() const {
        return held_;
    }
    [[nodiscard]] uint16_t spilled() const {
        return spilled_;
    }

  private:
    [[nodiscard]] static uint16_t priority(uint16_t id) {
        return id >= SCROLL_UP ? ENGINE_PRIORITY : id;
    }

    [[nodiscard]] static Place slot_at(uint8_t slot) {
        return chipmap::HITAREAS + Place{slot} * BYTES;
    }

    /// The first live slot holding @p id, or COUNT.
    [[nodiscard]] VERB_BANKED uint8_t find(uint16_t id) const {
        for (uint8_t slot = 0; slot < held_; ++slot) {
            const Box one = read(slot);
            if (one.live() && one.id == id)
                return slot;
        }
        return COUNT;
    }

    /// A box is stored as it is laid out: words little-endian, as both the
    /// target and the host keep them, and no padding between.
    [[nodiscard]] VERB_BANKED static Box read(uint8_t slot) {
        Box box;
        far_read(slot_at(slot), reinterpret_cast<uint8_t*>(&box), BYTES);
        return box;
    }

    VERB_BANKED static void write(uint8_t slot, const Box& box) {
        far_write(slot_at(slot), reinterpret_cast<const uint8_t*>(&box), BYTES);
    }

    /// How far up the region a live box has ever been put: the walk stops
    /// there rather than at 192, and 45 ids is what the release uses.
    uint8_t held_ = 0;
    uint16_t spilled_ = 0;
};

} // namespace agos
