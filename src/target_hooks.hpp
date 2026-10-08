// SPDX-License-Identifier: GPL-3.0-or-later

// What the VMs reach for that the machine has to answer: files off the card, the
// palette and its fades, the pointer and sound, and the counts each leaves in
// report (diagnostics.hpp).

#pragma once

#include "banks.hpp"
#include "cardfs.hpp"
#include "chipmap.hpp"
#include "cursor.hpp"
#include "diagnostics.hpp"
#include "fault.hpp"
#include "game_store.hpp"
#include "mouse.hpp"
#include "sdcard.hpp"
#include "sound.hpp"

#include <mega65.h>

namespace agos {

/// The raster interrupt's frame counter, which is what paces a fade.
extern "C" volatile uint16_t frames;

/// A palette block in a type-1 file: 32 entries of three bytes (vga_s1.cpp:96
/// uses this stride whether it reads 32 entries or 16).
inline constexpr uint8_t PALETTE_BLOCK_BYTES = 96;

/// The display palette, as the file states it: 0..63 a component, before the
/// scaling and the nybble swap the registers want.
///
/// In the reserved low memory (main.cpp), which is neither loaded nor zeroed,
/// so main clears it.
inline constexpr uint16_t PALETTE_ENTRIES = 256;
inline constexpr uint16_t SHADOW_BYTES = PALETTE_ENTRIES * 3;
extern uint8_t shadow_palette[SHADOW_BYTES];

/// Whether the shadow has moved since the display last took it, and whether a
/// fade is holding the display black against it.
inline uint8_t palette_dirty = 0;
inline uint8_t palette_held = 0;
/// Whether the screen shows black from a fade-out, until a palette next
/// reaches it. A fade-in only lifts the hold; the screen changes at the next
/// frame, as the engine's does at its next displayScreen (draw.cpp).
inline uint8_t palette_black = 0;

/// How much of the display palette a fade takes down: 208 entries while the
/// room is the current window and all 256 otherwise (_fastFadeCount,
/// vc62_fastFadeOut, vga_ww.cpp:204-208). The room's fade leaves the panel's
/// colours above it lit; the panel's own picture (window 0, zone 0's script)
/// fades everything, so a sprite in block 13 and the panel's old colours go
/// dark with the rest while the new panel is painted under the hold.
inline constexpr uint16_t FADE_ENTRIES = 208;

/// What a fade takes the display down by, and in how many steps: the
/// engine's own figures for Simon 1 (vga_ww.cpp:218-222). One step a frame:
/// its delay(5) between steps is not five milliseconds, since each delay puts
/// the screen up first (event.cpp, delay's updateScreen), which waits for the
/// display's frame. So 1.3 s for this fade, 0.6 s for FADE_TO_BLACK.
inline constexpr uint8_t FADE_STEPS = 64;
inline constexpr uint8_t FADE_BY = 4;

/// The palette registers hold their nybbles the other way round
/// (iomap.txt:376), and a zone stores 0..63, which the engine scales by four
/// into a byte and lets wrap.
[[nodiscard]] inline uint8_t swapped(uint8_t raw) {
    const uint8_t v = static_cast<uint8_t>(raw * 4);
    return static_cast<uint8_t>((v >> 4) | (v << 4));
}

/// Set while a read walk is a write: each sector the walk holds is first
/// stored from the save image, by the save bank's door, which clears this if
/// one would not write. The walk is the reader's, chain and all, so writing
/// costs the card bank one branch rather than a second walker it has no room
/// for; and what the walk then lands is the card's read-back, over the source.
inline uint8_t card_writing = 0;
inline uint32_t card_sector = 0;
extern "C" void card_store_banked();

/// The SD controller, as the card reader wants it: what has to be picked
/// apart goes to near memory, and what only passes through never does.
struct Controller {
    // always_inline so these fold into the reader in its own bank rather than
    // landing back in the fixed region, which is what CODE_BANK cannot do for
    // a callee by itself.
    [[nodiscard]] [[gnu::always_inline]] static bool near(uint32_t sector, uint8_t* into) {
        return ::card::read_sector_near(sector, into);
    }
    [[nodiscard]] [[gnu::always_inline]] static bool hold(uint32_t sector) {
        if (card_writing == 0)
            return ::card::fetch_sector(sector);
        card_sector = sector;
        // Re-entered: the save bank called the walk that calls back into it, the
        // shape banks.hpp's static-stack trap has.
        banked_reenter<card_store_banked>(AGOS_SAVE_BANK);
        return card_writing != 0;
    }
    [[gnu::always_inline]] static void spill(uint16_t from, Place to, uint16_t n) {
        far_copy(::card::SECTOR_BUFFER + from, to, n);
    }
};

/// The card, mounted once at boot. One of it, as with the store and the VMs.
inline ::card::Files<Controller> files;

/// What a door was asked for, and what it answered. banked_call takes a
/// function of no arguments, so the request travels in these, as the
/// animation VM's pending opcode does.
/// The name is cleared by the door that took it: the caller's buffer is
/// usually on its stack, and the request does not outlive the call.
inline const char* card_name = nullptr;
inline Place card_into = NOWHERE;
inline uint32_t card_most = 0;
inline uint32_t card_answer = 0;
inline uint8_t card_zone = 0;
inline uint8_t card_slot = 0;
inline bool card_pixels = false;
inline uint32_t card_pixel_bytes = 0;

inline uint32_t card_from = 0;
inline uint16_t card_count = 0;
inline fat32::Entry card_entry;

extern "C" void card_mount_banked();
extern "C" void card_read_banked();
extern "C" void card_zone_banked();
extern "C" void card_find_banked();
extern "C" void card_next_banked();
extern "C" void card_sectors_banked();

/// Mount the volume the machine booted from and find the game's directory.
///
/// False is fatal rather than something to fall back from: Hyppo parsed this
/// same partition to load this program, so a mount that fails here is a fault
/// in this code, and a fallback would hide it behind a game that merely
/// stutters.
[[nodiscard]] inline bool mount_card(const char* directory) {
    card_name = directory;
    banked_call(AGOS_CARD_BANK, card_mount_banked);
    return card_answer != 0;
}

/// The card as card::Runs reaches it from outside the card's bank: each
/// call a door, the request in the globals above.
struct DoorCard {
    [[nodiscard]] static fat32::Entry find(const char* name) {
        card_name = name;
        banked_call(AGOS_CARD_BANK, card_find_banked);
        return card_entry;
    }
    [[nodiscard]] static uint32_t next(uint32_t cluster) {
        card_from = cluster;
        banked_call(AGOS_CARD_BANK, card_next_banked);
        return card_from;
    }
    [[nodiscard]] static const fat32::Volume& volume() {
        return files.volume();
    }
    [[nodiscard]] static bool read(uint32_t first, Place to, uint16_t count) {
        card_from = first;
        card_into = to;
        card_count = count;
        banked_call(AGOS_CARD_BANK, card_sectors_banked);
        return card_answer != 0;
    }
};

/// A named file from the game's directory to a 28-bit address.
///
/// Read off the card itself, a sector at a time. Hyppo's whole-file load is
/// one hypervisor trap, and **the CPU takes no interrupt at all while in
/// hypervisor mode** -- dispatch requires `hypervisor_mode='0'`
/// (`gs4510.vhdl:6833`), for NMI as well as IRQ -- which silenced the MOD
/// player for 598 ms on a 50 KB zone. Opening the file that way walks the
/// directory inside another trap (`dos.asm:2275-2312`), 253 to 470 ms a file.
/// Neither happens here: the controller is polled, and the raster interrupt
/// runs between polls.
inline uint32_t read_game_file(const char* name, Place into, uint32_t most) {
    card_name = name;
    card_into = into;
    card_most = most;
    banked_call(AGOS_CARD_BANK, card_read_banked);
    return card_answer;
}

/// A zone's joined file: its header says how much is scripts, and the rest is
/// pixels, both landed from one walk of the directory.
inline bool read_zone_file(uint8_t zone, uint8_t slot, bool* pixels_ok, uint32_t* pixel_bytes) {
    card_zone = zone;
    card_slot = slot;
    banked_call(AGOS_CARD_BANK, card_zone_banked);
    *pixels_ok = card_pixels;
    *pixel_bytes = card_pixel_bytes;
    return card_answer != 0;
}

/// Which script was running when it read something that is not an opcode: the
/// sprite, its zone, and where its pointer had got to. The opcode alone says
/// only that the pointer is wrong, never whose.
inline bool lost_noted = false;

inline void note_lost(uint16_t id, uint8_t zone, uint32_t pc) {
    // Latched on a flag of its own: any of the four values can be nought -- a
    // timer with no script runs from address nought -- and a latch keyed on one
    // of them stays open, so every later loss overwrites the first.
    if (lost_noted)
        return; // the first one, before anything cascades
    lost_noted = true;
    report::counts[report::LOST_ID] = id;
    report::counts[report::LOST_ZONE] = zone;
    report::counts[report::LOST_PC_LO] = static_cast<uint16_t>(pc);
    report::counts[report::LOST_PC_HI] = static_cast<uint16_t>(pc >> 16);
}

/// Set the bit for @p opcode in the mask starting at @p base.
///
/// A table and not `1 << n`: a variable shift is a subroutine call on this
/// target, and this runs from both dispatch loops.
inline void note_opcode(uint8_t base, uint8_t opcode) {
    static constexpr uint16_t BIT[16] = {0x0001,
        0x0002,
        0x0004,
        0x0008,
        0x0010,
        0x0020,
        0x0040,
        0x0080,
        0x0100,
        0x0200,
        0x0400,
        0x0800,
        0x1000,
        0x2000,
        0x4000,
        0x8000};
    const uint8_t word = static_cast<uint8_t>(base + (opcode >> 4));
    report::counts[word] = static_cast<uint16_t>(report::counts[word] | BIT[opcode & 15]);
}

inline void script_unimplemented(uint8_t opcode) {
    report::counts[report::SCRIPT_MISSING] =
        static_cast<uint16_t>(report::counts[report::SCRIPT_MISSING] + 1);
    note_opcode(report::SCRIPT_SEEN, opcode);
}

inline void script_line(uint16_t id, uint16_t offset) {
    if (far_read8(atticmap::LINE_TRACE) == 0)
        return;
    const uint16_t next = far_read16_le(atticmap::LINE_TRACE + 2);
    const Place at = atticmap::LINE_TRACE + atticmap::LINE_TRACE_HEADER +
        Place{static_cast<uint16_t>(next & (atticmap::LINE_TRACE_ENTRIES - 1))} * 4;
    far_write16_le(at, id);
    far_write16_le(at + 2, offset);
    far_write16_le(atticmap::LINE_TRACE + 2, static_cast<uint16_t>(next + 1));
}

inline void vga_unimplemented(uint8_t opcode) {
    report::counts[report::VGA_LAST] = opcode;
    report::counts[report::VGA_MISSING] =
        static_cast<uint16_t>(report::counts[report::VGA_MISSING] + 1);
    note_opcode(report::VGA_SEEN, opcode);
}

/// The first fault a run raises, kept whole beside the count: with no screen
/// and no characters, two numbers are all a fault can say, and the first fault
/// is the one that explains the rest.
inline void note_fault(Fault what, uint16_t value) {
    if (report::counts[report::FAULTS] == 0) {
        report::counts[report::FAULT_KIND] = static_cast<uint8_t>(what);
        report::counts[report::FAULT_VALUE] = value;
    }
    report::counts[report::FAULTS] = static_cast<uint16_t>(report::counts[report::FAULTS] + 1);
}

/// Out of line, because a code is cheap enough to pass that the optimiser
/// inlines the whole body at every site: 65 bytes of counter update landed in
/// the script VM's bank, which had nine to spare. One call apiece, as the
/// strings got.
[[gnu::noinline]] inline void script_fault(Fault what, uint16_t value) {
    note_fault(what, value);
}
[[gnu::noinline]] inline void vga_fault(Fault what, uint16_t value) {
    note_fault(what, value);
}

/// The item whose children changed, for the verb bank's door: the icons
/// showing it are redrawn there.
inline uint16_t changed_item = 0;
extern "C" void items_changed_banked();

inline void script_items_changed(uint16_t item) {
    report::counts[report::ITEMS_MOVED] =
        static_cast<uint16_t>(report::counts[report::ITEMS_MOVED] + 1);
    changed_item = item;
    banked_reenter<items_changed_banked>(AGOS_VERB_BANK);
}

/// What the door below was asked to paint. banked_call takes a function of no
/// arguments, so the request travels in these.
inline uint16_t paint_image = 0;
inline uint8_t paint_block = 0;
inline uint8_t paint_zone = 0;
inline int16_t paint_x = 0, paint_y = 0;
inline uint16_t paint_flags = 0;
/// Not a DRAW flag: the paint goes over chip PANEL alone, leaving the
/// panel's master as it was, as the inventory's arrows do.
inline constexpr uint16_t PAINT_OVERLAY = 0x8000;

extern "C" void paint_banked();
extern "C" void window_image_banked();

/// DRAW, doing what its name says.
///
/// The decoders are in the room's bank and this runs in the animation VM's,
/// so it goes through the door rather than calling across (banks.hpp).
inline void vga_paint(
    uint8_t zone, uint16_t image, uint8_t block, int16_t x, int16_t y, uint16_t flags) {
    paint_zone = zone;
    paint_image = image;
    paint_block = block;
    paint_x = x;
    paint_y = y;
    paint_flags = flags;
    banked_call(AGOS_ROOM_BANK, paint_banked);
    report::counts[report::PAINTED] = static_cast<uint16_t>(report::counts[report::PAINTED] + 1);
    report::counts[report::LAST_BLOCK] = block;
}

/// IF_SPEECH's answer: whether channel 3 is still playing speech, an effect
/// apart (sound.hpp).
inline bool vga_voice_playing() {
    return sound::speaking();
}

/// SET_WINDOW_IMAGE: the window's picture becomes another image.
///
/// Only the clearing is here. Running the image script is the VM's own work
/// and it does it itself, the way CALL does -- what the engine adds around it
/// (setWindowImage, gfx.cpp:1381) is bookkeeping for a renderer this port has
/// not got, save one thing: clearVideoWindow, which is this.
///
/// The window is read and dropped: this port draws one window, the room
/// (initialVideoWindows_Simon, agos.cpp:723), and every SET_WINDOW_IMAGE in
/// the release names it.
inline void vga_window_image(uint16_t window) {
    (void)window;
    banked_call(AGOS_ROOM_BANK, window_image_banked);
}

/// A palette block, straight to the VIC. Sixteen entries at block * 16, or
/// thirty-two when the block is nought (vga_s1.cpp:96); the source block is
/// 96 bytes into the zone's type-1 file past its six-byte header, and the
/// entries are 0..63 scaled by four -- wrapping, as the engine does.
///
/// It lands in the shadow, not on the display. The engine writes
/// `_displayPalette` and marks it (vga_s1.cpp:139), and only `displayScreen`
/// puts it on screen, once a frame and not at all while a fade is holding
/// (draw.cpp:964). Writing each block straight to the VIC showed every one of
/// them in turn: a scene loads three, so a transition cycled through the
/// colours of each, black among them.
inline void vga_palette(uint8_t zone, uint8_t block, uint8_t source) {
    VgaZone* file = vga_zone(zone);
    if (file == nullptr)
        return;

    // The block comes near in one move rather than ninety-six far calls, and
    // then walks under a pointer instead of a multiply an entry.
    const uint8_t count = block == 0 ? 32 : 16;
    uint8_t rgb[PALETTE_BLOCK_BYTES];
    far_read(file->base() + 6 + static_cast<Place>(source) * PALETTE_BLOCK_BYTES,
        rgb,
        PALETTE_BLOCK_BYTES);
    const uint8_t* from = rgb;
    uint8_t* to = &shadow_palette[uint16_t(block) * 16 * 3];
    for (uint8_t i = 0; i < count; ++i) {
        *to++ = *from++;
        *to++ = *from++;
        *to++ = *from++;
    }
    palette_dirty = 1;
    report::counts[report::PALETTES] = static_cast<uint16_t>(report::counts[report::PALETTES] + 1);
}

/// Every entry to black, once, before anything is shown.
///
/// The hardware palette comes up holding whatever the ROM left, and nothing
/// else writes the entries above a fade's 208: a figure whose palette block
/// is 13 or higher would be drawn in the ROM's colours until a script loaded
/// them. Measured at boot -- 45 of the 48 above 208 were still the ROM's six
/// seconds in.
inline void blacken_palette() {
    VICIV.ctrla |= VIC3_PAL_MASK;
    VICIV.palsel &= 0x0C;
    for (uint16_t i = 0; i < PALETTE_ENTRIES; ++i) {
        PALETTE.red[i] = 0;
        PALETTE.green[i] = 0;
        PALETTE.blue[i] = 0;
    }
}

/// Wait for the next frame, the pointer following the mouse meanwhile: a
/// fade waits out a frame a step, and the engine's pointer moves through its
/// delays, which run timerProc (handleMouseMoved, event.cpp:691). A press is
/// latched for the loop, as the tick latches one. Inlined into the fades'
/// banks: out of line it is 26 bytes of the fixed region.
[[gnu::always_inline]] inline void fade_frame() {
    const uint16_t was = frames;
    while (frames == was)
        ;
    const mouse::Point pointer = mouse::poll();
    cursor::at(pointer.x, pointer.y);
}

/// The shadow on the display, entry for entry.
///
/// A zone stores 0..63 and the registers hold their nybbles the other way
/// round, so the scaling and the swap happen here rather than in the shadow:
/// what the shadow holds is what the file held, which is what a fade steps
/// down.
inline void show_palette() {
    // The bank the writes reach and the one the display reads have to agree,
    // and hardware boots them disagreeing.
    VICIV.ctrla |= VIC3_PAL_MASK;
    VICIV.palsel &= 0x0C;
    const uint8_t* from = shadow_palette;
    for (uint16_t i = 0; i < PALETTE_ENTRIES; ++i) {
        PALETTE.red[i] = swapped(*from++);
        PALETTE.green[i] = swapped(*from++);
        PALETTE.blue[i] = swapped(*from++);
    }
    palette_dirty = 0;
    palette_black = 0;
}

/// One channel n steps down the ramp, clamped at black.
[[nodiscard]] inline uint8_t faded(uint8_t value, uint16_t taken) {
    return value > taken ? static_cast<uint8_t>(value - taken) : 0;
}

/// Fade the display down to black, and hold it there.
///
/// The engine's own shape: 64 steps of 4 for Simon 1, the screen following
/// each step (vc62_fastFadeOut, vga_ww.cpp:198). The shadow is left alone, so
/// the palettes a script loads while the screen is black are waiting when the
/// fade-in releases them.
inline void vga_fade_out(uint8_t window) {
    const uint16_t entries = window == ROOM_WINDOW ? FADE_ENTRIES : PALETTE_ENTRIES;
    // The engine fades its current palette in place (paletteFadeOut,
    // gfx.cpp:1092), so a fade while the screen is still black takes black to
    // black -- a room's script runs fade out, fade in, and the next picture's
    // fade out in one tick (PICTURE runs it at once). This one steps off the
    // shadow, which is the room: fading again would light it up to fade it.
    // The hold is left as it is, so a fade-in already asked for still shows
    // the new room at the next frame.
    if (palette_black != 0)
        return;
    for (uint8_t step = 0; step < FADE_STEPS; ++step) {
        fade_frame();
        // Stepped off the shadow, not off a copy of it: subtracting FADE_BY and
        // clamping at nought, n times over, is subtracting n * FADE_BY once. The
        // copy walked 768 bytes of near memory for a value the step number
        // already gives, and near memory is what this program runs out of.
        const uint16_t taken = static_cast<uint16_t>(step + 1) * FADE_BY;
        VICIV.ctrla |= VIC3_PAL_MASK;
        VICIV.palsel &= 0x0C;
        // Only the entries it fades: the engine puts up _fastFadeCount of them
        // and leaves the rest of the hardware palette alone
        // (setPalette(_currentPalette, 0, _fastFadeCount), vga_ww.cpp:229).
        const uint8_t* from = shadow_palette;
        for (uint16_t i = 0; i < entries; ++i) {
            PALETTE.red[i] = swapped(faded(*from++, taken));
            PALETTE.green[i] = swapped(faded(*from++, taken));
            PALETTE.blue[i] = swapped(faded(*from++, taken));
        }
    }
    palette_held = 1;
    palette_black = 1;
    palette_dirty = 0;
}

/// FADE_TO_BLACK, script opcode 187, as a room is left (os1_specialFade,
/// script_s1.cpp:561-575): 32 steps of 8 on the engine's 0-255 scale, two on
/// the file's 0-63, paced as vga_fade_out is. The pointer's entries 32-47 and
/// the text's 192-207 stay lit. The engine fades its current palette in
/// place, so what it took down stays black until the next room sets its own:
/// the shadow goes black too.
inline void fade_to_black() {
    constexpr uint8_t STEPS = 32, BY = 2;
    struct Span {
        uint8_t first, count;
    };
    constexpr Span SPANS[] = {{0, 32}, {48, 144}, {208, 48}};
    // A screen already black has nothing to fade, and stepping off the shadow
    // would light the room up to fade it, as vga_fade_out says.
    if (palette_black == 0) {
        VICIV.ctrla |= VIC3_PAL_MASK;
        VICIV.palsel &= 0x0C;
    }
    for (uint8_t step = 0; palette_black == 0 && step < STEPS; ++step) {
        fade_frame();
        const uint16_t taken = static_cast<uint16_t>(step + 1) * BY;
        for (const Span& span : SPANS) {
            const uint8_t* from = shadow_palette + span.first * 3;
            for (uint8_t i = 0; i < span.count; ++i) {
                const uint8_t entry = static_cast<uint8_t>(span.first + i);
                PALETTE.red[entry] = swapped(faded(*from++, taken));
                PALETTE.green[entry] = swapped(faded(*from++, taken));
                PALETTE.blue[entry] = swapped(faded(*from++, taken));
            }
        }
    }
    for (const Span& span : SPANS)
        __builtin_memset(shadow_palette + span.first * 3, 0, span.count * 3U);
}

/// And let it back. The fast form is not a ramp at all: the engine sets the
/// whole display palette in one go once the flag comes off
/// (fastFadeIn, draw.cpp:1043-1051), which is the new scene arriving at once
/// on a screen that is already black.
inline void vga_fade_in() {
    palette_held = 0;
    palette_dirty = 1;
}

/// The display list, which nothing draws yet. Counting it is how a run shows
/// the animation VM reached the point of having something to say.
inline void vga_draw(const VgaSprite*, uint8_t count) {
    report::counts[report::DRAWN] = static_cast<uint16_t>(report::counts[report::DRAWN] + 1);
    report::counts[report::SPRITES] = count;
}

} // namespace agos
