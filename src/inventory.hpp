// SPDX-License-Identifier: GPL-3.0-or-later

// The inventory: icons of what an item holds, drawn into a panel window from
// ICONS.BIN (icon.pkd unpacked by mkicons.py), a box apiece, and the arrows
// that scroll them a row at a time.
//
// The CD32 binary is the reference (runit2: FUN_00015314 the array, 151f6
// its removal, 1ad80 an icon drawn, 1adc8 its box, 1b57c the arrows).
// ScummVM's drawIconArray (icons.cpp:480-578) walks the same way but spaces
// the rows 25 lines apart, where runit2 spaces them 24.
//
// The icons are drawn into chip PANEL only, over the panel's Attic master,
// so taking them away is copying the master back.

#pragma once

#include "atticmap.hpp"
#include "chipmap.hpp"
#include "gamedb.hpp"
#include "hitareas.hpp"
#include "windows.hpp"

#ifdef __mos__
#include "banks.hpp"
#else
#ifndef VERB_BANKED
#define VERB_BANKED
#endif
#ifndef TICK_BANKED
#define TICK_BANKED
#endif
#endif

namespace agos {

/// What the program does for the inventory: paint the scroll arrows, zone
/// 1's image 1 (1b57c), and pass an icon to Inventory::draw in the bank it
/// lives in, @p x_cell cells across and @p top lines down the panel.
void inventory_arrows();
void inventory_icon(uint16_t icon, uint8_t x_cell, uint8_t top);

class Inventory {
  public:
    /// An icon is 24 pixels square.
    static constexpr uint8_t SIDE = 24;
    /// Where an icon box's verb and the arrows' sit (1adfc; icons.cpp:761).
    static constexpr uint16_t ICON_VERB = 0xD0, ARROW_VERB = 1;

    /// Show @p owner's children that have icons in @p window, numbered
    /// @p number, starting @p line rows down (FUN_00015314). Whatever icons
    /// were up go first.
    VERB_BANKED void show(HitAreas& boxes,
        const GameDb& db,
        uint16_t owner,
        uint8_t number,
        const TextWindow& window,
        uint8_t line) {
        remove(boxes);
        const Item holder = db.item(owner);
        if (!holder.valid())
            return;
        owner_ = owner;
        window_ = number;
        line_ = line;
        x_ = window.x;
        top_ = static_cast<uint8_t>(window.y - chipmap::PICTURE_LINES);
        const uint8_t across = window.cells / ICON_CELLS;
        const uint8_t down = window.rows / ICON_CELLS;

        // Past the rows scrolled off; too far, and the walk starts again at
        // the top.
        uint16_t id = holder.child();
        for (uint8_t skip = line; id != NO_ITEM && skip != 0; --skip)
            for (uint8_t n = 0; id != NO_ITEM && n < across; id = next_of(db, id))
                if (icon_of(db, id) != NO_ICON)
                    ++n;
        if (id == NO_ITEM) {
            line_ = 0;
            id = holder.child();
        }

        uint8_t column = 0, row = 0;
        bool more = false;
        for (; id != NO_ITEM; id = next_of(db, id)) {
            const uint16_t icon = icon_of(db, id);
            if (icon == NO_ICON)
                continue;
            if (row < down) {
                const uint8_t x = static_cast<uint8_t>(x_ + column * ICON_CELLS);
                const uint8_t y = static_cast<uint8_t>(top_ + row * SIDE);
                inventory_icon(icon, x, y);
                boxes.add({HitAreas::ICON,
                    static_cast<uint16_t>(x * chipmap::CELL_LINES),
                    static_cast<uint16_t>(y + chipmap::PICTURE_LINES),
                    SIDE,
                    SIDE,
                    ICON_VERB,
                    id,
                    static_cast<uint8_t>(HitAreas::BOX_ITEM | HitAreas::DRAG),
                    0});
            } else {
                more = true;
            }
            if (++column >= across) {
                column = 0;
                ++row;
            }
        }

        if (more || line_ != 0) {
            inventory_arrows();
            boxes.add({HitAreas::SCROLL_UP,
                ARROW_X,
                ARROW_UP_Y,
                ARROW_W,
                ARROW_H,
                ARROW_VERB,
                NO_ITEM,
                HitAreas::NO_TOUCH_NAME,
                0});
            boxes.add({HitAreas::SCROLL_DOWN,
                ARROW_X,
                ARROW_DOWN_Y,
                ARROW_W,
                ARROW_H,
                ARROW_VERB,
                NO_ITEM,
                HitAreas::NO_TOUCH_NAME,
                0});
        }
    }

