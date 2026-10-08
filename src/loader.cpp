// SPDX-License-Identifier: GPL-3.0-or-later

// SIMON65.PRG: the program the machine runs, which loads the one that plays.
//
// Two programs, because a program cannot blank the screen before it is
// loaded. This one is BASIC 65's own start address, so `RUN` reaches it; it
// darkens the display and silences the audio-DMA channels -- neither of which
// a reset clears -- and then loads the player over itself and goes there.
//
// The loader code must run from memory the load does not overwrite (twp65's
// src/handover.h). The load writes from $2000, so the code is copied to $0400 --
// above the hardware stack, below both images, in a page nothing else keeps.
//
// Built for the plain mega65 platform rather than the banked one: it wants
// Hyppo and a BASIC line, and none of the player's banks.

#include <mega65.h>
#include <stdint.h>

namespace {

/// Where the player is linked and entered (the same address; no BASIC line).
constexpr uint16_t PLAYER_LINKED = 0x2000;
constexpr uint16_t PLAYER_ENTRY = PLAYER_LINKED;

/// Where its bytes go, two lower. A PRG's header (load address) is written
/// like any other byte, so the file goes early and the header falls in the
/// gap below $2000, in neither image.
constexpr uint32_t PLAYER_AT = PLAYER_LINKED - 2;

/// The player, by its FAT name. 8.3: Hyppo matches nothing longer.
constexpr const char PLAYER_FILE[] = "SIMON65M.PRG";

/// Above the stack and below both images (the load overwrites $2000).
/// The filename sits after, for the same reason (Hyppo takes a name by page).
constexpr uint16_t RUNS_AT = 0x0400;
constexpr uint16_t NAME_AT = 0x0500;

/// Load the file Hyppo is named with to @p at, then jump to @p entry.
/// Hyppo's loadfile ($36) is triggered at $D640 with the destination in X, Y, Z.
/// The map is cleared before the jump so the player starts as it expects.
struct Stub {
    uint8_t code[22];
};

[[nodiscard]] constexpr Stub stub(uint32_t at, uint16_t entry) {
    return {{
        0xA2,
        static_cast<uint8_t>(at), // ldx #<at
        0xA0,
        static_cast<uint8_t>(at >> 8U), // ldy #>at
        0xA3,
        static_cast<uint8_t>(at >> 16U), // ldz #^at
        0xA9,
        0x36, // lda #$36    hyppo loadfile
        0x8D,
        0x40,
        0xD6, // sta $d640
        0xB8, // clv
        0xA9,
        0x00, // lda #$00
        0xAA, // tax
        0xA8, // tay
        0x4B, // taz
        0x5C, // map
        0xEA, // eom
        0x4C,
        static_cast<uint8_t>(entry), // jmp entry
        static_cast<uint8_t>(entry >> 8U),
    }};
}

/// Every audio-DMA channel off, and the master switch. A reset leaves them
/// as the last program did (the noise a freshly loaded machine makes).
void silence() {
    *reinterpret_cast<volatile uint8_t*>(0xD711) = 0;
    auto* const channel = reinterpret_cast<volatile uint8_t*>(0xD720);
    for (uint8_t at = 0; at < 4 * 0x10; at += 0x10) {
        channel[at] = 0;     // flags: enable, loop and the rest
        channel[at + 9] = 0; // volume
    }
}

/// The display off and the border black: ROM text and the loader's leftovers.
void darken() {
    *reinterpret_cast<volatile uint8_t*>(0xD020) = 0; // border
    *reinterpret_cast<volatile uint8_t*>(0xD021) = 0; // background
    auto* const ctrl1 = reinterpret_cast<volatile uint8_t*>(0xD011);
    *ctrl1 = static_cast<uint8_t>(*ctrl1 & ~0x10); // DEN
}

} // namespace

int main() {
    darken();
    silence();

    // Name in this program's page, not $0100 (the stack): calls between
    // naming and loading push return addresses over it, and the load then
    // fails silently.
    auto* const name = reinterpret_cast<uint8_t*>(NAME_AT);
    uint8_t i = 0;
    for (; PLAYER_FILE[i] != '\0'; ++i)
        name[i] = static_cast<uint8_t>(PLAYER_FILE[i]);
    name[i] = 0;
    (void)mega65_h_setname_page(NAME_AT >> 8);

    static constexpr Stub PIECE = stub(PLAYER_AT, PLAYER_ENTRY);
    auto* const there = reinterpret_cast<uint8_t*>(RUNS_AT);
    for (uint8_t i = 0; i < sizeof PIECE.code; ++i)
        there[i] = PIECE.code[i];
    reinterpret_cast<void (*)()>(RUNS_AT)();
    return 0; // not reached: the player has the machine now
}
