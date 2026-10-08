// SPDX-License-Identifier: GPL-3.0-or-later

// What a VM can fail at, and the hook that records it.
//
// A code rather than a string: a literal lives in the bank that raised it, and
// a bank that is not mapped reads as nothing, so the machine reported a fault
// it could not name. A number reaches the counters from any bank.

#pragma once

#include <stdint.h>

namespace agos {

/// A state a VM cannot represent: out of range, or an assumption about the
/// data broken. Nought is no fault, so the counter reads unset until one is
/// raised; the animation VM's start at a base of their own, so the number says
/// which VM as well as which fault.
enum class Fault : uint8_t {
    NO_SUBROUTINE = 1,
    TIMEOUTS_FULL,
    VARIABLE_RANGE,
    ITEM_MISSING,
    BIT_RANGE,
    ITEM_ITS_OWN_PARENT,
    MOVING_NULL_ITEM,
    ITEM_OFF_NULL_ITEM,
    ITEM_NOT_A_CHILD,
    OPCODE_RANGE,
    RECURSION_DEEP,
    DIVIDE_BY_ZERO,
    MODULO_BY_ZERO,
    VGA_SLOT_RANGE,
    ITEM_STORE_RANGE,
    COMMENT_SURVIVED,
    /// A line ran past the end of its own subroutine. The heap holds them back
    /// to back, so the opcodes after it are the next subroutine's, executed
    /// from the middle with nothing having entered it.
    LINE_RAN_PAST,

    VGA_SKIPPED_OPCODE = 32,
    VGA_TIMERS_FULL,
    VGA_SLEEPERS_FULL,
    VGA_SPRITES_FULL,
    VGA_NO_ANIMATION,
    VGA_OPCODE_MISSING,
    VGA_NO_IMAGE_SCRIPT,
    VGA_FLAGS_TOO_WIDE,
    VGA_PATH_SLOT_RANGE,
    VGA_NO_ROUTE,
    VGA_PATH_OVERRAN,
    VGA_ROOM_UNDECODED,
    // Appended, never inserted: a kind's number is what a readback shows.
    STACK_OVERRAN,
    /// DMA could not reach the compositor's buffers where the CPU sees them.
    COMPOSITE_SCRATCH,
    /// A window number past the table the VM keeps (SET_WINDOW, 26 and 31).
    VGA_WINDOW_RANGE,
    /// A save file this database cannot take; the value is the item, or nought
    /// for the header or the length (savegame.hpp).
    SAVE_REFUSED,
    /// The state would not fit the image buffer; the value is its size.
    SAVE_TOO_LONG,
    /// The save file is not on the card, is shorter than an image, or did not
    /// read back as written.
    SAVE_NO_FILE,
};

/// Raise one. @p value is whatever named it: an id, a slot, an opcode.
/// One hook per VM, because the counters read the same either way and a
/// caller reads better saying which machine it is.
void script_fault(Fault what, uint16_t value);
void vga_fault(Fault what, uint16_t value);

} // namespace agos
