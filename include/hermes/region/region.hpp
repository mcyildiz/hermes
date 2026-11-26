#pragma once

// Region-based work partitioning.
//
// The die is tiled into regions. Region-local work (incremental place,
// optimize, legalize, reroute) runs on a self-contained RegionProblem cut
// out of the design: the movable cells inside the region, every net that
// touches them, and the pins of those nets outside the region frozen as
// fixed terminals. A solver turns the problem into a RegionSolution (new
// cell locations), and apply() writes it back on the design's writer
// thread. Nothing outside the region can change: a solution may move only
// the problem's own cells, and apply() rejects anything else.
//
// Because problems are self-contained flat arrays, they are also the unit
// of distribution: serialize() a problem, solve it on another thread,
// process or machine, and serialize the solution back (RegionExecutor).
//
//   region::Partition part(design, region::RegionGrid(design.die(), 8, 8));
//   region::LocalExecutor exec;                       // thread pool
//   auto stats = region::run_regions(design, part, part.all_regions(),
//                                    region::solver("median"), exec);
//
// Concurrency model: all problems of one run are cut from the same design
// version, so regions solved side by side see each other's cells at their
// old positions (Jacobi across regions). For Gauss-Seidel style updates,
// run the regions of one color at a time (RegionGrid::color: no two
// regions of the same color touch).

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "hermes/db/bin_grid.hpp"
#include "hermes/db/design.hpp"
#include "hermes/par/job.hpp"

namespace hermes::region {

/// nx * ny rectangular regions over an area (usually the die).
class RegionGrid {
 public:
  RegionGrid() = default;
  RegionGrid(const Rect& area, int nx, int ny) : grid_(area, nx, ny) {}

  int nx() const { return grid_.nx(); }
  int ny() const { return grid_.ny(); }
  int size() const { return grid_.nx() * grid_.ny(); }
  Rect rect(int r) const { return grid_.bin_rect(r % nx(), r / nx()); }
  int region_of(Point p) const {
    const auto [ix, iy] = grid_.bin_of(p);
    return iy * nx() + ix;
  }
  /// 4-coloring: regions of the same color never share an edge or corner.
  int color(int r) const { return (r % nx()) % 2 + 2 * ((r / nx()) % 2); }
  static constexpr int kColors = 4;
  const BinGrid& bins() const { return grid_; }

 private:
  BinGrid grid_;
};

/// Assignment of live cells to regions (by cell center) and of nets to
/// "internal" (all pins in one region) or "cut".
class Partition {
 public:
  Partition() = default;
  Partition(const Design& design, const RegionGrid& grid);

  const RegionGrid& grid() const { return grid_; }
  int num_regions() const { return grid_.size(); }
  /// Region of a cell (-1 for removed cells).
  int region_of_cell(Id cell) const { return cell_region_[static_cast<std::size_t>(cell)]; }
  /// Live cells in region r (movable and fixed), ascending ids.
  std::span<const Id> cells(int r) const;
  /// True if the net has pins in more than one region (or I/O pins).
  bool is_cut(Id net) const { return net_cut_[static_cast<std::size_t>(net)] != 0; }
  std::size_t num_cut_nets() const { return num_cut_; }
  /// All cell ids grouped by region (regions in a locality-preserving
  /// serpentine order). Pass it to SnapshotPublisher::set_cell_order() so
  /// snapshots copy only the chunks of the regions that changed.
  std::vector<Id> spatial_order() const;
  std::vector<int> all_regions() const;
  std::vector<int> regions_of_color(int color) const;

 private:
  RegionGrid grid_;
  std::vector<int> cell_region_;
  std::vector<int> start_;   // CSR region -> cells
  std::vector<Id> cells_;
  std::vector<std::uint8_t> net_cut_;
  std::size_t num_cut_ = 0;
};

/// Self-contained subproblem of one region, in flat arrays (SoA).
struct RegionProblem {
  int region = -1;
  Rect bounds;                     ///< Cells must stay inside (lower-left + size).
  std::uint64_t design_version = 0;
  std::string solver;              ///< Solver name (for remote execution).
  // Movable cells (local index = position in these arrays).
  std::vector<Id> cell_id;         ///< Design ids.
  std::vector<Point> cell_loc;
  std::vector<Dbu> cell_w, cell_h;
  // Nets touching the movable cells, CSR over pins.
  std::vector<Id> net_id;
  std::vector<double> net_weight;
  std::vector<int> net_start;      ///< size nets + 1
  std::vector<int> pin_cell;       ///< local cell index, or -1 for a fixed terminal
  std::vector<Point> pin_pos;      ///< offset from the cell (movable) or absolute (fixed)

