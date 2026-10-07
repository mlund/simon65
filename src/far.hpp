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
//
// Blocks move through DMAgic rather than the CPU, target only.
//
// A job of no bytes is refused here rather than passed on: DMAgic reads a
// count of nought as 65,536 (gs4510.vhdl:5956-5957), so a length that happens
// to be zero moves 64 KB over whatever follows the destination and reports
// nothing. Where the length is a constant the compiler drops the test, so only
// a caller whose length can really be zero pays for it.
//
// A job does not cross a megabyte either. Each end's megabyte is an option of
// its own ($81 and $85) and the address wraps inside it unless option $01 says
// otherwise, which the SDK's job never sets. A transfer that straddles the
// line silently corrupts, appearing as noise: one decode's pixels garbled from
// the boundary point, drawing an invalid rectangle (mega65-common/include/dma.hpp:49).

#pragma once

#include <stdint.h>

#ifdef __mos__
#include <dma.hpp>
#include <mega65.h>

/// Interrupts held off for the guard's lifetime: around anything the raster
/// interrupt also touches, such as the DMA trigger registers and the speech
/// refill's counters (sound.S). php/plp rather than sei/cli, because some of
/// it runs at startup before the vector is set, with interrupts already off.
/// leaf: the blocks call nothing, so callers keep their static stack frames.
struct Masked {
    [[gnu::always_inline]] Masked() {
        __attribute__((leaf)) asm volatile("php\n\tsei" :: : "memory");
    }
    [[gnu::always_inline]] ~Masked() {
        __attribute__((leaf)) asm volatile("plp" :: : "memory");
    }
    Masked(const Masked&) = delete;
    Masked& operator=(const Masked&) = delete;
};

/// How much of @p n lies before the end of @p at's own megabyte.
///
/// A length is 16 bits, so a transfer can only straddle from the megabyte's
/// last 64 KB -- which makes the common answer one nibble test, and the rest
/// 16-bit arithmetic rather than 32.
[[nodiscard]] inline uint16_t within_megabyte(uint32_t at, uint16_t n) {
    if (((at >> 16) & 0x0Fu) != 0x0Fu)
        return n;
    const uint16_t room = static_cast<uint16_t>(0u - static_cast<uint16_t>(at));
    return room != 0 && room < n ? room : n; // nought means a whole 64 KB
}

namespace move_detail {

/// The one job every move runs, rewritten in place.
///
/// The SDK's trigger builds a job on the soft stack at each call and wraps it
/// in inline assembly without `leaf`, so every function reaching a move kept
/// its frame on the soft stack too. The one interrupt that moves bytes, the
/// speech refill, has a job of its own (sound.S), so one job serves here.
/// Near memory, where its 16-bit address is its physical one: the trigger
/// writes list bank and megabyte nought ($D702 clears $D704, iomap.txt).
/// Built by the SDK's maker so the constant options are in the image.
[[gnu::section(".data")]] inline mega65::dma::CommonDMAJob job =
    mega65::dma::make_dma_fill(0, 0, 1);

/// Option bytes the move rewrites: each end's megabyte.
inline constexpr uint8_t SOURCE_MEGABYTE = 2, DEST_MEGABYTE = 4;

inline constexpr uint8_t TRANSPARENCY_ON = 0x07, TRANSPARENT_VALUE = 0x86;

/// The same job with a byte of nought left unwritten: option $07 turns the
/// $86 value on (gs4510.vhdl:5888-5889; the comments above them swap the
/// two), and the end of a job turns it off again (:6306-6312).
using ClearJob = mega65::dma::DMAJob<10, decltype(job.dmalist)>;
[[nodiscard]] constexpr ClearJob make_clear_job() {
    ClearJob out{};
    const auto common = mega65::dma::make_dma_fill(0, 0, 1);
    for (uint8_t i = 0; i < 7; ++i)
        out.options[i] = common.options[i];
    out.options[7] = TRANSPARENCY_ON;
    out.options[8] = TRANSPARENT_VALUE;
    out.options[9] = 0; // the value: nought
    out.dmalist = common.dmalist;
    return out;
}
[[gnu::section(".data")]] inline ClearJob clear_job = make_clear_job();

/// Run @p command from @p from to @p to, @p n bytes within one megabyte at
/// each end, on @p job; for a fill @p from is the value.
template <typename Job>
inline void run_on(Job& job, uint8_t command, uint32_t from, uint32_t to, uint16_t n) {
    job.options[SOURCE_MEGABYTE] = static_cast<uint8_t>(from >> 20);
    job.options[DEST_MEGABYTE] = static_cast<uint8_t>(to >> 20);
    job.dmalist.command = command;
    job.dmalist.count = n;
    job.dmalist.source_addr = static_cast<uint16_t>(from);
    job.dmalist.source_bank = static_cast<uint8_t>((from >> 16) & 0x0F);
    job.dmalist.dest_addr = static_cast<uint16_t>(to);
    job.dmalist.dest_bank = static_cast<uint8_t>((to >> 16) & 0x0F);
    const auto list = reinterpret_cast<uint16_t>(&job);
    // The speech refill triggers a job of its own from the raster interrupt
    // (sound.S): landing between these writes would leave this one running
    // that one's list.
    const Masked masked;
    DMA.enable_f018b = 1;
    DMA.addr_bank = 0;
    DMA.addr_msb = static_cast<uint8_t>(list >> 8);
    DMA.trigger_enhanced = static_cast<uint8_t>(list);
}

inline void run(uint8_t command, uint32_t from, uint32_t to, uint16_t n) {
    run_on(job, command, from, to, n);
}

/// A copy on @p job, a megabyte at a time at either end.
template <typename Job> inline void copy_on(Job& job, uint32_t from, uint32_t to, uint16_t n) {
    while (n != 0) {
        const uint16_t step = within_megabyte(from, within_megabyte(to, n));
        run_on(job, DMA_COPY_CMD, from, to, step);
        from += step;
        to += step;
        n = static_cast<uint16_t>(n - step);
    }
}

} // namespace move_detail

inline void copy(uint32_t from, uint32_t to, uint16_t n) {
    move_detail::copy_on(move_detail::job, from, to, n);
}

/// copy, leaving the destination where the source holds nought: how a
/// transparent picture goes over another in one job.
inline void copy_over(uint32_t from, uint32_t to, uint16_t n) {
    move_detail::copy_on(move_detail::clear_job, from, to, n);
}

inline void fill(uint32_t at, uint8_t value, uint16_t n) {
    while (n != 0) {
        const uint16_t step = within_megabyte(at, n);
        move_detail::run(DMA_FILL_CMD, value, at, step);
        at += step;
        n = static_cast<uint16_t>(n - step);
    }
}
#endif // __mos__

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
