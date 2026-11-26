// CPU compute backend: the placement kernels on the hermes::par pool.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <mutex>
#include <stdexcept>

#include "hermes/compute/backend.hpp"
#include "hermes/log/log.hpp"
#include "hermes/par/parallel.hpp"
#include "hermes/par/row_bands.hpp"
#include "hermes/place/analytic.hpp"

namespace hermes::compute {

namespace {

HERMES_LOG_CHANNEL(log_compute, "compute");

using par::parallel_for;
using par::parallel_sum;

inline void footprint_of(const DensityView& o, std::span<const double> pos, int k, double r[4]) {
  const std::size_t N = pos.size() / 2;
  const Rect& die = o.die;
  const double cw = o.charge_w[k], ch = o.charge_h[k];
  double xlo = pos[k] - 0.5 * cw, ylo = pos[N + k] - 0.5 * ch;
  if (cw <= static_cast<double>(die.width())) {
    xlo = std::clamp(xlo, static_cast<double>(die.xlo), static_cast<double>(die.xhi) - cw);
  }
  if (ch <= static_cast<double>(die.height())) {
    ylo = std::clamp(ylo, static_cast<double>(die.ylo), static_cast<double>(die.yhi) - ch);
  }
  r[0] = xlo;
  r[1] = ylo;
  r[2] = xlo + cw;
  r[3] = ylo + ch;
}

class CpuDensitySolver final : public DensitySolver {
 public:
  CpuDensitySolver(const Rect& region, int nx, int ny) : es_(region, nx, ny) {}

  int nx() const override { return es_.nx(); }
  int ny() const override { return es_.ny(); }
  double bin_w() const override { return es_.bin_w(); }
  double bin_h() const override { return es_.bin_h(); }

  void deposit(const DensityView& o, std::span<const double> pos, int begin, int end,
               std::span<const double> base, std::span<double> charge) override {
    const std::size_t nb = charge.size();
    parallel_for(0, nb, 1 << 14, [&](std::size_t lo, std::size_t hi) {
      if (base.empty()) {
        std::fill(charge.begin() + lo, charge.begin() + hi, 0.0);
      } else {
        std::copy(base.begin() + lo, base.begin() + hi, charge.begin() + lo);
      }
    });
    const std::size_t n = end > begin ? static_cast<std::size_t>(end - begin) : 0;
    fp_.resize(4 * n);
    parallel_for(0, n, 4096, [&](std::size_t lo, std::size_t hi) {
      for (std::size_t i = lo; i < hi; ++i) footprint_of(o, pos, begin + static_cast<int>(i), &fp_[4 * i]);
    });
    bands_.build(n, es_.ny(), [&](std::size_t i) {
      const double* r = &fp_[4 * i];
      return es_.rect_rows(r[0], r[1], r[2], r[3]);
    });
    double* q = charge.data();
    bands_.run([&](std::size_t i, int r0, int r1) {
      const double* r = &fp_[4 * i];
      es_.add_rect_rows(q, r[0], r[1], r[2], r[3], o.ratio[begin + i], r0, r1);
    });
  }

  void solve(std::span<const double> charge) override {
    tmp_.assign(charge.begin(), charge.end());
    es_.solve_from(tmp_, false);
  }

  void gradient(const DensityView& o, std::span<const double> pos, int count,
                std::span<double> grad) override {
    const std::size_t N = pos.size() / 2;
    parallel_for(0, static_cast<std::size_t>(count), 2048, [&](std::size_t lo, std::size_t hi) {
      double r[4];
      for (std::size_t k = lo; k < hi; ++k) {
        footprint_of(o, pos, static_cast<int>(k), r);
        es_.rect_gradient(r[0], r[1], r[2], r[3], o.ratio[k], grad[k], grad[N + k]);
      }
    });
  }

  double overflow(std::span<const double> charge, double cap) override {
    return parallel_sum(0, charge.size(), 1 << 14, [&](std::size_t lo, std::size_t hi) {
      double s = 0.0;
      for (std::size_t b = lo; b < hi; ++b) s += std::max(0.0, charge[b] - cap);
      return s;
    });
  }

