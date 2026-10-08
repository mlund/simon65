// SPDX-License-Identifier: GPL-3.0-or-later

// tbllist and the TABLES files: which file holds a subroutine, and the walk
// that turns a disk block into the form the VM runs.

#pragma once

#include "agos_tables.hpp"
#include "far.hpp"
#include "gamedb.hpp"

#include <stdint.h>

#ifdef __mos__
// AGOS_STORE_BANK and STORE_BANKED: the store's loading work shares one bank
// with the two VM resets, so the calls between them are plain ones. A caller
// outside it goes through banked_call, which is what script_vm's path into
// bring_in does.
#include "banks.hpp"
#else
#define STORE_BANKED
#endif

namespace agos {

/// Ends a line of opcodes, in both forms of the stream.
inline constexpr uint8_t LINE_END = 0xFF;

/// A `T` operand's 0 means the null string, and becomes -1 in memory
/// (subroutine.cpp:812) -- not item 0, which is what a nil `I` operand becomes.
inline constexpr uint16_t NULL_STRING = 0xFFFF;

/// Erased at load: three bytes on disk and nothing in memory
/// (readSubroutineBlock, subroutine.cpp:701).
inline constexpr uint8_t COMMENT_OPCODE = 87;

/// Subroutine 0 is the only one whose lines carry a verb/noun triple, and the
/// only one matched on them (subroutine.cpp:644).
inline constexpr uint16_t MATCHING_SUBROUTINE = 0;
inline constexpr uint8_t MATCH_BYTES = 6;

/// SUBJECT, OBJECT, ME, ACTOR and PARENT are 1, 3, 5, 7 and 9 on disk and their
/// own negatives in memory (subroutine.cpp:784), so the transcode is a
/// negation. Anything else is a real item and a `uint32` follows.
[[nodiscard]] inline bool is_pseudo_item(uint16_t value) {
    return value != 0 && value <= 9 && (value & 1) != 0;
}

/// Copy one opcode's operands from the disk form to the form the VM runs
/// (readSingleOpcode, subroutine.cpp:715). Returns where the disk stream
/// continues and advances `to` past what it wrote.
///
/// On disk an item or string is two bytes or six by a sentinel; in memory both
/// are two. A byte operand keeps its 0xFF escape in both.
inline Place transcode_operands(Place from, uint8_t opcode, Place& to) {
    for (const DiskArg* arg = &SCRIPT_ARG_POOL[SCRIPT_ARG_INDEX[opcode]]; *arg != DiskArg::END;
        ++arg) {
        switch (*arg) {
            case DiskArg::WORD:
                far_write16(to, far_read16(from));
                to += 2;
                from += 2;
                break;

            case DiskArg::BYTE: {
                const uint8_t value = far_read8(from++);
                far_write8(to++, value);
                if (value == LINE_END)
                    far_write8(to++, far_read8(from++));
                break;
            }

            case DiskArg::ITEM: {
                uint16_t value = far_read16(from);
                from += 2;
                if (is_pseudo_item(value)) {
                    value = static_cast<uint16_t>(0U - value);
                } else {
                    const uint32_t raw = far_read32(from);
                    from += 4;
                    value = raw == 0xFFFFFFFFUL ? NO_ITEM : static_cast<uint16_t>(raw + 2);
                }
                far_write16(to, value);
                to += 2;
                break;
            }

            case DiskArg::TEXT: {
                uint16_t value = far_read16(from);
                from += 2;
                if (value == 0) {
                    value = NULL_STRING;
                } else if (value == 3) {
                    value = static_cast<uint16_t>(0U - value);
                } else {
                    value = static_cast<uint16_t>(far_read32(from));
                    from += 4;
                }
                far_write16(to, value);
                to += 2;
                break;
            }

            case DiskArg::END:
                break;
        }
    }
    return from;
}

/// Step over one opcode's operands in the form the VM runs: two bytes each,
/// except a byte operand's 0xFF escape.
[[nodiscard]] inline Place skip_operands(Place at, uint8_t opcode) {
    for (const DiskArg* arg = &SCRIPT_ARG_POOL[SCRIPT_ARG_INDEX[opcode]]; *arg != DiskArg::END;
        ++arg) {
        if (*arg != DiskArg::BYTE)
            at += 2;
        else if (far_read8(at++) == LINE_END)
            ++at;
    }
    return at;
}

/// Where a line's opcodes end, one past its 0xFF.
///
/// The terminator cannot be scanned for. 0xFF is also a byte operand's escape
/// and the low half of a nil item or a null string, so the only way past a line
/// is to decode it -- with the same table the VM dispatches on. ScummVM pays
/// two bytes a line to store the answer instead (SubroutineLine::next).
[[nodiscard]] inline Place line_end(Place at) {
    for (;;) {
        const uint8_t opcode = far_read8(at++);
        if (opcode == LINE_END)
            return at;
        at = skip_operands(at, opcode);
    }
}

/// One subroutine in the heap: a four-byte header, then its lines back to back.
///
/// The lines carry no length of their own, unlike ScummVM's, because each one
/// already ends in 0xFF -- two bytes saved on each of the game's 10,944 lines.
class Subroutine {
  public:
    explicit Subroutine(Place at = NOWHERE) : at_(at) {
    }
    [[nodiscard]] bool valid() const {
        return at_ != NOWHERE;
    }
    [[nodiscard]] uint16_t id() const {
        return far_read16(at_);
    }
    [[nodiscard]] uint16_t bytes() const {
        return far_read16(at_ + 2);
    }

    [[nodiscard]] Place begin() const {
        return at_ + HEADER;
    }
    [[nodiscard]] Place end() const {
        return begin() + bytes();
    }

