// SPDX-License-Identifier: GPL-3.0-or-later

// The save slots: their files on the card, the image and its checksum, the F
// keys that save and load them, and the border flash after. The game's own
// SAVE_USER_GAME and LOAD_USER_GAME hooks are defined here too.

#include "saves.hpp"

#include "atticmap.hpp"
#include "banks.hpp"
#include "cardfs.hpp"
#include "cursor.hpp"
#include "display.hpp"
#include "fat32.hpp"
#include "savegame.hpp"
#include "script_vm.hpp"
#include "target_hooks.hpp"
#include "vmstate.hpp"

#include <mega65.h>

namespace {

/// Whether the pointer was hidden when the key was pressed: a save or load
/// key is refused then, decided once so the tick's unwind and the loop's load
/// cannot disagree.
uint8_t key_refused = 0;

/// The F key the tick took, for the loop to save or load by.
uint8_t slot_key = 0;

/// Four slots on F keys; slot 0 is the postcard (F1 loads, Shift+F1 saves);
/// F3/F4 slot 1, to F7/F8. F1 to F8 are $F1 to $F8 in ASCIIKEY
/// (matrix_to_ascii.vhdl:91-94, :168-171), so an odd key loads.
constexpr uint8_t FIRST_SLOT_KEY = 0xF1;
constexpr uint8_t LAST_SLOT_KEY = 0xF8;
[[nodiscard]] constexpr bool is_slot_key(uint8_t key) {
    return key >= FIRST_SLOT_KEY && key <= LAST_SLOT_KEY;
}
[[nodiscard]] constexpr bool is_load_key(uint8_t key) {
    return is_slot_key(key) && (key & 1) != 0;
}
[[nodiscard]] constexpr uint8_t slot_of(uint8_t key) {
    return static_cast<uint8_t>((key - FIRST_SLOT_KEY) >> 1);
}

/// What a save or load key did, shown by the border for half a second: the
/// only way to see the card was written. The border takes a palette index and
/// the game owns all 256, so the flash is the room's entry nearest the colour.
enum Flash : uint8_t { FLASH_FAILED, FLASH_SAVED, FLASH_LOADED };
constexpr uint8_t FULL = 63; // a palette component's top (0..63)

/// The palette entry nearest pure red, green or blue for @p flash, by summed
/// difference.
CODE_BANK(AGOS_SAVE_BANK) static uint8_t nearest_flash_colour(Flash flash) {
    uint8_t best = 0;
    uint16_t best_distance = UINT16_MAX;
    for (uint16_t entry = 0; entry < agos::PALETTE_ENTRIES; ++entry) {
        uint16_t distance = 0;
        for (uint8_t c = 0; c < 3; ++c) {
            const uint8_t have = agos::shadow_palette[entry * 3 + c];
            const uint8_t want = c == flash ? FULL : 0;
            distance = static_cast<uint16_t>(distance + (have > want ? have - want : want - have));
        }
        if (distance < best_distance) {
            best_distance = distance;
            best = static_cast<uint8_t>(entry);
        }
    }
    return best;
}
constexpr uint16_t FLASH_FRAMES = 25;
/// The frame the flash ends on, or nought when there is none.
uint16_t flash_ends = 0;
/// Whether the last save or load key's work landed.
uint8_t key_worked = 0;

/// Set while Simon idles (subroutine 160). The game saves on a click, which
/// ends the idle first (subroutine 0 runs 161), so its saves never hold the
/// bit; one that does loads with no idle animation left to send the sync 161
/// waits for, and stalls the next click for 50 s or more.
constexpr uint16_t IDLE_BIT = 70;
constexpr uint16_t IDLE_MASK = 1U << (IDLE_BIT % 16);

} // namespace

/// The save files' names on the card, a slot each (genSaveName,
/// saveload.cpp:89); slot 0 is the postcard's. In the card bank, which is what
/// reads them, so the fixed region pays nothing.
constexpr uint8_t SAVE_SLOTS = 4;
constexpr uint8_t SAVE_NAME_BYTES = sizeof "SIMON1.000";
RODATA_BANK(AGOS_CARD_BANK)
static const char SAVE_FILE[SAVE_SLOTS][SAVE_NAME_BYTES] = {
    "SIMON1.000", "SIMON1.001", "SIMON1.002", "SIMON1.003"};
