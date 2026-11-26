#pragma once

// Building blocks of the analytic global placer (ePlace / RePlAce style):
//   * weighted-average (WA) wirelength with analytic gradient
//   * electrostatic density: charges on a bin grid, Poisson's equation solved
//     spectrally with DCT/DST, electric field used as the density gradient.
//
// MixedSizePlacer uses these internally; they are exposed for testing and for
// other placement passes that need a smooth wirelength or density model.

#include <complex>
#include <utility>
#include <vector>

#include "hermes/core/types.hpp"

namespace hermes::gp {

/// Weighted-average wirelength of one coordinate set (x or y of a net's pins):
///   WA = sum x e^{x/g} / sum e^{x/g}  -  sum x e^{-x/g} / sum e^{-x/g}
/// WA <= max - min and approaches it as gamma -> 0. If `grad` is non-null,
/// d WA / d x_i is *added* to grad[i].
double wa_wirelength_1d(const double* x, int n, double gamma, double* grad);

/// Rounds up to the next power of two (minimum 1).
int next_pow2(int n);

/// Electrostatic density system on an nx * ny bin grid over `region`
/// (nx, ny must be powers of two). Objects are charges whose amount is their
/// (scaled) area; the density is the charge per bin divided by bin area.
/// solve() computes the potential psi and field xi = -grad psi from
///   laplacian(psi) = -(rho - mean(rho))   with Neumann boundaries,
/// using the cosine expansion of rho (the DC term is dropped).
class ElectrostaticSystem {
 public:
  ElectrostaticSystem(const Rect& region, int nx, int ny);

  int nx() const { return nx_; }
  int ny() const { return ny_; }
  double bin_w() const { return bw_; }
  double bin_h() const { return bh_; }
  double bin_area() const { return bw_ * bh_; }
  const Rect& region() const { return region_; }

  /// Resets the charge map to `base` (or to zero when `base` is empty).
  void reset(const std::vector<double>& base = {});

  /// Adds `scale` * overlap area of the rectangle with each bin.
  void add_rect(double xlo, double ylo, double xhi, double yhi, double scale);
  /// Same, into an external per-bin charge vector of size nx*ny.
  void add_rect_to(std::vector<double>& charge, double xlo, double ylo,
                   double xhi, double yhi, double scale) const;

  /// Solves for the field (and the potential when `with_potential`).
  void solve(bool with_potential = false);

  /// Gradient of the electrostatic energy w.r.t. a rectangle's center, for a
  /// rectangle that deposited `scale` * overlap into the grid:
  ///   dE/dc = -sum_b scale * overlap_b * xi_b
  void rect_gradient(double xlo, double ylo, double xhi, double yhi,
                     double scale, double& gx, double& gy) const;

  /// Energy 0.5 * sum_b Q_b psi_b (needs solve(true)).
  double energy() const;

  /// Per-bin charge (area units), field components, potential.
  const std::vector<double>& charge() const { return charge_; }
  std::vector<double>& charge() { return charge_; }
  double field_x(int ix, int iy) const { return ex_[idx(ix, iy)]; }
  double field_y(int ix, int iy) const { return ey_[idx(ix, iy)]; }
  double potential(int ix, int iy) const { return psi_[idx(ix, iy)]; }
  const std::vector<double>& field_x() const { return ex_; }
  const std::vector<double>& field_y() const { return ey_; }

  // --- Building blocks of the parallel kernels -----------------------------
  /// Grid rows a rectangle overlaps: [first, last], first > last if none.
  std::pair<int, int> rect_rows(double xlo, double ylo, double xhi, double yhi) const;
  /// add_rect_to() restricted to grid rows [row_lo, row_hi]. Splitting a
  /// rectangle by rows this way adds exactly the same values per bin.
  void add_rect_rows(double* charge, double xlo, double ylo, double xhi, double yhi,
                     double scale, int row_lo, int row_hi) const;
  /// Solves for the field from an external charge vector (thread-parallel).
  void solve_from(const std::vector<double>& charge, bool with_potential);

 private:
  struct Scratch;
  std::size_t idx(int ix, int iy) const {
    return static_cast<std::size_t>(iy) * nx_ + ix;
  }
  // Visits the bins overlapped by a rectangle in rows [r0, r1]:
  // f(bin index, overlap area).
  template <class F>
  void for_each_overlap(double xlo, double ylo, double xhi, double yhi, int r0, int r1,
                        F&& f) const;

  // Tables for 1-D transforms of one length n (power of two), computed once.
  struct Tables {
    std::size_t n = 0;
    std::vector<std::complex<double>> twiddle;  // 2n-point FFT
    std::vector<std::complex<double>> rot;      // e^{i pi u / 2n}
  };
  static void make_tables(Tables& t, std::size_t n);
  // 1-D transforms of length n via a 2n-point complex FFT (const: scratch is
  // per thread, so rows/columns run in parallel).
  static void dct(double* v, const Tables& t, Scratch& s);      // X_u = sum_x f_x cos(pi u (x+.5)/n)
  static void cos_sum(double* v, const Tables& t, Scratch& s);  // g_x = sum_u c_u cos(pi u (x+.5)/n)
  static void sin_sum(double* v, const Tables& t, Scratch& s);  // h_x = sum_u c_u sin(pi u (x+.5)/n)
  static void fft(std::complex<double>* a, const Tables& t);
  using Transform = void (*)(double*, const Tables&, Scratch&);
  // Applies a 1-D transform along x (rows) or y (columns) of a grid.
  void along_x(std::vector<double>& grid, Transform f) const;
  void along_y(std::vector<double>& grid, Transform f) const;

  Rect region_;
  int nx_, ny_;
  double x0_, y0_, bw_, bh_;
  std::vector<double> charge_, coef_, ex_, ey_, psi_;
  std::vector<double> wu_, wv_;  // angular frequencies pi*u/W, pi*v/H
  Tables tx_, ty_;
};

}  // namespace hermes::gp