    /// A matching line's verb and two nouns; -1 is a wildcard and -2 matches only
    /// the unset case (subroutine.cpp:644).
    [[nodiscard]] int16_t verb(Place line) const {
        return static_cast<int16_t>(far_read16(line));
    }
    [[nodiscard]] int16_t noun1(Place line) const {
        return static_cast<int16_t>(far_read16(line + 2));
    }
    [[nodiscard]] int16_t noun2(Place line) const {
        return static_cast<int16_t>(far_read16(line + 4));
    }
    /// The opcodes of a line, past any verb/noun triple.
    [[nodiscard]] Place opcodes(Place line) const {
        return id() == MATCHING_SUBROUTINE ? line + MATCH_BYTES : line;
    }

    /// Where the line after this one begins.
    [[nodiscard]] Place next(Place line) const {
        return line_end(opcodes(line));
    }

    static constexpr uint8_t HEADER = 4;

  private:
    Place at_;
};

/// The table heap: gameamiga's resident subroutines first, then one TABLES file
/// at a time. A table load rewinds to a watermark after the resident block
/// (subroutine.cpp:368), so only one file need ever be in memory.
class SubroutineHeap {
  public:
    void reset(Place heap, uint16_t size) {
        heap_ = heap;
        end_ = heap + size;
        top_ = heap;
        resident_ = heap;
    }

    /// Transcode a disk block onto the heap. False if it would not fit.
    [[nodiscard]] STORE_BANKED bool load(Place disk, uint16_t size);

    /// Keep what is loaded; every later load rewinds to here.
    void make_resident() {
        resident_ = top_;
    }
    void rewind() {
        top_ = resident_;
    }

    [[nodiscard]] Subroutine find(uint16_t id) const;
    /// Where the heap starts: what a load builds, in one piece.
    [[nodiscard]] Place bytes() const {
        return heap_;
    }
    [[nodiscard]] uint16_t used() const {
        return static_cast<uint16_t>(top_ - heap_);
    }
    [[nodiscard]] uint16_t resident_bytes() const {
        return static_cast<uint16_t>(resident_ - heap_);
    }

  private:
    /// The most one opcode can add: its own byte, and seven operands of two.
    static constexpr uint8_t OPCODE_MAX = 15;

    Place heap_ = NOWHERE;
    Place end_ = NOWHERE;
    Place top_ = NOWHERE;
    Place resident_ = NOWHERE;
};

/// tbllist: a filename, then inclusive id ranges, repeated until a zero byte
/// (AGOSEngine_Waxworks::loadTablesIntoMem, subroutine.cpp:346). The base
/// class's version -- fixed 6-byte records -- is Elvira's and does not describe
/// this file.
class TableList {
  public:
    void reset(const uint8_t* data, uint16_t size) {
        data_ = data;
        size_ = size;
    }

    /// The file holding this subroutine, as a pointer into the list itself, or
    /// nullptr when no file claims it -- which means it is resident.
    [[nodiscard]] const char* file_for(uint16_t subroutine_id) const;
    [[nodiscard]] uint8_t file_count() const;

  private:
    const uint8_t* data_ = nullptr;
    uint16_t size_ = 0;
};

// ---------------------------------------------------------------- definitions

inline STORE_BANKED bool SubroutineHeap::load(Place disk, uint16_t size) {
    Place at = disk;
    const Place disk_end = disk + size;

    while (at + 4 <= disk_end && far_read16(at) == 0) {
        at += 2;
        if (end_ - top_ < Subroutine::HEADER)
            return false;
        const Place header = top_;
        const uint16_t id = far_read16(at);
        far_write16(header, id);
        at += 2;
        top_ = header + Subroutine::HEADER;

        while (at + 2 <= disk_end && far_read16(at) == 0) {
            at += 2;
            if (id == MATCHING_SUBROUTINE) {
                if (end_ - top_ < MATCH_BYTES)
                    return false;
                for (uint8_t i = 0; i < MATCH_BYTES; ++i)
                    far_write8(top_++, far_read8(at++));
            }
            for (;;) {
                if (end_ - top_ < OPCODE_MAX || at >= disk_end)
                    return false; // a short file must not walk into what follows it
                const uint8_t opcode = far_read8(at++);
                if (opcode == LINE_END) {
                    far_write8(top_++, LINE_END);
                    break;
                }
                if (opcode == COMMENT_OPCODE) {
                    at += 2; // erased, and its operand with it
                    continue;
                }
                far_write8(top_++, opcode);
                at = transcode_operands(at, opcode, top_);
            }
        }
        at += 2;
        far_write16(header + 2, static_cast<uint16_t>(top_ - header - Subroutine::HEADER));
    }
    return true;
}

inline Subroutine SubroutineHeap::find(uint16_t id) const {
    for (Place at = heap_; at < top_;) {
        const Subroutine subroutine(at);
        if (subroutine.id() == id)
            return subroutine;
        at = subroutine.end();
    }
    return Subroutine();
}

inline const char* TableList::file_for(uint16_t subroutine_id) const {
    for (uint16_t at = 0; at < size_ && data_[at];) {
        const uint16_t name = at;
        while (at < size_ && data_[at])
            ++at;
        ++at;
        for (uint16_t low; at + 4 <= size_ && (low = be16(data_ + at)) != 0; at += 4)
            if (subroutine_id >= low && subroutine_id <= be16(data_ + at + 2))
                return reinterpret_cast<const char*>(data_ + name);
        at += 2;
    }
    return nullptr;
}

inline uint8_t TableList::file_count() const {
    uint8_t files = 0;
    for (uint16_t at = 0; at < size_ && data_[at];) {
        ++files;
        while (at < size_ && data_[at])
            ++at;
        ++at;
        while (at + 2 <= size_ && be16(data_ + at) != 0)
            at += 4;
        at += 2;
    }
    return files;
}

} // namespace agos