  void footprint(const DensityView& o, std::span<const double> pos, int k,
                 double r[4]) const override {
    footprint_of(o, pos, k, r);
  }

 private:
  gp::ElectrostaticSystem es_;
  par::RowBands bands_;
  std::vector<double> fp_, tmp_;
};

class CpuBackend final : public Backend {
 public:
  std::string_view name() const override { return "cpu"; }

  double hpwl(const NetlistView& nl, std::span<const double> pos) override {
    const std::size_t N = static_cast<std::size_t>(nl.num_objects);
    return parallel_sum(0, nl.num_nets(), 1024, [&](std::size_t lo, std::size_t hi) {
      double total = 0.0;
      for (std::size_t n = lo; n < hi; ++n) {
        double xl = 1e300, xh = -1e300, yl = 1e300, yh = -1e300;
        for (int pp = nl.net_start[n]; pp < nl.net_start[n + 1]; ++pp) {
          const int k = nl.pin_obj[pp];
          const double x = (k >= 0 ? pos[k] : 0.0) + nl.pin_dx[pp];
          const double y = (k >= 0 ? pos[N + k] : 0.0) + nl.pin_dy[pp];
          xl = std::min(xl, x);
          xh = std::max(xh, x);
          yl = std::min(yl, y);
          yh = std::max(yh, y);
        }
        total += (xh - xl) + (yh - yl);
      }
      return total;
    });
  }

  void wirelength_gradient(const NetlistView& nl, std::span<const double> pos, double gamma,
                           std::span<double> pin_scratch, std::span<double> grad) override {
    const std::size_t N = static_cast<std::size_t>(nl.num_objects);
    const std::size_t P = nl.num_pins();
    // Pass 1, per net: weighted WA gradient of each pin.
    parallel_for(0, nl.num_nets(), 1024, [&](std::size_t lo, std::size_t hi) {
      thread_local std::vector<double> buf;
      if (buf.size() < 4 * static_cast<std::size_t>(nl.max_degree)) buf.resize(4 * nl.max_degree);
      double* xs = buf.data();
      double* ys = xs + nl.max_degree;
      double* gx = ys + nl.max_degree;
      double* gy = gx + nl.max_degree;
      for (std::size_t n = lo; n < hi; ++n) {
        const int b = nl.net_start[n], cnt = nl.net_start[n + 1] - b;
        for (int i = 0; i < cnt; ++i) {
          const int pp = b + i, k = nl.pin_obj[pp];
          xs[i] = (k >= 0 ? pos[k] : 0.0) + nl.pin_dx[pp];
          ys[i] = (k >= 0 ? pos[N + k] : 0.0) + nl.pin_dy[pp];
          gx[i] = gy[i] = 0.0;
        }
        gp::wa_wirelength_1d(xs, cnt, gamma, gx);
        gp::wa_wirelength_1d(ys, cnt, gamma, gy);
        const double w = nl.net_weight[n];
        for (int i = 0; i < cnt; ++i) {
          pin_scratch[b + i] = w * gx[i];
          pin_scratch[P + b + i] = w * gy[i];
        }
      }
    });
    // Pass 2, per object: gather its pins (pin order = net order, so the sum
    // is the same as the serial scatter loop's).
    parallel_for(0, N, 4096, [&](std::size_t lo, std::size_t hi) {
      for (std::size_t k = lo; k < hi; ++k) {
        double sx = 0.0, sy = 0.0;
        if (k + 1 < nl.obj_start.size()) {
          for (int i = nl.obj_start[k]; i < nl.obj_start[k + 1]; ++i) {
            const int p = nl.obj_pins[i];
            sx += pin_scratch[p];
            sy += pin_scratch[P + p];
          }
        }
        grad[k] = sx;
        grad[N + k] = sy;
      }
    });
  }

