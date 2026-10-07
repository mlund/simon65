// The text windows a script defines, picks, clears and writes into.
//
// Six of them in the whole release, each defined once (101), and two carry
// nearly every line: window 3 numbers a conversation's choices and window 4
// lists them, which is 1,479 of the 1,490 changes (102). Two inks are ever
// set (160). So this is a table and a cursor rather than a window system --
// the shape the data asks for, measured rather than assumed.
//
// A window's x and width are cells, its y is pixels from the top of the
// screen and its height rows (charset.cpp:287). All six sit below the
// picture, which is what chipmap::PANEL holds.

#pragma once

#include "atticmap.hpp"
#include "chipmap.hpp"
#include "text.hpp"

namespace agos {

struct TextWindow {
    uint8_t x = 0;     //!< cells from the left
    uint16_t y = 0;    //!< pixels from the top of the screen
    uint8_t cells = 0; //!< width, in cells
    uint8_t rows = 0;  //!< height, in rows
    uint8_t ink = 0;
    /// A clear puts the panel's picture back rather than nothing.
    bool restores = false;

    uint16_t column = 0; //!< pixels across, where the next character goes
    uint8_t row = 0;     //!< rows down from the window's top

    [[nodiscard]] bool real() const {
        return cells != 0 && rows != 0;
    }
};

class Windows {
  public:
    static constexpr uint8_t COUNT = 8;

    /// Where the panel's glyphs start, and how wide it is. The windows all sit
    /// below the picture; a y of 136 is the panel's first line.
    static constexpr uint16_t PANEL_TOP = chipmap::PICTURE_LINES;
    static constexpr uint8_t ACROSS = chipmap::CELLS_ACROSS;

    /// The flags' 0x10 says a clear restores what is under the window rather
    /// than filling it (clearWindow, window.cpp:112): the inventory's windows 0
    /// and 2 have it, so clearing them keeps the panel's art.
    static constexpr uint8_t RESTORES = 0x10;

    void define(
        uint8_t which, uint8_t x, uint16_t y, uint8_t cells, uint8_t rows, uint8_t flags = 0) {
        if (which >= COUNT)
            return;
        TextWindow& one = window_[which];
        one.x = x;
        one.y = y;
        one.cells = cells;
        one.rows = rows;
        one.restores = (flags & RESTORES) != 0;
        one.column = 0;
        one.row = 0;
    }

    void use(uint8_t which) {
        if (which < COUNT)
            current_ = which;
    }

    void ink(uint8_t colour) {
        window_[current_].ink = colour;
    }
    /// Where the next line starts across the current window, in pixels.
    void place(uint16_t column) {
        window_[current_].column = column;
    }

    /// Clear the current window and put its cursor back at the top left (103).
    void clear() {
        TextWindow& one = window_[current_];
        if (!one.real())
            return;
        for (uint8_t row = 0; row < one.rows; ++row)
            wipe_row(one, row);
        one.column = 0;
        one.row = 0;
    }

    /// Write a line and start a new one, which is what 63 does.
    ///
    /// A line that would run past the window's width is not wrapped here: the
    /// game's own lines are choices and numbers, and the one place it needs a
    /// break it puts one in the string.
    /// Pinned into whichever bank its one caller sits in. CODE_BANK banks only
    /// what carries an attribute, and at 1,300 bytes this is the largest thing
    /// the fixed region had no reason to hold (banks.hpp).
    [[gnu::always_inline]] inline void say(const char* s, uint8_t n) {
        // Not an overrun: use() and define() are the only writers of current_
        // and both refuse one past the end.  The analyzer infers otherwise from
        // at()'s clamp below.
        // NOLINTNEXTLINE(clang-analyzer-security.ArrayBound)
        TextWindow& one = window_[current_];
        if (!one.real())
            return;
        if (one.row >= one.rows)
            scroll(one);
        text::render_window(s,
            n,
            one.ink,
            chipmap::PANEL,
            ACROSS,
            uint16_t(uint16_t(one.x) * chipmap::CELL_LINES + one.column),
            uint16_t(one.y - PANEL_TOP + uint16_t(one.row) * chipmap::CELL_LINES));
        one.column = 0;
        ++one.row;
    }

    [[nodiscard]] const TextWindow& at(uint8_t which) const {
        return window_[which < COUNT ? which : 0];
    }
    [[nodiscard]] uint8_t current() const {
        return current_;
    }

  private:
    /// One row of a window, back to the panel's picture or to nothing. Nothing
    /// is transparent rather than a colour: the panel shows the border behind
    /// it, which is the black the release's fill colour 0 names.
    static void wipe_row(const TextWindow& one, uint8_t row) {
        const uint16_t top = uint16_t(one.y - PANEL_TOP + uint16_t(row) * chipmap::CELL_LINES);
        const uint16_t first = uint16_t(uint16_t(top / chipmap::CELL_LINES) * ACROSS + one.x);
        const Place offset = Place{first} * chipmap::GLYPH_BYTES;
        const uint16_t bytes = uint16_t(uint16_t(one.cells) * chipmap::GLYPH_BYTES);
        if (one.restores)
            far_copy(atticmap::PANEL_MASTER + offset, chipmap::PANEL + offset, bytes);
        else
            far_fill(chipmap::PANEL + offset, 0, bytes);
    }

    /// A window that has run out of rows starts again at the top.
    ///
    /// The engine scrolls its window up a row instead. Nothing in this release
    /// writes more lines than a window has rows without a CLS between -- 213 of
    /// those against 663 lines -- so scrolling is owed rather than missing, and
    /// starting again is what says so on screen.
    void scroll(TextWindow& one) {
        for (uint8_t row = 0; row < one.rows; ++row)
            wipe_row(one, row);
        one.row = 0;
    }

    TextWindow window_[COUNT];
    uint8_t current_ = 0;
};

} // namespace agos
