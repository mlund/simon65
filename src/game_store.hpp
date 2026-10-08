// SPDX-License-Identifier: GPL-3.0-or-later

// The game's files, and which of them are in memory.
//
// The residency rules here are the engine's, not this port's: gameamiga's
// subroutines stay for ever and exactly one TABLES file is in the heap at a
// time (subroutine.cpp:368), and a zone's scripts are fetched when a sprite in
// it first animates (gfx.cpp:1112). They live here rather than in whatever
// moves the bytes, because they are engine behaviour -- the host and the
// machine differ only in how a named file becomes bytes at an address, which
// is the one thing left to `read_game_file`.

#pragma once

#include "atticmap.hpp"
#include "chipmap.hpp"
#include "script_vm.hpp"
#include "tables.hpp"
#include "vga_vm.hpp"
#include "vga_zone.hpp"
#include "zonepix.hpp"

#ifdef __mos__
#include "banks.hpp"
#else
#ifndef STORE_BANKED
#define STORE_BANKED
#endif
#endif

namespace agos {

/// A zone's file name: NNN1.out for its scripts and palettes, NNN2.out for
/// its pixels. All three digits, because a zone number runs past ninety-nine
/// and a name left at '0' asks the card for a file that is not there.
inline void zone_digits(char out[3], uint8_t zone) {
    out[0] = static_cast<char>('0' + zone / 100);
    out[1] = static_cast<char>('0' + zone / 10 % 10);
    out[2] = static_cast<char>('0' + zone % 10);
}

inline void zone_file(char out[9], uint8_t zone, char kind) {
    zone_digits(out, zone);
    out[3] = kind;
    out[4] = '.';
    out[5] = 'o';
    out[6] = 'u';
    out[7] = 't';
    out[8] = '\0';
}

/// Read a named file from the game's directory to an address. How many bytes
/// it had, or zero if it is not there or will not fit.
///
/// The count is the point rather than a convenience: what a file claims about
/// itself is what bounds the walk over it, and handing the region's capacity
/// to a parser instead means a short or damaged file is walked to the end of
/// the buffer holding it.
[[nodiscard]] uint32_t read_game_file(const char* name, Place into, uint32_t most);

/// The item database, by the name the card holds it under.
///
/// The release calls it gameamiga (nine characters, more than an 8.3 name).
/// The card stores it as GAMEAM~1 with the real name in long-name entries;
/// the machine reads it by 8.3 name. This name is stated here once rather
/// than forked by the preprocessor.
inline constexpr const char* DATABASE_FILE = "GAMEDATA";

/// The header on a joined zone file: the length of the script half, little
/// endian, and the pixels are whatever follows it.
inline constexpr uint8_t ZONE_HEADER = 4;

/// A zone's file name on the card, where its two halves are joined: NNN.zon.
///
/// The release ships them apart, and the card does not, because opening a file
/// costs a linear scan of the directory inside one uninterruptible trap --
/// 253 ms for a zone's scripts and 470 ms for its pixels, measured. One name
/// is one scan, and 164 fewer entries shortens every other scan too.
inline void zone_name(char out[8], uint8_t zone) {
    zone_digits(out, zone);
    out[3] = '.';
    out[4] = 'z';
    out[5] = 'o';
    out[6] = 'n';
    out[7] = '\0';
}

/// Both halves of a zone: the scripts into slot `slot` of ZONESCRIPTS, the
/// pixels into PACKED, from one open of one file.
///
/// The destinations are not parameters because the memory map settles them,
/// and a narrow call is what makes the joined read worth having. False when
/// the scripts did not arrive; `pixels_ok` says whether the other half did,
/// which is a question of its own -- a zone may have no pixels, and the
/// figure cache remembers that rather than asking the card again.
[[nodiscard]] bool read_zone_file(
    uint8_t zone, uint8_t slot, bool* pixels_ok, uint32_t* pixel_bytes);

/// The zone the beard is drawn from, and the file of its pixels with the
/// beard on; off, they are the zone's own (os1_loadBeard, script_s1.cpp:514;
/// loadVGABeardFile maps 328 and 23 to 0119 and 0112, res.cpp:769-776).
inline constexpr uint8_t BEARD_ZONE = 11;
inline constexpr const char* BEARD_FILE = "0119.out";

/// Whether the animation VM still has scripts of this zone. The store cannot
/// see the VM and the VM cannot see the store, so the question crosses here.
[[nodiscard]] bool zone_in_use(uint8_t zone);

/// How many zones are kept at once. Measured: a zone's scripts are a median of
/// 2,246 bytes and a worst of 65,468, so a slot is a full 64 KiB and the game
/// works through them a room at a time.
inline constexpr uint8_t ZONES_KEPT = atticmap::ZONE_SLOTS;

class GameStore {
  public:
    /// The database, the table list, and the subroutines that never leave.
    ///
    /// This must clear prior state: the object is in reserved memory the
    /// linker doesn't zero. Stale zone numbers and slot indices corrupt
    /// initialization.
    ///
    /// `into` is near memory big enough for the item records and the strings;
    /// the block behind them stays in the load area and is transcoded from
    /// there, because it is far too big to bring near.
    [[nodiscard]] bool open(uint8_t* into, uint16_t into_bytes);