  std::size_t num_cells() const { return cell_id.size(); }
  std::size_t num_nets() const { return net_id.size(); }
  /// HPWL of the problem's nets with cells at `loc` (default: cell_loc).
  double hpwl(std::span<const Point> loc = {}) const;
};

/// New locations for (a subset of) a problem's cells.
struct RegionSolution {
  int region = -1;
  std::uint64_t design_version = 0;
  std::vector<Id> cell_id;
  std::vector<Point> cell_loc;
  double hpwl_before = 0.0, hpwl_after = 0.0;  ///< Problem-local, for reporting.
};

/// Cuts region r's problem out of the design (read-only; thread safe).
/// Fixed cells inside the region become terminals too.
RegionProblem extract(const Design& design, const Partition& part, int region,
                      std::string_view solver = {});

/// Writes a solution back (writer thread). Throws std::invalid_argument if
/// it moves a cell that is not one of the problem's movable cells, or if
/// the design changed since the problem was extracted; cells are clamped to
/// the region bounds. Does not flush.
void apply(Design& design, const RegionProblem& problem, const RegionSolution& sol);

// --- Serialization (versioned little-endian binary) ---------------------------
std::vector<std::uint8_t> serialize(const RegionProblem& p);
std::vector<std::uint8_t> serialize(const RegionSolution& s);
RegionProblem deserialize_problem(std::span<const std::uint8_t> bytes);
RegionSolution deserialize_solution(std::span<const std::uint8_t> bytes);

// --- Solvers -----------------------------------------------------------------
/// A region solver: pure function of the problem (no Design access), so it
/// can run on any thread or machine.
using RegionSolver = std::function<RegionSolution(const RegionProblem&)>;

/// Solvers by name, so a remote worker can run the solver a problem names.
void register_solver(std::string name, RegionSolver solver);
/// Throws std::invalid_argument for an unknown name.
RegionSolver solver(std::string_view name);
std::vector<std::string> solver_names();

/// Built-in demonstration solver ("median"): moves each cell toward the
/// median of the pins it connects to when that lowers the problem's HPWL,
/// clamped to the region. It ignores overlaps (no legality): it exists to
/// exercise the region machinery, not as a placement algorithm.
RegionSolution solve_median(const RegionProblem& p);

// --- Executors -----------------------------------------------------------------
/// Runs a batch of problems somewhere and returns their solutions (same
/// order).
class RegionExecutor {
 public:
  virtual ~RegionExecutor() = default;
  virtual std::vector<RegionSolution> run(std::span<const RegionProblem> problems,
                                          const RegionSolver& solver, RunControl* ctl) = 0;
};

/// On the local thread pool, one task per region.
class LocalExecutor final : public RegionExecutor {
 public:
  std::vector<RegionSolution> run(std::span<const RegionProblem> problems,
                                  const RegionSolver& solver, RunControl* ctl) override;
};

/// Byte transport to one remote worker: send a request, get the response.
/// The worker deserializes a problem, runs the named solver, and returns
/// the serialized solution (see serve()).
class Transport {
 public:
  virtual ~Transport() = default;
  virtual std::vector<std::uint8_t> call(std::span<const std::uint8_t> request) = 0;
};

/// Worker side of the protocol: request bytes -> response bytes.
std::vector<std::uint8_t> serve(std::span<const std::uint8_t> request);

/// A "remote" worker in this process (serialize -> serve -> deserialize).
/// Exercises the full wire path; a socket/MPI/gRPC transport replaces it
/// for real machines.
class LoopbackTransport final : public Transport {
 public:
  std::vector<std::uint8_t> call(std::span<const std::uint8_t> request) override {
    ++calls_;
    return serve(request);
  }
  std::size_t calls() const { return calls_; }

 private:
  std::size_t calls_ = 0;
};

/// Sends problems to workers over transports (round-robin, transports in
/// parallel, one request at a time per transport). The solver is chosen by
/// name on the worker: problems must carry a registered solver name.
class RemoteExecutor final : public RegionExecutor {
 public:
  explicit RemoteExecutor(std::vector<std::shared_ptr<Transport>> workers)
      : workers_(std::move(workers)) {}
  std::vector<RegionSolution> run(std::span<const RegionProblem> problems,
                                  const RegionSolver& solver, RunControl* ctl) override;
  std::size_t bytes_sent() const { return sent_; }
  std::size_t bytes_received() const { return received_; }

 private:
  std::vector<std::shared_ptr<Transport>> workers_;
  std::size_t sent_ = 0, received_ = 0;
};

struct RegionRunStats {
  int regions = 0;
  std::size_t cells_moved = 0;
  double hpwl_before = 0.0, hpwl_after = 0.0;  ///< Sum over the problems.
  double extract_s = 0.0, solve_s = 0.0, apply_s = 0.0;
};

/// Extracts the regions' problems (in parallel), solves them with `exec`,
/// applies the solutions and flushes once. `solver_name` is recorded in the
/// problems (needed by RemoteExecutor).
RegionRunStats run_regions(Design& design, const Partition& part, std::span<const int> regions,
                           std::string_view solver_name, RegionExecutor& exec,
                           RunControl* ctl = nullptr);

}  // namespace hermes::region
