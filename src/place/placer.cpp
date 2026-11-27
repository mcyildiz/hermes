#include "hermes/place/placer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <optional>
#include <random>
#include <stdexcept>

#include "hermes/db/geometry.hpp"
#include "hermes/log/log.hpp"
#include "hermes/compute/backend.hpp"
#include "hermes/par/parallel.hpp"
#include "hermes/place/analytic.hpp"
#include "hermes/track/run_tracker.hpp"

namespace hermes {

// ===========================================================================
// Nesterov global placer
// ===========================================================================
namespace {

/// One optimization problem: movable cells (std cells and macros) plus filler
/// cells, positions stored as centers [x_0..x_{M-1}, y_0..y_{M-1}].
///
/// All per-iteration work goes through compute::Backend kernels on flat
/// arrays (CSR netlist, footprint arrays, position vectors), so it runs on
/// the thread pool today and can move to a GPU backend unchanged. Results
/// do not depend on the number of threads.
class NesterovPlacer {
 public:
  /// `filler_init` (x0, y0, x1, y1, ...): start positions for the fillers,
  /// e.g. those of an earlier placer on the same design (extra fillers start
  /// at random, surplus positions are ignored).
  NesterovPlacer(Design& design, const PlacerOptions& opts,
                 const std::vector<double>& net_weight, compute::Backend& be,
                 const std::vector<double>* filler_init = nullptr)
      : d_(design), opts_(opts), net_weight_(net_weight), be_(be), filler_init_(filler_init) {
    build_objects();
    build_grid();
    build_fillers();
    build_nets();
    place_initial();
    set_inflation({});
    restart();
  }

  bool empty() const { return num_ == 0; }
  double overflow() const { return overflow_; }
  double hpwl() const { return hpwl_; }
  double lambda() const { return lambda_; }
  double gamma() const { return gamma_; }
  double alpha() const { return alpha_; }
  int num_fillers() const { return num_ - num_cells_; }

  /// Current filler centers (x0, y0, x1, y1, ...).
  std::vector<double> filler_positions() const {
    std::vector<double> v;
    v.reserve(2 * static_cast<std::size_t>(num_ - num_cells_));
    for (int k = num_cells_; k < num_; ++k) {
      v.push_back(u_[k]);
      v.push_back(u_[num_ + k]);
    }
    return v;
  }

  /// Density penalty to start from (instead of the gradient-balanced
  /// lambda_0); call before the first step().
  void set_lambda(double lambda) { lambda_ = lambda; }

  /// Recomputes inflated charges (inflation per design cell, area factor).
  void set_inflation(const std::vector<double>& inflation) {
    const double rt = opts_.target_density;
    double base = 0.0, extra = 0.0;
    for (int k = 0; k < num_cells_; ++k) {
      if (macro_[k]) continue;
      const double a = w_[k] * h_[k];
      const double f = inflation.empty() ? 1.0 : inflation[cell_[k]];
      base += a;
      extra += a * std::max(0.0, f - 1.0);
    }
    // Keep the inflated area inside what the target density can hold.
    const double room = rt * free_area_ - rt * macro_area_ - base;
    const double cap = extra > 0.0 ? std::clamp(0.95 * room / extra, 0.0, 1.0) : 1.0;
    double inflated = base;
    for (int k = 0; k < num_cells_; ++k) {
      double f = 1.0;
      if (!macro_[k] && !inflation.empty()) f = 1.0 + std::max(0.0, inflation[cell_[k]] - 1.0) * cap;
      if (!macro_[k]) inflated += w_[k] * h_[k] * (f - 1.0);
      const double s = std::sqrt(f);
      set_charge(k, w_[k] * s, h_[k] * s, (macro_[k] ? rt : 1.0));
    }
    // Fillers absorb whatever whitespace is left at the target density.
    const double fill = std::max(0.0, rt * (free_area_ - macro_area_) - inflated);
    const double each = num_ > num_cells_ ? fill / (num_ - num_cells_) : 0.0;
    for (int k = num_cells_; k < num_; ++k) {
      set_charge(k, w_[k], h_[k], each / (w_[k] * h_[k]));
    }
    nonfiller_charge_ = rt * macro_area_ + inflated;
  }

  /// Sets net-weight-dependent preconditioner and restarts the momentum
  /// (call after the objective changes: weights, inflation).
  void restart() {
    for (std::size_t n = 0; n < net_id_.size(); ++n) nw_[n] = net_weight_[net_id_[n]];
    pin_weight_.assign(num_, 0.0);
    par::parallel_for(0, static_cast<std::size_t>(num_), 4096, [&](std::size_t lo, std::size_t hi) {
      for (std::size_t k = lo; k < hi; ++k) {
        double s = 0.0;
        for (int i = obj_start_[k]; i < obj_start_[k + 1]; ++i) s += nw_[pin_net_[obj_pins_[i]]];
        pin_weight_[k] = s;
      }
    });
    started_ = false;
  }