    /// Bring in whichever TABLES file claims this subroutine, unless it is
    /// already the one in the heap. False when no file claims it, which for a
    /// resident subroutine is the ordinary answer.
    [[nodiscard]] bool bring_in(uint16_t subroutine_id);

    /// Where this zone's pixels are, held from the last time it was asked for
    /// or read off the card now. Nothing when the card has none for it.
    [[nodiscard]] STORE_BANKED PackedZones::Pixels pixels_of(uint8_t number);

    /// The same, with the zone and the answer in members, because banked_call
    /// takes a function of no arguments -- as bring_in does above.
    void want_pixels(uint8_t number) {
        pixels_zone_ = number;
    }
    void take_pixels() {
        pixels_ = pixels_of(pixels_zone_);
    }
    [[nodiscard]] PackedZones::Pixels pixels() const {
        return pixels_;
    }

    /// Whether answering that cost the arena everything it held, so anything
    /// remembering where a zone's pixels were is now wrong.
    [[nodiscard]] bool pixels_moved() const {
        return pixels_moved_;
    }

    /// The same, with the id and the answer in members: banked_call takes a
    /// function of no arguments, as the animation VM's pending opcode does.
    void want(uint16_t subroutine_id) {
        wanted_ = subroutine_id;
    }
    [[nodiscard]] bool wanted_arrived() const {
        return wanted_ok_;
    }
    void bring_in_wanted() {
        wanted_ok_ = bring_in(wanted_);
    }

    /// LOAD_BEARD and UNLOAD_BEARD (182, 183): BEARD_ZONE's pixels swapped,
    /// and whatever held the old ones forgotten. False when nothing changed.
    STORE_BANKED bool wear_beard(bool on) {
        if (on == beard_)
            return false;
        beard_ = on;
        if (packed_zone_ == BEARD_ZONE)
            packed_zone_ = NO_ZONE;
        zone_pixels.forget(BEARD_ZONE);
        return true;
    }

    /// The zone holding a sprite's scripts, fetched if this is its first sight.
    [[nodiscard]] VgaZone* zone(uint8_t number);

    [[nodiscard]] GameDb& db() {
        return db_;
    }
    [[nodiscard]] SubroutineHeap& heap() {
        return heap_;
    }

    /// Forget every file. On the machine this is a new game; in a test it is
    /// what keeps one case's zone out of the next one.
    void forget() {
        *this = GameStore();
    }

