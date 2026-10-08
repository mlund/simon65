// SPDX-License-Identifier: GPL-3.0-or-later

// The verb bar: the box under the pointer lit and named, the verb chosen, and
// a click turned into a command.
//
// The CD32 binary is the reference (runit2: FUN_0001a050 hit test, a140
// hover, a992 select, a942 default verb, a9f0/aa74 sentence line, afb8 the
// click loop); ScummVM's waitForInput (input.cpp:200-360) agrees but for the
// highlight, which it toggles symmetrically.
//
// The engine blocks in its input loop; this is driven instead, once a tick
// with the pointer and once a click, and keeps between them what the loop
// keeps in its locals and globals.

#pragma once

#include "chipmap.hpp"
#include "hitareas.hpp"

#ifdef __mos__
#include "banks.hpp"
#else
#ifndef VERB_BANKED
#define VERB_BANKED
#endif
#endif

namespace agos {

/// What the bar asks of the rest of the program.
///
/// The sentence line, text window 1, cleared; then @p n characters centred
/// in it when @p n is not nought.
void sentence_say(const char* text, uint8_t n);
/// The line cleared and an item's name centred in it. False when the item
/// has no name to give, which leaves the line clear.
[[nodiscard]] bool sentence_name(uint16_t item);
/// The line cleared and a text box's name, its SET_SHORT_TEXT, centred in
/// it. False when the slot has none.
[[nodiscard]] bool sentence_text(uint8_t slot);
/// What a click that hit a box tells the scripts: where it was, variables 1
/// and 2, which Walk to reads (verb.cpp:858-861); and which text box, or
/// NOT_A_TEXT_BOX, variable 60, which the verb subroutines branch on
/// (input.cpp:34-55; runit2 a:1b146).
void click_noted(uint16_t x, uint16_t y, uint16_t text_slot);
inline constexpr uint16_t NOT_A_TEXT_BOX = 0xFFFF;
/// A command: subroutine 0 and then 100 with this verb and subject
/// (FUN_000115f4; handleVerbClicked, verb.cpp:352).
void command_run(uint16_t verb, uint16_t subject);
/// The inventory scrolled a row up or down (inventoryUp/Down, verb.cpp:694).
void inventory_scroll(bool up);

/// The twelve verb names in box order from 101, each after its length
/// (runit2's table at $3496E; verb.cpp:123-136), and the prompts while a
/// second item is wanted, the same way (table at $349C8, all but Use's and
/// Give's empty; verb.cpp:189-193). Defined where they can be placed.
extern const char VERB_NAMES[];
extern const char VERB_PROMPTS[];

class VerbBar {
  public:
    /// The verb boxes' ids (sub 101's ADD_BOX 3101..3112).
    static constexpr uint16_t FIRST_VERB = 101, LAST_VERB = 112;
    static constexpr uint16_t WALK_TO = 101, LOOK_AT = 102;
    /// Box 200 covers the screen while the game is not taking commands; the
    /// verb's name is shown only while it is disabled (a9f0).
    static constexpr uint16_t BLOCKER = 200;

    /// The pointer is at @p x, @p y; light the box under it and handle the
    /// default verb across the picture-to-panel line (a942's trigger, a:1acf2).
    VERB_BANKED void pointer(HitAreas& boxes, uint16_t x, uint16_t y) {
        if (!armed_) {
            armed_ = true;
            ready(boxes, y);
        } else if (default_verb_ != 0 && default_verb_ != default_for(y)) {
            choose_default(boxes, y);
        }
        const HitAreas::Box box = boxes.at(x, y);
        if (lit_ != 0 && lit_ != box.id) {
            paint(boxes.with_id(lit_), LIT, lit_ == verb_box_ ? SELECTED : PLAIN);
            lit_ = 0;
        }
        if (!box.live() || (box.flags & HitAreas::NO_TOUCH_NAME) != 0)
            say_verb(boxes);
        else if (box.id != name_on_ || box.item != name_item_)
            say_name(box); // the icons share one id, so the item tells them apart
        if ((box.flags & HitAreas::INVERT_TOUCH) != 0 && box.id != verb_box_ && box.id != lit_) {
            paint(box, PLAIN, LIT);
            lit_ = box.id;
        }
    }