  void step() {
    if (!started_) start();
    const double kMinRatio = 0.95;
    u_new_.resize(2 * num_);
    v_new_.resize(2 * num_);
    g_new_.resize(2 * num_);
    double a_new = 1.0, alpha_new = alpha_;
    for (int bt = 0; bt < std::max(1, opts_.max_backtracks); ++bt) {
      a_new = 0.5 * (1.0 + std::sqrt(4.0 * a_ * a_ + 1.0));
      const double coef = (a_ - 1.0) / a_new;
      be_.nesterov_predict(v_, g_, u_, alpha_, coef, lo_, hi_, u_new_, v_new_);
      gradient(v_new_, g_new_);
      const double dv = be_.distance(v_new_, v_), dg = be_.distance(g_new_, g_);
      alpha_new = dg > 0.0 ? dv / dg : alpha_;
      if (alpha_new > kMinRatio * alpha_) break;
      alpha_ = alpha_new;
    }
    u_.swap(u_new_);
    v_.swap(v_new_);
    g_.swap(g_new_);
    a_ = a_new;
    alpha_ = alpha_new;
    update_metrics(u_);
    update_parameters();
  }

  /// Writes the current solution (lower-left, rounded) into the design.
  void write_back() {
    locs_.resize(num_cells_);
    par::parallel_for(0, static_cast<std::size_t>(num_cells_), 8192, [&](std::size_t lo, std::size_t hi) {
      for (std::size_t k = lo; k < hi; ++k) {
        locs_[k] = {static_cast<Dbu>(std::llround(u_[k] - 0.5 * w_[k])),
                    static_cast<Dbu>(std::llround(u_[num_ + k] - 0.5 * h_[k]))};
      }
    });
    d_.move_cells(cell_, locs_);
  }

 private:
  compute::NetlistView netlist() const {
    compute::NetlistView v;
    v.num_objects = num_;
    v.net_start = net_start_;
    v.pin_obj = pin_obj_;
    v.pin_dx = pin_dx_;
    v.pin_dy = pin_dy_;
    v.net_weight = nw_;
    v.obj_start = obj_start_;
    v.obj_pins = obj_pins_;
    v.max_degree = max_degree_;
    return v;
  }
  compute::DensityView density_view() const {
    return {charge_w_, charge_h_, ratio_, d_.die()};
  }

  void build_objects() {
    const auto cells = d_.cells();
    for (Id ci = 0; ci < static_cast<Id>(cells.size()); ++ci) {
      const Cell& c = cells[ci];
      if (!c.movable()) continue;
      cell_.push_back(ci);
      w_.push_back(std::max<double>(1.0, static_cast<double>(c.width)));
      h_.push_back(std::max<double>(1.0, static_cast<double>(c.height)));
      macro_.push_back(c.is_macro);
      if (c.is_macro) macro_area_ += w_.back() * h_.back();
    }
    num_cells_ = num_ = static_cast<int>(cell_.size());
    const Rect& die = d_.die();
    die_area_ = static_cast<double>(die.width()) * static_cast<double>(die.height());
  }

  void build_grid() {
    const Rect& die = d_.die();
    int nx = opts_.bins_x, ny = opts_.bins_y;
    if (nx <= 0 || ny <= 0) {
      // RePlAce's rule: a bin holds about one average movable std cell at the
      // target density (macros would skew the average, so they are left out).
      double area = 0.0;
      int count = 0;
      for (int pass = 0; pass < 2 && count == 0; ++pass) {
        for (int k = 0; k < num_cells_; ++k) {
          if (pass == 0 && macro_[k]) continue;  // all-macro designs: pass 1
          area += w_[k] * h_[k];
          ++count;
        }
      }
      const double ideal_bin = count > 0 ? area / count / opts_.target_density : die_area_;
      const double side = std::sqrt(std::max(1.0, die_area_ / ideal_bin));
      const int n = std::clamp(1 << static_cast<int>(std::lround(std::log2(side))), 8, 1024);
      if (nx <= 0) nx = n;
      if (ny <= 0) ny = n;
    }
    nx = gp::next_pow2(nx);
    ny = gp::next_pow2(ny);
    solver_ = be_.make_density_solver(die, nx, ny);
    bin_w_ = solver_->bin_w();
    bin_h_ = solver_->bin_h();

    // Fixed objects: charge scaled by target density, so a bin fully covered
    // by a fixed macro is exactly "full" and pushes movable cells out.
    gp::ElectrostaticSystem setup(die, nx, ny);  // geometry helper only
    fixed_charge_.assign(static_cast<std::size_t>(nx) * ny, 0.0);
    double fixed_area = 0.0;
    std::vector<double> cover(fixed_charge_.size(), 0.0);
    for (const Cell& c : d_.cells()) {
      if (!c.fixed || c.removed) continue;
      const Rect bb = c.bbox();
      setup.add_rect_to(cover, static_cast<double>(bb.xlo), static_cast<double>(bb.ylo),
                        static_cast<double>(bb.xhi), static_cast<double>(bb.yhi), 1.0);
    }
    for (std::size_t b = 0; b < cover.size(); ++b) {
      const double a = std::min(cover[b], solver_->bin_area());  // overlapping fixed cells
      fixed_charge_[b] = opts_.target_density * a;
      fixed_area += a;
    }
    free_area_ = std::max(0.0, die_area_ - fixed_area);
    charge_.assign(fixed_charge_.size(), 0.0);
  }