    /// What the monitor reads back to see how far a run got.
    [[nodiscard]] uint8_t tables_loaded() const {
        return tables_loaded_;
    }
    [[nodiscard]] uint8_t zones_loaded() const {
        return zones_loaded_;
    }

    /// How many bytes of pixels the staging area holds, which is the zone's own
    /// length and not the region's.
    [[nodiscard]] uint32_t packed_bytes() const {
        return packed_bytes_;
    }

    /// How often a zone went over one still in use, because every slot was.
    [[nodiscard]] uint8_t zones_forced() const {
        return zones_forced_;
    }

  private:
    /// PACKED holding this zone's pixels, ready to copy out of. False when the
    /// card has no pixels for it.
    [[nodiscard]] bool stage_pixels(uint8_t number);
    /// Whether this zone's pixels are the beard's file rather than its own.
    [[nodiscard]] bool bearded(uint8_t number) const {
        return beard_ && number == BEARD_ZONE;
    }
    /// The same for BEARD_ZONE with the beard on.
    [[nodiscard]] STORE_BANKED bool stage_beard() {
        if (packed_zone_ != BEARD_ZONE) {
            packed_bytes_ = read_game_file(BEARD_FILE, atticmap::PACKED, atticmap::PACKED_BYTES);
            packed_zone_ = packed_bytes_ != 0 ? BEARD_ZONE : NO_ZONE;
        }
        return packed_zone_ == BEARD_ZONE;
    }

    GameDb db_;
    SubroutineHeap heap_;
    TableList tables_;
    /// tbllist, which is 696 bytes in this release; the rest is margin, and
    /// near memory is too tight for much of it.
    uint8_t list_[720];

    VgaZone zones_[ZONES_KEPT];
    uint8_t zone_number_[ZONES_KEPT] = {};
    uint8_t next_slot_ = 0;

    /// Not a zone number the game uses, so it reads as "PACKED holds nothing".
    /// Not nought: zone 0 is the one Simon himself is drawn from (its files are
    /// 0001.out and 0002.out, both in the release), and using it as the empty
    /// mark is why he never appeared.
    static constexpr uint8_t NO_ZONE = 0xFF;

    /// Not a slot, so it reads as "this zone is not resident".
    static constexpr uint8_t NO_SLOT = 0xFF;

    [[nodiscard]] uint8_t slot_of(uint8_t number) const;
    [[nodiscard]] uint8_t free_slot(uint8_t number);
    [[nodiscard]] uint8_t take_slot();
    [[nodiscard]] bool fetch(uint8_t number, uint8_t slot);

    uint8_t packed_zone_ = NO_ZONE; // whose pixels PACKED holds
    uint32_t packed_bytes_ = 0;     // and how many of them

    uint16_t wanted_ = 0; // the subroutine a banked bring_in is for
    bool wanted_ok_ = false;

    bool pixels_moved_ = false; // and whether holding them cost the rest
    uint8_t pixels_zone_ = 0;   // the zone a banked pixels_of is for
    PackedZones::Pixels pixels_;

