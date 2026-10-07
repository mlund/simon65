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

/// Where the player is linked, and where it is entered: the same address,
/// since it carries no BASIC line of its own.
constexpr uint16_t PLAYER_LINKED = 0x2000;
constexpr uint16_t PLAYER_ENTRY = PLAYER_LINKED;

/// Where its bytes go, which is two lower. A PRG opens with two bytes saying
/// where it loads and Hyppo's loadfile writes them like any others, so the
/// file goes down two bytes early and the header falls into the gap -- below
/// $2000, which is in neither image.
constexpr uint32_t PLAYER_AT = PLAYER_LINKED - 2;

/// The player, by the name the card holds it under. Eight and three: Hyppo
/// matches nothing longer, whatever the directory's long-name entries say.
constexpr const char PLAYER_FILE[] = "SIMON65M.PRG";

/// Above the hardware stack and below both images: the few instructions that
/// do the load cannot live in either image, because the load overwrites
/// $2000. The filename sits in the page after it, for the same reason and one
/// more -- Hyppo takes a name by page.
constexpr uint16_t RUNS_AT = 0x0400;
constexpr uint16_t NAME_AT = 0x0500;

/// Load the file Hyppo has been named with to @p at, then go to @p entry.
///
/// Hyppo's loadfile is DOS function $36 through the trap at $D640, with the
/// destination in X, Y and Z. The map is cleared before the jump so the
/// player starts as it expects to.
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

/// Every audio-DMA channel off, and the master switch with it. A reset leaves
/// these as the last program left them, which is the noise a freshly loaded
/// machine makes.
void silence() {
    *reinterpret_cast<volatile uint8_t*>(0xD711) = 0;
    auto* const channel = reinterpret_cast<volatile uint8_t*>(0xD720);
    for (uint8_t at = 0; at < 4 * 0x10; at += 0x10) {
        channel[at] = 0;     // flags: enable, loop and the rest
        channel[at + 9] = 0; // volume
    }
}

/// The display off and the border black: what is on screen is the ROM's text
/// and whatever the loader left of it.
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

    // The name goes in a page this program owns, not in $0100 (the hardware
    // stack): every call between naming the file and loading it pushes a return
    // address over the name. A name overwritten by a return address makes the
    // load fail silently.
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
