// SPDX-License-Identifier: GPL-3.0-or-later

// The line an actor says over the room, from the script's asking to the
// layer the frame lays: where each speaker's line goes, the string resolved,
// the voice started, the timer sprites that pace the script, and the line
// wrapped, rendered once into a pool slot and placed. The script's text hooks
// are defined here, beside what they drive.

#include "speech.hpp"

#include "atticmap.hpp"
#include "banks.hpp"
#include "chipmap.hpp"
#include "diagnostics.hpp"
#include "display.hpp"
#include "game_store.hpp"
#include "placed.hpp"
#include "room.hpp"
#include "script_vm.hpp"
#include "talk.hpp"
#include "text.hpp"
#include "vmstate.hpp"

namespace {

/// Where a line of text is placed on screen, pixel-rather than glyph-aligned.
/// Four slots: sprites 1, 2, 101, 102 (string.cpp:182).
struct SaidAt {
    int16_t x = 0;
    uint8_t y = 0;
    uint16_t width = 0;
};
constexpr uint8_t SAY_PLACES = 4;
SaidAt said_at[SAY_PLACES];
/// The highest a line may sit (printScreenText, string.cpp:562-563).
constexpr int16_t TEXT_TOP_LEAST = 2;

[[nodiscard]] uint8_t place_of(uint8_t which) {
    switch (which) {
        case 1:
            return 0;
        case 2:
            return 1;
        case 101:
            return 2;
        case 102:
            return 3;
        default:
            return SAY_PLACES;
    }
}

/// A line's timer sprite is 199 + the speaker (printScreenText,
/// string.cpp:566): one per place a line can be said.
constexpr uint16_t TEXT_SPRITE_BASE = 199;
/// The room's own strings. Globals are in gameamiga and the store has them.
agos::LocalText local_text;

/// A line the script has asked for, and the line resolved. Kept apart because
/// resolving reads the card, and the VM runs inside a frame.
uint16_t say_string = 0;
/// The line's voice, nought for none: spoken on channel 3 (talk.cpp).
uint16_t say_speech = 0;
uint8_t say_which = 0, say_colour = 0, say_asked = 0;
char say_line[agos::LocalText::MOST_CHARS];
uint8_t say_len = 0;
uint16_t say_serial = 0;

/// Lines of speech a pool slot holds: at three, a picture-wide box is 15 cells
/// by 5 glyphs, 75 of the slot's 94.
constexpr uint8_t SAY_LINES = 3;
/// The palette block a line's sprite is drawn with (printScreenText,
/// string.cpp:568), and so its text's.
constexpr uint8_t TEXT_PALETTE = 12;

/// What a line starts besides its pixels (printScreenText, string.cpp:495-499
/// and 560-568): sprite 199 + the speaker, whose script in zone 2 waits
/// variable 85 ticks and sends sync 200, which a wait for a line ends on.
/// Without it every line holds the game for the wait's thousand ticks.
constexpr uint8_t TALK_RATE_VARIABLE = 141; // ticks a three letters, talkie
constexpr uint8_t TALK_TICKS_VARIABLE = 85;
constexpr int16_t TALK_RATE_DEFAULT = 9;
constexpr uint16_t WIDE_TEXT_BIT = 133; // window 4 rather than 3

} // namespace

/// A global string -- gameamiga's -- copied near. Locals come from LocalText;
/// these are indexed already (GameDb::string).
uint8_t speech::copy_global(uint16_t id, char* into, uint8_t most) {
    const agos::Place from = agos::store.db().string(id);
    if (from == agos::NOWHERE)
        return 0;
    uint8_t n = 0;
    while (n + 1u < most) {
        const uint8_t ch = agos::far_read8(from + n);
        if (ch == 0)
            break;
        into[n++] = static_cast<char>(ch);
    }
    into[n] = '\0';
    return n;
}

/// Any string, global or the room's own. One call site for the local copy, a
/// card read of nearly a kilobyte, so it is not inlined twice; in the world's
/// bank, where its first caller is, not the fixed region.
[[gnu::noinline]] CODE_BANK(AGOS_TICK_BANK) uint8_t speech::resolve(
    uint16_t id, char* into, uint8_t most) {
    return id >= agos::LocalText::FIRST_LOCAL ? local_text.copy(id, into, most)
                                              : speech::copy_global(id, into, most);
}