  void build_fillers() {
    // Filler size: average of the middle 90% of movable std cells by area.
    std::vector<std::pair<double, int>> by_area;
    for (int k = 0; k < num_cells_; ++k) {
      if (!macro_[k]) by_area.push_back({w_[k] * h_[k], k});
    }
    double fw = bin_w_, fh = bin_h_, std_area = 0.0;
    if (!by_area.empty()) {
      std::sort(by_area.begin(), by_area.end());
      const std::size_t lo = by_area.size() / 20, hi = by_area.size() - by_area.size() / 20;
      double sw = 0, sh = 0;
      for (std::size_t i = lo; i < hi; ++i) {
        sw += w_[by_area[i].second];
        sh += h_[by_area[i].second];
      }
      fw = sw / static_cast<double>(hi - lo);
      fh = sh / static_cast<double>(hi - lo);
    }
    for (const auto& [a, k] : by_area) std_area += a;
    const double rt = opts_.target_density;
    const double fill = rt * (free_area_ - macro_area_) - std_area;
    if (fill <= 0.0) return;
    double count = fill / (fw * fh);
    // Bound the filler count; fillers smaller than a bin are stretched anyway.
    const double max_count = static_cast<double>(solver_->nx()) * solver_->ny();
    if (count > max_count) {
      const double s = std::sqrt(count / max_count);
      fw *= s;
      fh *= s;
      count = max_count;
    }
    const int nf = static_cast<int>(std::floor(count));
    std::mt19937 rng(opts_.seed + 7919u);
    const Rect& die = d_.die();
    std::uniform_real_distribution<double> ux(die.xlo + 0.5 * fw, die.xhi - 0.5 * fw);
    std::uniform_real_distribution<double> uy(die.ylo + 0.5 * fh, die.yhi - 0.5 * fh);
    filler_pos_.reserve(2 * nf);
    for (int i = 0; i < nf; ++i) {
      w_.push_back(fw);
      h_.push_back(fh);
      macro_.push_back(false);
      filler_pos_.push_back(ux(rng));
      filler_pos_.push_back(uy(rng));
    }
    num_ = num_cells_ + nf;
  }

  void build_nets() {
    std::vector<int> obj_of(d_.cells().size(), -1);
    for (int k = 0; k < num_cells_; ++k) obj_of[cell_[k]] = k;
    net_start_.push_back(0);
    for (Id n = 0; n < static_cast<Id>(d_.num_nets()); ++n) {
      const auto net_pins = d_.net_pins(n);
      if (net_pins.size() < 2) continue;
      const std::size_t begin = pin_obj_.size();
      bool movable = false;
      for (Id pid : net_pins) {
        const Pin& p = d_.pin(pid);
        const int k = p.cell == kInvalidId ? -1 : obj_of[p.cell];
        if (k >= 0) {
          movable = true;
          pin_dx_.push_back(static_cast<double>(p.offset.x) - 0.5 * w_[k]);
          pin_dy_.push_back(static_cast<double>(p.offset.y) - 0.5 * h_[k]);
        } else {
          const Point q = pin_location(d_, pid);
          pin_dx_.push_back(static_cast<double>(q.x));
          pin_dy_.push_back(static_cast<double>(q.y));
        }
        pin_obj_.push_back(k);
      }
      if (movable) {
        const int local = static_cast<int>(net_id_.size());
        net_id_.push_back(n);
        pin_net_.resize(pin_obj_.size(), local);
        net_start_.push_back(static_cast<int>(pin_obj_.size()));
        max_degree_ = std::max(max_degree_, static_cast<int>(pin_obj_.size() - begin));
      } else {
        pin_obj_.resize(begin);
        pin_dx_.resize(begin);
        pin_dy_.resize(begin);
      }
    }
    nw_.assign(net_id_.size(), 1.0);
    // Object -> pins (CSR), pins ascending: counting sort.
    obj_start_.assign(num_ + 1, 0);
    for (int k : pin_obj_) {
      if (k >= 0) ++obj_start_[k + 1];
    }
    for (int k = 0; k < num_; ++k) obj_start_[k + 1] += obj_start_[k];
    obj_pins_.resize(obj_start_[num_]);
    std::vector<int> fill(obj_start_.begin(), obj_start_.end() - 1);
    for (int p = 0; p < static_cast<int>(pin_obj_.size()); ++p) {
      if (pin_obj_[p] >= 0) obj_pins_[fill[pin_obj_[p]]++] = p;
    }
    pin_scratch_.assign(2 * pin_obj_.size(), 0.0);
  }

