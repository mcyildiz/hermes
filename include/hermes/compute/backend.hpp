#pragma once

// Compute backends: where the data-parallel placement kernels run.
//
// Engines describe their data as flat structure-of-arrays views (spans of
// ints and doubles, CSR for variable-length lists) and call kernels through
// a Backend. The CPU backend runs them on the hermes::par thread pool. A GPU
// backend implements the same interface; because every view is plain
// arrays with no pointers into the Design, the data moves to a device as is.
//
//   auto& be = hermes::compute::backend();        // HERMES_BACKEND=cpu (default)
//   be.wirelength_gradient(netlist, pos, gamma, pin_scratch, grad);
//
// Memory model: views are host spans. On Apple Silicon (Metal) CPU and GPU
// share memory, so a Metal backend can wrap these arrays without copies
// (MTLBuffer over page-aligned host memory). A CUDA backend keeps device
// mirrors and uploads only what changed per call (positions), or allocates
// with managed memory. Kernels never call back into the Design.
//
// Results: every CPU kernel is deterministic (independent of thread count).
// A GPU backend should keep that property where it can (sorted/segmented
// reductions instead of float atomics), or report is_deterministic() false.

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "hermes/core/types.hpp"

namespace hermes::compute {

/// Pin-level netlist of one placement problem, in CSR form. Objects are the
/// movable things being placed (cells, fillers); positions are object
/// centers stored as [x_0 .. x_{N-1}, y_0 .. y_{N-1}].
struct NetlistView {
  int num_objects = 0;
  std::span<const int> net_start;     ///< size nets + 1: pins of net n are [net_start[n], net_start[n+1])
  std::span<const int> pin_obj;       ///< per pin: object index, or -1 for a fixed pin
  std::span<const double> pin_dx;     ///< offset from the object center (absolute if fixed)
  std::span<const double> pin_dy;
  std::span<const double> net_weight; ///< per net
  std::span<const int> obj_start;     ///< size objects + 1: pins of object k (CSR)
  std::span<const int> obj_pins;      ///< pin indices, ascending within each object
  int max_degree = 0;

  std::size_t num_nets() const { return net_start.empty() ? 0 : net_start.size() - 1; }
  std::size_t num_pins() const { return pin_obj.size(); }
};

/// Density footprints: object k covers a charge_w x charge_h rectangle
/// around its center (kept inside `die`) with charge density ratio[k].
struct DensityView {
  std::span<const double> charge_w;
  std::span<const double> charge_h;
  std::span<const double> ratio;
  Rect die;
};

/// Grid-resident density state of one problem (charge map, field). Created
/// by Backend::make_density_solver; a GPU backend keeps the grid on device.
class DensitySolver {
 public:
  virtual ~DensitySolver() = default;
  virtual int nx() const = 0;
  virtual int ny() const = 0;
  virtual double bin_w() const = 0;
  virtual double bin_h() const = 0;
  double bin_area() const { return bin_w() * bin_h(); }

  /// charge = base + footprints of objects [begin, end) at `pos`.
  virtual void deposit(const DensityView& objs, std::span<const double> pos, int begin, int end,
                       std::span<const double> base, std::span<double> charge) = 0;
  /// Field from a charge map (Poisson's equation, spectral).
  virtual void solve(std::span<const double> charge) = 0;
  /// grad[k], grad[N + k] = gradient of the energy w.r.t. object k's center
  /// (overwrites the first `count` objects).
  virtual void gradient(const DensityView& objs, std::span<const double> pos, int count,
                        std::span<double> grad) = 0;
  /// sum_b max(0, charge_b - cap).
  virtual double overflow(std::span<const double> charge, double cap) = 0;
  /// Bin index range a footprint touches (helper for bookkeeping/tests).
  virtual void footprint(const DensityView& objs, std::span<const double> pos, int k,
                         double r[4]) const = 0;
};

class Backend {
 public:
  virtual ~Backend() = default;
  virtual std::string_view name() const = 0;
  virtual bool is_gpu() const { return false; }
  virtual bool is_deterministic() const { return true; }

  // --- Wirelength -----------------------------------------------------------
  /// Sum over nets of the half-perimeter of the pin bounding box.
  virtual double hpwl(const NetlistView& nl, std::span<const double> pos) = 0;
  /// grad = d/dpos of sum_n w_n * WA_gamma(n) (overwritten, size 2N).
  /// `pin_scratch` holds 2 * num_pins doubles (per-pin partial gradients;
  /// the second pass gathers them per object, so no two threads ever add
  /// into the same element).
  virtual void wirelength_gradient(const NetlistView& nl, std::span<const double> pos,
                                   double gamma, std::span<double> pin_scratch,
                                   std::span<double> grad) = 0;

  // --- Density --------------------------------------------------------------
  virtual std::unique_ptr<DensitySolver> make_density_solver(const Rect& region, int nx,
                                                             int ny) = 0;

  // --- Vector kernels of the Nesterov solver ----------------------------------
  /// u_new = clamp(v - alpha * g);  v_new = clamp(u_new + coef * (u_new - u)),
  /// clamp(x)_i = min(max(x_i, lo_i), hi_i).
  virtual void nesterov_predict(std::span<const double> v, std::span<const double> g,
                                std::span<const double> u, double alpha, double coef,
                                std::span<const double> lo, std::span<const double> hi,
                                std::span<double> u_new, std::span<double> v_new) = 0;
  /// || a - b ||_2
  virtual double distance(std::span<const double> a, std::span<const double> b) = 0;
  /// g_i = (gw_i + lambda * gd_i) / max(1, pw_k + lambda * q_k) with k = i mod N
  /// (preconditioned gradient; gw/gd/g have size 2N, pw/q size N).
  virtual void precondition(std::span<const double> gw, std::span<const double> gd,
                            std::span<const double> pw, std::span<const double> q,
                            double lambda, std::span<double> g) = 0;
};

/// The backend engines use by default: HERMES_BACKEND (environment) or the
/// last set_backend(), else "cpu".
Backend& backend();
/// Selects the default backend by name; throws std::invalid_argument if no
/// such backend is registered (or it cannot run on this machine).
void set_backend(std::string_view name);
/// Registered backend names ("cpu", plus GPU backends compiled in).
std::vector<std::string> backends();
/// Adds a backend (e.g. from a plugin). The factory runs on first use.
void register_backend(std::string name, std::function<std::unique_ptr<Backend>()> factory);

/// The CPU backend (always available).
Backend& cpu_backend();

}  // namespace hermes::compute
