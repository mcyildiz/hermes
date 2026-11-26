#pragma once

// Bookshelf reader and .pl writer (ISPD 2005/2006 placement contests, plus
// the .route file of the ISPD 2011 / DAC 2012 routability contests).
//
//   .aux    RowBasedPlacement : x.nodes x.nets x.wts x.pl x.scl [x.shapes] [x.route]
//   .nodes  name width height [terminal | terminal_NI]
//   .nets   NetDegree : k [name]  then k lines  node I|O|B [: dx dy]
//           (pin offsets from the node's center, unrotated)
//   .pl     name x y [: orient] [/FIXED | /FIXED_NI]   (lower-left corner)
//   .scl    CoreRow Horizontal ... Coordinate, Height, Sitewidth,
//           Sitespacing, Siteorient, Sitesymmetry, SubrowOrigin, NumSites ... End
//   .wts    name weight    (net weights; node weights are ignored)
//   .route  Grid, capacities, tile size, NiTerminals, BlockageNodes, ...
//
// Conventions in the resulting Design:
//   * Coordinates are Bookshelf units times `scale` (option bookshelf.scale,
//     default 1), rounded to integers. dbu_per_micron() is set to `scale`,
//     i.e. one Bookshelf unit is treated as one micron when writing DEF.
//   * terminal / terminal_NI nodes and /FIXED nodes are fixed cells. Nodes
//     taller than the most common row height are macros (is_macro).
//   * Fixed cells are `placed`. Movable cells are `placed` only when the
//     .pl says so: a .pl written by hermes carries a "# hermes: placed"
//     line; other .pl files count as unplaced unless bookshelf.pl_placed=1.
//   * Pins are named p0, p1, ... in the order they appear on their node.
//   * The die is the bounding box of the rows (of all nodes if no .scl).
//
// Parsing runs in parallel (files split at line / NetDegree boundaries;
// names resolved with parallel lookups), and the Design is filled in bulk.
//
// This reader follows the published ISPD formats. Extend `BookshelfReader`
// (bookshelf.cpp) for new sections or files: each file type has its own
// parse function, and unknown .aux entries are skipped with a warning.

#include <string>
#include <vector>

#include "hermes/db/design.hpp"
#include "hermes/io/common.hpp"

namespace hermes::io {

struct BookshelfFiles {
  std::string nodes, nets, pl, scl, wts, route, shapes;
};

struct BookshelfReport {
  std::size_t num_nodes = 0, num_terminals = 0, num_macros = 0, num_fixed = 0;
  std::size_t num_nets = 0, num_pins = 0, num_rows = 0;
  std::size_t num_weighted_nets = 0;
  bool route = false;
  bool pl_placed = false;
  std::vector<std::string> warnings;  ///< First few.
  std::size_t num_warnings = 0;
};

/// Files named by an .aux file (paths resolved relative to it).
BookshelfFiles parse_aux(const std::string& aux_path);

/// Reads a Bookshelf design. Options: bookshelf.scale (default 1),
/// bookshelf.pl_placed (default: auto from the .pl marker).
Design read_bookshelf(const BookshelfFiles& files, const Options& opts = {},
                      BookshelfReport* report = nullptr);
Design read_bookshelf_aux(const std::string& aux_path, const Options& opts = {},
                          BookshelfReport* report = nullptr);

/// Writes a .pl of the current cell locations divided by `scale` (default
/// 1: DBU as they are; pass the reader's bookshelf.scale to get the
/// original Bookshelf units back). With `placed_marker`,
/// a "# hermes: placed" line tells the reader that movable cells are placed.
void write_bookshelf_pl(const Design& d, const std::string& path, double scale = 0.0,
                        bool placed_marker = true);

/// Writes a complete Bookshelf design: <dir>/<base>.aux, .nodes, .nets,
/// .pl, .scl, .wts (base defaults to the design name). Sizes, offsets and
/// coordinates are divided by `scale` (default 1: DBU as they are).
/// Fixed cells become terminals. Pin offsets are written relative to the
/// node center in its unrotated frame, so a read gives back the same
/// geometry. Returns the .aux path.
std::string write_bookshelf(const Design& d, const std::string& dir, std::string base = {},
                            double scale = 0.0);

}  // namespace hermes::io