  void place_initial() {
    // Movable cells start at the die center with a small jitter (so the
    // density field can separate them); fillers start uniformly spread.
    // TODO(hermes): quadratic (B2B) initial placement.
    const Rect& die = d_.die();
    const double cx = 0.5 * (die.xlo + die.xhi), cy = 0.5 * (die.ylo + die.yhi);
    // Clamp bounds: an object's center stays inside the die (objects wider
    // than the die sit at its center).
    lo_.resize(2 * num_);
    hi_.resize(2 * num_);
    for (int k = 0; k < num_; ++k) {
      const double hw = 0.5 * w_[k], hh = 0.5 * h_[k];
      if (2 * hw >= die.width()) {
        lo_[k] = hi_[k] = 0.5 * (die.xlo + die.xhi);
      } else {
        lo_[k] = die.xlo + hw;
        hi_[k] = die.xhi - hw;
      }
      if (2 * hh >= die.height()) {
        lo_[num_ + k] = hi_[num_ + k] = 0.5 * (die.ylo + die.yhi);
      } else {
        lo_[num_ + k] = die.ylo + hh;
        hi_[num_ + k] = die.yhi - hh;
      }
    }
    std::mt19937 rng(opts_.seed);
    std::normal_distribution<double> jx(0.0, 1e-3 * die.width()), jy(0.0, 1e-3 * die.height());
    u_.assign(2 * num_, 0.0);
    for (int k = 0; k < num_cells_; ++k) {
      const double dx = jx(rng), dy = jy(rng);  // Drawn for every cell: same stream either way.
      const Cell& c = d_.cell(cell_[k]);
      if (opts_.keep_placed && c.placed) {
        // Placed by the input (DEF PLACED, placed .pl) or an earlier run.
        u_[k] = c.loc.x + 0.5 * c.width;
        u_[num_ + k] = c.loc.y + 0.5 * c.height;
      } else {
        u_[k] = cx + dx;
        u_[num_ + k] = cy + dy;
      }
    }
    const std::size_t given = filler_init_ ? filler_init_->size() / 2 : 0;
    for (int k = num_cells_; k < num_; ++k) {
      const std::size_t f = static_cast<std::size_t>(k - num_cells_);
      const std::vector<double>& src = f < given ? *filler_init_ : filler_pos_;
      u_[k] = src[2 * f];
      u_[num_ + k] = src[2 * f + 1];
    }
    clamp(u_);
    charge_w_.assign(num_, 0.0);
    charge_h_.assign(num_, 0.0);
    ratio_.assign(num_, 0.0);
    q_.assign(num_, 0.0);
  }

  // Density footprint of object k: inflated size (cw, ch) with charge density
  // `scale`, stretched to at least sqrt(2) bins with the total kept.
  void set_charge(int k, double cw, double ch, double scale) {
    const double sw = std::max(cw, kSqrt2 * bin_w_);
    const double sh = std::max(ch, kSqrt2 * bin_h_);
    charge_w_[k] = sw;
    charge_h_[k] = sh;
    ratio_[k] = scale * cw * ch / (sw * sh);
    q_[k] = ratio_[k] * sw * sh;
  }

  void clamp(std::vector<double>& p) const {
    for (std::size_t i = 0; i < p.size(); ++i) p[i] = std::min(std::max(p[i], lo_[i]), hi_[i]);
  }

  // Weighted WA wirelength gradient (overwrites g).
  void wirelength_gradient(const std::vector<double>& p, std::vector<double>& g) {
    g.resize(2 * num_);
    be_.wirelength_gradient(netlist(), p, gamma_, pin_scratch_, g);
  }

  // Electrostatic density gradient (unscaled by lambda, overwrites g).
  void density_gradient(const std::vector<double>& p, std::vector<double>& g) {
    g.resize(2 * num_);
    const auto dv = density_view();
    solver_->deposit(dv, p, 0, num_, fixed_charge_, charge_);
    solver_->solve(charge_);
    solver_->gradient(dv, p, num_, g);
  }

  // Preconditioned gradient of  WL + lambda * D  at p.
  void gradient(const std::vector<double>& p, std::vector<double>& g) {
    wirelength_gradient(p, gw_);
    density_gradient(p, gd_);
    g.resize(2 * num_);
    be_.precondition(gw_, gd_, pin_weight_, q_, lambda_, g);
  }

