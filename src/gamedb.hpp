// The item database out of gameamiga: item records, their room or object
// payload, and the global string table.

#pragma once

#include "atticmap.hpp"
#include "far.hpp"

#include <stdint.h>

namespace agos {

/// Big-endian load. Everything in this data is big-endian, the DOS files too
/// (`readUint16Wrapper`, res.cpp:56). The shift is done in an unsigned type
/// because `int` is 16 bits on the target, where `at[0] << 8` overflows a
/// signed int for any byte above 127.
[[nodiscard]] inline uint16_t be16(const uint8_t* at) {
    return static_cast<uint16_t>(static_cast<uint16_t>(at[0]) << 8 | at[1]);
}

[[nodiscard]] inline uint32_t be32(const uint8_t* at) {
    return static_cast<uint32_t>(be16(at)) << 16 | be16(at + 2);
}

inline void put_be16(uint8_t* at, uint16_t value) {
    at[0] = static_cast<uint8_t>(value >> 8);
    at[1] = static_cast<uint8_t>(value);
}

/// A nil item link. On disk a link is a `uint32`, `0xFFFFFFFF` for nil and
/// otherwise a 0-based index biased by two (`fileReadItemID`, res.cpp:524).
/// Nil becomes **item 0**, not a large index: item 0 is the null item, the one
/// `derefItem` hands back as a null pointer (items.cpp:382), and it is what
/// ends every child and sibling chain.
inline constexpr uint16_t NO_ITEM = 0;

/// Items 0 and 1 are not in the file: 1 is the player, built by `createPlayer`
/// (items.cpp:77), and both counts in the header gain two (res.cpp:122).
inline constexpr uint16_t SYNTHETIC_ITEMS = 2;
inline constexpr uint16_t PLAYER = 1;

/// The player's fixed part, with no children: this release reads none of the
/// `kPlayerType` block `createPlayer` also allocates, which is Elvira's.
inline constexpr uint8_t PLAYER_RECORD = 26;
inline constexpr int16_t PLAYER_NOUN = 10000;

/// This release's 192 records and the synthetic pair. The index is a word per
/// item, so a bigger figure is paid for whether or not it is used; load() says
/// so rather than overrunning if a file ever needs more.
inline constexpr uint16_t MAX_ITEMS = 194;

/// Four big-endian counts, then the text block (allocGamePcVars, res.cpp:107).
inline constexpr uint16_t HEADER_BYTES = 20;

/// Strings the index has room for. This release has 368.
inline constexpr uint16_t MAX_STRINGS = 384;

/// Local strings live in `TEXTnn` and are not in this file (string.cpp:310).
inline constexpr uint16_t LOCAL_STRING_MIN = 0x8000;

enum : uint8_t { ROOM_TYPE = 1, OBJECT_TYPE = 2 };

/// A room's exits, six two-bit fields deciding which are present
/// (`readItemChildren`, res.cpp:437).
class Room {
  public:
    explicit Room(uint8_t* at) : at_(at) {
    }
    [[nodiscard]] uint16_t subroutine_id() const {
        return be16(at_);
    }
    [[nodiscard]] uint16_t exit_states() const {
        return be16(at_ + 2);
    }
    [[nodiscard]] uint8_t exit_state(uint8_t side) const {
        return static_cast<uint8_t>((exit_states() >> (2 * side)) & 3);
    }
    /// The item beyond one side, or NO_ITEM. Present exits pack densely, so
    /// reaching side n means counting the ones before it.
    [[nodiscard]] uint16_t exit(uint8_t side) const;

    static constexpr uint8_t SIDES = 6;

  private:
    uint8_t* at_;
};

/// An object's flag values, packed densely in flag-bit order (res.cpp:456).
/// Bit 0 is `kOFText` and is four bytes on disk where the rest are two.
class Object {
  public:
    explicit Object(uint8_t* at) : at_(at) {
    }
    [[nodiscard]] uint32_t flags() const {
        return be32(at_);
    }
    /// Bits 16 and up are the script's to set; the low 16 say which values the
    /// record carries and must not move (o_oset, script.cpp:355).
    void set_flag(uint8_t bit, bool on) const {
        const uint16_t half = bit < 16 ? 2 : 0;
        const uint8_t shift = static_cast<uint8_t>(bit & 15);
        uint16_t word = be16(at_ + half);
        word = static_cast<uint16_t>(on ? word | (1U << shift) : word & ~(1U << shift));
        put_be16(at_ + half, word);
    }
    [[nodiscard]] bool has(uint8_t bit) const {
        return (flags() >> bit) & 1;
    }
    /// Bits 16 and up at once, as a loaded game has them.
    void set_high_flags(uint16_t high) const {
        put_be16(at_, high);
    }
    /// The values after kOFText's, packed in flag-bit order: what a save file
    /// holds of an object, and in that order (saveload.cpp:1567-1572).
    [[nodiscard]] uint8_t* saved_values() const {
        return at_ + ((at_[3] & 1) ? 8 : 4);
    }
    /// The value behind one flag bit, or 0 when the flag is clear.
    [[nodiscard]] uint16_t value(uint8_t bit) const;
    void set_value(uint8_t bit, uint16_t value) const;
    /// The name string id, after every value.
    [[nodiscard]] uint16_t name() const;
    /// Where the name sits, which is also how long the values are.
    [[nodiscard]] uint16_t name_offset() const;

