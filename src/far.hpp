// Reaching memory a pointer cannot.
//
// A pointer is sixteen bits here and the data is not: the subroutine heap sits
// at $28000, the zone scripts in Attic, and the largest zone script is 65,468
// bytes on its own. So the streams the two VMs walk are addressed by value
// rather than pointed at, and every access goes through here.
//
// On the machine that is one instruction, `lda [zp],z`. On the host it reaches
// a buffer standing in for the machine's memory, so a test exercises the very
// addresses the target will.

#pragma once

#include <stdint.h>

#ifdef __mos__
#include "move.hpp"

#include <mega65.h>
#endif

namespace agos {

/// Little-endian out of a near buffer, as the card and the CPU both are.
///
/// Widened before the shift: `int` is 16 bits here, so a byte above 127
/// overflows a signed one. Here and not beside either reader, because the card
/// and the click boxes both want it and two copies had the same bug twice.
[[nodiscard]] inline uint16_t le16(const uint8_t* at) {
    return static_cast<uint16_t>(at[0] | (static_cast<uint16_t>(at[1]) << 8));
}

[[nodiscard]] inline uint32_t le32(const uint8_t* at) {
    return static_cast<uint32_t>(le16(at)) | (static_cast<uint32_t>(le16(at + 2)) << 16);
}

/// A 28-bit address: chip RAM, Attic or I/O.
using Place = uint32_t;

/// No address at all. Nothing the game reads lives at zero -- the first page of
/// chip RAM is the zero page -- so it can stand for absence.
inline constexpr Place NOWHERE = 0;

#ifdef __mos__

[[nodiscard]] inline uint8_t far_read8(Place at) {
    return mega65_peek_far(at);
}
inline void far_write8(Place at, uint8_t value) {
    mega65_poke_far(at, value);
}

/// A block of near bytes to far memory, by DMA.
///
/// Worth gathering bytes near for: one far_write8 is nine instructions
/// rebuilding a zero-page pointer around the one store that matters, so a
/// block that goes out in one job stops paying that per byte.
inline void far_write(Place at, const uint8_t* from, uint16_t n) {
    copy(reinterpret_cast<uint16_t>(from), at, n);
}

/// One value across a block of far memory, by DMA.
inline void far_fill(Place at, uint8_t value, uint16_t n) {
    fill(at, value, n);
}

/// A block from one far address to another, by DMA.
inline void far_copy(Place from, Place to, uint16_t n) {
    copy(from, to, n);
}

/// far_copy, leaving the destination where the source holds nought.
inline void far_copy_over(Place from, Place to, uint16_t n) {
    copy_over(from, to, n);
}

/// A block of far bytes into near memory, by DMA, so they can be worked on
/// with a pointer rather than a call apiece.
inline void far_read(Place at, uint8_t* into, uint16_t n) {
    copy(at, reinterpret_cast<uint16_t>(into), n);
}

#else

[[nodiscard]] uint8_t far_read8(Place at);
void far_write8(Place at, uint8_t value);

/// No DMA off the machine; what the two share is which bytes go where.
inline void far_write(Place at, const uint8_t* from, uint16_t n) {
    for (uint16_t i = 0; i < n; ++i)
        far_write8(at + i, from[i]);
}

inline void far_fill(Place at, uint8_t value, uint16_t n) {
    for (uint16_t i = 0; i < n; ++i)
        far_write8(at + i, value);
}

inline void far_copy(Place from, Place to, uint16_t n) {
    for (uint16_t i = 0; i < n; ++i)
        far_write8(to + i, far_read8(from + i));
}

inline void far_copy_over(Place from, Place to, uint16_t n) {
    for (uint16_t i = 0; i < n; ++i)
        if (const uint8_t byte = far_read8(from + i); byte != 0)
            far_write8(to + i, byte);
}

inline void far_read(Place at, uint8_t* into, uint16_t n) {
    for (uint16_t i = 0; i < n; ++i)
        into[i] = far_read8(at + i);
}

#endif

/// Big-endian, like everything in this data (res.cpp:56). The shift is done in
/// an unsigned type because `int` is 16 bits on the target, where a byte above
/// 127 would overflow a signed one.
[[nodiscard]] inline uint16_t far_read16(Place at) {
    return static_cast<uint16_t>(static_cast<uint16_t>(far_read8(at)) << 8 | far_read8(at + 1));
}

[[nodiscard]] inline uint32_t far_read32(Place at) {
    return static_cast<uint32_t>(far_read16(at)) << 16 | far_read16(at + 2);
}

inline void far_write16(Place at, uint16_t value) {
    far_write8(at, static_cast<uint8_t>(value >> 8));
    far_write8(at + 1, static_cast<uint8_t>(value));
}

/// Little-endian, for the one field that is: vc20_setRepeat stores its loop
/// counter inside the script this way, in a stream that is big-endian
/// everywhere else (vga.cpp:885).
[[nodiscard]] inline uint16_t far_read16_le(Place at) {
    return static_cast<uint16_t>(static_cast<uint16_t>(far_read8(at + 1)) << 8 | far_read8(at));
}

inline void far_write16_le(Place at, uint16_t value) {
    far_write8(at, static_cast<uint8_t>(value));
    far_write8(at + 1, static_cast<uint8_t>(value >> 8));
}

} // namespace agos
