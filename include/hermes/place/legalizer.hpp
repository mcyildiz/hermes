#pragma once

#include <string>

#include "hermes/db/design.hpp"

namespace hermes {

// ---------------------------------------------------------------------------
// Macro legalization
// ---------------------------------------------------------------------------

struct MacroLegalizerOptions {
  /// Minimum spacing kept between any two macros (and macro vs. fixed cell).
  Dbu halo = 0;
  /// Snap macro y to the standard-cell row grid so row segments stay whole.
  bool snap_to_rows = true;
};

struct MacroLegalizeResult {
  int moved = 0;                ///< Movable macros processed.
  int unresolved = 0;           ///< Macros that could not be placed overlap-free.
  double total_displacement = 0.0;  ///< Sum of Manhattan displacement (DBU).
  Dbu max_displacement = 0;
};

/// Removes overlap between movable macros, fixed macros and fixed cells.
/// Macros are processed largest-first; each goes to the overlap-free position
/// with the least Manhattan displacement from its global-placement location.
/// Candidate positions are the macro's own location, the die edges and the
/// edges of every already-placed obstacle (the "corner" positions on which an
/// optimal greedy placement always exists). Does not change `fixed`.
MacroLegalizeResult legalize_macros(Design& design,
                                    const MacroLegalizerOptions& opts = {});

// ---------------------------------------------------------------------------
// Standard-cell legalization (Abacus)
// ---------------------------------------------------------------------------

struct CellLegalizeResult {
  int legalized = 0;            ///< Movable std cells placed in a row.
  int failed = 0;               ///< Cells that did not fit anywhere (left clamped).
  int multi_row = 0;            ///< Cells taller than a row (placed first, greedily).
  double total_displacement = 0.0;
  Dbu max_displacement = 0;
};

/// Abacus legalization (Spindler et al., ISPD'08) of movable standard cells
/// into rows, around every macro and fixed cell. Rows start at die.ylo with
/// pitch `design.row_height()`; if that is 0, the smallest movable std-cell
/// height is used. Obstacles split each row into free segments, and Abacus
/// clustering runs per segment. Cells taller than one row are placed first
/// at their nearest free multi-row spot and then treated as obstacles.
CellLegalizeResult abacus_legalize(Design& design);

/// Returns true if the placement is legal: every cell inside the die, no two
/// cells overlap, and (when `row_height > 0`) every movable std cell sits on a
/// row. On failure `why` (if given) names the first violation.
bool check_legal(const Design& design, Dbu row_height, std::string* why = nullptr);

}  // namespace hermes