static_assert((LAST_SLOT_KEY - FIRST_SLOT_KEY + 1) / 2 == SAVE_SLOTS);
/// Each slot's name, for an index without a multiply.
SAVE_CONST static const char* const SAVE_NAME[SAVE_SLOTS] = {
    SAVE_FILE[0], SAVE_FILE[1], SAVE_FILE[2], SAVE_FILE[3]};

/// The slot the next save or load uses: an F key's, else the postcard's.
SAVE_DATA static uint8_t save_slot;

/// The image, worked on near while the save bank is mapped.
SAVE_DATA static uint8_t save_image[agos::SAVE_IMAGE_BYTES];

/// Where the next sector of a save comes from while the card walks, and the
/// image's length and sum, kept here rather than in locals because the walk
/// calls back into this bank (banks.hpp, banked_reenter).
SAVE_DATA static agos::Place save_from;
SAVE_DATA static uint16_t save_bytes, save_sum;

/// Where DMA finds the image (in_bank).
[[nodiscard, gnu::always_inline]] static agos::Place save_image_place() {
    return in_bank(BANK_BASE_OF(AGOS_SAVE_BANK), save_image);
}

/// The slot's file through SAVEGAME, read back or written as card_writing says;
/// its length, nought when it is not on the card.
CODE_BANK(AGOS_SAVE_BANK) static uint32_t read_slot_file() {
    return agos::read_game_file(SAVE_NAME[save_slot], atticmap::SAVEGAME, atticmap::SAVEGAME_BYTES);
}

/// Fletcher's two sums, mod 256: enough to tell a sector that did not land
/// from one that did, without a second 3.5 KB buffer to compare against.
CODE_BANK(AGOS_SAVE_BANK) static uint16_t image_sum(uint16_t bytes) {
    uint8_t low = 0, high = 0;
    for (uint16_t i = 0; i < bytes; ++i) {
        low = static_cast<uint8_t>(low + save_image[i]);
        high = static_cast<uint8_t>(high + low);
    }
    return static_cast<uint16_t>(high << 8 | low);
}

/// One sector of a save, asked for by the card reader's walk while
/// card_writing is set.
extern "C" CODE_BANK(AGOS_SAVE_BANK) void card_store_banked() {
    if (!card::store_sector(agos::card_sector, save_from))
        agos::card_writing = 0;
    save_from += fat32::SECTOR_BYTES;
}

/// The game's state to its slot's file, which is staged at its full size so that
/// writing it changes nothing in the FAT. The walk lands the card's read-back
/// over the source, and that is summed against the image: a save that did not
/// land is a fault now rather than a lost game later.
extern "C" CODE_BANK(AGOS_SAVE_BANK) void save_game_banked() {
    save_bytes = agos::SaveGame::save(vmstate::script, save_image, sizeof save_image);
    if (save_bytes == 0)
        return; // SAVE_TOO_LONG, raised by the codec
    save_sum = image_sum(save_bytes);
    agos::far_copy(save_image_place(), atticmap::SAVEGAME, save_bytes);
    agos::far_fill(atticmap::SAVEGAME + save_bytes,
        0,
        static_cast<uint16_t>(atticmap::SAVEGAME_BYTES - save_bytes));
    save_from = atticmap::SAVEGAME;
    agos::card_writing = 1;
    const uint32_t file = read_slot_file();
    const bool wrote = agos::card_writing != 0;
    agos::card_writing = 0;
    if (wrote && file >= save_bytes) {
        agos::far_copy(atticmap::SAVEGAME, save_image_place(), save_bytes);
        if (image_sum(save_bytes) == save_sum) {
            key_worked = 1;
            return;
        }
    }
    agos::script_fault(agos::Fault::SAVE_NO_FILE, save_bytes);
}

/// The slot's file into save_image: how many bytes, or nought with the
/// fault raised when it is not on the card.
CODE_BANK(AGOS_SAVE_BANK) static uint16_t read_save() {
    const uint32_t bytes = read_slot_file();
    if (bytes == 0) {
        agos::script_fault(agos::Fault::SAVE_NO_FILE, 0);
        return 0;
    }
    // The file's padding past the largest image is never read.
    const uint16_t held =
        bytes < sizeof save_image ? static_cast<uint16_t>(bytes) : sizeof save_image;
    agos::far_copy(atticmap::SAVEGAME, save_image_place(), held);
    return held;
}

