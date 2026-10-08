// SPDX-License-Identifier: GPL-3.0-or-later

// The room as the scripts drive it: zones loaded and their pictures shown,
// sprites animated, stopped and killed, syncs sent, and the tunes. The
// arguments of the room's banked doors are private here; the script's room
// hooks are defined here, beside what they drive.

#include "room.hpp"

#include "atticmap.hpp"
#include "banks.hpp"
#include "chipmap.hpp"
#include "diagnostics.hpp"
#include "display.hpp"
#include "frame.hpp"
#include "game_store.hpp"
#include "script_vm.hpp"
#include "sound.hpp"
#include "target_hooks.hpp"
#include "vga_vm.hpp"
#include "vmstate.hpp"
#include "zonepix.hpp"

namespace {

/// No zone asked for. Not nought, which is Simon's own zone.
constexpr uint8_t NO_ZONE_WANTED = 0xFF;

/// The zone whose figures the next banked load should fetch.
uint8_t zone_wanted = NO_ZONE_WANTED;

} // namespace

/// Bring a zone's figures in: its pixels staged in PACKED, then decoded whole.
/// In the bank with the decoders, because that is where the decoding is.
///
/// The store stages them, because the pixels share a file with the scripts and
/// asking for the scripts already brought them: no second scan of the card.
extern "C" CODE_BANK(AGOS_STORE_BANK) void load_zone_banked() {
    // The arena holds them or marks them absent. The cache is handed pixels
    // when a figure is actually wanted, keeps no pointer of its own.
    (void)agos::zone_pixels_of(zone_wanted);
    report::count_one(report::ZONES_DECODED);
    report::counts[report::ZONES_HELD] = agos::zone_pixels.held();
    report::counts[report::ZONES_EMPTIED] = agos::zone_pixels.emptied();
}

/// The room on the screen, once its script has painted it.
extern "C" CODE_BANK(AGOS_ROOM_BANK) void show_room_banked() {
    display_show();
}

// What a script asks the animation VM for, in the room's bank, not the
// dispatch's: the picture path alone is the zone loader, the image script and
// the show, and bank 4 already holds 188 opcodes. The dispatch reaches these
// through doors, as it does all that is not its own.
namespace {

uint8_t asked_zone = 0, asked_window = 0, asked_palette = 0;
uint16_t asked_image = 0, asked_sprite = 0, asked_ident = 0;
int16_t asked_x = 0, asked_y = 0;
bool asked_halt = false, asked_beard = false;

/// The verb bar's window, y 136-199 (zone 0 image script 0, vc26_setSubWindow, vga.cpp:1073).
constexpr uint8_t PANEL_WINDOW = 5;

} // namespace

extern "C" CODE_BANK(AGOS_ROOM_BANK) void load_asked_zone_banked() {
    agos::note_opcode(report::ZONES_ASKED, asked_zone);
    if (!agos::zone_pixels.knows(asked_zone)) {
        // The only tick work not this program's: deleted FAT entries stay walked.
        // One moved a load from 292 ms to 1,242.
        const uint16_t began = frames_now();
        zone_wanted = asked_zone;
        banked_call(AGOS_STORE_BANK, load_zone_banked);
        zone_wanted = NO_ZONE_WANTED;
        const uint16_t took = static_cast<uint16_t>(frames_now() - began);
        report::count_one(report::LOADS);
        report::note_peak(report::WORST_LOAD, took);
    }
}

extern "C" CODE_BANK(AGOS_ROOM_BANK) void picture_banked() {
    load_asked_zone_banked();
    if (!agos::zone_pixels.find(asked_zone).valid())
        return; // the card had nothing; SKIPPED_ZONE says so
    vmstate::animation.use_window(asked_window);
    vmstate::animation.run_image(asked_zone, asked_image);
    show_room_banked();
    report::count_one(report::PICTURES);
    // The panel owes only its painted rows.
    if (asked_window != PANEL_WINDOW)
        rows_owed(0, chipmap::SCREEN_ROWS);
}

