// Where everything lives in Attic RAM.
//
// Eight megabytes the VIC and the audio DMA cannot see, so nothing displayed
// and nothing played goes here -- it is where data waits to be copied down.
// Reads cost 1.18x chip RAM a byte at a time and 8.1x by DMA, measured on
// hardware, so what lives here is what is read rarely or in bulk.

#pragma once

#include "chipmap.hpp"
#include "far.hpp"

#include <stdint.h>

namespace atticmap {

/// A slot is a shift, so an address costs no multiply.
inline constexpr uint8_t SHIFT = 16; // 64 KiB a slot

[[nodiscard]] constexpr agos::Place slot(agos::Place base, uint8_t index) {
    return base + (static_cast<agos::Place>(index) << SHIFT);
}

/// Where Attic starts. Everything below is chip RAM.
inline constexpr agos::Place BASE = 0x8000000;
inline constexpr uint32_t BYTES = 0x800000; // 8 MiB, the whole of it

/// The mapper's own Attic banks, which start where Attic does.
///
/// The SDK bases bank n at BASE + (n - 13) * BANK_SLOT, banks 13 to 31
/// (mega65-banked-nokernal/include/mapper.h:36-55). The stride is $6000
/// whatever the window size, so a smaller window leaves holes rather than
/// packing the banks tighter.
inline constexpr uint32_t BANK_SLOT = 0x6000;
inline constexpr uint32_t BANK_COUNT = 19; // banks 13 to 31
inline constexpr agos::Place BANKS = BASE;
inline constexpr uint32_t BANKS_BYTES = BANK_COUNT * BANK_SLOT;
inline constexpr agos::Place BANKS_END = BANKS + BANKS_BYTES;

/// Where this program's data starts: clear of every bank the mapper can place.
///
/// Not of every bank *declared* -- the count a program gives says which banks
/// are loaded, not which are addressable, and the card is read by more than one
/// program. A region below this line is overwritten by the bank loader in
/// silence, which is how twp65 lost a room's art.
///
/// Rounded up to a 64 KB slot so the addresses stay readable.
inline constexpr agos::Place DATA = 0x8080000;
static_assert(DATA >= BANKS_END, "the Attic data would sit under a code bank");

/// One zone's pixel file as it arrives from the card. Largest is 230,962 bytes;
/// chip RAM cannot hold it.
inline constexpr agos::Place PACKED = DATA;
inline constexpr uint32_t PACKED_BYTES = 0x40000; // 256 KiB

/// gameamiga's global string table, which is read-only and has no business in
/// the 64 KB a pointer reaches: 6,127 bytes of item names and their look-at
/// lines. GameDb keeps a near index of offsets into it, so showing a line
/// reads only that string -- 58 bytes at the longest in this release.
inline constexpr agos::Place GAMETEXT = PACKED + PACKED_BYTES;
inline constexpr uint32_t GAMETEXT_BYTES = 0x2000; // 8 KiB

/// The game's two character sets, lifted out of its executable by mkfont.py
/// and staged to the card. Attic and not chip RAM because **nothing the VIC
/// reads is a font**: the CPU reads these to build glyphs, and it is the
/// glyphs -- in the figure pool, in chip RAM -- that the VIC fetches.
///
/// The windows' font is fixed at six by eight, one bit a pixel, 98 glyphs
/// from character 32 (`windowDrawChar`, charset-fontdata.cpp:2917). Speech
/// over an actor uses the other one: 90 characters of 41 bytes, ten rows of
/// three shading masks and an outline, proportional, from '!'
/// (`renderStringAmiga`, :1101).
inline constexpr agos::Place WINFONT = GAMETEXT + GAMETEXT_BYTES;
inline constexpr uint32_t WINFONT_BYTES = 1024;
inline constexpr agos::Place SAYFONT = WINFONT + WINFONT_BYTES;
inline constexpr uint32_t SAYFONT_BYTES = 4096;

/// One TEXTnn file, the room's own strings, resident as the engine keeps it:
/// a room's lines come from one file and the next room's from another
/// (string.cpp:310). The largest in this release is 2,857 bytes.
inline constexpr agos::Place LOCALTEXT = SAYFONT + SAYFONT_BYTES;
inline constexpr uint32_t LOCALTEXT_BYTES = 4096;

/// What a run leaves for the monitor to read: the animation VM's sprite list
/// and what the draw did with each entry (diagnostics.hpp). Here and not in chip
/// RAM because nothing reads it but the serial monitor -- not the VIC, not
/// the audio DMA -- and chip RAM is the map with nothing to spare.
inline constexpr agos::Place DIAGNOSTICS = LOCALTEXT + LOCALTEXT_BYTES;
inline constexpr uint32_t DIAGNOSTICS_BYTES = 2048;

/// One file from the card at a time, and nothing else is ever in flight.
///
/// Attic and not chip RAM: it is a staging buffer, not resident. gameamiga is
/// parsed out of here into the item records and GAMETEXT, a TABLES file is
/// transcoded out into the script heap, tbllist is copied near -- and then the
/// bytes are free again. Nothing the VIC or the audio DMA reads ever lands
/// here, and the card writes it by DMA at a 28-bit address either way
/// (sdcard.hpp:25), so the move costs one file copy at Attic rates on a load
/// that already takes a quarter of a second.
///
/// Sized by the largest file that comes through it, which is gameamiga at
/// 30,139; the next is TABLES07 at 25,583 and the largest TEXTnn is 2,857.
inline constexpr agos::Place LOAD = DIAGNOSTICS + DIAGNOSTICS_BYTES;
inline constexpr uint32_t LOAD_BYTES = 31744;
static_assert(LOAD_BYTES >= 30139, "gameamiga does not fit the load area");

/// The panel's own picture, the verb bar, laid out as chipmap::PANEL's grid.
/// The master copy, so chip PANEL is a buffer: a paint goes here, then the
/// whole of it is copied down.
inline constexpr agos::Place PANEL_MASTER = LOAD + LOAD_BYTES;
inline constexpr uint32_t PANEL_MASTER_BYTES =
    static_cast<uint32_t>(chipmap::PANEL_CELLS) * chipmap::GLYPH_BYTES;

/// The animation VM's windows: x in sixteen-pixel units, y and height in
/// lines (_videoWindows, vga.cpp:1073). Near RAM has no 51 bytes left, and a
/// DRAW reading three from here costs nothing a frame notices. The release's
/// SET_WINDOW names 4, 5 and 16.
inline constexpr uint8_t VIDEO_WINDOWS = 17;
inline constexpr uint8_t WINDOW_ENTRY_BYTES = 3;
inline constexpr agos::Place WINDOW_ORIGINS = PANEL_MASTER + PANEL_MASTER_BYTES;
inline constexpr uint32_t WINDOW_ORIGINS_BYTES = WINDOW_ENTRY_BYTES * VIDEO_WINDOWS;

/// A save file, SIMON1.00n, as it goes to and comes from the card: whole
/// sectors, because the card is written a sector at a time along a file staged
/// at this size. Seven of them, where this release's largest image is 3,182
/// bytes: 3,086 and six a timer, sixteen timers at most (savegame.hpp).
inline constexpr agos::Place SAVEGAME = WINDOW_ORIGINS + WINDOW_ORIGINS_BYTES;
inline constexpr uint16_t SAVEGAME_BYTES = 3584; // seven sectors

/// Which script lines ran: an arm byte, spare, next entry's index
/// (little-endian), then a ring of (subroutine id, line's offset in subroutine),
/// little-endian. Only the monitor arms it; sub 160 runs every tick and would
/// wrap it at once.
inline constexpr agos::Place LINE_TRACE = SAVEGAME + SAVEGAME_BYTES;
inline constexpr uint16_t LINE_TRACE_ENTRIES = 4096;
inline constexpr uint8_t LINE_TRACE_HEADER = 4;
inline constexpr uint32_t LINE_TRACE_BYTES = LINE_TRACE_HEADER + 4UL * LINE_TRACE_ENTRIES;

/// The inventory's pictures, read once at start: full-colour column strips,
/// 576 bytes for each of 97.
inline constexpr agos::Place ICONS = LINE_TRACE + LINE_TRACE_BYTES;
inline constexpr uint16_t ICON_BYTES = 576;
inline constexpr uint8_t ICON_COUNT = 97; // icon.pkd's table
inline constexpr uint16_t ICONS_BYTES = ICON_COUNT * ICON_BYTES;
inline constexpr const char* ICONS_FILE = "ICONS.BIN";

/// Where each voice is in SPEECH.BIN: first sector and length in bytes,
/// eight bytes a voice id, zero for none.
inline constexpr uint16_t VOICES = 3624;
inline constexpr agos::Place VOICE_INDEX = ICONS + ICONS_BYTES;
inline constexpr uint32_t VOICE_INDEX_BYTES = 8UL * VOICES;
inline constexpr const char* VOICE_INDEX_FILE = "SPEECH.IDX";

/// SPEECH.BIN's clusters as runs of card sectors, mapped once at start
/// (cardfs.hpp): a card staged in one go keeps it to one run or a few.
inline constexpr agos::Place SPEECH_RUNS = VOICE_INDEX + VOICE_INDEX_BYTES;
inline constexpr uint16_t SPEECH_RUNS_MOST = 512;
inline constexpr uint32_t SPEECH_RUNS_BYTES = 8UL * SPEECH_RUNS_MOST;
inline constexpr const char* SPEECH_FILE = "SPEECH.BIN";

/// What the rounding leaves between the staging buffer and the slotted regions
/// above it. Named and empty, for chipmap's reason: bytes nothing is named for
/// get taken by whoever notices them first, and TUNES has to start on a slot
/// boundary because atticmap::slot() indexes from it by a shift.
inline constexpr agos::Place SPARE = SPEECH_RUNS + SPEECH_RUNS_BYTES;
inline constexpr uint32_t SPARE_BYTES = 0x8100000 - SPARE;

/// Pattern data is read from here all through a row, which measured no slower
/// than chip RAM.
inline constexpr agos::Place TUNES = 0x8100000;

/// Zone scripts, one file to a slot. Writable: vc20_setRepeat stores its loop
/// counter inside the script it is running, in 59 places across the release.
inline constexpr agos::Place ZONESCRIPTS = 0x8180000;
inline constexpr uint8_t ZONE_SLOTS = 8;

/// The zones' figures, decoded once when a zone arrives.
///
/// Not decoded per frame: measured, the decoder does 1,501 pixels a frame, so
/// one frame's worth of live figures would take 12.3 of them. From here the
/// pool is filled by DMA at 1,188 KB/s, which is 0.76 of a frame for the same
/// bytes.
///
/// A slot holds a scene's worth of images, not a zone whole, because images
/// are decoded on demand as they are drawn. figures.hpp asserts against the
/// largest single figure. One arena of pages (figures.hpp), less the
/// sound room below.
///
/// The room space is where this came from: 5 MiB reserved for a cache of
/// decoded backdrops that no code has ever touched. Reserve it again when
/// something caches rooms; the addresses below are the only statement of it.
inline constexpr agos::Place FIGURES = 0x8200000;
inline constexpr uint32_t FIGURES_BYTES = 0x260000 - 0x4000;

/// What the arena holds, said in Attic rather than in near memory.
///
/// Sixteen kilobytes off the top of the arena, 2,048 rows of eight bytes, so
/// a row's place is a shift. Near memory keeps only a hash of (zone, image)
/// pointing in here: an index of five near arrays could name 128 figures --
/// 6% of the arena -- and figures were re-decoded because their name was
/// gone, not their bytes.
inline constexpr agos::Place FIGURE_INDEX = FIGURES + FIGURES_BYTES;
inline constexpr uint32_t FIGURE_INDEX_BYTES = 0x4000; // 16 KiB

/// The sound set's effects, SETnn.BIN, as script opcode 185 chooses them:
/// a header page and effect pages, then the clips page-aligned. Largest is
/// 647,936 bytes.
inline constexpr agos::Place EFFECTS = FIGURE_INDEX + FIGURE_INDEX_BYTES;
inline constexpr uint32_t EFFECTS_BYTES = 0xA0000; // 640 KiB
inline constexpr uint8_t EFFECT_IDS = 141;

/// The voice being spoken, read off the card whole for channel 3 to stream
/// from (sound.hpp): page-aligned, as the refill steps a page at a time. Taken
/// from the figures above; the longest voice is 908,032 bytes.
inline constexpr agos::Place SOUND = EFFECTS + EFFECTS_BYTES;
inline constexpr uint32_t SOUND_BYTES = 0x100000; // 1 MiB
static_assert(SOUND % 256 == 0 && EFFECTS % 256 == 0, "the refill steps whole pages");

/// Every zone's packed pixels as they arrive from the card. Without this
/// cache, a zone reads again when its figure slot is reused (21 frames, 420 ms,
/// in the 50 ms intro tick with five zones).
///
/// Measured: 164 pixel files are 6.90 MiB total, 160 named by scripts are 6.59;
/// a chapter's zones median 0.27 MiB, worst 0.93; largest file 226 KiB. Two
/// megabytes hold several chapters with no zone read twice.
inline constexpr agos::Place ZONEPIX = 0x8600000;
inline constexpr uint32_t ZONEPIX_BYTES = 0x200000; // 2 MiB

/// How many tunes fit below the zone scripts. The map owns this, so a tenant
/// asserts only what it knows -- how many tunes there are.
inline constexpr uint8_t TUNE_SLOTS = (ZONESCRIPTS - TUNES) >> SHIFT;

inline constexpr chipmap::Region MAP[] = {
    {PACKED, PACKED_BYTES},
    {GAMETEXT, GAMETEXT_BYTES},
    {WINFONT, WINFONT_BYTES},
    {SAYFONT, SAYFONT_BYTES},
    {LOCALTEXT, LOCALTEXT_BYTES},
    {DIAGNOSTICS, DIAGNOSTICS_BYTES},
    {LOAD, LOAD_BYTES},
    {PANEL_MASTER, PANEL_MASTER_BYTES},
    {WINDOW_ORIGINS, WINDOW_ORIGINS_BYTES},
    {SAVEGAME, SAVEGAME_BYTES},
    {LINE_TRACE, LINE_TRACE_BYTES},
    {ICONS, ICONS_BYTES},
    {VOICE_INDEX, VOICE_INDEX_BYTES},
    {SPEECH_RUNS, SPEECH_RUNS_BYTES},
    {SPARE, SPARE_BYTES},
    {TUNES, static_cast<uint32_t>(TUNE_SLOTS) << SHIFT},
    {ZONESCRIPTS, static_cast<uint32_t>(ZONE_SLOTS) << SHIFT},
    {FIGURES, FIGURES_BYTES},
    {FIGURE_INDEX, FIGURE_INDEX_BYTES},
    {EFFECTS, EFFECTS_BYTES},
    {SOUND, SOUND_BYTES},
    {ZONEPIX, ZONEPIX_BYTES},
};

inline constexpr uint8_t REGIONS = sizeof MAP / sizeof MAP[0];

static_assert(chipmap::sound(MAP, REGIONS, BASE + BYTES),
    "the Attic tenants overlap or run past the eight megabytes");
static_assert(
    chipmap::snug(MAP, REGIONS, MAP[0].at, BASE + BYTES), "a byte of Attic nothing is named for");

/// And clear of the mapper. sound() cannot see this: the banks are not tenants
/// of the map, they are placed by the SDK before any of it exists.
static_assert(MAP[0].at >= BANKS_END, "an Attic code bank would overwrite the first tenant");

/// Whether Attic answers, checked against the map's own corners.
///
/// A dead or absent HyperRAM reads back whatever was last on the bus, so a
/// file loads successfully and stores nothing -- the one failure here that
/// looks exactly like success. Every probe is written before any is read, so
/// a bus that only remembers the last write fails; each carries a different
/// value, so two addresses that alias fail; and the second pass writes the
/// complement, so a bit stuck either way fails.
[[nodiscard]] inline bool attic_works() {
    // The map's own tenants, not a list beside it: a probe writes, so an address
    // the map does not own is one it can corrupt. Probing BASE overwrote a code
    // bank's first instruction with this pass's own $5A, and the machine ran
    // garbage. MAP[0].at >= BANKS_END is what makes that unreachable now, and a
    // tenant added later is probed without anyone remembering to add it.
    const auto probe = [](uint8_t i) {
        return i < REGIONS ? MAP[i].at : MAP[REGIONS - 1].at + MAP[REGIONS - 1].bytes - 1;
    };
    for (uint8_t pass = 0; pass < 2; ++pass) {
        const uint8_t flip = pass ? 0xFF : 0x00;
        for (uint8_t i = 0; i <= REGIONS; ++i)
            agos::far_write8(probe(i), static_cast<uint8_t>((0xA5 + i) ^ flip));
        for (uint8_t i = 0; i <= REGIONS; ++i)
            if (agos::far_read8(probe(i)) != static_cast<uint8_t>((0xA5 + i) ^ flip))
                return false;
    }
    return true;
}

} // namespace atticmap

using namespace atticmap;
