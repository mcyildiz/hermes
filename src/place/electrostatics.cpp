// Analytic building blocks (hermes::gp): WA wirelength and the spectral
// electrostatic density solver. The solver's transforms run rows/columns in
// parallel on the hermes::par pool; per-bin results do not depend on the
// thread count.

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "hermes/par/parallel.hpp"
#include "hermes/place/analytic.hpp"

namespace hermes::gp {

namespace {
constexpr double kPi = 3.14159265358979323846;
}  // namespace

struct ElectrostaticSystem::Scratch {
  std::vector<std::complex<double>> buf;
  std::vector<double> line;
};

double wa_wirelength_1d(const double* x, int n, double gamma, double* grad) {
  if (n < 2) return 0.0;
  double xmax = x[0], xmin = x[0];
  for (int i = 1; i < n; ++i) {
    xmax = std::max(xmax, x[i]);
    xmin = std::min(xmin, x[i]);
  }
  // The exponentials are computed once and reused by the gradient.
  constexpr int kStack = 64;
  double stack[2 * kStack];
  std::vector<double> heap;
  double* ep = stack;
  if (n > kStack) {
    heap.resize(2 * static_cast<std::size_t>(n));
    ep = heap.data();
  }
  double* en = ep + (n > kStack ? n : kStack);
  double sp = 0, tp = 0, sn = 0, tn = 0;
  for (int i = 0; i < n; ++i) {
    ep[i] = std::exp((x[i] - xmax) / gamma);
    en[i] = std::exp((xmin - x[i]) / gamma);
    sp += ep[i];
    tp += x[i] * ep[i];
    sn += en[i];
    tn += x[i] * en[i];
  }
  const double fp = tp / sp;
  const double fn = tn / sn;
  if (grad) {
    for (int i = 0; i < n; ++i) {
      grad[i] += ep[i] / sp * (1.0 + (x[i] - fp) / gamma) -
                 en[i] / sn * (1.0 - (x[i] - fn) / gamma);
    }
  }
  return fp - fn;
}

int next_pow2(int n) {
  int p = 1;
  while (p < n) p <<= 1;
  return p;
}

ElectrostaticSystem::ElectrostaticSystem(const Rect& region, int nx, int ny)
    : region_(region), nx_(nx), ny_(ny) {
  if (nx <= 0 || ny <= 0 || (nx & (nx - 1)) || (ny & (ny - 1)) ||
      region.width() <= 0 || region.height() <= 0) {
    throw std::invalid_argument("ElectrostaticSystem: grid must be 2^k and region non-empty");
  }
  x0_ = static_cast<double>(region.xlo);
  y0_ = static_cast<double>(region.ylo);
  bw_ = static_cast<double>(region.width()) / nx;
  bh_ = static_cast<double>(region.height()) / ny;
  const std::size_t n = static_cast<std::size_t>(nx) * ny;
  charge_.assign(n, 0.0);
  coef_.assign(n, 0.0);
  ex_.assign(n, 0.0);
  ey_.assign(n, 0.0);
  psi_.assign(n, 0.0);
  wu_.resize(nx);
  wv_.resize(ny);
  for (int u = 0; u < nx; ++u) wu_[u] = kPi * u / static_cast<double>(region.width());
  for (int v = 0; v < ny; ++v) wv_[v] = kPi * v / static_cast<double>(region.height());
  make_tables(tx_, static_cast<std::size_t>(nx));
  make_tables(ty_, static_cast<std::size_t>(ny));
}

void ElectrostaticSystem::make_tables(Tables& t, std::size_t n) {
  t.n = n;
  const std::size_t m = 2 * n;  // FFT length
  t.twiddle.resize(m / 2);
  for (std::size_t k = 0; k < m / 2; ++k) {
    const double ang = -2.0 * kPi * static_cast<double>(k) / static_cast<double>(m);
    t.twiddle[k] = {std::cos(ang), std::sin(ang)};
  }
  t.rot.resize(n);
  for (std::size_t u = 0; u < n; ++u) {
    const double a = kPi * static_cast<double>(u) / (2.0 * static_cast<double>(n));
    t.rot[u] = {std::cos(a), std::sin(a)};  // e^{i pi u / 2n}
  }
}

void ElectrostaticSystem::reset(const std::vector<double>& base) {
  if (base.empty()) {
    std::fill(charge_.begin(), charge_.end(), 0.0);
  } else {
    charge_ = base;
  }
}

std::pair<int, int> ElectrostaticSystem::rect_rows(double xlo, double ylo, double xhi,
                                                   double yhi) const {
  const double rxhi = x0_ + bw_ * nx_, ryhi = y0_ + bh_ * ny_;
  xlo = std::max(xlo, x0_);
  ylo = std::max(ylo, y0_);
  xhi = std::min(xhi, rxhi);
  yhi = std::min(yhi, ryhi);
  if (xhi <= xlo || yhi <= ylo) return {1, 0};
  return {std::clamp(static_cast<int>((ylo - y0_) / bh_), 0, ny_ - 1),
          std::clamp(static_cast<int>((yhi - y0_) / bh_), 0, ny_ - 1)};
}

template <class F>
void ElectrostaticSystem::for_each_overlap(double xlo, double ylo, double xhi, double yhi,
                                           int r0, int r1, F&& f) const {
  const double rxhi = x0_ + bw_ * nx_, ryhi = y0_ + bh_ * ny_;
  xlo = std::max(xlo, x0_);
  ylo = std::max(ylo, y0_);
  xhi = std::min(xhi, rxhi);
  yhi = std::min(yhi, ryhi);
  if (xhi <= xlo || yhi <= ylo) return;
  const int ix0 = std::clamp(static_cast<int>((xlo - x0_) / bw_), 0, nx_ - 1);
  const int ix1 = std::clamp(static_cast<int>((xhi - x0_) / bw_), 0, nx_ - 1);
  const int iy0 = std::max(r0, std::clamp(static_cast<int>((ylo - y0_) / bh_), 0, ny_ - 1));
  const int iy1 = std::min(r1, std::clamp(static_cast<int>((yhi - y0_) / bh_), 0, ny_ - 1));
  for (int iy = iy0; iy <= iy1; ++iy) {
    const double by = y0_ + iy * bh_;
    const double oy = std::min(yhi, by + bh_) - std::max(ylo, by);
    if (oy <= 0) continue;
    for (int ix = ix0; ix <= ix1; ++ix) {
      const double bx = x0_ + ix * bw_;
      const double ox = std::min(xhi, bx + bw_) - std::max(xlo, bx);
      if (ox > 0) f(idx(ix, iy), ox * oy);
    }
  }
}

void ElectrostaticSystem::add_rect(double xlo, double ylo, double xhi, double yhi,
                                   double scale) {
  add_rect_to(charge_, xlo, ylo, xhi, yhi, scale);
}

void ElectrostaticSystem::add_rect_to(std::vector<double>& charge, double xlo, double ylo,
                                      double xhi, double yhi, double scale) const {
  add_rect_rows(charge.data(), xlo, ylo, xhi, yhi, scale, 0, ny_ - 1);
}

void ElectrostaticSystem::add_rect_rows(double* charge, double xlo, double ylo, double xhi,
                                        double yhi, double scale, int r0, int r1) const {
  for_each_overlap(xlo, ylo, xhi, yhi, r0, r1,
                   [&](std::size_t b, double a) { charge[b] += scale * a; });
}

void ElectrostaticSystem::rect_gradient(double xlo, double ylo, double xhi, double yhi,
                                        double scale, double& gx, double& gy) const {
  double sx = 0, sy = 0;
  for_each_overlap(xlo, ylo, xhi, yhi, 0, ny_ - 1, [&](std::size_t b, double a) {
    sx += a * ex_[b];
    sy += a * ey_[b];
  });
  gx = -scale * sx;
  gy = -scale * sy;
}

void ElectrostaticSystem::fft(std::complex<double>* a, const Tables& t) {
  // Iterative radix-2 FFT of length 2n. Products are spelled out:
  // std::complex operator* is slow without -ffast-math (it guards against
  // inf/nan through a library call).
  const std::size_t n = 2 * t.n;
  for (std::size_t i = 1, j = 0; i < n; ++i) {
    std::size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  double* d = reinterpret_cast<double*>(a);
  const std::complex<double>* tw = t.twiddle.data();
  for (std::size_t len = 2; len <= n; len <<= 1) {
    const std::size_t half = len / 2, stride = n / len;
    for (std::size_t i = 0; i < n; i += len) {
      for (std::size_t k = 0; k < half; ++k) {
        const double wr = tw[k * stride].real(), wi = tw[k * stride].imag();
        double* p = d + 2 * (i + k);
        double* q = d + 2 * (i + k + half);
        const double tr = q[0] * wr - q[1] * wi, ti = q[0] * wi + q[1] * wr;
        q[0] = p[0] - tr;
        q[1] = p[1] - ti;
        p[0] += tr;
        p[1] += ti;
      }
    }
  }
}

// X_u = sum_x f_x cos(pi u (x+1/2)/n): mirror f to length 2n, FFT, rotate.
void ElectrostaticSystem::dct(double* v, const Tables& t, Scratch& s) {
  const std::size_t n = t.n;
  s.buf.assign(2 * n, {});
  for (std::size_t x = 0; x < n; ++x) s.buf[x] = s.buf[2 * n - 1 - x] = v[x];
  fft(s.buf.data(), t);
  for (std::size_t u = 0; u < n; ++u) {
    // Re(Y_u * e^{-i pi u / 2n})
    v[u] = 0.5 * (s.buf[u].real() * t.rot[u].real() + s.buf[u].imag() * t.rot[u].imag());
  }
}

// g_x = sum_u c_u cos(pi u (x+1/2)/n) = Re sum_u (c_u e^{i pi u/2n}) e^{2 pi i u x/2n}.
void ElectrostaticSystem::cos_sum(double* v, const Tables& t, Scratch& s) {
  const std::size_t n = t.n;
  s.buf.assign(2 * n, {});
  for (std::size_t u = 0; u < n; ++u) {
    s.buf[u] = {v[u] * t.rot[u].real(), -v[u] * t.rot[u].imag()};  // conj(c_u e^{i pi u/2n})
  }
  fft(s.buf.data(), t);  // FFT of conj(z) = conj(IDFT-sum of z); real part is the same.
  for (std::size_t x = 0; x < n; ++x) v[x] = s.buf[x].real();
}

// sin(pi u (x+1/2)/n) = (-1)^x cos(pi (n-u) (x+1/2)/n): reverse, cos_sum, sign.
void ElectrostaticSystem::sin_sum(double* v, const Tables& t, Scratch& s) {
  const std::size_t n = t.n;
  s.line.assign(n, 0.0);
  for (std::size_t u = 1; u < n; ++u) s.line[n - u] = v[u];
  std::copy_n(s.line.begin(), n, v);
  cos_sum(v, t, s);
  for (std::size_t x = 1; x < n; x += 2) v[x] = -v[x];
}

void ElectrostaticSystem::along_x(std::vector<double>& grid, Transform f) const {
  par::parallel_for(0, static_cast<std::size_t>(ny_), 8, [&](std::size_t lo, std::size_t hi) {
    thread_local Scratch s;
    for (std::size_t iy = lo; iy < hi; ++iy) f(grid.data() + iy * nx_, tx_, s);
  });
}

void ElectrostaticSystem::along_y(std::vector<double>& grid, Transform f) const {
  // Columns in groups of 8 (one cache line per row read) to avoid striding
  // through memory once per column.
  constexpr std::size_t G = 8;
  const std::size_t ngroups = (static_cast<std::size_t>(nx_) + G - 1) / G;
  par::parallel_for(0, ngroups, 1, [&](std::size_t glo, std::size_t ghi) {
    thread_local Scratch s;
    thread_local std::vector<double> cols;
    cols.resize(G * ny_);
    for (std::size_t g = glo; g < ghi; ++g) {
      const std::size_t x0 = g * G, w = std::min(G, static_cast<std::size_t>(nx_) - x0);
      for (int iy = 0; iy < ny_; ++iy) {
        const double* row = grid.data() + idx(static_cast<int>(x0), iy);
        for (std::size_t c = 0; c < w; ++c) cols[c * ny_ + iy] = row[c];
      }
      for (std::size_t c = 0; c < w; ++c) f(cols.data() + c * ny_, ty_, s);
      for (int iy = 0; iy < ny_; ++iy) {
        double* row = grid.data() + idx(static_cast<int>(x0), iy);
        for (std::size_t c = 0; c < w; ++c) row[c] = cols[c * ny_ + iy];
      }
    }
  });
}

void ElectrostaticSystem::solve(bool with_potential) { solve_from(charge_, with_potential); }

void ElectrostaticSystem::solve_from(const std::vector<double>& charge, bool with_potential) {
  // a_uv: cosine coefficients of rho = charge / bin area.
  coef_ = charge;
  along_x(coef_, &ElectrostaticSystem::dct);
  along_y(coef_, &ElectrostaticSystem::dct);
  const double inv_a = 1.0 / bin_area();
  par::parallel_for(0, static_cast<std::size_t>(ny_), 16, [&](std::size_t vlo, std::size_t vhi) {
    for (int v = static_cast<int>(vlo); v < static_cast<int>(vhi); ++v) {
      const double kv = (v == 0 ? 1.0 : 2.0) / ny_;
      for (int u = 0; u < nx_; ++u) {
        const double ku = (u == 0 ? 1.0 : 2.0) / nx_;
        const double w2 = wu_[u] * wu_[u] + wv_[v] * wv_[v];
        // a_uv / (wu^2 + wv^2); the DC term (mean density) carries no field.
        const std::size_t i = idx(u, v);
        const double c = (u == 0 && v == 0) ? 0.0 : coef_[i] * ku * kv * inv_a / w2;
        coef_[i] = c;
        // xi_x = sum a/(w^2) wu sin(wu x) cos(wv y); xi_y = sum a/(w^2) wv cos sin.
        ex_[i] = c * wu_[u];
        ey_[i] = c * wv_[v];
      }
    }
  });
  if (with_potential) {
    psi_ = coef_;
    par::parallel_invoke(
        [&] {
          along_x(ex_, &ElectrostaticSystem::sin_sum);
          along_y(ex_, &ElectrostaticSystem::cos_sum);
        },
        [&] {
          along_x(ey_, &ElectrostaticSystem::cos_sum);
          along_y(ey_, &ElectrostaticSystem::sin_sum);
        },
        [&] {
          along_x(psi_, &ElectrostaticSystem::cos_sum);
          along_y(psi_, &ElectrostaticSystem::cos_sum);
        });
  } else {
    par::parallel_invoke(
        [&] {
          along_x(ex_, &ElectrostaticSystem::sin_sum);
          along_y(ex_, &ElectrostaticSystem::cos_sum);
        },
        [&] {
          along_x(ey_, &ElectrostaticSystem::cos_sum);
          along_y(ey_, &ElectrostaticSystem::sin_sum);
        });
  }
}

double ElectrostaticSystem::energy() const {
  double e = 0.0;
  for (std::size_t b = 0; b < charge_.size(); ++b) e += charge_[b] * psi_[b];
  return 0.5 * e;
}

}  // namespace hermes::gp
