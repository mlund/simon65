// SPDX-License-Identifier: GPL-3.0-only

// The SD controller, one sector at a time, into any 28-bit address.
//
// The point of reading the card ourselves is that this polls: the raster
// interrupt runs between polls, so the MOD player keeps its 50 Hz. Hyppo's
// whole-file read cannot, because no interrupt at all is dispatched while the
// CPU is in hypervisor mode (gs4510.vhdl:6833).
//
// Ported from mega65-freezer's src/sdcard.c, itself from the MEGA65 freeze
// menu (github.com/MEGA65/mega65-freezemenu, GPL-3), with the border flicker
// dropped and writes cut to the one sector a save needs.

#pragma once

#include "banks.hpp"
#include "far.hpp"
#include "fat32.hpp"

#ifdef __mos__
#include <mega65.h>
#endif

#include <stdint.h>

namespace card {

/// The controller's own buffer. A 28-bit address, reached by DMA rather than
/// by a pointer, which is what lets a sector land anywhere -- Attic included.
inline constexpr agos::Place SECTOR_BUFFER = 0xFFD6E00;

#ifdef __mos__

/// Polls, not time: how long a read waits before the card counts as stalled.
/// The controller answers in microseconds when it is well.
inline constexpr uint16_t READY_POLLS = 50000;

/// A card that is not SDHC is addressed by byte, and sector * 512 has to stay
/// inside 32 bits, so the sector number stops here.
inline constexpr uint32_t MAX_BYTE_ADDRESSED = 0x7FFFFF;

/// Busy, still in reset, or in error: all must be clear before a command's
/// result means anything.
inline constexpr uint8_t BUSY = SD_SDIO_BUSY_MASK | SD_CARD_BUSY_MASK;
inline constexpr uint8_t UNSETTLED = BUSY | SD_RESET_MASK | SD_FSM_ERROR_MASK | SD_ERROR_MASK;

/// How many times a read is retried, each time through a controller reset.
inline constexpr uint8_t TRIES = 10;

/// A controller reset, bounded: a card that never leaves busy must not take
/// the machine with it, and the caller has a fault code to report.
CARD_BANKED inline void reset() {
    SDCARD.command = SDCARD_RESET_BEGIN;
    SDCARD.command = SDCARD_RESET_END;
    for (uint16_t wait = READY_POLLS; wait != 0 && (SDCARD.status & BUSY); --wait)
        ;
    // SD_SDHC_MASK reads sdhc_mode, which is the controller's addressing mode
    // rather than anything about the card (sdcardio.vhdl:1271, :283), and only
    // the $40/$41 commands change it (:3040-3041) -- a reset leaves it alone.
    // Setting it back is therefore belt and braces against a core that does not.
    if (SDCARD.status & SD_SDHC_MASK)
        SDCARD.command = SDCARD_SDHC_MODE_ON;
}

/// Wait for the controller to stop being busy. False if it never does, or if
/// it reports an error: sdcard.vhdl thinking a job done where sdcardio.vhdl
/// does not is a read error rather than a state worth waiting out.
[[nodiscard]] CARD_BANKED inline bool waited() {
    for (uint16_t timeout = READY_POLLS; timeout != 0; --timeout) {
        if (!(SDCARD.status & BUSY))
            return true;
        if ((SDCARD.status & SD_ERROR_MASK) || SDCARD.status == SD_SDIO_BUSY_MASK)
            return false;
    }
    return false;
}

/// A sector into the controller's own buffer, and no further: where it goes
/// next is the caller's, and the two forms below are the only answers.
/// False when the controller would not settle, which is a card fault rather
/// than a missing file: there is nothing else to read the card with.
///
/// The addressing mode is read from the controller rather than remembered,
/// because Hyppo has already opened the card by the time anything here runs
/// and the status bit is what it left behind.
[[nodiscard]] CARD_BANKED inline bool fetch_sector(uint32_t sector) {
    // $FFD6E00 is the SD controller's buffer only while BUFFSEL is set; clear,
    // the same window is the F011 floppy controller's (iomap.txt:775). Hyppo
    // asserts it before every directory walk of its own (dos.asm:2356-2358),
    // which is the evidence that it is found clear -- a D81 mount is enough to
    // do it. Reading the wrong buffer would hand back plausible rubbish rather
    // than fail, so it is set here rather than once at boot.
    SDCARD.control |= SD_BUFFSEL_MASK;

    const bool sdhc = (SDCARD.status & SD_SDHC_MASK) != 0;
    if (!sdhc && sector >= MAX_BYTE_ADDRESSED)
        return false;
    SDCARD.sector_number = sdhc ? sector : sector * fat32::SECTOR_BYTES;

    for (uint8_t attempt = 0; attempt < TRIES; ++attempt) {
        // Every wait is bounded, and every way out of one leads to the reset at
        // the foot of this loop: a card that wedges busy without raising an error
        // would otherwise hang here with the music still playing, which reads
        // from the outside as a slow load rather than as a fault.
        if (!waited()) {
            reset();
            continue;
        }
        SDCARD.command = SDCARD_READ_SECTOR;
        if (!waited()) {
            reset();
            continue;
        }
        if (!(SDCARD.status & UNSETTLED))
            return true;
        reset();
    }
    return false;
}

/// Rounds of READY_POLLS a write may stay busy. A card part way through an
/// internal erase is busy far longer than a read; the freezer's figure, which
/// over 1,600 hardware writes was never reached (mega65-freezer sdcard.c:250).
inline constexpr uint8_t WRITE_ROUNDS = 200;

/// Idle, given a write's time. Busy alone is asked, as the freezer asks it:
/// waited()'s early outs judge a read, and a write still under way would trip
/// them.
[[nodiscard]] SAVE_BANKED inline bool settled_for_write() {
    for (uint8_t round = 0; round < WRITE_ROUNDS; ++round)
        for (uint16_t poll = READY_POLLS; poll != 0; --poll)
            if (!(SDCARD.status & BUSY))
                return true;
    return false;
}

/// 512 bytes from @p from onto a sector, and the sector read back into the
/// controller's buffer.
///
/// In the save bank, its one caller's, not the card reader's: bank 2 has not
/// the room. The gate opens the controller for one write and closes on its
/// own (sdcardio.vhdl:2944-2948, :3048-3050); without it the write is an
/// error. No reset and retry, unlike a read: the freezer's code holds that a
/// reset under a write still in progress makes things worse. The read after
/// it is the freezer's too -- the controller misbehaves unless a read follows
/// a write -- and it leaves what the card now holds for the caller to check.
[[nodiscard]] SAVE_BANKED inline bool store_sector(uint32_t sector, agos::Place from) {
    SDCARD.control |= SD_BUFFSEL_MASK; // see fetch_sector
    const bool sdhc = (SDCARD.status & SD_SDHC_MASK) != 0;
    if (!sdhc && sector >= MAX_BYTE_ADDRESSED)
        return false;
    if (!settled_for_write())
        return false;
    agos::far_copy(from, SECTOR_BUFFER, fat32::SECTOR_BYTES);
    SDCARD.sector_number = sdhc ? sector : sector * fat32::SECTOR_BYTES;
    SDCARD.command = SDCARD_WRITE_GATE;
    SDCARD.command = SDCARD_WRITE_SECTOR;
    if (!settled_for_write() || (SDCARD.status & UNSETTLED))
        return false;
    SDCARD.command = SDCARD_READ_SECTOR;
    return settled_for_write();
}

/// The same sector where a pointer can reach it: the card's own structures are
/// read by picking fields out of them, which wants near memory.
[[nodiscard]] CARD_BANKED inline bool read_sector_near(uint32_t sector, uint8_t* into) {
    if (!fetch_sector(sector))
        return false;
    agos::far_read(SECTOR_BUFFER, into, fat32::SECTOR_BYTES);
    return true;
}

#endif // __mos__

} // namespace card
