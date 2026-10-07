// Which code bank holds what, as plain macros an assembly file can read too.

#pragma once

/// Which bank holds what. One statement of it, because a plain call from one
/// bank to another runs whatever sits at that address in the caller's bank --
/// so two headers naming the same number is a trap that springs the day one
/// of them is changed. Macros, not constants: the SDK builds a section name by
/// stringifying the token it is given.
#define AGOS_VGA_BANK 1      // the animation dispatch, most of a bank on its own
#define AGOS_VGA_TICK_BANK 2 // the tick that drives it, and the card reader
#define AGOS_CARD_BANK AGOS_VGA_TICK_BANK
#define AGOS_COMPOSITE_BANK 3 // stacked sprites merged, work and all: chip, hot
#define AGOS_SCRIPT_BANK 4    // the script dispatch, 188 opcodes of it
#define AGOS_ROOM_BANK 5      // the decoder and the room painter: chip, they are hot
#define AGOS_DISPLAY_BANK 6   // the VIC setup and the frame's own drawing
#define AGOS_EXTRA_BANK 7     // in the Attic: what fires on a scene change
#define AGOS_STORE_BANK 8     // in the Attic: the store's loading, the VM resets
#define AGOS_TICK_BANK 9      // in the Attic: the world's turn, mostly doors
#define AGOS_VERB_BANK 10     // in the Attic: the verb bar and the boxes it reads
#define AGOS_SAVE_BANK 11     // in the Attic: save and load, and the image between
#define AGOS_SOUND_BANK 12    // in the Attic: speech off the card, card-bound

/// A bank's section name, for an assembly file: `.section BANK_SECTION(n)`.
#define BANK_SECTION_(n) .bank_##n
#define BANK_SECTION(n) BANK_SECTION_(n)