/// The line rendered into a pool slot and placed, wrapped at the script's
/// width and centred by padding with spaces (string.cpp:533). Rendered once:
/// the slot is keyed by the line.
static void say_over_the_room(agos::FigureCache& figures) {
    const uint8_t place = place_of(say_which);
    const SaidAt& box = said_at[place < SAY_PLACES ? place : 0];
    const uint16_t wide = box.width != 0 ? box.width : chipmap::PICTURE_LINES;
    const uint8_t cells = text::cells_for(wide);

    // Three lines at most: a pool slot is 94 glyphs, and a wider box with more
    // lines than that is counted rather than drawn (TOO_WIDE).
    uint8_t lines = 0;
    uint8_t start[SAY_LINES] = {};
    uint8_t length[SAY_LINES] = {};
    for (uint8_t at = 0; at < say_len && lines < SAY_LINES;) {
        const uint8_t took = text::say_break(say_line + at, wide);
        if (took == 0)
            break;
        start[lines] = at;
        length[lines] = took;
        ++lines;
        at = static_cast<uint8_t>(at + took + 1); // past the space it broke at
    }
    if (lines == 0)
        return;

    const uint8_t rows = text::rows_for(lines);
    bool fresh = false;
    const agos::Figure fig = figures.text_slot(say_serial, cells, rows, &fresh);
    if (!fig.valid())
        return;
    if (fresh) {
        const agos::Place at = agos::Place{fig.glyph} * chipmap::GLYPH_BYTES;
        const uint8_t stride = static_cast<uint8_t>(rows + 2);
        text::clear(at, cells, stride);
        for (uint8_t i = 0; i < lines; ++i) {
            const uint16_t drawn = text::say_width(say_line + start[i], length[i]);
            const uint8_t left = static_cast<uint8_t>(drawn < wide ? (wide - drawn) / 2 : 0);
            // In the block the line's sprite is drawn with (string.cpp:568), not
            // the room's.
            text::render_say(say_line + start[i],
                length[i],
                text::say_ink(TEXT_PALETTE, say_colour),
                at,
                stride,
                cells,
                static_cast<uint8_t>(i * text::SAY_ROWS),
                left);
        }
    }
    // Each line past the first lifts the box a line (string.cpp:528-529).
    int16_t top = static_cast<int16_t>(box.y - (lines - 1) * text::SAY_ROWS);
    if (top < TEXT_TOP_LEAST)
        top = TEXT_TOP_LEAST;
    const Placed line = placed(fig, static_cast<int16_t>(box.x / chipmap::CELL_LINES), top);
    if (line.cells != 0)
        layers[layer_count++] = line;
}

/// The voice's timer: window 4 at nought, nought, speakers under 100 only.
constexpr uint16_t VOICE_SPRITE_BASE = 201;
constexpr uint8_t VOICE_WINDOW = 4, VOICED_SPEAKERS = 100;

CODE_BANK(AGOS_TICK_BANK) static void time_the_voice(uint8_t which, uint16_t speech) {
    // Ids past the table are cues, not voices (9999: no voice at all), and
    // only a speaker under 100 has a timer (playSpeech, res_snd.cpp:72-78).
    if (which >= VOICED_SPEAKERS || speech >= atticmap::VOICES)
        return;
    room::start_sprite(static_cast<uint16_t>(VOICE_SPRITE_BASE + which), VOICE_WINDOW, 0, 0, 0);
}