  std::unique_ptr<DensitySolver> make_density_solver(const Rect& region, int nx, int ny) override {
    return std::make_unique<CpuDensitySolver>(region, nx, ny);
  }

  void nesterov_predict(std::span<const double> v, std::span<const double> g,
                        std::span<const double> u, double alpha, double coef,
                        std::span<const double> lo, std::span<const double> hi,
                        std::span<double> u_new, std::span<double> v_new) override {
    parallel_for(0, v.size(), 8192, [&](std::size_t b, std::size_t e) {
      for (std::size_t i = b; i < e; ++i) {
        const double un = std::min(std::max(v[i] - alpha * g[i], lo[i]), hi[i]);
        u_new[i] = un;
        v_new[i] = std::min(std::max(un + coef * (un - u[i]), lo[i]), hi[i]);
      }
    });
  }

  double distance(std::span<const double> a, std::span<const double> b) override {
    return std::sqrt(parallel_sum(0, a.size(), 8192, [&](std::size_t lo, std::size_t hi) {
      double s = 0.0;
      for (std::size_t i = lo; i < hi; ++i) s += (a[i] - b[i]) * (a[i] - b[i]);
      return s;
    }));
  }

  void precondition(std::span<const double> gw, std::span<const double> gd,
                    std::span<const double> pw, std::span<const double> q, double lambda,
                    std::span<double> g) override {
    const std::size_t N = pw.size();
    parallel_for(0, N, 8192, [&](std::size_t lo, std::size_t hi) {
      for (std::size_t k = lo; k < hi; ++k) {
        const double pre = 1.0 / std::max(1.0, pw[k] + lambda * q[k]);
        g[k] = (gw[k] + lambda * gd[k]) * pre;
        g[N + k] = (gw[N + k] + lambda * gd[N + k]) * pre;
      }
    });
  }
};

struct Registry {
  std::mutex mu;
  std::map<std::string, std::function<std::unique_ptr<Backend>()>, std::less<>> factories;
  std::map<std::string, std::unique_ptr<Backend>, std::less<>> instances;
  Backend* current = nullptr;
};

Registry& registry() {
  static Registry r;
  return r;
}

Backend& instance_locked(Registry& r, std::string_view name) {
  if (auto it = r.instances.find(name); it != r.instances.end()) return *it->second;
  auto f = r.factories.find(name);
  if (f == r.factories.end()) {
    throw std::invalid_argument("unknown compute backend '" + std::string(name) + "'");
  }
  auto be = f->second();
  if (!be) throw std::invalid_argument("compute backend '" + std::string(name) + "' unavailable");
  Backend& ref = *be;
  r.instances.emplace(std::string(name), std::move(be));
  return ref;
}

}  // namespace

Backend& cpu_backend() {
  static CpuBackend cpu;
  return cpu;
}

void register_backend(std::string name, std::function<std::unique_ptr<Backend>()> factory) {
  Registry& r = registry();
  std::lock_guard lk(r.mu);
  r.factories[std::move(name)] = std::move(factory);
}

std::vector<std::string> backends() {
  Registry& r = registry();
  std::lock_guard lk(r.mu);
  std::vector<std::string> out{"cpu"};
  for (const auto& [n, f] : r.factories) {
    if (n != "cpu") out.push_back(n);
  }
  return out;
}

void set_backend(std::string_view name) {
  Registry& r = registry();
  std::lock_guard lk(r.mu);
  r.current = name == "cpu" ? &cpu_backend() : &instance_locked(r, name);
}

Backend& backend() {
  Registry& r = registry();
  std::lock_guard lk(r.mu);
  if (!r.current) {
    r.current = &cpu_backend();
    if (const char* e = std::getenv("HERMES_BACKEND"); e && std::string_view(e) != "cpu") {
      try {
        r.current = &instance_locked(r, e);
      } catch (const std::exception& ex) {
        HLOG_WARN_ID(log_compute, "CMP-0001", "HERMES_BACKEND={}: {}; using cpu", e, ex.what());
      }
    }
  }
  return *r.current;
}

}  // namespace hermes::compute