    static constexpr uint8_t FLAG_BITS = 16;

  private:
    /// Where one flag's value sits, or nullptr when the flag is clear.
    [[nodiscard]] uint8_t* slot(uint8_t bit) const;
    uint8_t* at_;
};

/// One item record, read and written in place in the loaded image.
///
/// A cursor, not a copy: the game moves items between parents and changes their
/// state throughout, and the 192 records are 7.5 KB there is no reason to hold
/// twice. Links are normalised to 16 bits by GameDb::load, so every access here
/// is one big-endian word.
///
/// The setters are const because the handle is what a `const Item` freezes, the
/// same way `T *const` does; the record it points at is the game's to change.
class Item {
  public:
    explicit Item(uint8_t* at = nullptr) : at_(at) {
    }
    [[nodiscard]] bool valid() const {
        return at_ != nullptr;
    }

    [[nodiscard]] int16_t adjective() const {
        return word(ADJECTIVE);
    }
    [[nodiscard]] int16_t noun() const {
        return word(NOUN);
    }
    [[nodiscard]] int16_t state() const {
        return word(STATE);
    }
    void set_state(int16_t value) const {
        set_word(STATE, value);
    }

    [[nodiscard]] uint16_t next() const {
        return link(NEXT);
    }
    [[nodiscard]] uint16_t child() const {
        return link(CHILD);
    }
    [[nodiscard]] uint16_t parent() const {
        return link(PARENT);
    }
    void set_next(uint16_t item) const {
        set_link(NEXT, item);
    }
    void set_child(uint16_t item) const {
        set_link(CHILD, item);
    }
    void set_parent(uint16_t item) const {
        set_link(PARENT, item);
    }

    [[nodiscard]] uint16_t class_flags() const {
        return be16(at_ + CLASS_FLAGS);
    }
    void set_class_flags(uint16_t flags) const {
        put_be16(at_ + CLASS_FLAGS, flags);
    }

    [[nodiscard]] bool is_room() const {
        return child_type() == ROOM_TYPE;
    }
    [[nodiscard]] bool is_object() const {
        return child_type() == OBJECT_TYPE;
    }
    [[nodiscard]] Room room() const {
        return Room(at_ + PAYLOAD);
    }
    [[nodiscard]] Object object() const {
        return Object(at_ + PAYLOAD);
    }

    /// Where the next record begins. The only way to find it is to measure this
    /// one, which is why GameDb indexes them once at load.
    [[nodiscard]] const uint8_t* end() const;

  private:
    // A link is a uint32 on disk, normalised in place to its low word.
    enum : uint8_t {
        ADJECTIVE = 0,
        NOUN = 2,
        STATE = 4,
        NEXT = 8,
        CHILD = 12,
        PARENT = 16,
        CLASS_FLAGS = 20,
        HAS_CHILDREN = 22,
        CHILD_TYPE = 26,
        PAYLOAD = 28,
    };

    [[nodiscard]] int16_t word(uint8_t at) const {
        return static_cast<int16_t>(be16(at_ + at));
    }
    void set_word(uint8_t at, int16_t value) const {
        put_be16(at_ + at, static_cast<uint16_t>(value));
    }
    [[nodiscard]] uint16_t link(uint8_t at) const {
        return be16(at_ + at);
    }
    void set_link(uint8_t at, uint16_t item) const {
        put_be16(at_ + at, item);
    }
    [[nodiscard]] uint16_t child_type() const {
        return be32(at_ + HAS_CHILDREN) ? be16(at_ + CHILD_TYPE) : 0;
    }

