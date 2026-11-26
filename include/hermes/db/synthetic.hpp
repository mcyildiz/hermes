#pragma once

// Synthetic mixed-size designs for benchmarks, demos and GUI tests:
// standard cells on rows, fixed macros, I/O pins, nets of 2-6 pins that
// connect nearby cell ids (so an ordered placement has short wires).

#include <cstdint>

#include "hermes/db/design.hpp"

namespace hermes {

struct SyntheticOptions {
  int cells = 20000;
  std::uint32_t seed = 7;
  /// Placement of the standard cells:
  ///  * Ordered: cell ids fill the rows in snake order around the macros
  ///    (looks placed, short wires),
  ///  * Random: uniform random sites (overlapping),
  ///  * Unplaced: all at the die center with placed = false (for a placer).
  enum class Placement : std::uint8_t { Ordered, Random, Unplaced } placement = Placement::Ordered;
  double utilization = 0.7;
};

/// 2000 DBU per micron, 0.1 um sites, 1 um rows; masters CELLW1..4 and RAM.
Design make_synthetic_design(const SyntheticOptions& o = {});

}  // namespace hermes
