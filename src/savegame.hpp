// The game's own save file, in ScummVM's Simon 1 layout, so a state saved
// here loads there and back. Simon 1 inherits AGOSEngine_Elvira2's saveGame
// and loadGame (agos.h:1823, :1890); the fields below follow those two
// (saveGame saveload.cpp:1463, loadGame :1231), taking the Simon 1 branch at
// each fork.

#pragma once

#include "fault.hpp"
#include "gamedb.hpp"
#include "script_vm.hpp"

#include <stdint.h>

#ifdef __mos__
#include "banks.hpp"
#define SAVE_CONST RODATA_BANK(AGOS_SAVE_BANK)
#else
#define SAVE_BANKED
#define SAVE_CONST
#define SAVE_DATA
#endif

namespace agos {

/// The CD32 release's one slot and its caption (o_saveUserGame,
/// script.cpp:808), in the 18 bytes Simon 1 gives a caption
/// (saveload.cpp:1260, :1483) -- exactly, so no NUL. In the save bank, which reads
/// it, rather than in the fixed region.
SAVE_CONST inline constexpr char SAVE_CAPTION[] = "Default Saved Game";
inline constexpr uint8_t SAVE_CAPTION_BYTES = 18;
static_assert(sizeof SAVE_CAPTION - 1 == SAVE_CAPTION_BYTES);

/// What follows the item count, and what loadGame checks it against
/// (saveload.cpp:1267, :1489).
inline constexpr uint32_t SAVE_MARK = 0xFFFFFFFFUL;

/// ScummVM's Simon 1 sizes (agos.cpp:819-823). Variable 255 and item-store
/// slots 10 to 15 are not saved; the release never names them. No third bit
/// array is saved.
inline constexpr uint16_t SAVED_VARS = 255;
inline constexpr uint8_t SAVED_ITEM_STORE = 10;
static_assert(SAVED_VARS <= NUM_VARS && SAVED_ITEM_STORE <= ITEM_STORE);

/// The image's fixed part: 3,086 bytes whatever the state. Each live timer
/// adds six. A file is longer, by the card's padding.
inline constexpr uint16_t SAVE_FIXED_BYTES = 3086;
inline constexpr uint8_t SAVE_TIMER_BYTES = 6;
inline constexpr uint16_t SAVE_IMAGE_BYTES =
    SAVE_FIXED_BYTES + SAVE_TIMER_BYTES * ScriptVm::TIMEOUTS;
static_assert(SAVE_IMAGE_BYTES <= atticmap::SAVEGAME_BYTES,
    "the save file is staged too short for the largest image");

/// The player's user-flag child, which createPlayer gives it (items.cpp:95)
/// and saveGame writes four words of (saveload.cpp:1576-1580). No Simon 1
/// opcode sets one, so they go out as nought and are read past.
inline constexpr uint8_t PLAYER_USER_FLAGS = 4;

/// Writes and reads the save image in a near buffer.
///
/// Near rather than walked through far memory: the image is 3 KB and is
/// moved to and from the card whole, so one DMA each way beats a call a byte.
class SaveGame {
  public:
    /// The VM's state into @p image. How many bytes it took, or nought with
    /// SAVE_TOO_LONG raised when @p room would not hold it.
    [[nodiscard]] SAVE_BANKED static uint16_t save(
        const ScriptVm& vm, uint8_t* image, uint16_t room);

    /// The state in @p image into the VM, or nothing. Fails with SAVE_REFUSED
    /// naming the item, or zero for the header, when the file is from another
    /// game or short, or when an item's record layout changed: an object's value
    /// slots and a room's exit list are packed by flags the record was loaded
    /// with. In this release neither changes: o_oset sets only bits 16 and up
    /// (script.cpp:352), and the door opcodes are Elvira 2's (script_e2.cpp:395-407),
    /// not in Simon 1's table (script_s1.cpp:35).
    [[nodiscard]] SAVE_BANKED static bool load(ScriptVm& vm, const uint8_t* image, uint16_t bytes);

    /// Whether load would take @p image, changing nothing either way.
    [[nodiscard]] SAVE_BANKED static bool check(
        ScriptVm& vm, const uint8_t* image, uint16_t bytes) {
        begin(image, bytes);
        return walk(vm, false);
    }

  private:
    /// The stream, one at a time: where in the image, what is left, and
    /// whether a word fell off the end. Static and in the save bank, so every
    /// access is an absolute address rather than one through a pointer.
    SAVE_DATA static inline uint8_t* stream_at = nullptr;
    SAVE_DATA static inline uint16_t stream_left = 0;
    SAVE_DATA static inline bool stream_over = false;

    static void begin(const uint8_t* image, uint16_t bytes) {
        stream_at = const_cast<uint8_t*>(image);
        stream_left = bytes;
        stream_over = false;
    }