    const char* resident_ = nullptr; // which TABLES file the heap holds
    uint8_t tables_loaded_ = 0;
    uint8_t zones_loaded_ = 0;
    uint8_t zones_forced_ = 0;
    bool beard_ = false;
};

/// The one store. Declared here and defined once, beside the VMs.
extern GameStore store;

#ifdef __mos__
extern "C" void store_bring_in_banked();
extern "C" void store_pixels_banked();
#endif

// ---------------------------------------------------------------- definitions

inline bool GameStore::open(uint8_t* into, uint16_t into_bytes) {
    // Nothing here may be assumed empty; see the declaration.
    for (uint8_t slot = 0; slot < ZONES_KEPT; ++slot) {
        zones_[slot] = VgaZone();
        zone_number_[slot] = NO_ZONE;
    }
    next_slot_ = 0;
    packed_zone_ = NO_ZONE;
    resident_ = nullptr;
    tables_loaded_ = zones_loaded_ = zones_forced_ = 0;
    beard_ = false;

    const uint32_t database_bytes =
        read_game_file(DATABASE_FILE, atticmap::LOAD, atticmap::LOAD_BYTES);
    if (database_bytes == 0)
        return false;

    // The records come near, where the game writes item state back into them,
    // and the strings go to their own far home -- before tbllist is read below,
    // because that read lands on top of this one.
    if (!db_.load(atticmap::LOAD, database_bytes, into, into_bytes))
        return false;

    heap_.reset(chipmap::SCRIPTS, chipmap::SCRIPTS_BYTES);
    if (!heap_.load(atticmap::LOAD + db_.subroutines(),
            static_cast<uint16_t>(database_bytes - db_.subroutines())))
        return false;
    heap_.make_resident();

    if (!read_game_file("tbllist", atticmap::LOAD, sizeof list_))
        return false;
    far_read(atticmap::LOAD, list_, sizeof list_);
    tables_.reset(list_, sizeof list_);
    return true;
}

/// always_inline because CODE_BANK banks only the wrapper: left to itself
/// this lands back in the fixed region, which is the whole point of moving
/// it. The same trap as the two decoders and FigureCache::want.
[[gnu::always_inline]] inline bool GameStore::bring_in(uint16_t subroutine_id) {
    const char* name = tables_.file_for(subroutine_id);
    if (!name || name == resident_)
        return false;
    const uint32_t bytes = read_game_file(name, atticmap::LOAD, atticmap::LOAD_BYTES);
    if (bytes == 0)
        return false;
    heap_.rewind();
    resident_ = name;
    ++tables_loaded_;
    // The file's own length, not the region's: past the end is whatever the
    // last file left there, and the transcoder would go on walking it.
    return heap_.load(atticmap::LOAD, static_cast<uint16_t>(bytes));
}

inline VgaZone* GameStore::zone(uint8_t number) {
    const uint8_t slot = slot_of(number);
    if (slot != NO_SLOT)
        return &zones_[slot];
    const uint8_t into = free_slot(number);
    return fetch(number, into) ? &zones_[into] : nullptr;
}

/// Which slot holds this zone, or NO_SLOT.
inline uint8_t GameStore::slot_of(uint8_t number) const {
    for (uint8_t slot = 0; slot < ZONES_KEPT; ++slot)
        if (zones_[slot].valid() && zone_number_[slot] == number)
            return slot;
    return NO_SLOT;
}

/// Where a zone should land: its own slot if it has one, else the next in
/// turn. Round-robin rather than least-recently-used, because a room's own
/// zone is asked for constantly and would never be the one evicted anyway.
inline uint8_t GameStore::free_slot(uint8_t number) {
    const uint8_t slot = slot_of(number);
    if (slot != NO_SLOT)
        return slot;
    // Round-robin, but never over a zone whose scripts are still live: a
    // script parked on a sync keeps its resume pointer into the file. Moving
    // the file wakes it mid-table. The engine's rule forbids this
    // (checkRunningAnims, zones.cpp:141).
    for (uint8_t tried = 0; tried < ZONES_KEPT; ++tried) {
        const uint8_t next = take_slot();
        if (!zone_in_use(zone_number_[next]))
            return next;
    }
    ++zones_forced_; // every slot spoken for: the oldest goes regardless
    return take_slot();
}

inline uint8_t GameStore::take_slot() {
    const uint8_t next = next_slot_;
    next_slot_ = static_cast<uint8_t>((next_slot_ + 1) % ZONES_KEPT);
    return next;
}

/// A hit costs a walk of the arena's directory: no card read, no copy, and
/// no staging area in between. A miss reads the card and copies the pixels
/// into the arena.
inline STORE_BANKED PackedZones::Pixels GameStore::pixels_of(uint8_t number) {
    pixels_moved_ = false;
    // What the arena knows, pixels or not: a zone the card has not got is an
    // answer too, and asking the card again every frame for one pins the main
    // loop to a failing access.
    if (CACHE_PIXELS)
        if (zone_pixels.knows(number))
            return zone_pixels.find(number);
    // The beard's pixels are a file of their own, read here in the store's
    // bank: in stage_pixels they cost the fixed region 120 bytes.
    const bool staged = bearded(number) ? stage_beard() : stage_pixels(number);
    if (!staged) {
        if (CACHE_PIXELS)
            zone_pixels.note_absent(number);
        return {};
    }
    if (!CACHE_PIXELS)
        return {atticmap::PACKED, packed_bytes_};
    const uint16_t was_emptied = zone_pixels.emptied();
    const PackedZones::Pixels held = zone_pixels.hold(number, atticmap::PACKED, packed_bytes_);
    pixels_moved_ = zone_pixels.emptied() != was_emptied;
    return held;
}

[[gnu::noinline]] inline bool GameStore::stage_pixels(uint8_t number) {
    // The scripts half is asked for first and brings the pixels with it, so the
    // common case is that they are already staged and this costs nothing. A
    // second zone's fetch in between is what puts them back on the card.
    if (packed_zone_ == number)
        return true;
    return fetch(number, free_slot(number)) && packed_zone_ == number;
}

inline bool GameStore::fetch(uint8_t number, uint8_t slot) {
    bool pixels_ok = false;
    packed_bytes_ = 0;
    if (!read_zone_file(number, slot, &pixels_ok, &packed_bytes_))
        return false;

    const Place at = atticmap::slot(atticmap::ZONESCRIPTS, slot);
    zones_[slot].reset(at, 1UL << atticmap::SHIFT);
    zone_number_[slot] = number;
    // With the beard on, the joined file's pixels are the wrong ones.
    packed_zone_ = pixels_ok && !bearded(number) ? number : NO_ZONE;
    ++zones_loaded_;
    return true;
}

#ifndef __mos__
/// The host reads the release's own two files, because that is what a test has
/// in front of it; joining them is something the card staging does.
inline bool read_zone_file(uint8_t zone, uint8_t slot, bool* pixels_ok, uint32_t* pixel_bytes) {
    char name[9];
    zone_file(name, zone, '1');
    if (!read_game_file(name, atticmap::slot(atticmap::ZONESCRIPTS, slot), 1UL << atticmap::SHIFT))
        return false;
    zone_file(name, zone, '2');
    *pixel_bytes = read_game_file(name, atticmap::PACKED, atticmap::PACKED_BYTES);
    *pixels_ok = *pixel_bytes != 0;
    return true;
}
#endif

// The two hooks whose answer is the same on the host and the machine, so that
// neither has to state the residency rules a second time.

inline bool script_load_subroutine(uint16_t subroutine_id) {
#ifdef __mos__
    // bring_in carries the same 2 KB transcoder store_open does, so it stays in
    // the store's bank and the script dispatch -- which runs in a bank of its
    // own -- reaches it the only way one bank may reach another.
    store.want(subroutine_id);
    banked_call(AGOS_STORE_BANK, store_bring_in_banked);
    return store.wanted_arrived();
#else
    return store.bring_in(subroutine_id);
#endif
}

/// A zone's pixels, held from last time or read now, whoever asks.
///
/// Through the store's bank for the same reason as bring_in: the card path
/// and the arena's copy are the store's own weight, the room bank has 590
/// bytes left, and the fixed region is what this is all trying to keep.
inline PackedZones::Pixels zone_pixels_of(uint8_t number) {
#ifdef __mos__
    store.want_pixels(number);
    banked_call(AGOS_STORE_BANK, store_pixels_banked);
    return store.pixels();
#else
    return store.pixels_of(number);
#endif
}

inline VgaZone* vga_zone(uint8_t number) {
    return store.zone(number);
}

} // namespace agos
