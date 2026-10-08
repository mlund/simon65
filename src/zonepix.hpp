// SPDX-License-Identifier: GPL-3.0-or-later

// Every zone's packed pixels, held in Attic so the card is read once.

#pragma once

#include "atticmap.hpp"
#include "far.hpp"

#ifdef __mos__
#include "banks.hpp"
#else
#ifndef STORE_BANKED
#define STORE_BANKED
#endif
#endif

#include <stdint.h>

namespace agos {

/// A bump arena over atticmap::ZONEPIX with a directory of what is in it.
///
/// Demand-filled, not prefetched by chapter: what a scene draws from is what
/// it has asked for, and asking is already how a zone arrives. A hit costs a
/// walk of the directory and nothing else -- no card read, no copy.
///
/// Nothing is evicted one at a time. The game moves forward through chapters
/// and an arena that has filled is holding the chapter behind it, so it is
/// thrown away whole and filled again. Two megabytes against a chapter's
/// median 0.27 MiB means that is rare; ZONES_EMPTIED says how rare.
class PackedZones {
  public:
    /// Where a zone's pixels are and how many there are. Zero bytes is no zone:
    /// a file of nothing is not one either.
    struct Pixels {
        Place at = 0;
        uint32_t bytes = 0;
        [[nodiscard]] bool valid() const {
            return bytes != 0;
        }
    };

    /// As many zones as one chapter has, with room over.
    static constexpr uint8_t ENTRIES = 48;

    /// A DMA job's length is sixteen bits and a pixel file is a quarter of a
    /// megabyte at worst, so a copy is a handful of jobs.
    static constexpr uint32_t COPY_STEP = 0x8000;

    /// Where this zone's pixels are, or nothing.
    ///
    /// Starts are kept in 256-byte pages rather than summed from the lengths:
    /// the sum wants 32-bit arithmetic in the search loop, and a page shifts
    /// into place by moving bytes. 2 MiB is 8,192 pages, which is a uint16.
    [[nodiscard]] Pixels find(uint8_t zone) const {
        for (uint8_t i = 0; i < count_; ++i)
            if (zone_[i] == zone)
                return {place(page_[i]), bytes_[i]};
        return {};
    }

    /// Take a copy of a zone's pixels and say where they landed.
    ///
    /// Emptying first when the arena or the directory is full: see the class.
    /// A file too big for the arena is refused rather than half held.
    ///
    /// In the store's bank with the card read it follows, not in the fixed
    /// region: it runs once per zone and is 911 bytes of code.
    [[nodiscard]] [[gnu::noinline]] STORE_BANKED Pixels hold(
        uint8_t zone, Place from, uint32_t bytes) {
        const uint16_t pages = pages_for(bytes);
        if (pages == 0 || pages > PAGES)
            return {};
        if (count_ >= ENTRIES || pages > PAGES - used_)
            empty();

        const Place at = place(used_);
        for (uint32_t done = 0; done < bytes; done += COPY_STEP) {
            const uint32_t left = bytes - done;
            far_copy(
                from + done, at + done, static_cast<uint16_t>(left < COPY_STEP ? left : COPY_STEP));
        }

        zone_[count_] = zone;
        page_[count_] = used_;
        bytes_[count_] = bytes;
        ++count_;
        used_ = static_cast<uint16_t>(used_ + pages);
        ++held_;
        return {at, bytes};
    }

    /// The arena in pages, and a page's address. A file is rounded up to a
    /// page, which wastes 128 bytes a zone on average against the 32-bit
    /// arithmetic it saves everywhere else.
    static constexpr uint16_t PAGE_BYTES = 256;
    static constexpr uint16_t PAGES = atticmap::ZONEPIX_BYTES / PAGE_BYTES;

    [[nodiscard]] static constexpr Place place(uint16_t page) {
        return atticmap::ZONEPIX + (static_cast<Place>(page) << 8);
    }
    [[nodiscard]] static constexpr uint16_t pages_for(uint32_t bytes) {
        const uint32_t pages = (bytes + (PAGE_BYTES - 1)) / PAGE_BYTES;
        return pages > PAGES ? 0 : static_cast<uint16_t>(pages);
    }

    /// Remember that the card has not got this zone, so nothing asks again.
    ///
    /// A row of no bytes, which find() already reads as "no pixels", so the
    /// negative needs no field of its own. Without it a zone whose file will
    /// not read is asked for every frame, pinning the main loop to a failing
    /// card access.
    STORE_BANKED void note_absent(uint8_t zone) {
        if (knows(zone))
            return;
        if (count_ >= ENTRIES)
            empty();
        zone_[count_] = zone;
        page_[count_] = used_;
        bytes_[count_] = 0;
        ++count_;
    }

    /// Whether the store has an answer for this zone at all, which is not the
    /// same question as whether the answer is any pixels.
    [[nodiscard]] bool knows(uint8_t zone) const {
        for (uint8_t i = 0; i < count_; ++i)
            if (zone_[i] == zone)
                return true;
        return false;
    }

    /// Forget one zone, whose pixels changed under its name (the beard). Its
    /// bytes stay where they are until the arena is next emptied: a toggle
    /// costs one copy of the zone, and the game toggles six times at most.
    void forget(uint8_t zone) {
        for (uint8_t i = 0; i < count_; ++i)
            if (zone_[i] == zone)
                zone_[i] = GONE;
    }

    /// Forget the lot. Whatever remembers where a zone's pixels were is wrong
    /// from here on, which is what emptied() is for.
    void empty() {
        count_ = 0;
        used_ = 0;
        ++emptied_;
    }

    [[nodiscard]] uint8_t holding() const {
        return count_;
    }
    [[nodiscard]] uint16_t held() const {
        return held_;
    }
    [[nodiscard]] uint16_t emptied() const {
        return emptied_;
    }

  private:
    /// Not a zone the game has: what a forgotten entry is renamed to.
    static constexpr uint8_t GONE = 0xFF;

    uint8_t zone_[ENTRIES] = {};
    uint16_t page_[ENTRIES] = {};
    uint32_t bytes_[ENTRIES] = {};
    uint16_t used_ = 0; //!< the bump, in pages
    uint8_t count_ = 0;
    uint16_t held_ = 0;
    uint16_t emptied_ = 0;
};

/// Whether a zone's pixels are kept at all. Off, every ask reads the card and
/// everything else about the arena stays in place, which tells a fault in the
/// caching from a fault in what surrounds it.
inline constexpr bool CACHE_PIXELS = true;

/// The one arena. In .bss, not in the reserved low memory the VM tables use:
/// ram_low has 144 bytes left and this is 343. .bss is zeroed by the crt, so
/// unlike everything in that reserved region nothing has to empty it at boot.
inline PackedZones zone_pixels;

} // namespace agos
