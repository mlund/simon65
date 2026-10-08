// SPDX-License-Identifier: GPL-3.0-or-later

// The voice off SPEECH.BIN and the effects of SETnn.BIN: what plays on
// channel 3 besides the music. Its state and doors are private; the VM's
// sound hooks are defined here, beside what they drive.

#include "talk.hpp"

#include "atticmap.hpp"
#include "banks.hpp"
#include "diagnostics.hpp"
#include "display.hpp"
#include "fat32.hpp"
#include "script_vm.hpp"
#include "sound.hpp"
#include "target_hooks.hpp"
#include "vga_vm.hpp"

/// A voice off the card: its head before it plays, the rest a frame at a time
/// behind it. The card is far faster than the voice's 43 sectors a second at
/// 22,050 Hz, so the head need only cover the frames to the pump's next turn,
/// and a zone load's stall.
namespace talk {
namespace {
constexpr uint16_t HEAD_SECTORS = 16; // 8 KB, 186 ms
constexpr uint16_t PUMP_SECTORS = 8;  // a frame's share
constexpr uint16_t SECTOR = fat32::SECTOR_BYTES;
static_assert(HEAD_SECTORS * SECTOR / sound::PAGE >= sound::RING_PAGES,
    "the head primes the ring whole, so none of it starts as silence");

uint16_t voice = 0;       // asked for by speak()
uint32_t next_sector = 0; // in SPEECH.BIN
agos::Place next_into = 0;
uint16_t sectors_left = 0;
uint16_t pages_left = 0; // of the voice, not yet handed over
uint16_t began = 0;      // the frame it was asked for

uint16_t effect_asked = 0; // asked for by play_effect()
constexpr uint8_t NO_SET = 0xFF;
constexpr uint8_t SETS = 100;  // two digits in SETnn.BIN
uint16_t sound_set = 0;        // asked for by SOUND_SET
uint8_t sound_set_in = NO_SET; // whose effects are in the Attic

/// SPEECH.BIN, mapped once at start.
card::Runs<agos::DoorCard> runs;

/// Read @p n sectors and hand the refill the pages they hold.
SOUND_BANKED bool fetch(uint16_t n) {
    if (!runs.read(next_sector, next_into, n))
        return false;
    next_sector += n;
    next_into += agos::Place{n} * SECTOR;
    sectors_left = static_cast<uint16_t>(sectors_left - n);
    return true;
}

/// Pages of the voice in @p n sectors just read: two a sector, but the last
/// sector's padding is not voice.
SOUND_BANKED uint16_t pages_in(uint16_t n) {
    const uint16_t pages = static_cast<uint16_t>(n * (SECTOR / sound::PAGE));
    const uint16_t given = pages < pages_left ? pages : pages_left;
    pages_left = static_cast<uint16_t>(pages_left - given);
    return given;
}
} // namespace
} // namespace talk

/// Channel 3 set up, and SPEECH.BIN mapped for reading from anywhere: no
/// speech if it is missing or too scattered, which SPEECH_RUNS reads as nought.
extern "C" SOUND_BANKED void speech_map_banked() {
    sound::begin();
    report::counts[report::SPEECH_RUNS] =
        talk::runs.map(atticmap::SPEECH_FILE, atticmap::SPEECH_RUNS, atticmap::SPEECH_RUNS_MOST);
}

/// SETnn.BIN for talk::sound_set, unless it is the one in already. A set
/// that will not read leaves effects silent. What is playing of the old one
/// stops first: its bytes are about to be written over.
extern "C" SOUND_BANKED void sound_set_banked() {
    if (talk::sound_set == talk::sound_set_in)
        return;
    sound::stop_effect();
    talk::sound_set_in = talk::NO_SET;
    if (talk::sound_set >= talk::SETS)
        return;
    char name[] = "SET00.BIN";
    name[3] = static_cast<char>('0' + talk::sound_set / 10);
    name[4] = static_cast<char>('0' + talk::sound_set % 10);
    // The door clears card_name before it returns, which the analyzer cannot
    // see through banked_call.
    // NOLINTBEGIN(clang-analyzer-core.StackAddressEscape)
    if (agos::read_game_file(name, atticmap::EFFECTS, atticmap::EFFECTS_BYTES) != 0)
        talk::sound_set_in = talk::sound_set;
    // NOLINTEND(clang-analyzer-core.StackAddressEscape)
}