CODE_BANK(AGOS_TICK_BANK) static void time_the_line(uint8_t which, uint8_t length) {
    int16_t rate = vmstate::script.variable(TALK_RATE_VARIABLE);
    if (rate == 0) {
        rate = TALK_RATE_DEFAULT;
        vmstate::script.set_variable(TALK_RATE_VARIABLE, rate);
    }
    vmstate::script.set_variable(
        TALK_TICKS_VARIABLE, static_cast<int16_t>(rate * ((length + 3) / 3)));
    const uint8_t place = place_of(which);
    const SaidAt at = place < SAY_PLACES ? said_at[place] : SaidAt{};
    room::start_sprite(static_cast<uint16_t>(TEXT_SPRITE_BASE + which),
        vmstate::script.bit(WIDE_TEXT_BIT) ? agos::ROOM_WINDOW : agos::TEXT_WINDOW,
        static_cast<int16_t>(at.x / chipmap::CELL_LINES),
        at.y < TEXT_TOP_LEAST ? TEXT_TOP_LEAST : at.y,
        TEXT_PALETTE);
}

namespace speech {

bool begin() {
    return local_text.begin();
}

// always_inline: the tick bank calls resolve() directly, and a copy of this
// in the fixed region would call into whatever bank was mapped.
[[gnu::always_inline]] void pick_up() {
    // The line the script asked for, resolved now that the frame is done with:
    // a local string is a card read, which blocks the frame.
    if (say_asked != 0) {
        say_asked = 0;
        say_len = resolve(say_string, say_line, sizeof say_line);
        report::counts[report::SAID_LEN] = say_len;
        // The voice first, as the engine plays it before it prints: said at
        // every place, timed by a sprite at a speaker's.
        if (say_speech != 0) {
            talk::speak(say_speech);
            time_the_voice(say_which, say_speech);
        }
        if (say_len != 0)
            time_the_line(say_which, say_len);
        ++say_serial; // a new line wants a slot of its own
        rows_owed(0, chipmap::SCREEN_ROWS);
    }
}

// always_inline, into the display bank's frame.
[[gnu::always_inline]] void lay(
    agos::FigureCache& figures, const agos::VgaSprite* sprite, uint8_t count) {
    // The line's text is its timer sprite's image in the engine (string.cpp:
    // 545-556), so it goes when that sprite does: at its end, or a KILL_ANIMATE
    // on a room change. Searched here: the frame's sprite loop can stop short.
    bool line_live = false;
    for (uint8_t k = 0; say_len != 0 && k < count; ++k)
        if (sprite[k].id == TEXT_SPRITE_BASE + say_which)
            line_live = true;
    if (say_len != 0 && line_live && layer_count < LAYERS_MAX)
        say_over_the_room(figures);
}

/// Whether @p id is a line's timer sprite, 200 to 199 + 255.
bool is_timer_sprite(uint16_t id) {
    const uint16_t which = static_cast<uint16_t>(id - TEXT_SPRITE_BASE);
    return id > TEXT_SPRITE_BASE && which <= UINT8_MAX &&
        place_of(static_cast<uint8_t>(which)) < SAY_PLACES;
}

char* lent_line() {
    return say_line;
}

[[gnu::always_inline]] void note_lost() {
    report::counts[report::LOST_SAY_WHICH] = say_which;
    report::counts[report::LOST_SAY_SPEECH] = say_speech;
}

} // namespace speech

/// Where the next lines are said (161), and a line to say (162).
///
/// The line is not resolved here: a string can be a card read and the VM runs
/// inside a frame, so the loop picks it up once the frame is done with.
void agos::script_text_box(uint8_t which, int16_t x, uint8_t y, uint16_t width) {
    const uint8_t place = place_of(which);
    if (place < SAY_PLACES) {
        said_at[place].x = x;
        said_at[place].y = y;
        said_at[place].width = width;
    }
}

void agos::script_text_msg(uint8_t which, uint8_t colour, uint16_t string, uint16_t speech) {
    report::count_one(report::SAID);
    report::counts[report::SAID_STRING] = string;
    say_which = which;
    say_colour = colour;
    say_string = string;
    say_asked = 1;
    // The voice's half of a line: its id, kept for time_the_voice (playSpeech,
    // res_snd.cpp:55). A talkie line is often voice alone -- string 0xFFFF --
    // and then the timer sprite is the only thing that sends sync 200: zone 2's
    // 201 + the speaker loops while the voice plays (IF_SPEECH) and then sends
    // it.
    say_speech = speech;
}