extern "C" CODE_BANK(AGOS_ROOM_BANK) void animate_banked() {
    load_asked_zone_banked();
    vmstate::animation.animate(
        asked_window, asked_zone, asked_sprite, asked_x, asked_y, asked_palette);
}

/// Beside the tick it stops.
extern "C" CODE_BANK(AGOS_VGA_TICK_BANK) void halt_animation_banked() {
    vmstate::animation.halt(asked_halt);
}

/// The tune PLAY_TUNE asked for, for the door below.
static uint8_t asked_tune = 0;

/// Which tune each Attic slot holds, plus one so nought is none. A tune
/// stays till its slot is wanted, so a revisited room finds its music.
EXTRA_DATA static uint8_t slot_tune[atticmap::TUNE_SLOTS];
EXTRA_DATA static uint8_t next_slot, playing_slot;

/// PLAY_TUNE's tune from its slot, read off the card into the next if none
/// holds it. Never the playing slot (interrupt reads its patterns). A tune
/// that will not load is counted; the old one plays on.
extern "C" CODE_BANK(AGOS_EXTRA_BANK) void play_tune_banked() {
    const auto want = static_cast<uint8_t>(asked_tune + 1);
    uint8_t slot = 0;
    while (slot < atticmap::TUNE_SLOTS && slot_tune[slot] != want)
        ++slot;
    if (slot == atticmap::TUNE_SLOTS) {
        slot = next_slot;
        if (slot == playing_slot && tune_loaded)
            slot = static_cast<uint8_t>((slot + 1) % atticmap::TUNE_SLOTS);
        next_slot = static_cast<uint8_t>((slot + 1) % atticmap::TUNE_SLOTS);
        slot_tune[slot] = 0;
        const agos::Place base = atticmap::slot(atticmap::TUNES, slot);
        char name[] = "TUNE00.BIN"; // the game's number
        constexpr uint8_t TENS = 4, UNITS = 5;
        for (uint8_t n = asked_tune; n != 0; --n)
            if (++name[UNITS] > '9') {
                name[UNITS] = '0';
                ++name[TENS];
            }
        if (agos::read_game_file(name, base, 1UL << atticmap::SHIFT) == 0 || !module_landed(base)) {
            report::count_one(report::TUNE_MISSING);
            return;
        }
        slot_tune[slot] = want;
    }
    playing_slot = slot;
    tune_store_select(atticmap::slot(atticmap::TUNES, slot));
}

extern "C" CODE_BANK(AGOS_ROOM_BANK) void kill_animate_banked() {
    // The screen keeps killed sprites till the next tick, fading them with
    // the room if the cutscene skips (2928). Sprites in palette 13+ stay lit
    // through the room's 208-entry fade.
    vmstate::animation.reset_sprites();
}

/// The beard on or off, in the store's bank: cold, and the store's to do.
extern "C" CODE_BANK(AGOS_STORE_BANK) void beard_banked() {
    if (agos::store.wear_beard(asked_beard))
        frame::forget_zone(agos::BEARD_ZONE);
}

/// One sprite stopped (vc60, script.cpp:1058). The one call site: inlining
/// a second would cost the fixed region 332 bytes.
extern "C" CODE_BANK(AGOS_TICK_BANK) void stop_animate_banked() {
    vmstate::animation.stop(asked_sprite);
}

extern "C" CODE_BANK(AGOS_ROOM_BANK) void sync_banked() {
    report::counts[report::SYNC_SENT] = asked_ident;
    vmstate::animation.send_sync(asked_ident);
}