/// Start talk::effect_asked from the set in: the effect playing stops, as a new
/// effect cuts the old on the CD32, and speech plays on.
extern "C" SOUND_BANKED void effect_banked() {
    if (talk::sound_set_in == talk::NO_SET || talk::effect_asked >= atticmap::EFFECT_IDS)
        return;
    uint16_t entry[2]; // first page, pages
    agos::far_read(atticmap::EFFECTS + agos::Place{talk::effect_asked} * sizeof entry,
        reinterpret_cast<uint8_t*>(entry),
        sizeof entry);
    if (entry[1] != 0)
        sound::effect(atticmap::EFFECTS + agos::Place{entry[0]} * sound::PAGE, entry[1]);
}

/// The voice cut, as a skipped cutscene cuts it (endCutscene,
/// subroutine.cpp:253-262): an effect plays on.
extern "C" SOUND_BANKED void voice_stop_banked() {
    sound::hush();
    talk::sectors_left = 0;
}

/// STOP_ALL_SOUNDS: both streams, and the speech's reading.
extern "C" SOUND_BANKED void sounds_stop_banked() {
    sound::stop();
    talk::sectors_left = 0;
}

/// Start talk::voice: the speech that was playing stops, as a new voice cuts
/// the old on the CD32 (runit2 0x1cd4a), and an effect plays on. A voice not
/// on the card is silence; a cue past the table (9999) leaves the old one
/// playing (playSpeech, res_snd.cpp:56-65).
extern "C" SOUND_BANKED void speak_banked() {
    if (talk::voice >= atticmap::VOICES)
        return;
    voice_stop_banked();
    uint32_t entry[2]; // first sector, bytes
    agos::far_read(atticmap::VOICE_INDEX + agos::Place{talk::voice} * sizeof entry,
        reinterpret_cast<uint8_t*>(entry),
        sizeof entry);
    if (entry[1] == 0)
        return;
    report::counts[report::VOICE_SPOKEN] = talk::voice;
    talk::began = frames_now();
    talk::next_sector = entry[0];
    talk::next_into = atticmap::SOUND;
    talk::sectors_left = static_cast<uint16_t>((entry[1] + talk::SECTOR - 1) / talk::SECTOR);
    talk::pages_left = static_cast<uint16_t>((entry[1] + sound::PAGE - 1) / sound::PAGE);
    const uint16_t head =
        talk::sectors_left < talk::HEAD_SECTORS ? talk::sectors_left : talk::HEAD_SECTORS;
    if (!talk::fetch(head)) {
        talk::sectors_left = 0;
        return;
    }
    sound::speak(atticmap::SOUND, talk::pages_in(head), talk::sectors_left != 0);
}

/// One frame's share of the voice off the card.
extern "C" SOUND_BANKED void speech_pump_banked() {
    const Stamp began = stamp();
    const uint16_t n =
        talk::sectors_left < talk::PUMP_SECTORS ? talk::sectors_left : talk::PUMP_SECTORS;
    if (talk::fetch(n)) {
        sound::arrived(static_cast<uint8_t>(talk::pages_in(n)));
    } else {
        talk::sectors_left = 0; // a card error: play what landed
    }
    const uint16_t took = lines_since(began.frame, began.line);
    report::count_add(report::PUMP_LINES, took);
    report::note_peak(report::WORST_PUMP, took);
    if (talk::sectors_left == 0) {
        sound::all_arrived();
        report::counts[report::VOICE_LOAD_FRAMES] =
            static_cast<uint16_t>(frames_now() - talk::began);
    }
}

namespace talk {

void begin() {
    banked_call(AGOS_SOUND_BANK, speech_map_banked);
}

void speak(uint16_t asked) {
    voice = asked;
    banked_call(AGOS_SOUND_BANK, speak_banked);
}

void stop_voice() {
    banked_call(AGOS_SOUND_BANK, voice_stop_banked);
}

void pump() {
    if (sectors_left != 0)
        banked_call(AGOS_SOUND_BANK, speech_pump_banked);
}

} // namespace talk

/// Start effect @p id from the set in; both machines ask for them.
static void play_effect(uint16_t id) {
    talk::effect_asked = id;
    banked_call(AGOS_SOUND_BANK, effect_banked);
}

namespace agos {

void script_effect(uint16_t id) {
    play_effect(id);
}

/// Load SETnn.BIN for set @p set, unless it is in already.
void script_sound_set(uint16_t set) {
    talk::sound_set = set;
    banked_call(AGOS_SOUND_BANK, sound_set_banked);
}

/// Silence both, and stop reading the voice.
void vga_stop_sounds() {
    banked_call(AGOS_SOUND_BANK, sounds_stop_banked);
}

void vga_effect(uint16_t id) {
    play_effect(id);
}

} // namespace agos
