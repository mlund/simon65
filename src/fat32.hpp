// SPDX-License-Identifier: GPL-3.0-only

// FAT32 as the card stores it: where a partition begins, where a volume's FATs
// and data begin, and how a name in a directory becomes a sector number.
//
// Arithmetic over a 512-byte sector and nothing else -- no card, no I/O, no
// state -- so the format can be settled on the host and only the reading left
// to the machine. Ported from mega65-freezer's src/fat32.c, itself from the
// MEGA65 freeze menu (github.com/MEGA65/mega65-freezemenu, GPL-3), extended
// where it only ever searched the root directory.

#pragma once

#include "far.hpp"

#include <stdint.h>

namespace fat32 {

inline constexpr uint16_t SECTOR_BYTES = 512;
inline constexpr uint8_t DIRENT_BYTES = 32;
inline constexpr uint8_t DIRENTS_PER_SECTOR = SECTOR_BYTES / DIRENT_BYTES;

/// Links per FAT sector: four bytes apiece.
inline constexpr uint16_t LINKS_PER_SECTOR = SECTOR_BYTES / 4;

/// A formatter may leave the top four bits of a link set; they are reserved
/// and a link read with them compares as neither a cluster nor an end mark,
/// which stops a walk early and reports a file that exists as absent.
inline constexpr uint32_t LINK_MASK = 0x0FFFFFFF;

/// Past this a link is an end-of-chain mark rather than a cluster.
inline constexpr uint32_t LAST_CLUSTER = 0x0FFFFFF8;

/// Where the FATs and the data begin, and how big a cluster is. Everything
/// else here is arithmetic on these three numbers.
struct Volume {
    uint32_t fat = 0;        // first sector of the first FAT
    uint32_t data = 0;       // sector holding cluster 2
    uint8_t per_cluster = 0; // sectors in a cluster

    [[nodiscard]] bool valid() const {
        return per_cluster != 0;
    }

    /// The first sector of a cluster. Clusters are numbered from two, which is
    /// why the data region starts at the number it does.
    [[nodiscard]] uint32_t sector_of(uint32_t cluster) const {
        return data + (cluster - 2) * per_cluster;
    }

    /// Which FAT sector holds this cluster's link, and where in it.
    [[nodiscard]] uint32_t link_sector(uint32_t cluster) const {
        return fat + cluster / LINKS_PER_SECTOR;
    }
};

using agos::le16;
using agos::le32;

/// The first FAT32 partition in a boot sector, or zero if it holds none.
///
/// Only the type and the LBA are read: the CHS geometry is what a BIOS wanted,
/// and the size is implied by the FAT.
[[nodiscard]] inline uint32_t partition_lba(const uint8_t* boot) {
    if (boot[0x1FE] != 0x55 || boot[0x1FF] != 0xAA)
        return 0;
    for (uint8_t i = 0; i < 4; ++i) {
        const uint16_t at = static_cast<uint16_t>(0x1BE + (i << 4));
        const uint8_t type = boot[at + 4];
        if (type == 0x0B || type == 0x0C) // FAT32, CHS or LBA
            return le32(boot + at + 8);
    }
    return 0;
}

/// The volume a FAT32 boot sector describes, given the sector it was read from.
/// An invalid volume comes back with no sectors per cluster.
[[nodiscard]] inline Volume volume_at(const uint8_t* bpb, uint32_t lba) {
    Volume v;
    if (le16(bpb + 0x0B) != SECTOR_BYTES) // anything else is not this format
        return v;
    const uint32_t per_fat = le32(bpb + 0x24);
    const uint8_t copies = bpb[0x10];
    if (per_fat == 0 || copies == 0 || bpb[0x0D] == 0)
        return v;
    v.per_cluster = bpb[0x0D];
    v.fat = lba + le16(bpb + 0x0E);    // after the reserved sectors
    v.data = v.fat + per_fat * copies; // cluster 2 follows the last copy
    return v;
}

/// The cluster a directory's entries start at: the root's comes from the BPB,
/// any other from its own entry.
[[nodiscard]] inline uint32_t root_cluster(const uint8_t* bpb) {
    return le32(bpb + 0x2C);
}

/// Lower case to upper, and anything else left alone: a card's 8.3 names are
/// upper case and the program's own are not, which is the only case here.
[[nodiscard]] inline char upper(char c) {
    return c >= 'a' && c <= 'z' ? static_cast<char>(c - ('a' - 'A')) : c;
}

/// A name as FAT stores it: eight and three, space padded, upper case, no dot.
/// "0123.zon" becomes "0123    ZON".
///
/// False when the name will not fit that, because what the card then holds is
/// a mangled stem -- GAMEAM~1 for gameamiga -- with the real name in long-name
/// entries beside it. The tilde number depends on what else was in the
/// directory when the file was written, so it cannot be worked out from the
/// name alone, and reading long names to recover it is a great deal of code
/// for a file we get to name ourselves. The staging tool keeps every name
/// inside 8.3, and this is what says so.
[[nodiscard]] inline bool fat_name(char out[11], const char* name) {
    for (uint8_t i = 0; i < 11; ++i)
        out[i] = ' ';
    uint8_t at = 0;
    for (; name[at] != '.' && name[at] != '\0'; ++at) {
        if (at >= 8)
            return false;
        out[at] = upper(name[at]);
    }
    if (name[at] != '.')
        return true;
    uint8_t i = 0;
    for (; name[at + 1 + i] != '\0'; ++i) {
        if (i >= 3)
            return false;
        out[8 + i] = upper(name[at + 1 + i]);
    }
    return i != 0;
}

/// What a directory entry says about a file, once one matches.
struct Entry {
    uint32_t cluster = 0;
    uint32_t bytes = 0;
    bool found = false;
    bool directory = false;
};

/// The named entry in one directory sector, if it is there.
///
/// Long-name entries are skipped by their attribute byte, and a first byte of
/// zero ends the directory -- no later sector holds anything.
[[nodiscard]] inline Entry find_in_sector(const uint8_t* sector, const char name[11], bool* ended) {
    *ended = false;
    for (uint8_t i = 0; i < DIRENTS_PER_SECTOR; ++i) {
        const uint8_t* at = sector + i * DIRENT_BYTES;
        if (at[0] == 0x00) {
            *ended = true;
            return {};
        }
        if (at[0] == 0xE5) // deleted, and still in the way
            continue;
        const uint8_t attributes = at[0x0B];
        if ((attributes & 0x0F) == 0x0F) // a long name, not an entry
            continue;
        bool same = true;
        for (uint8_t c = 0; c < 11 && same; ++c)
            same = static_cast<char>(at[c]) == name[c];
        if (!same)
            continue;
        Entry found;
        found.found = true;
        found.directory = (attributes & 0x10) != 0;
        found.cluster = (static_cast<uint32_t>(le16(at + 0x14)) << 16) | le16(at + 0x1A);
        found.bytes = le32(at + 0x1C);
        return found;
    }
    return {};
}

/// A cluster's link, out of the FAT sector that holds it.
[[nodiscard]] inline uint32_t link_in_sector(const uint8_t* fat_sector, uint32_t cluster) {
    const uint16_t at = static_cast<uint16_t>((cluster % LINKS_PER_SECTOR) * 4);
    return le32(fat_sector + at) & LINK_MASK;
}

/// Whether a link ends the chain rather than naming the next cluster.
[[nodiscard]] inline bool chain_ends(uint32_t link) {
    return link >= LAST_CLUSTER || link < 2;
}

} // namespace fat32