/// The slot, plus one, whose image load_check_banked left checked in
/// save_image, and its length: the load that follows takes it from there
/// rather than off the card again. Nought is none.
SAVE_DATA static uint8_t checked_slot;
SAVE_DATA static uint16_t checked_held;

/// The slot's file into the game. False, with the fault raised, when it is not on
/// the card or is not this game's.
CODE_BANK(AGOS_SAVE_BANK) static bool load_game() {
    const uint16_t held = checked_slot == save_slot + 1 ? checked_held : read_save();
    checked_slot = 0;
    return held != 0 && agos::SaveGame::load(vmstate::script, save_image, held);
}

/// Whether load key slot_key's slot would load, asked before the tick
/// unwinds the running script for it: a blank slot refuses the key, and the
/// script runs on. Refusal is key_refused, which the border then shows.
extern "C" CODE_BANK(AGOS_SAVE_BANK) void load_check_banked() {
    save_slot = slot_of(slot_key);
    checked_held = read_save();
    if (checked_held != 0 && agos::SaveGame::check(vmstate::script, save_image, checked_held))
        checked_slot = static_cast<uint8_t>(save_slot + 1);
    else
        key_refused = 1;
    save_slot = 0;
}

/// LOAD_USER_GAME, inside a script: the script that asked rebuilds the scene
/// itself (gameamiga subroutine 141), as it does in the engine.
extern "C" CODE_BANK(AGOS_SAVE_BANK) void load_game_banked() {
    (void)load_game();
}

/// The load key, from the main loop with no script running, so nothing is
/// left to rebuild the scene: saves::reloaded() does it instead.
extern "C" CODE_BANK(AGOS_SAVE_BANK) void load_key_banked() {
    if (!load_game())
        return;
    key_worked = 1;
    saves::reloaded();
}

/// The save and load keys, and the border saying how each went.
///
/// Refused while the pointer is hidden, as ScummVM refuses its quick save and
/// load (quickLoadOrSave, saveload.cpp:127-140): the intro, cutscenes and
/// conversations, where the game itself offers neither. A load there skips
/// what the intro's end sets up -- the panel, the sentence ink, the first
/// room's palettes -- and Escape gets past it. A save is refused too while
/// Simon idles (IDLE_BIT).
extern "C" CODE_BANK(AGOS_SAVE_BANK) void save_load_key_banked() {
    key_worked = 0;
    const bool load = is_load_key(slot_key);
    if (!load && (vmstate::script.bits()[IDLE_BIT / 16] & IDLE_MASK) != 0)
        key_refused = 1;
    if (key_refused == 0) {
        save_slot = slot_of(slot_key);
        if (load)
            load_key_banked();
        else
            save_game_banked();
        save_slot = 0;
    }
    Flash flash = FLASH_SAVED;
    if (key_worked == 0)
        flash = FLASH_FAILED;
    else if (load)
        flash = FLASH_LOADED;
    VICIV.bordercol = nearest_flash_colour(flash);
    flash_ends = static_cast<uint16_t>(frames_now() + FLASH_FRAMES);
    if (flash_ends == 0)
        flash_ends = 1; // nought means no flash
}

extern "C" CODE_BANK(AGOS_SCRIPT_BANK) void slot_key_banked() {
    if (is_slot_key(slot_key))
        banked_call(AGOS_SAVE_BANK, save_load_key_banked); // Attic: cold
}

namespace saves {

bool press(uint8_t key) {
    slot_key = key;
    key_refused = cursor::visible() ? 0 : 1;
    if (!is_load_key(key) || key_refused != 0)
        return false;
    banked_call(AGOS_SAVE_BANK, load_check_banked);
    return key_refused == 0;
}

void act() {
    // Through the door, which sorts the rest out: the fixed region has no
    // room for a compare apiece.
    if (slot_key != 0) {
        banked_call(AGOS_SCRIPT_BANK, slot_key_banked);
        slot_key = 0;
    }
}

void flash_over(uint16_t now) {
    if (flash_ends != 0 && static_cast<int16_t>(now - flash_ends) >= 0) {
        VICIV.bordercol = 0;
        flash_ends = 0;
    }
}

} // namespace saves

namespace agos {

void script_save_game() {
    banked_call(AGOS_SAVE_BANK, save_game_banked);
}
void script_load_game() {
    banked_call(AGOS_SAVE_BANK, load_game_banked);
}

} // namespace agos