/// One of a line's sprites started afresh: the last with its number goes
/// first (string.cpp:546; playSpeech's stopAnimate).
CODE_BANK(AGOS_TICK_BANK)
void room::start_sprite(uint16_t sprite, uint8_t window, int16_t x, uint8_t y, uint8_t palette) {
    asked_x = x;
    asked_y = y;
    asked_palette = palette;
    asked_sprite = sprite;
    asked_zone = static_cast<uint8_t>(sprite / agos::SPRITES_PER_ZONE);
    asked_window = window;
    stop_animate_banked(); // same bank: one call site keeps it here
    banked_call(AGOS_ROOM_BANK, animate_banked);
}

namespace room {

void load_zone(uint8_t zone) {
    asked_zone = zone;
    banked_call(AGOS_ROOM_BANK, load_asked_zone_banked);
}

void show() {
    banked_call(AGOS_ROOM_BANK, show_room_banked);
}

void kill_all() {
    banked_call(AGOS_ROOM_BANK, kill_animate_banked);
}

void want_zone(uint8_t zone) {
    if (zone_wanted == NO_ZONE_WANTED && !agos::zone_pixels.knows(zone))
        zone_wanted = zone;
}

uint8_t wanted() {
    return zone_wanted;
}

void fetch_wanted() {
    // Fetch what the draw asked for, now that the frame is done. A zone that
    // will not read is marked absent, so this asks the card once total.
    if (zone_wanted != NO_ZONE_WANTED) {
        banked_call(AGOS_STORE_BANK, load_zone_banked);
        zone_wanted = NO_ZONE_WANTED;
    }
}

} // namespace room

namespace agos {

void script_picture(uint8_t zone, uint16_t image, uint8_t window) {
    asked_zone = zone;
    asked_image = image;
    asked_window = window;
    banked_call(AGOS_ROOM_BANK, picture_banked);
}

void script_load_zone(uint8_t zone) {
    asked_zone = zone;
    banked_call(AGOS_ROOM_BANK, load_asked_zone_banked);
}

void script_animate(
    uint16_t window, uint8_t zone, uint16_t sprite, int16_t x, int16_t y, uint8_t palette) {
    asked_window = window;
    asked_zone = zone;
    asked_sprite = sprite;
    asked_x = x;
    asked_y = y;
    asked_palette = palette;
    banked_call(AGOS_ROOM_BANK, animate_banked);
}

void script_kill_animate() {
    banked_call(AGOS_ROOM_BANK, kill_animate_banked);
}

// In the dispatch's bank, its only caller: no fixed-region bytes for it.
CODE_BANK(AGOS_SCRIPT_BANK) void script_halt_animation(bool halted) {
    asked_halt = halted;
    banked_call(AGOS_VGA_TICK_BANK, halt_animation_banked);
}

void script_stop_animate(uint16_t sprite) {
    asked_sprite = sprite;
    banked_call(AGOS_TICK_BANK, stop_animate_banked);
}

void script_sync(uint16_t ident) {
    asked_ident = ident;
    banked_call(AGOS_ROOM_BANK, sync_banked);
}

/// The last tune asked for, plus one so nought is none (the engine starts at
/// -1, agos.cpp:926). Not saved, as the engine does not save it either.
static uint8_t last_tune = 0;

/// A tune unless it is the one last asked for: the engine lets a room ask
/// again without restarting it (o_playTune, script.cpp:787). In the
/// dispatch's bank, its only caller.
CODE_BANK(AGOS_SCRIPT_BANK) void script_play_tune(uint16_t music, uint16_t /*track*/) {
    report::counts[report::TUNE_WANTED] = music;
    // The release's tunes number thirty-four, so a byte holds one.
    const auto asked = static_cast<uint8_t>(music + 1);
    if (asked == last_tune)
        return;
    last_tune = asked;
    asked_tune = static_cast<uint8_t>(music);
    banked_call(AGOS_EXTRA_BANK, play_tune_banked);
}

// In the dispatch's bank, its only caller.
CODE_BANK(AGOS_SCRIPT_BANK) void script_beard(bool on) {
    asked_beard = on;
    banked_call(AGOS_STORE_BANK, beard_banked);
}
} // namespace agos
