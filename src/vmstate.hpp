// The VMs and the database they walk, placed rather than left to the linker.
//
// One copy of each, at namespace scope: they are far too big for a stack frame
// and there is never a second. Where each one sits is decided in layout.ld and
// says why; the sizes below are what the target compiler reports, so a change
// that grows one shows up here rather than as a region overflow with no name
// attached.

#pragma once

#include "chipmap.hpp"
#include "game_store.hpp"
#include "script_vm.hpp"
#include "vga_vm.hpp"

namespace vmstate {

using chipmap::DATABASE_BYTES;

[[gnu::section(".gamedb")]] extern uint8_t database[DATABASE_BYTES];

/// The animation VM, in ram_low, which nothing else here uses.
[[gnu::section(".vmstate")]] extern agos::VgaVm animation;

/// The script VM, in ram_low with the other one, not in ram_fixed: the store
/// owns the database and heap, so the VM reaches them through pointers from
/// any location, and ram_fixed has less room to spare.
[[gnu::section(".vmstate")]] extern agos::ScriptVm script;

// Measured with the target compiler, and checked against the region the linker
// script gives each of them.
static_assert(
    sizeof(agos::VgaVm) + sizeof(agos::ScriptVm) <= 0x1D00, "the two VMs outgrew ram_low");

} // namespace vmstate