    /// A press at @p x, @p y (afb8's loop body, input.cpp:254-345).
    VERB_BANKED void click(HitAreas& boxes, uint16_t x, uint16_t y) {
        // Box 0 is always present: the room's floor, the whole picture.
        const HitAreas::Box box = boxes.at(x, y);
        if (!box.live())
            return;
        if (scrolled(box))
            return;
        const bool text_box = (box.flags & HitAreas::TEXT_BOX) != 0;
        click_noted(x, y, text_box ? box.text : NOT_A_TEXT_BOX);
        if (box.id >= FIRST_VERB && box.id <= LAST_VERB) {
            verb_ = box.verb;
            select(boxes, box);
            default_verb_ = 0;
            return;
        }
        if (text_box && verb_ == 0) {
            // A conversation's choice: the verbs are disabled while one is asked,
            // so there is no verb to wait for.
            command_run(box.verb, box.item);
            return;
        }
        if (box.item != 0 &&
            (box.verb == 0 || verb_ != 0 ||
                (box.item != subject_ && (box.flags & HitAreas::BOX_ITEM) != 0))) {
            subject_ = box.item;
            say_name(box);
            if (verb_ != 0)
                run(boxes, y);
            return;
        }
        if (box.verb != 0) {
            verb_ = box.verb & ~VERB_TAKES_ITEM;
            if ((box.verb & VERB_TAKES_ITEM) != 0)
                subject_ = box.item;
            if (subject_ != 0)
                run(boxes, y);
        }
    }

    /// A box has gone (108) or died (110): what was lit or named for it is not
    /// any more, and the verbs reset when the one that died is Look at
    /// (a:1b51a; verb.cpp:467).
    VERB_BANKED void box_gone(HitAreas& boxes, uint16_t id, uint16_t y) {
        if (id == lit_)
            lit_ = 0;
        if (id == name_on_) {
            name_on_ = 0;
            verb_shown_ = 0;
            say_verb(boxes);
        }
        if (id == LOOK_AT && armed_)
            choose_default(boxes, y);
    }

    /// A second item is wanted (164, FUN_0001b1ac): the line says what for
    /// until answered() (FUN_00015906 picks the prompts while $3491A is set).
    /// The box named before is still counted as named, so the item the
    /// pointer rests on -- the one just clicked -- does not write over the
    /// prompt (setup_cond_c_helper keeps _lastNameOn, input.cpp:119-121).
    VERB_BANKED void ask(HitAreas& boxes) {
        asking_ = true;
        verb_shown_ = 0;
        const uint16_t named = name_on_;
        say_verb(boxes);
        name_on_ = named;
    }
    VERB_BANKED void answered() {
        asking_ = false;
        verb_shown_ = 0;
    }

    /// A press while a second item is wanted: the box with an item it hit, or
    /// one whose id is nought. The arrows scroll meanwhile (b1ac's loop).
    [[nodiscard]] VERB_BANKED HitAreas::Box pick(HitAreas& boxes, uint16_t x, uint16_t y) {
        const HitAreas::Box box = boxes.at(x, y);
        return scrolled(box) || box.item == 0 ? HitAreas::Box{} : box;
    }

    [[nodiscard]] uint16_t verb() const {
        return verb_;
    }
    [[nodiscard]] uint16_t subject() const {
        return subject_;
    }

  private:
    /// The verb bit ADD_BOX sets for an x past a thousand: the box's item is the
    /// subject whatever the verb (script.cpp:684; input.cpp:326).
    static constexpr uint16_t VERB_TAKES_ITEM = 0x4000;
    /// a942's mark for a default verb whose box is dead: no verb, and the test
    /// fires again on every pointer until the box lives.
    static constexpr uint16_t NO_DEFAULT = 999;

    /// An arrow scrolls the inventory (input.cpp:258-261; b1ac's loop).
    /// True when @p box was one.
    [[nodiscard]] VERB_BANKED static bool scrolled(const HitAreas::Box& box) {
        if (box.id != HitAreas::SCROLL_UP && box.id != HitAreas::SCROLL_DOWN)
            return false;
        inventory_scroll(box.id == HitAreas::SCROLL_UP);
        return true;
    }

    /// Shift amounts: a140 lights by 5, a5ee selects by 10; putting either
    /// back subtracts the same.
    enum Look : uint8_t { PLAIN = 0, LIT = 5, SELECTED = 10 };

    /// The default verb for a pointer at line @p y.
    [[nodiscard]] static uint16_t default_for(uint16_t y) {
        return y < chipmap::PICTURE_LINES ? WALK_TO : LOOK_AT;
    }

    /// What a command leaves behind: no verb, no subject, the default verb
    /// chosen afresh (afb8's opening).
    VERB_BANKED void ready(HitAreas& boxes, uint16_t y) {
        verb_ = 0;
        subject_ = 0;
        choose_default(boxes, y);
    }

    /// The chosen verb and subject run as a command, then the bar made ready.
    VERB_BANKED void run(HitAreas& boxes, uint16_t y) {
        command_run(verb_, subject_);
        ready(boxes, y);
    }

    /// a942: Walk to over the picture, Look at over the panel.
    VERB_BANKED void choose_default(HitAreas& boxes, uint16_t y) {
        default_verb_ = default_for(y);
        const HitAreas::Box box = boxes.with_id(default_verb_);
        if (!box.live())
            return;
        if (box.dead()) {
            default_verb_ = NO_DEFAULT;
            verb_ = 0;
            return;
        }
        verb_ = box.verb;
        select(boxes, box);
    }

