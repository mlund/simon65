// A ring of what wrote each arena row, for reading back later.
//
// A row goes wrong while a zone is live and nothing is watching -- it ends up
// describing an image it does not hold -- and by the time a check notices,
// whatever did it is several rooms in the past. Sampling cannot catch it --
// a monitor read is a second and a decode is a frame -- so every write records
// itself here instead and the ring is read whole afterwards.
//
// It sits at the top of the diagnostics region, above the sprite census.

#pragma once

#include "atticmap.hpp"
#include "far.hpp"

#include <stdint.h>

namespace trace {

/// zone, image and page (two bytes each, little-endian), cells, rows, spare.
/// Eight, so a record's place in the ring is a shift rather than a multiply.
inline constexpr uint8_t RECORD = 8;

/// A power of two, so the wrap is a mask rather than a division: a modulo by
/// anything else is a call on this target. Thirty-two of them is what fits
/// above the sprite census in the same region.
inline constexpr uint16_t RECORDS = 32;
inline constexpr uint16_t COUNT_BYTES = 2;
inline constexpr uint16_t RING_BYTES = COUNT_BYTES + RECORDS * RECORD;

inline constexpr agos::Place AT = atticmap::DIAGNOSTICS + atticmap::DIAGNOSTICS_BYTES - RING_BYTES;

// census.hpp is what checks the two do not overlap: it knows its own size,
// and it is compiled for the target only -- a sprite is fifteen bytes there
// and sixteen on the host, so this header must not drag it into a host build.

#ifdef __mos__
/// One entry as the arena records it, taking the row the caller has already
/// packed: the ring's record and the index's row are the same eight bytes, and
/// packing them twice cost the fixed region -- which is the region with least
/// to spare -- a second copy of the shifts and stores.
///
/// Out of line and in the fixed region on purpose: the decoder that calls it
/// is in a bank with little to spare.
void log_entry(const uint8_t (&row)[RECORD]);
#else
inline void log_entry(const uint8_t (&)[RECORD]) {
}
#endif

} // namespace trace
