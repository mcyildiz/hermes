#pragma once

#include <span>
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
  int attempts = 0;             ///< Windows tried (a cell list widens its window on failure).
  double total_displacement = 0.0;
  Dbu max_displacement = 0;
};

struct CellLegalizerOptions {
  /// Cells to legalize. Empty = every movable std cell (inside `fence` if
  /// one is given). Every other live cell, including movable std cells not
  /// in the list, is an obstacle and does not move.
  std::span<const Id> cells;
  /// Non-empty: work only inside this rectangle. Rows that are not fully
  /// inside it are not used, and nothing outside it moves. Without a fence,
  /// a cell list is legalized in a window around the cells (see below).
  Rect fence{};
  /// Cell list without a fence: the window is the cells' bounding box grown
  /// by max(window_margin_rows rows, 2 cell widths); it doubles until every
  /// cell fits or it covers the die.
  int window_margin_rows = 8;
  /// Put cells on the rows' site grid (DEF ROW STEP, Bookshelf Sitespacing).
  /// Cell widths are rounded up to whole sites.
  bool snap_to_sites = true;
  /// Abacus cell weight = 1 + width_weight * width / row height (0 = all
  /// cells weigh the same; wider cells then move as easily as small ones).
  double width_weight = 0.0;
};

/// Abacus legalization (Spindler et al., ISPD'08) of movable standard cells
/// into rows, around every macro, fixed cell and (for a partial run) every
/// cell that is not being legalized.
///
/// Rows are the design's floorplan rows (Design::rows(): subrow spans, site
/// grid) when it has them; otherwise rows of `design.row_height()` from
/// die.ylo across the die (if that is 0, the smallest movable std-cell
/// height). Obstacles split each row into free segments, and Abacus
/// clustering runs per segment. Cells taller than one row are placed first
/// at their nearest free multi-row spot and then treated as obstacles.
///
/// Region-local / incremental use: pass `opts.cells` (e.g. the cells an
/// optimization just moved) and/or `opts.fence`. Only those cells move; the
/// cost is proportional to the window, plus one pass over the cell array.
/// The design is written in one move_cells() batch (not flushed).
CellLegalizeResult abacus_legalize(Design& design, const CellLegalizerOptions& opts = {});

/// Row height the legalizer uses: design.row_height(), or the smallest
/// movable std-cell height if that is 0.
Dbu legalizer_row_height(const Design& design);

/// Returns true if the placement is legal: every cell inside the die, no two
/// cells overlap, and (when `row_height > 0`) every movable std cell sits on a
/// row: on a floorplan row span and its site grid when the design has rows,
/// else on the grid die.ylo + k * row_height. On failure `why` (if given) names the first violation.
bool check_legal(const Design& design, Dbu row_height, std::string* why = nullptr);

}  // namespace hermes