  void start() {
    update_metrics(u_);
    gamma_ = gamma_for(overflow_);
    if (lambda_ <= 0.0) {
      // lambda_0 balances the two gradients (RePlAce / DREAMPlace).
      std::vector<double> gw, gd;
      wirelength_gradient(u_, gw);
      density_gradient(u_, gd);
      double nw = 0, nd = 0;
      // Over real cells only: fillers carry most of the charge but no nets.
      for (int k = 0; k < num_cells_; ++k) {
        nw += std::abs(gw[k]) + std::abs(gw[num_ + k]);
        nd += std::abs(gd[k]) + std::abs(gd[num_ + k]);
      }
      lambda_ = nd > 0.0 ? opts_.initial_density_penalty * nw / nd : 1e-12;
      if (nw == 0.0 && nd > 0.0) lambda_ = 1.0 / nd;  // no nets: density only
    }
    v_ = u_;
    gradient(v_, g_);
    // Initial step length from a tiny perturbation (Lipschitz estimate).
    std::vector<double> vp(v_), gp_(2 * num_);
    const double eps = 1e-2 * std::min(bin_w_, bin_h_);
    for (int i = 0; i < 2 * num_; ++i) vp[i] -= eps * (g_[i] > 0 ? 1.0 : (g_[i] < 0 ? -1.0 : 0.0));
    clamp(vp);
    gradient(vp, gp_);
    const double dv = be_.distance(v_, vp), dg = be_.distance(g_, gp_);
    alpha_ = (dg > 0.0 && dv > 0.0) ? dv / dg : eps;
    a_ = 1.0;
    prev_hpwl_ = hpwl_;
    started_ = true;
  }

  static double gamma_coef(double tau) {
    return std::pow(10.0, (std::clamp(tau, 0.0, 1.0) - 0.1) * 20.0 / 9.0 - 1.0);
  }
  double gamma_for(double tau) const {
    return 4.0 * (bin_w_ + bin_h_) * gamma_coef(tau);
  }

  void update_parameters() {
    gamma_ = gamma_for(overflow_);
    // Density penalty grows fast while wirelength is not getting worse and
    // slows down (or backs off) when HPWL jumps (RePlAce's mu schedule).
    const double kMuMax = 1.1, kMuMin = 0.95;
    const double ref = std::max(1.0, 1e-2 * prev_hpwl_);
    const double delta = hpwl_ - prev_hpwl_;
    const double mu = delta < 0.0 ? kMuMax
                                  : std::max(kMuMin, std::pow(kMuMax, 1.0 - delta / ref));
    lambda_ *= mu;
    prev_hpwl_ = hpwl_;
  }

  void update_metrics(const std::vector<double>& p) {
    // HPWL of the continuous solution.
    hpwl_ = be_.hpwl(netlist(), p);
    // Density overflow of real objects (fillers excluded) over target capacity.
    metric_charge_.resize(fixed_charge_.size());
    solver_->deposit(density_view(), p, 0, num_cells_, fixed_charge_, metric_charge_);
    const double over = solver_->overflow(metric_charge_, opts_.target_density * solver_->bin_area());
    overflow_ = nonfiller_charge_ > 0.0 ? over / nonfiller_charge_ : 0.0;
  }

  static constexpr double kSqrt2 = 1.41421356237309504880;

  Design& d_;
  const PlacerOptions& opts_;
  const std::vector<double>& net_weight_;  // PlacerState::net_weight
  compute::Backend& be_;
  const std::vector<double>* filler_init_ = nullptr;
  int num_cells_ = 0, num_ = 0, max_degree_ = 0;
  std::vector<Id> cell_;
  std::vector<double> w_, h_;
  std::vector<bool> macro_;
  std::vector<double> filler_pos_;
  // Density footprints (DensityView) and per-object charge q = ratio * area.
  std::vector<double> charge_w_, charge_h_, ratio_, q_, pin_weight_;
  // CSR netlist (NetlistView).
  std::vector<int> net_start_, pin_obj_, pin_net_, obj_start_, obj_pins_;
  std::vector<Id> net_id_;             // local net -> design net
  std::vector<double> pin_dx_, pin_dy_, nw_, pin_scratch_;
  std::unique_ptr<compute::DensitySolver> solver_;
  double bin_w_ = 1, bin_h_ = 1;
  std::vector<double> fixed_charge_, charge_, metric_charge_;
  double die_area_ = 0, free_area_ = 0, macro_area_ = 0;
  double nonfiller_charge_ = 0;

