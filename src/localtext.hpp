// SPDX-License-Identifier: GPL-3.0-or-later

// The strings a room carries, which are not in gameamiga.
//
// An id under $8000 is a global and GameDb has it. Above that it is local:
// `stripped.txt` says which TEXTnn file holds which ids, and the file is a
// block of NUL-terminated strings indexed by the id's place in the file's own
// range (string.cpp:310-356). One file is resident at a time, as it is in the
// engine, because that is how the game asks -- a room's lines come from one
// file and the next room's from another.

#pragma once

#include "atticmap.hpp"
#include "chipmap.hpp"
#include "far.hpp"

namespace agos {

/// The card, through whatever is standing in for it: the same declaration
/// game_store.hpp leans on, so a host test serves these files too.
[[nodiscard]] uint32_t read_game_file(const char* name, Place into, uint32_t most);

class LocalText {
  public:
    /// Ids at or above this are local; below it they are gameamiga's.
    static constexpr uint16_t FIRST_LOCAL = 0x8000;

    /// The longest string in the release is 86 characters, and the engine
    /// copies into 180 (string.cpp:135).
    static constexpr uint8_t MOST_CHARS = 96;

    /// Read the table of which file holds which ids.
    ///
    /// 27 files in this release, ids $8000 to $8374. The table is a list of
    /// name-then-top pairs: a NUL-terminated `TEXTnn`, then the first id the
    /// *next* file holds, big-endian.
    [[nodiscard]] bool begin() {
        const uint32_t bytes = read_game_file(TABLE_FILE, atticmap::LOAD, TABLE_MOST);
        if (bytes == 0)
            return false;
        files_ = 0;
        uint32_t at = 0;
        while (at < bytes && files_ < MOST_FILES) {
            // TEXTnn, and only the number is worth keeping: the rest is the same
            // six characters every time.
            uint8_t name[NAME_CHARS + 1];
            uint8_t n = 0;
            while (at < bytes && n <= NAME_CHARS) {
                name[n] = far_read8(atticmap::LOAD + at++);
                if (name[n] == 0)
                    break;
                ++n;
            }
            if (n != NAME_CHARS || at + 2 > bytes)
                break;
            file_[files_] = static_cast<uint8_t>((name[4] - '0') * 10 + (name[5] - '0'));
            top_[files_] = static_cast<uint16_t>(
                far_read8(atticmap::LOAD + at) * 256 + far_read8(atticmap::LOAD + at + 1));
            at += 2;
            ++files_;
        }
        return files_ != 0;
    }

    /// Where a local string is, as a far address, loading its file if the one
    /// resident does not hold it. NOWHERE if no file claims the id.
    [[nodiscard]] Place at(uint16_t id) {
        if (id < base_ || id >= limit_)
            if (!bring_in(id))
                return NOWHERE;
        const uint16_t index = static_cast<uint16_t>(id - base_);
        return index < held_ ? atticmap::LOCALTEXT + offset_[index] : NOWHERE;
    }

    /// A local string copied near, NUL-terminated, for something to read a
    /// character at a time. Its length, or nought if there is no such string.
    uint8_t copy(uint16_t id, char* into, uint8_t most) {
        const Place from = at(id);
        if (from == NOWHERE || most == 0)
            return 0;
        uint8_t n = 0;
        while (n + 1u < most) {
            const uint8_t ch = far_read8(from + n);
            if (ch == 0)
                break;
            into[n++] = static_cast<char>(ch);
        }
        into[n] = '\0';
        return n;
    }

    [[nodiscard]] uint8_t files() const {
        return files_;
    }

  private:
    static constexpr const char* TABLE_FILE = "stripped.txt";
    static constexpr uint16_t TABLE_MOST = 1024; // 243 bytes in this release
    static constexpr uint8_t NAME_CHARS = 6;     // TEXTnn
    static constexpr uint8_t MOST_FILES = 32;    // 27, and room to spare

    /// The most strings any one file holds: TEXT03's 89, in this release.
    static constexpr uint8_t MOST_STRINGS = 96;

    /// Read the file that holds @p id and index its strings.
    ///
    /// The index is built here rather than walked per lookup: a far byte is a
    /// call, and a line near the end of a file would be two thousand of them.
    [[nodiscard]] bool bring_in(uint16_t id) {
        uint16_t base = FIRST_LOCAL;
        for (uint8_t i = 0; i < files_; ++i) {
            if (id < top_[i]) {
                char name[NAME_CHARS + 1] = {'T', 'E', 'X', 'T', '0', '0', '\0'};
                name[4] = static_cast<char>('0' + file_[i] / 10);
                name[5] = static_cast<char>('0' + file_[i] % 10);
                const uint32_t bytes =
                    read_game_file(name, atticmap::LOCALTEXT, atticmap::LOCALTEXT_BYTES);
                if (bytes == 0)
                    return false;
                base_ = base;
                limit_ = top_[i];
                index(static_cast<uint16_t>(bytes));
                return true;
            }
            base = top_[i];
        }
        return false;
    }

    /// Where each string in the resident file starts.
    ///
    /// A chunk at a time through a near buffer rather than a far read a byte,
    /// and the last string need not be terminated: the file's end ends it.
    void index(uint16_t bytes) {
        held_ = 0;
        offset_[held_++] = 0;
        uint8_t chunk[CHUNK];
        for (uint16_t at = 0; at < bytes && held_ < MOST_STRINGS;) {
            const uint16_t n = bytes - at < CHUNK ? static_cast<uint16_t>(bytes - at) : CHUNK;
            far_read(atticmap::LOCALTEXT + at, chunk, n);
            for (uint16_t i = 0; i < n && held_ < MOST_STRINGS; ++i)
                if (chunk[i] == 0 && at + i + 1u < bytes)
                    offset_[held_++] = static_cast<uint16_t>(at + i + 1);
            at = static_cast<uint16_t>(at + n);
        }
    }

    static constexpr uint16_t CHUNK = 256;

    uint16_t top_[MOST_FILES] = {};
    uint8_t file_[MOST_FILES] = {};
    uint8_t files_ = 0;

    uint16_t base_ = 0, limit_ = 0; // the id range resident now
    uint16_t offset_[MOST_STRINGS] = {};
    uint8_t held_ = 0;
};

} // namespace agos
