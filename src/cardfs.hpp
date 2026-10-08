// SPDX-License-Identifier: GPL-3.0-or-later

// The game's files off the card, without the hypervisor: mount the volume the
// machine booted from, find a name in the game's directory, and follow a
// file's clusters into memory.
//
// The sector source is a policy rather than a call, so the walking can be
// driven on the host against a card built in memory -- which is where the
// arithmetic belongs, and where a wrong cluster is a failed test rather than
// a room of noise.

#pragma once

#include "far.hpp"
#include "fat32.hpp"

#ifdef __mos__
#include "banks.hpp"
#define RUNS_BANKED SOUND_BANKED
#else
#define CARD_BANKED
#define RUNS_BANKED
#endif

#include <stdint.h>

namespace card {

/// Where a file's bytes go, and how many are left there.
struct Landing {
    agos::Place at = 0;
    uint32_t room = 0;
};

/// The volume and the game's own directory, found once at boot.
///
/// `Sectors` supplies three things: `near(sector, uint8_t*)` for what has to
/// be picked apart with a pointer, and `hold(sector)` then `spill(offset, to,
/// n)` for what only passes through. On the machine they are the SD
/// controller; in a test they are an image in memory.
template <typename Sectors> class Files {
  public:
    /// The card the machine booted from, and the directory the game's files are
    /// in. False if there is no FAT32 partition, no volume, or no such
    /// directory -- all of which mean the card is not the one we were loaded
    /// from, so nothing is gained by carrying on.
    [[nodiscard]] CARD_BANKED bool mount(const char* directory) {
        if (!load_near(0))
            return false;
        const uint32_t lba = fat32::partition_lba(sector_);
        if (lba == 0 || !load_near(lba))
            return false;
        volume_ = fat32::volume_at(sector_, lba);
        if (!volume_.valid())
            return false;

        const fat32::Entry dir = walk(fat32::root_cluster(sector_), directory);
        if (!dir.found || !dir.directory)
            return false;
        directory_ = dir.cluster;
        return true;
    }

    [[nodiscard]] bool mounted() const {
        return directory_ != 0;
    }

    /// What the directory says about a name, or `found` clear.
    [[nodiscard]] CARD_BANKED fat32::Entry find(const char* name) {
        return mounted() ? walk(directory_, name) : fat32::Entry{};
    }

    /// A file into one landing, or into two split at `first_bytes`.
    ///
    /// Sector by sector, so the raster interrupt can run between controller
    /// polls; a whole-file hypervisor read would dispatch nothing. False if the
    /// file is absent or would not fit.
    [[nodiscard]] CARD_BANKED bool read(const char* name,
        Landing first,
        uint32_t first_bytes,
        Landing second,
        bool* reached_second) {
        const fat32::Entry file = find(name);
        if (!file.found || file.directory)
            return false;
        return read_from(file, 0, first, first_bytes, second, reached_second);
    }

    /// One landing, every file but a zone's. Returns the file's byte count, or
    /// zero: the parser uses this to bound its walk, avoiding the region's
    /// capacity which a short file would exhaust prematurely.
    [[nodiscard]] CARD_BANKED uint32_t read(const char* name, agos::Place into, uint32_t most) {
        const fat32::Entry file = find(name);
        if (!file.found || file.directory)
            return 0;
        if (!read_from(file, 0, {into, most}, 0xFFFFFFFF, {}, nullptr))
            return 0;
        return file.bytes;
    }

    /// A file that says where its own split is: the first two bytes are how
    /// many of the rest go to the first landing, and everything after that to
    /// the second. That is the joined zone container (game_store's
    /// ZONE_HEADER), and reading the count here saves walking the directory a
    /// second time to learn it.
    [[nodiscard]] CARD_BANKED bool read_headed(const char* name,
        uint8_t header,
        Landing first,
        Landing second,
        bool* reached_second,
        uint32_t* second_bytes = nullptr) {
        *reached_second = false;
        if (second_bytes != nullptr)
            *second_bytes = 0;
        const fat32::Entry file = find(name);
        if (!file.found || file.directory || file.bytes < header || file.cluster < 2)
            return false;
        if (!load_near(volume_.sector_of(file.cluster)))
            return false;
        const uint32_t to_first = fat32::le16(sector_);
        // What the second landing takes is the rest of the file, and the caller
        // wants it: it is the length of the zone's pixels, which nothing else
        // states. Without it a caller has only the region's size, and copying
        // that is a quarter of a megabyte moved for a median zone of 31 KiB.
        if (second_bytes != nullptr && file.bytes > header + to_first) {
            const uint32_t rest = file.bytes - header - to_first;
            *second_bytes = rest < second.room ? rest : second.room;
        }
        return read_from(file, header, first, to_first, second, reached_second);
    }

    /// @p count consecutive card sectors from @p first to @p to: a run of a
    /// file mapped by `Runs`, below.
    [[nodiscard]] CARD_BANKED bool read_sectors(uint32_t first, agos::Place to, uint16_t count) {
        for (; count != 0; --count, ++first, to += fat32::SECTOR_BYTES) {
            if (!Sectors::hold(first))
                return false;
            Sectors::spill(0, to, fat32::SECTOR_BYTES);
        }
        return true;
    }

    [[nodiscard]] const fat32::Volume& volume() const {
        return volume_;
    }

    /// The link out of the FAT, through a one-sector cache: a cluster's
    /// neighbours share a FAT sector, so a file's whole chain is usually one
    /// read rather than one a cluster.
    [[nodiscard]] CARD_BANKED uint32_t next_cluster(uint32_t cluster) {
        const uint32_t at = volume_.link_sector(cluster);
        if (at != fat_held_) {
            if (!load_near(at))
                return 0;
            fat_held_ = at;
        }
        return fat32::link_in_sector(sector_, cluster);
    }

  private:
    /// The walk itself, once the entry is in hand: `skip` bytes off the front
    /// go nowhere, which is how a container's header is stepped over.
    [[nodiscard]] CARD_BANKED bool read_from(const fat32::Entry& file,
        uint8_t skip,
        Landing first,
        uint32_t first_bytes,
        Landing second,
        bool* reached_second) {
        if (reached_second != nullptr)
            *reached_second = false;

        uint32_t left = file.bytes;
        uint32_t cluster = file.cluster;
        uint32_t to_first = first_bytes;
        while (left != 0) {
            if (cluster < 2)
                return false;
            const uint32_t base = volume_.sector_of(cluster);
            for (uint8_t i = 0; i < volume_.per_cluster && left != 0; ++i) {
                const uint16_t n =
                    left < fat32::SECTOR_BYTES ? static_cast<uint16_t>(left) : fat32::SECTOR_BYTES;
                if (!place(base + i, skip, n, first, to_first, second, reached_second))
                    return false;
                left -= n;
                skip = 0; // only the first sector has the header on it
            }
            if (left == 0)
                break;
            cluster = next_cluster(cluster);
            if (fat32::chain_ends(cluster))
                return false; // the chain stopped before the size did
        }
        return true;
    }

    /// A directory's entries, cluster by cluster, until the name turns up or the
    /// directory ends. A deleted entry is stepped over inside the sector, which
    /// is why a restaged card scans longer than it holds.
    [[nodiscard]] CARD_BANKED fat32::Entry walk(uint32_t cluster, const char* name) {
        char wanted[11];
        if (!fat32::fat_name(wanted, name))
            return {}; // nothing 8.3 cannot hold is on our card
        while (!fat32::chain_ends(cluster)) {
            const uint32_t base = volume_.sector_of(cluster);
            for (uint8_t i = 0; i < volume_.per_cluster; ++i) {
                if (!load_near(base + i))
                    return {};
                bool ended = false;
                const fat32::Entry found = fat32::find_in_sector(sector_, wanted, &ended);
                if (found.found || ended)
                    return found;
            }
            cluster = next_cluster(cluster);
        }
        return {};
    }

    /// Everything that fills the near buffer goes through here, because the FAT
    /// cache below is a claim about what is in it.
    [[nodiscard]] CARD_BANKED bool load_near(uint32_t sector) {
        fat_held_ = 0;
        return Sectors::near(sector, sector_);
    }

    /// One sector's worth into whichever landing is still owed bytes. The
    /// straddling sector is the only case worth naming: its front finishes the
    /// first landing and the rest starts the second.
    [[nodiscard]] CARD_BANKED bool place(uint32_t sector,
        uint16_t skip,
        uint16_t n,
        Landing& first,
        uint32_t& to_first,
        Landing& second,
        bool* reached_second) {
        if (skip >= n)
            return skip == n; // a header filling its own sector
        n = static_cast<uint16_t>(n - skip);
        uint16_t head = n;
        if (to_first < head)
            head = static_cast<uint16_t>(to_first);
        if (head > first.room)
            return false;
        const uint16_t tail = static_cast<uint16_t>(n - head);
        if (tail > second.room)
            return false;

        if (!Sectors::hold(sector))
            return false;
        if (head != 0) {
            Sectors::spill(skip, first.at, head);
            first.at += head;
            first.room -= head;
            to_first -= head;
        }
        if (tail != 0) {
            Sectors::spill(static_cast<uint16_t>(skip + head), second.at, tail);
            second.at += tail;
            second.room -= tail;
            if (reached_second != nullptr)
                *reached_second = true;
        }
        return true;
    }

    fat32::Volume volume_;
    uint32_t directory_ = 0;
    uint32_t fat_held_ = 0;
    uint8_t sector_[fat32::SECTOR_BYTES] = {};
};

/// A file's chain as runs of card sectors, so it can be read from anywhere
/// without walking the chain from its start: a voice deep in 171 MB of speech
/// is thousands of links in. A run is its first card sector and its length in
/// sectors, little-endian, RUN_BYTES apiece, kept in the Attic.
///
/// Apart from `Files` and through `Card` because the bank holding the card
/// reader has no room for it, and it need not be quick: the card is slower.
/// `Card` supplies `find(name)`, `next(cluster)`, `volume()` and
/// `read(first, to, count)` for consecutive sectors -- on the machine, doors
/// into the card's bank; in a test, a `Files` over an image in memory.
template <typename Card> class Runs {
  public:
    struct Run {
        uint32_t first;
        uint32_t sectors;
    };
    static constexpr uint8_t RUN_BYTES = sizeof(Run);

    /// Map @p name into the Attic at @p into. How many runs, or nought if the
    /// file is absent, empty, cut short, or needs more than @p most -- then
    /// nothing is mapped.
    [[nodiscard]] RUNS_BANKED uint16_t map(const char* name, agos::Place into, uint16_t most) {
        count_ = 0;
        const fat32::Entry file = Card::find(name);
        if (!file.found || file.directory || file.bytes == 0)
            return 0;
        const fat32::Volume& volume = Card::volume();
        uint32_t left = (file.bytes + fat32::SECTOR_BYTES - 1) / fat32::SECTOR_BYTES;
        uint32_t cluster = file.cluster;
        uint16_t runs = 0;
        Run run{};
        while (left != 0) {
            if (cluster < 2 || fat32::chain_ends(cluster))
                return 0; // the chain stopped before the size did
            const uint32_t first = volume.sector_of(cluster);
            const uint32_t n = left < volume.per_cluster ? left : volume.per_cluster;
            if (run.sectors != 0 && run.first + run.sectors == first) {
                run.sectors += n;
            } else {
                if (run.sectors != 0 && !keep(run, into, runs++, most))
                    return 0;
                run = {first, n};
            }
            left -= n;
            if (left != 0)
                cluster = Card::next(cluster);
        }
        if (!keep(run, into, runs++, most))
            return 0;
        at_ = into;
        count_ = runs;
        return runs;
    }

    /// @p count sectors of the mapped file, from its sector @p from, to @p to.
    /// False past its end or on a card error; what landed before then stays.
    [[nodiscard]] RUNS_BANKED bool read(uint32_t from, agos::Place to, uint16_t count) {
        for (uint16_t at = 0; at < count_ && count != 0; ++at) {
            Run run;
            agos::far_read(
                at_ + agos::Place{at} * RUN_BYTES, reinterpret_cast<uint8_t*>(&run), RUN_BYTES);
            // far_read fills run by DMA, which the analyzer cannot see.
            // NOLINTNEXTLINE(clang-analyzer-core.UndefinedBinaryOperatorResult)
            if (from >= run.sectors) {
                from -= run.sectors;
                continue;
            }
            const uint32_t room = run.sectors - from;
            const uint16_t n = count < room ? count : static_cast<uint16_t>(room);
            if (!Card::read(run.first + from, to, n))
                return false;
            to += agos::Place{n} * fat32::SECTOR_BYTES;
            count = static_cast<uint16_t>(count - n);
            from = 0;
        }
        return count == 0;
    }

  private:
    /// A finished run into its place in the list, if the list has room.
    [[nodiscard]] RUNS_BANKED static bool keep(
        const Run& run, agos::Place into, uint16_t index, uint16_t most) {
        if (index >= most)
            return false;
        agos::far_write(into + agos::Place{index} * RUN_BYTES,
            reinterpret_cast<const uint8_t*>(&run),
            RUN_BYTES);
        return true;
    }

    agos::Place at_ = 0;
    uint16_t count_ = 0;
};

} // namespace card