  std::vector<double> lo_, hi_;        // clamp bounds per coordinate
  std::vector<double> u_, v_, g_, u_new_, v_new_, g_new_, gw_, gd_;
  std::vector<Point> locs_;
  double a_ = 1.0, alpha_ = 0.0, lambda_ = 0.0, gamma_ = 1.0;
  double overflow_ = 1.0, hpwl_ = 0.0, prev_hpwl_ = 0.0;
  bool started_ = false;
};

}  // namespace

// ===========================================================================
// MixedSizePlacer
// ===========================================================================

namespace {
HERMES_LOG_CHANNEL(log_gp, "place.gp");
HERMES_LOG_CHANNEL(log_rd, "place.routability");
HERMES_LOG_CHANNEL(log_td, "place.timing");
HERMES_LOG_CHANNEL(log_lg, "place.legalize");
}  // namespace

PlacementResult MixedSizePlacer::run(Design& design, StaEngine* sta,
                                     const GlobalRouter* router, RunControl* ctl) {
  state_.inflation.assign(design.num_cells(), 1.0);
  state_.net_weight.resize(design.num_nets());
  for (Id n = 0; n < static_cast<Id>(design.num_nets()); ++n) {
    state_.net_weight[n] = design.net(n).weight;
  }
  initial_placement(design);

  std::optional<CongestionMap> map;
  if (router) map.emplace(router->make_map(design));

  PlacementResult res;
  // Progress: global placement covers 0..0.85 of this run, legalization the rest.
  auto report = [&](double f, std::string_view msg) {
    if (ctl) ctl->stage_progress(f, msg);
  };
  report(0.0, "global placement: setup");
  NesterovPlacer gp(design, opts_, state_.net_weight, compute::backend());
  if (!gp.empty()) {
    double gp_done = 0.0;
    for (int it = 1; it <= opts_.max_iterations; ++it) {
      if (ctl && ctl->cancelled()) {
        res.cancelled = true;
        break;
      }
      gp.step();
      res.iterations = it;
      if (ctl) {
        // Iterations bound the run; overflow usually ends it first.
        const double by_of = (1.0 - gp.overflow()) / std::max(1e-9, 1.0 - opts_.stop_overflow);
        gp_done = std::max({gp_done, static_cast<double>(it) / opts_.max_iterations,
                            std::clamp(by_of, 0.0, 1.0)});
        report(0.85 * gp_done, std::format("global placement: iteration {}, overflow {:.3f}", it,
                                           gp.overflow()));
      }
      HLOG_AT(log_gp,
              opts_.log_interval > 0 && (it == 1 || it % opts_.log_interval == 0)
                  ? log::Level::Verbose
                  : log::Level::Debug,
              "iter {:4d}  hpwl {:.4g}  overflow {:.4f}  lambda {:.3g}  gamma {:.3g}  step {:.3g}",
              it, gp.hpwl(), gp.overflow(), gp.lambda(), gp.gamma(), gp.alpha());
      if (track::active()) {  // run tracking (live charts); one load when off
        track::metric("gp.hpwl", it, gp.hpwl());
        track::metric("gp.overflow", it, gp.overflow());
      }
      bool changed = false;
      if (sta && opts_.timing_interval > 0 && it % opts_.timing_interval == 0) {
        gp.write_back();
        update_timing_weights(design, *sta);
        changed = true;
      }
      if (router && opts_.routability_interval > 0 &&
          it % opts_.routability_interval == 0 &&
          gp.overflow() <= opts_.routability_start_overflow) {
        gp.write_back();
        if (update_routability(design, *router, *map)) {
          gp.set_inflation(state_.inflation);
          changed = true;
        }
      }
      if (changed) {
        design.flush();  // Positions were written back: publish progress.
        gp.restart();  // New objective: drop the momentum, re-estimate the step.
      } else if (gp.overflow() < opts_.stop_overflow) {
        break;
      } else if (ctl && ctl->publish_due()) {
        gp.write_back();  // Let snapshot readers (GUI) follow the spreading.
        design.flush();
      }
    }
    gp.write_back();
    for (Id ci = 0; ci < static_cast<Id>(design.num_cells()); ++ci) {
      if (design.cells()[ci].movable()) {
        clamp_to_die(design, ci);
        design.set_placed(ci, true);
      }
    }
    res.density_overflow = gp.overflow();
    design.flush();
  }
  res.global_hpwl = total_hpwl(design);
  HLOG_INFO(log_gp, "global placement: {} iterations, HPWL {:.6g}, density overflow {:.4f}{}",
            res.iterations, res.global_hpwl, res.density_overflow,
            res.cancelled ? " (cancelled)" : "");
  if (res.cancelled) {
    res.hpwl = res.global_hpwl;
    return res;
  }
  report(0.85, "legalization");

  if (opts_.legalize) {
    const auto t0 = std::chrono::steady_clock::now();
    const bool movable_macros = std::any_of(design.cells().begin(), design.cells().end(),
                                            [](const Cell& c) { return c.is_macro && c.movable(); });
    legalize_macros(design);
    design.flush();
    if (movable_macros && !gp.empty()) {
      res.refine_iterations =
          refine_after_macros(design, gp.filler_positions(), gp.lambda(), res, ctl);
    }
    legalize_cells(design);
    design.flush();
    res.legalize_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  }
  if (ctl && ctl->cancelled()) {
    res.cancelled = true;
    res.hpwl = total_hpwl(design);
    return res;
  }
  if (opts_.detailed_placement) {
    report(0.97, "detailed placement");
    detailed_place(design);
    design.flush();
  }

  res.hpwl = total_hpwl(design);
  if (sta) {
    sta->update(design);
    res.wns_ps = sta->wns();
    res.tns_ps = sta->tns();
  }
  if (router) {
    // Legalization may have moved macros: rebuild blockages before reporting.
    *map = router->make_map(design);
    router->estimate_congestion(design, *map);
    res.overflow = map->total_overflow();
    res.max_utilization = map->max_utilization();
  }
  return res;
}

void MixedSizePlacer::initial_placement(Design& design) {
  // Unplaced movable objects start at the die center; NesterovPlacer adds a
  // small jitter and spreads them from there. Placed ones (input locations,
  // e.g. DEF PLACED) stay where they are when keep_placed is set.
  // TODO(hermes): quadratic (B2B) initial placement.
  const Point c = design.die().center();
  for (Id ci = 0; ci < static_cast<Id>(design.num_cells()); ++ci) {
    const Cell& cell = design.cells()[ci];
    if (!cell.movable()) continue;
    if (opts_.keep_placed && cell.placed) continue;
    design.move_cell(ci, {c.x - cell.width / 2, c.y - cell.height / 2});
  }
}

void MixedSizePlacer::update_timing_weights(Design& design, StaEngine& sta) {
  sta.update(design);
  HLOG_VERBOSE(log_td, "net re-weighting: WNS {:.2f} ps, TNS {:.2f} ps", sta.wns(), sta.tns());
  par::parallel_for(0, design.num_nets(), 8192, [&](std::size_t lo, std::size_t hi) {
    for (std::size_t n = lo; n < hi; ++n) {
      const double crit = sta.net_criticality(static_cast<Id>(n));
      state_.net_weight[n] = design.nets()[n].weight *
                             (1.0 + opts_.timing_weight * std::pow(crit, opts_.timing_exponent));
    }
  });
}

bool MixedSizePlacer::update_routability(Design& design,
                                         const GlobalRouter& router,
                                         CongestionMap& map) {
  // Macros move during global placement, so the blockage-derated capacities
  // must be rebuilt from their current positions on every update; otherwise
  // they stay frozen where the macros were when the map was first built.
  map = router.make_map(design);
  router.estimate_congestion(design, map);
  const auto cells = design.cells();
  auto& inflation = state_.inflation;
  const bool changed = par::parallel_reduce(
      0, cells.size(), 8192, false,
      [&](std::size_t lo, std::size_t hi) {
        bool ch = false;
        for (std::size_t ci = lo; ci < hi; ++ci) {
          const Cell& cell = cells[ci];
          if (!cell.movable() || cell.is_macro) continue;  // Only inflate std cells.
          const auto [ix, iy] = map.bin_of(cell.center());
          const double u = map.utilization(ix, iy);
          if (u > opts_.inflation_threshold) {
            const double f = std::min(opts_.max_inflation, inflation[ci] * std::sqrt(u));
            ch = ch || f != inflation[ci];
            inflation[ci] = f;
          }
        }
        return ch;
      },
      [](bool a, bool b) { return a || b; });
  HLOG_VERBOSE(log_rd, "congestion update: overflow {:.4g}, max utilization {:.3f}, inflation {}",
               map.total_overflow(), map.max_utilization(), changed ? "changed" : "unchanged");
  // run() hands state_.inflation to the density term (NesterovPlacer::set_inflation).
  return changed;
}

void MixedSizePlacer::legalize_macros(Design& design) {
  // Overlap removal against fixed macros/cells, row-aligned, then fix.
  // TODO(hermes): orientation selection, manufacturing-grid snapping.
  for (Id ci = 0; ci < static_cast<Id>(design.num_cells()); ++ci) {
    const Cell& cell = design.cells()[ci];
    if (cell.is_macro && cell.movable()) clamp_to_die(design, ci);
  }
  macro_result_ = hermes::legalize_macros(design);
  HLOG_VERBOSE(log_lg, "macro legalization: {} moved, displacement {:.6g}",
               macro_result_.moved, macro_result_.total_displacement);
  if (macro_result_.unresolved > 0) {
    HLOG_WARN_ID(log_lg, "PLC-0001", "{} macro(s) could not be placed without overlap",
                 macro_result_.unresolved);
  }
  for (Id ci = 0; ci < static_cast<Id>(design.num_cells()); ++ci) {
    const Cell& cell = design.cells()[ci];
    if (cell.is_macro && !cell.removed) design.set_fixed(ci, true);
  }
}

namespace {

/// Fraction of the movable std-cell area that overlaps a macro.
double std_area_on_macros(const Design& design) {
  const auto cells = design.cells();
  std::vector<Rect> macros;
  double std_area = 0.0;
  for (const Cell& c : cells) {
    if (c.removed) continue;
    if (c.is_macro) macros.push_back(c.bbox());
    else if (c.movable()) std_area += static_cast<double>(c.width) * static_cast<double>(c.height);
  }
  if (macros.empty() || std_area <= 0.0) return 0.0;
  // Bucket the macros on a coarse grid; each cell checks its buckets only.
  const Rect& die = design.die();
  const int n = std::clamp(static_cast<int>(std::sqrt(static_cast<double>(macros.size()))) * 2, 1, 64);
  const double bw = std::max(1.0, static_cast<double>(die.width()) / n);
  const double bh = std::max(1.0, static_cast<double>(die.height()) / n);
  auto bx = [&](Dbu x) { return std::clamp(static_cast<int>((x - die.xlo) / bw), 0, n - 1); };
  auto by = [&](Dbu y) { return std::clamp(static_cast<int>((y - die.ylo) / bh), 0, n - 1); };
  std::vector<std::vector<int>> bucket(static_cast<std::size_t>(n) * n);
  for (int m = 0; m < static_cast<int>(macros.size()); ++m) {
    const Rect& r = macros[m];
    for (int iy = by(r.ylo); iy <= by(r.yhi); ++iy)
      for (int ix = bx(r.xlo); ix <= bx(r.xhi); ++ix) bucket[iy * n + ix].push_back(m);
  }
  const double over = par::parallel_reduce(
      0, cells.size(), 8192, 0.0,
      [&](std::size_t lo, std::size_t hi) {
        double sum = 0.0;
        for (std::size_t i = lo; i < hi; ++i) {
          const Cell& c = cells[i];
          if (c.removed || c.is_macro || !c.movable()) continue;
          const Rect b = c.bbox();
          // A cell is much smaller than a bucket: its lower-left bucket is
          // enough unless it straddles a bucket edge.
          for (int iy = by(b.ylo); iy <= by(b.yhi); ++iy)
            for (int ix = bx(b.xlo); ix <= bx(b.xhi); ++ix)
              for (int m : bucket[iy * n + ix]) {
                // Count each (cell, macro) pair once: in the first shared bucket.
                const Rect& r = macros[m];
                if (std::max(bx(b.xlo), bx(r.xlo)) != ix || std::max(by(b.ylo), by(r.ylo)) != iy) continue;
                sum += static_cast<double>(b.overlap_area(r));
              }
        }
        return sum;
      },
      [](double a, double b) { return a + b; });
  return over / std_area;
}

}  // namespace

int MixedSizePlacer::refine_after_macros(Design& design, const std::vector<double>& fillers,
                                         double lambda, PlacementResult& res, RunControl* ctl) {
  res.macro_overlap = std_area_on_macros(design);
  if (opts_.refine_iterations <= 0 || res.macro_overlap <= opts_.refine_min_overlap) {
    HLOG_VERBOSE(log_lg, "refinement skipped: {:.3f}% of std-cell area on macros",
                 100.0 * res.macro_overlap);
    return 0;
  }
  PlacerOptions ro = opts_;
  ro.keep_placed = true;  // Start from the global placement.
  NesterovPlacer gp(design, ro, state_.net_weight, compute::backend(), &fillers);
  if (gp.empty()) return 0;
  gp.set_inflation(state_.inflation);
  gp.set_lambda(lambda * opts_.refine_lambda_scale);
  const double h0 = total_hpwl(design);
  int it = 0;
  // Done once the density overflow is back under stop_overflow and the
  // cells are off the macros (checked every few iterations: it needs a
  // write-back).
  constexpr int kCheckEvery = 5;
  double on_macros = res.macro_overlap;
  for (it = 1; it <= opts_.refine_iterations; ++it) {
    if (ctl && ctl->cancelled()) break;
    gp.step();
    if (it % kCheckEvery == 0 && gp.overflow() < opts_.stop_overflow) {
      gp.write_back();
      on_macros = std_area_on_macros(design);
      if (on_macros <= opts_.refine_min_overlap) break;
    }
  }
  it = std::min(it, opts_.refine_iterations);
  gp.write_back();
  for (Id ci = 0; ci < static_cast<Id>(design.num_cells()); ++ci) {
    if (design.cells()[ci].movable()) clamp_to_die(design, ci);
  }
  design.flush();
  HLOG_VERBOSE(log_lg,
               "refinement: {:.3f}% of std-cell area on macros, {} iterations, overflow {:.4f}, "
               "HPWL {:.6g} -> {:.6g} ({:+.2f}%), now {:.3f}% on macros",
               100.0 * res.macro_overlap, it, gp.overflow(), h0, total_hpwl(design),
               100.0 * (total_hpwl(design) / h0 - 1.0), 100.0 * on_macros);
  return it;
}

void MixedSizePlacer::legalize_cells(Design& design) {
  // Abacus row legalization around the (now fixed) macros.
  // TODO(hermes): site-width x snapping once the db carries site info.
  cell_result_ = abacus_legalize(design);
  HLOG_VERBOSE(log_lg, "cell legalization: {} legalized ({} multi-row), displacement {:.6g}",
               cell_result_.legalized, cell_result_.multi_row, cell_result_.total_displacement);
  if (cell_result_.failed > 0) {
    HLOG_WARN_ID(log_lg, "PLC-0002", "{} standard cell(s) did not fit in any row (left clamped)",
                 cell_result_.failed);
  }
}

void MixedSizePlacer::detailed_place(Design& /*design*/) {
  // TODO(hermes): timing-aware swaps, independent-set matching, local reorder.
}

void MixedSizePlacer::clamp_to_die(Design& design, Id ci) const {
  const Rect& die = design.die();
  const Cell& cell = design.cell(ci);
  const Point p{std::clamp(cell.loc.x, die.xlo, std::max(die.xlo, die.xhi - cell.width)),
                std::clamp(cell.loc.y, die.ylo, std::max(die.ylo, die.yhi - cell.height))};
  if (p != cell.loc) design.move_cell(ci, p);
}

}  // namespace hermes