    uint8_t* at_;
};

/// `gameamiga`, indexed. The image is kept and written in place, so it must
/// outlive the database and must be writable -- this is the seam that decides
/// where the data lives, and the only thing that changes if it moves.
class GameDb {
  public:
    /// The header, the strings and the item records, out of the file as it
    /// landed in far memory.
    ///
    /// Two destinations, because the two halves want different memory. The
    /// records go to @p records, near, where the game writes item state back
    /// into them all through a play. The strings go to @p text, far, because
    /// they are read-only and only a line on screen ever reads one -- what
    /// stays near for them is the offset index, which is 736 bytes against
    /// their 6,127.
    [[nodiscard]] bool load(Place image, uint32_t size, uint8_t* records, uint16_t records_bytes);

    /// Two more than the file holds, for the synthetic pair.
    [[nodiscard]] uint16_t item_count() const {
        return count_;
    }
    [[nodiscard]] uint16_t string_count() const {
        return strings_;
    }

    /// An item by index, or an invalid one for the null item 0.
    ///
    /// A table of pointers rather than offsets: it is the same two bytes on the
    /// target, it saves the add on every lookup, and it lets the player's record
    /// live here rather than in the file, where it is not.
    [[nodiscard]] Item item(uint16_t index) const {
        return index < count_ ? Item(record_[index]) : Item();
    }

    /// A global string, as a far address. NOWHERE if there is no such id.
    ///
    /// Indexed rather than scanned, because the block is far and a far byte is
    /// a call: walking to the last string would be 6 KB of them. The index is
    /// built once at load and costs 736 near bytes.
    [[nodiscard]] Place string(uint16_t id) const {
        return id < strings_ ? text_ + string_at_[id] : NOWHERE;
    }

    /// How far into the file the resident subroutine block starts.
    ///
    /// An offset and not a pointer: the item records are copied down to near
    /// memory where they can be walked, but the block behind them stays where the
    /// file landed, which is too high for a pointer to reach.
    [[nodiscard]] uint16_t subroutines() const {
        return subs_;
    }

  private:
    Place text_ = NOWHERE;
    uint16_t count_ = 0;
    uint16_t strings_ = 0;
    uint16_t subs_ = 0;
    uint8_t* record_[MAX_ITEMS] = {};
    /// How much of the text block is walked between far reads while the index
    /// is built. Sixty-four bytes is six strings at this release's median.
    static constexpr uint16_t STRING_CHUNK = 64;