    /// Big-endian words out, refused past the end rather than written there,
    /// and in, nought past the end; either way noted.
    SAVE_BANKED static void w16(uint16_t value);
    SAVE_BANKED static void w32(uint32_t value);
    [[nodiscard]] SAVE_BANKED static uint16_t r16();
    [[nodiscard]] SAVE_BANKED static uint32_t r32();

    /// One pass over the file: checking only, or applying. Checking comes first
    /// so a refusal leaves the game as it was; the walk's shape is the
    /// database's, which both passes share.
    [[nodiscard]] SAVE_BANKED static bool walk(ScriptVm& vm, bool apply);
};

// ---------------------------------------------------------------- definitions

inline void SaveGame::w16(uint16_t value) {
    if (stream_left < 2) {
        stream_over = true;
        return;
    }
    put_be16(stream_at, value);
    stream_at += 2;
    stream_left = static_cast<uint16_t>(stream_left - 2);
}

inline void SaveGame::w32(uint32_t value) {
    w16(static_cast<uint16_t>(value >> 16));
    w16(static_cast<uint16_t>(value));
}

inline uint16_t SaveGame::r16() {
    if (stream_left < 2) {
        stream_over = true;
        return 0;
    }
    const uint16_t value = be16(stream_at);
    stream_at += 2;
    stream_left = static_cast<uint16_t>(stream_left - 2);
    return value;
}

inline uint32_t SaveGame::r32() {
    const uint32_t high = r16();
    return high << 16 | r16();
}

inline uint16_t SaveGame::save(const ScriptVm& vm, uint8_t* image, uint16_t room) {
    begin(image, room);
    for (uint8_t i = 0; i < SAVE_CAPTION_BYTES; i = static_cast<uint8_t>(i + 2))
        w16(static_cast<uint16_t>(
            static_cast<uint16_t>(static_cast<uint8_t>(SAVE_CAPTION[i])) << 8 |
            static_cast<uint8_t>(SAVE_CAPTION[i + 1])));

    const GameDb& db = vm.db();
    const uint16_t count = db.item_count();
    w32(static_cast<uint32_t>(count - 1)); // _itemArrayInited - 1
    w32(SAVE_MARK);
    w32(vm.clock_); // read and dropped on load
    w32(0);

    // Each timer as the seconds it has left (te->time - curTime + gsc,
    // saveload.cpp:1501); overdue is negative, which loadGame clamps.
    uint8_t timers = 0;
    for (uint8_t i = 0; i < ScriptVm::TIMEOUTS; ++i)
        if (vm.waiting_[i] != 0)
            ++timers;
    w32(timers);
    for (uint8_t i = 0; i < ScriptVm::TIMEOUTS; ++i)
        if (vm.waiting_[i] != 0) {
            w32(vm.due_[i] - vm.clock_);
            w16(vm.waiting_[i]);
        }

    // Items 1 up, the player included (saveload.cpp:1537-1582).
    for (uint16_t id = PLAYER_ITEM; id < count; ++id) {
        const Item item = db.item(id);
        w16(item.parent());
        w16(item.next());
        w16(static_cast<uint16_t>(item.state()));
        w16(item.class_flags());
        if (item.is_room())
            w16(item.room().exit_states());
        if (item.is_object()) {
            const Object object = item.object();
            uint32_t flags = object.flags();
            w32(flags);
            const uint8_t* value = object.saved_values();
            for (uint8_t bit = 1; bit < Object::FLAG_BITS; ++bit) {
                flags >>= 1;
                if (flags & 1) {
                    w16(be16(value));
                    value += 2;
                }
            }
        }
        if (id == PLAYER_ITEM)
            for (uint8_t i = 0; i < PLAYER_USER_FLAGS; ++i)
                w16(0);
    }

    for (uint16_t i = 0; i < SAVED_VARS; ++i)
        w16(static_cast<uint16_t>(vm.variables_[i]));
    for (uint8_t i = 0; i < SAVED_ITEM_STORE; ++i)
        w16(vm.item_store_[i]);
    for (uint8_t i = 0; i < BIT_WORDS; ++i)
        w16(vm.bits_[i]);
    for (uint8_t i = 0; i < BIT_WORDS; ++i)
        w16(vm.bits2_[i]);

    if (stream_over) {
        script_fault(Fault::SAVE_TOO_LONG, room);
        return 0;
    }
    return static_cast<uint16_t>(room - stream_left);
}

inline bool SaveGame::load(ScriptVm& vm, const uint8_t* image, uint16_t bytes) {
    if (!check(vm, image, bytes))
        return false;
    begin(image, bytes);
    return walk(vm, true);
}

inline bool SaveGame::walk(ScriptVm& vm, bool apply) {
    for (uint8_t i = 0; i < SAVE_CAPTION_BYTES; i = static_cast<uint8_t>(i + 2))
        (void)r16();

    const GameDb& db = vm.db();
    const uint16_t count = db.item_count();
    const uint32_t items = r32();
    if (r32() != SAVE_MARK || items != static_cast<uint32_t>(count - 1)) {
        script_fault(Fault::SAVE_REFUSED, 0);
        return false;
    }
    (void)r32(); // the time it was saved at, unused
    (void)r32();

    // killAllTimers, then each one added back (saveload.cpp:1278-1291).
    const uint32_t timers = r32();
    if (timers > ScriptVm::TIMEOUTS) {
        script_fault(Fault::SAVE_REFUSED, 0);
        return false;
    }
    if (apply)
        for (uint8_t i = 0; i < ScriptVm::TIMEOUTS; ++i)
            vm.waiting_[i] = 0;
    for (uint8_t i = 0; i < static_cast<uint8_t>(timers); ++i) {
        // Read as int16, the engine's guard against the negative timeouts older
        // saves carry (:1288); addTimeEvent then makes them nought (event.cpp:49).
        const int16_t seconds = static_cast<int16_t>(r32());
        const uint16_t subroutine = r16();
        if (apply)
            vm.add_timeout(seconds < 0 ? 0 : static_cast<uint16_t>(seconds), subroutine);
    }

    // loadGame moves each item in turn with setItemParent, which links it at
    // the head of its new parent's list (saveload.cpp:1347-1367). Every item is
    // moved, so the same tree comes from every list emptied first and each item
    // linked in order -- and nothing has to be unlinked.
    if (apply)
        for (uint16_t id = PLAYER_ITEM; id < count; ++id) {
            const Item item = db.item(id);
            item.set_parent(NO_ITEM);
            item.set_next(NO_ITEM);
            item.set_child(NO_ITEM);
        }

    for (uint16_t id = PLAYER_ITEM; id < count; ++id) {
        const Item item = db.item(id);
        const uint16_t parent = r16();
        const uint16_t next = r16();
        const int16_t state = static_cast<int16_t>(r16());
        const uint16_t class_flags = r16();
        // derefItem stops the engine on an index past the table (items.cpp:383),
        // and an item its own parent is a tree this interpreter faults on.
        bool refused = parent >= count || parent == id;
        if (item.is_room() && r16() != item.room().exit_states())
            refused = true;
        if (item.is_object()) {
            const Object object = item.object();
            const uint32_t flags = r32();
            if (static_cast<uint16_t>(flags) != static_cast<uint16_t>(object.flags()))
                refused = true;
            // kOFText's value is not in the file: the values start past it (:1388).
            uint8_t* value = object.saved_values();
            uint32_t left = flags;
            for (uint8_t bit = 1; bit < Object::FLAG_BITS; ++bit) {
                left >>= 1;
                if (left & 1) {
                    const uint16_t saved = r16();
                    if (apply) // only reached once the first pass passed
                        put_be16(value, saved);
                    value += 2;
                }
            }
            if (apply)
                object.set_high_flags(static_cast<uint16_t>(flags >> 16));
        }
        if (id == PLAYER_ITEM)
            for (uint8_t i = 0; i < PLAYER_USER_FLAGS; ++i)
                (void)r16();
        if (refused) {
            script_fault(Fault::SAVE_REFUSED, id);
            return false;
        }
        if (apply) {
            // derefItem's null for item 0 keeps the file's own links (:1364-1367).
            const Item parent_item = db.item(parent);
            item.set_parent(parent);
            if (parent_item.valid()) {
                item.set_next(parent_item.child());
                parent_item.set_child(id);
            } else {
                item.set_next(next);
            }
            item.set_state(state);
            item.set_class_flags(class_flags);
        }
    }

    for (uint16_t i = 0; i < SAVED_VARS; ++i) {
        const uint16_t value = r16();
        if (apply)
            vm.variables_[i] = static_cast<int16_t>(value);
    }
    // Past the table derefItem stops the engine (items.cpp:383, saveload.cpp:1421).
    for (uint8_t i = 0; i < SAVED_ITEM_STORE; ++i) {
        const uint16_t stored = r16();
        if (stored >= count) {
            script_fault(Fault::SAVE_REFUSED, 0);
            return false;
        }
        if (apply)
            vm.item_store_[i] = stored;
    }
    for (uint8_t i = 0; i < BIT_WORDS; ++i) {
        const uint16_t word = r16();
        if (apply)
            vm.bits_[i] = word;
    }
    for (uint8_t i = 0; i < BIT_WORDS; ++i) {
        const uint16_t word = r16();
        if (apply)
            vm.bits2_[i] = word;
    }

    // Checked on the first pass, which is the one that may still refuse. What
    // lies past the end is the card file's padding and is not read, as loadGame
    // does not read it.
    if (stream_over) {
        script_fault(Fault::SAVE_REFUSED, 0);
        return false;
    }
    return true;
}

} // namespace agos