    /// a992: the chosen verb drawn darker, the one before it put back.
    VERB_BANKED void select(HitAreas& boxes, const HitAreas::Box& box) {
        if (box.id == verb_box_)
            return;
        if (verb_box_ != 0)
            paint(boxes.with_id(verb_box_), SELECTED, PLAIN);
        const Look was = lit_ == box.id ? LIT : PLAIN;
        if (lit_ == box.id)
            lit_ = 0;
        verb_box_ = box.id;
        paint(box, was, SELECTED);
    }

    /// a9f0: the chosen verb's name, unless the game is not taking commands.
    VERB_BANKED void say_verb(HitAreas& boxes) {
        if (verb_box_ == verb_shown_ && name_on_ == 0)
            return;
        name_on_ = 0;
        verb_shown_ = verb_box_;
        const HitAreas::Box blocker = boxes.with_id(BLOCKER);
        const HitAreas::Box verb = boxes.with_id(verb_box_);
        if (blocker.active() || !verb.active()) {
            sentence_say(nullptr, 0);
            return;
        }
        const char* name = asking_ ? VERB_PROMPTS : VERB_NAMES;
        for (uint8_t skip = static_cast<uint8_t>(verb_box_ - FIRST_VERB); skip != 0; --skip)
            name += static_cast<uint8_t>(*name) + 1;
        sentence_say(name + 1, static_cast<uint8_t>(*name));
    }

    /// aa74: a text box's short text, or the box's item named, or the line
    /// left clear.
    VERB_BANKED void say_name(const HitAreas::Box& box) {
        verb_shown_ = 0;
        const bool said = (box.flags & HitAreas::TEXT_BOX) != 0 ? sentence_text(box.text)
                                                                : sentence_name(box.item);
        name_on_ = said ? box.id : 0;
        name_item_ = box.item;
    }

    /// A box's panel pixels shifted from @p from to @p to. A pixel is
    /// lettering when unshifted to 0xDB..0xDF (the planes a140 tests). Only
    /// the panel is lit; boxes over the picture keep their pixels.
    VERB_BANKED static void paint(const HitAreas::Box& box, Look from, Look to) {
        constexpr uint16_t TOP = chipmap::PICTURE_LINES;
        constexpr uint16_t BOTTOM = chipmap::SCREEN_ROWS * chipmap::CELL_LINES;
        constexpr uint16_t RIGHT = chipmap::CELLS_ACROSS * chipmap::CELL_LINES;
        if (!box.live() || box.y < TOP || box.x >= RIGHT)
            return;
        const uint16_t right = box.x + box.w < RIGHT ? box.x + box.w : RIGHT;
        const uint16_t bottom = box.y + box.h < BOTTOM ? box.y + box.h : BOTTOM;
        uint8_t cell[chipmap::GLYPH_BYTES];
        for (uint16_t row = (box.y - TOP) / chipmap::CELL_LINES;
            row * chipmap::CELL_LINES + TOP < bottom;
            ++row)
            for (uint16_t column = box.x / chipmap::CELL_LINES;
                column * chipmap::CELL_LINES < right;
                ++column) {
                const Place offset =
                    Place{static_cast<uint16_t>(row * chipmap::CELLS_ACROSS + column)} *
                    chipmap::GLYPH_BYTES;
                far_read(chipmap::PANEL + offset, cell, chipmap::GLYPH_BYTES);
                for (uint8_t i = 0; i < chipmap::GLYPH_BYTES; ++i) {
                    const uint16_t px = column * chipmap::CELL_LINES + i % chipmap::CELL_LINES;
                    const uint16_t py = row * chipmap::CELL_LINES + TOP + i / chipmap::CELL_LINES;
                    const auto shaded = static_cast<uint8_t>(cell[i] + from);
                    if (px >= box.x && px < right && py >= box.y && py < bottom &&
                        shaded >= SHADED_FIRST && shaded <= SHADED_LAST)
                        cell[i] = static_cast<uint8_t>(shaded - to);
                }
                far_write(chipmap::PANEL + offset, cell, chipmap::GLYPH_BYTES);
            }
    }

    /// The verb art's lettering, the colours a highlight shifts (a140).
    static constexpr uint8_t SHADED_FIRST = 0xDB, SHADED_LAST = 0xDF;

    uint16_t verb_ = 0;         ///< _verbHitArea, the verb a command runs with
    uint16_t subject_ = 0;      ///< _hitAreaSubjectItem
    uint16_t verb_box_ = 0;     ///< the verb box drawn selected
    uint16_t default_verb_ = 0; ///< _defaultVerb: 0 once the player chose
    uint16_t lit_ = 0;          ///< the box drawn lit under the pointer
    uint16_t name_on_ = 0;      ///< _lastNameOn, the box named on the line
    uint16_t name_item_ = 0;    ///< and its item
    uint16_t verb_shown_ = 0;   ///< the verb box named on the line
    bool armed_ = false;        ///< whether the opening reset has run
    bool asking_ = false;       ///< a second item is wanted
};

} // namespace agos