    uint16_t string_at_[MAX_STRINGS] = {};
    uint8_t player_[PLAYER_RECORD] = {};
};

// ---------------------------------------------------------------- definitions

inline uint16_t Room::exit(uint8_t side) const {
    uint16_t states = exit_states(); // once, not once a side
    if (!((states >> (2 * side)) & 3))
        return NO_ITEM;
    const uint8_t* value = at_ + 4;
    for (uint8_t before = 0; before < side; ++before, states >>= 2)
        if (states & 3)
            value += 4;
    const uint32_t raw = be32(value);
    return raw == 0xFFFFFFFFUL ? NO_ITEM : static_cast<uint16_t>(raw + 2);
}

inline uint8_t* Object::slot(uint8_t bit) const {
    // The flag word once, then walked: `has` would re-read four bytes and do a
    // variable-length shift for every bit, and this runs sixteen times.
    uint32_t left = flags();
    if (!((left >> bit) & 1))
        return nullptr;
    // kOFText is four bytes where every other flag is two, and only its low half
    // is ever read or written (res.cpp:465).
    if (bit == 0)
        return at_ + 6;
    uint8_t* value = saved_values();
    left >>= 1;
    for (uint8_t before = 1; before < bit; ++before, left >>= 1)
        if (left & 1)
            value += 2;
    return value;
}

// Inlined: its callers are in two banks, and an out-of-line copy would land
// in the fixed region.
[[gnu::always_inline]] inline uint16_t Object::value(uint8_t bit) const {
    const uint8_t* at = slot(bit);
    return at ? be16(at) : 0;
}

inline void Object::set_value(uint8_t bit, uint16_t value) const {
    if (uint8_t* at = slot(bit))
        put_be16(at, value);
}

// Inlined, not shared: a copy out of line would land in the fixed region,
// which has no room for it, and its callers are in banks.
[[gnu::always_inline]] inline uint16_t Object::name_offset() const {
    const uint32_t left = flags();
    uint16_t bytes = (left & 1) ? 8 : 4;
    for (uint8_t bit = 1; bit < FLAG_BITS; ++bit)
        if ((left >> bit) & 1)
            bytes = static_cast<uint16_t>(bytes + 2);
    return bytes;
}

inline uint16_t Object::name() const {
    return static_cast<uint16_t>(be32(at_ + name_offset()));
}

inline const uint8_t* Item::end() const {
    if (!be32(at_ + HAS_CHILDREN))
        return at_ + HAS_CHILDREN + 4;
    const uint8_t* at = at_ + CHILD_TYPE;
    for (uint16_t type = be16(at); type != 0; type = be16(at)) {
        at += 2;
        if (type == ROOM_TYPE) {
            const Room room(const_cast<uint8_t*>(at));
            at += 4;
            for (uint8_t side = 0; side < Room::SIDES; ++side)
                if (room.exit_state(side))
                    at += 4;
        } else {
            const Object object(const_cast<uint8_t*>(at));
            at += object.name_offset() + 4;
        }
    }
    return at + 2;
}

inline bool GameDb::load(Place image, uint32_t size, uint8_t* records, uint16_t records_bytes) {
    // allocGamePcVars, res.cpp:107: four counts, then the text block.
    if (size < HEADER_BYTES || far_read32(image + 4) != 0x80)
        return false;
    count_ = static_cast<uint16_t>(far_read32(image + 8) + SYNTHETIC_ITEMS);
    strings_ = static_cast<uint16_t>(far_read32(image + 12));
    if (count_ > MAX_ITEMS || strings_ > MAX_STRINGS)
        return false;

    // In 32 bits until it is known to fit: a wrong text size would otherwise
    // wrap a uint16 and walk from an offset that looks perfectly reasonable.
    const uint32_t text_end = HEADER_BYTES + far_read32(image + 16);
    if (text_end > size || text_end - HEADER_BYTES > atticmap::GAMETEXT_BYTES)
        return false;

    // The strings to their own far home, and an index over them. The walk costs
    // a far byte apiece and happens once; what it buys is that showing a line
    // later reads only that line.
    text_ = atticmap::GAMETEXT;
    far_copy(image + HEADER_BYTES, text_, static_cast<uint16_t>(text_end - HEADER_BYTES));
    const uint16_t text_held = static_cast<uint16_t>(text_end - HEADER_BYTES);

    // Counted out of near chunks rather than a far byte at a time: far_read8 is
    // a jsr apiece and this walks six thousand of them at boot, where a DMA of
    // sixty-four costs one. A file whose string count outruns its terminators
    // ends up with fewer strings rather than with offsets past the block, which
    // string() would otherwise follow into whatever sits after GAMETEXT.
    uint8_t chunk[STRING_CHUNK];
    uint16_t id = 0, offset = 0;
    if (strings_ != 0)
        string_at_[id++] = 0;
    while (id < strings_ && offset < text_held) {
        const uint16_t left = static_cast<uint16_t>(text_held - offset);
        const uint16_t want = left < STRING_CHUNK ? left : STRING_CHUNK;
        far_read(text_ + offset, chunk, want);
        for (uint16_t i = 0; i < want && id < strings_; ++i)
            if (chunk[i] == 0)
                string_at_[id++] = static_cast<uint16_t>(offset + i + 1);
        offset = static_cast<uint16_t>(offset + want);
    }
    strings_ = id;

    // The records near, where the game writes item state back into them.
    const uint32_t records_have = size - text_end;
    far_read(image + text_end,
        records,
        static_cast<uint16_t>(records_have < records_bytes ? records_have : records_bytes));
    uint16_t at = static_cast<uint16_t>(text_end);

    // createPlayer, items.cpp:77. Everything else about the player starts at
    // zero, and the game moves it between parents like any other item.
    for (uint8_t i = 0; i < PLAYER_RECORD; ++i)
        player_[i] = 0;
    put_be16(player_, 0xFFFF); // adjective -1
    put_be16(player_ + 2, PLAYER_NOUN);
    record_[PLAYER] = player_;

    for (uint16_t index = SYNTHETIC_ITEMS; index < count_; ++index) {
        // Before walking from it, not after: a truncated file would otherwise be
        // read past its end and only noticed once the damage was done.
        if (at + PLAYER_RECORD > size || at - text_end + PLAYER_RECORD > records_bytes)
            return false;
        // The near buffer holds the file from text_end on, so a file offset and
        // an index into it differ by that much.
        uint8_t* const record = records + (at - text_end);
        record_[index] = record;
        // Normalise the three links in place, so that every later access is one
        // word rather than a four-byte load and an add.
        for (uint8_t field = 6; field <= 14; field = static_cast<uint8_t>(field + 4)) {
            const uint32_t raw = be32(record + field);
            put_be16(
                record + field + 2, raw == 0xFFFFFFFFUL ? NO_ITEM : static_cast<uint16_t>(raw + 2));
            static_assert(NO_ITEM == 0, "a nil link is item 0, and item 0 has no record");
        }
        at = static_cast<uint16_t>(static_cast<uint32_t>(Item(record).end() - records) + text_end);
        if (at > size)
            return false;
    }
    subs_ = at;
    return true;
}

} // namespace agos