    /// Take the icons, their boxes and the arrows away (151f6): the window
    /// and the arrows' strip right of it go back to the panel's picture.
    /// False when nothing was up.
    VERB_BANKED bool remove(HitAreas& boxes) {
        if (owner_ == NO_ITEM)
            return false;
        owner_ = NO_ITEM;
        const uint8_t cells = static_cast<uint8_t>(chipmap::CELLS_ACROSS - x_);
        for (uint8_t row = top_ / chipmap::CELL_LINES; row < PANEL_ROWS; ++row) {
            const Place offset = Place{static_cast<uint16_t>(row * chipmap::CELLS_ACROSS + x_)} *
                chipmap::GLYPH_BYTES;
            far_copy(atticmap::PANEL_MASTER + offset,
                chipmap::PANEL + offset,
                static_cast<uint16_t>(cells * chipmap::GLYPH_BYTES));
        }
        boxes.undefine(HitAreas::ICON);
        boxes.undefine(HitAreas::SCROLL_UP);
        boxes.undefine(HitAreas::SCROLL_DOWN);
        return true;
    }

    /// The item shown, or NO_ITEM; its window; the rows scrolled off.
    [[nodiscard]] uint16_t owner() const {
        return owner_;
    }
    [[nodiscard]] uint8_t window() const {
        return window_;
    }
    [[nodiscard]] uint8_t line() const {
        return line_;
    }

    /// An icon over what chip PANEL shows, its left edge @p x_cell cells
    /// across and its top @p top lines down the panel. Each of its three
    /// column strips (mkicons.py) goes into the cells it crosses, a job a
    /// cell, and nought leaves the panel's own pixel. An icon the file has not
    /// got is not drawn.
    TICK_BANKED static void draw(uint16_t icon, uint8_t x_cell, uint8_t top) {
        if (icon >= atticmap::ICON_COUNT)
            return;
        Place strip = atticmap::ICONS + Place{icon} * atticmap::ICON_BYTES;
        for (uint8_t column = 0; column < ICON_CELLS; ++column, strip += SIDE * chipmap::CELL_LINES)
            for (uint8_t line = 0; line < SIDE;) {
                const auto y = static_cast<uint8_t>(top + line);
                const uint8_t row = y / chipmap::CELL_LINES;
                if (row >= PANEL_ROWS)
                    break;
                const uint8_t within = y % chipmap::CELL_LINES;
                auto lines = static_cast<uint8_t>(chipmap::CELL_LINES - within);
                if (lines > SIDE - line)
                    lines = static_cast<uint8_t>(SIDE - line);
                const Place to = chipmap::PANEL +
                    Place{static_cast<uint16_t>(row * chipmap::CELLS_ACROSS + x_cell + column)} *
                        chipmap::GLYPH_BYTES +
                    within * chipmap::CELL_LINES;
                far_copy_over(strip + line * chipmap::CELL_LINES,
                    to,
                    static_cast<uint16_t>(lines * chipmap::CELL_LINES));
                line = static_cast<uint8_t>(line + lines);
            }
    }

  private:
    /// The next of an item's siblings; a record that is not there ends the
    /// chain rather than being read.
    [[nodiscard]] VERB_BANKED static uint16_t next_of(const GameDb& db, uint16_t id) {
        const Item item = db.item(id);
        return item.valid() ? item.next() : NO_ITEM;
    }

    /// An item's icon number: its object's kOFIcon value, where it has one
    /// (FUN_000151ba; itemGetIconNumber, items.cpp).
    static constexpr uint8_t ICON_FLAG_BIT = 4; // kOFIcon, intern.h:240
    static constexpr uint16_t NO_ICON = 0xFFFF;
    [[nodiscard]] VERB_BANKED static uint16_t icon_of(const GameDb& db, uint16_t id) {
        const Item item = db.item(id);
        if (!item.valid() || !item.is_object() || !item.object().has(ICON_FLAG_BIT))
            return NO_ICON;
        return item.object().value(ICON_FLAG_BIT);
    }

    /// Icons are three cells square, and a window holds a third of its
    /// cells and rows of them (icons.cpp:491-493).
    static constexpr uint8_t ICON_CELLS = 3;
    static constexpr uint8_t PANEL_ROWS = chipmap::SCREEN_ROWS - chipmap::PICTURE_ROWS;
    /// The arrows' boxes, on the screen (icons.cpp:753-770).
    static constexpr uint16_t ARROW_X = 308, ARROW_UP_Y = 149, ARROW_DOWN_Y = 176, ARROW_W = 12,
                              ARROW_H = 17;

    uint16_t owner_ = NO_ITEM;
    uint8_t window_ = 0;
    uint8_t line_ = 0;
    uint8_t x_ = 0;   ///< the window's left, in cells
    uint8_t top_ = 0; ///< the window's top, in lines down the panel
};

} // namespace agos
