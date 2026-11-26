#include "hermes/db/geometry.hpp"

#include <algorithm>
#include <limits>

#include "hermes/par/parallel.hpp"

namespace hermes {

Rect net_bbox(const Design& d, Id net) {
  const auto pins = d.net_pins(net);
  if (pins.empty()) return {};
  Rect r{std::numeric_limits<Dbu>::max(), std::numeric_limits<Dbu>::max(),
         std::numeric_limits<Dbu>::min(), std::numeric_limits<Dbu>::min()};
  const auto cells = d.cells();
  const auto all_pins = d.pins();
  for (Id pid : pins) {
    const Pin& p = all_pins[static_cast<std::size_t>(pid)];
    const Point q = p.cell == kInvalidId ? p.offset : cells[static_cast<std::size_t>(p.cell)].loc + p.offset;
    r.xlo = std::min(r.xlo, q.x);
    r.ylo = std::min(r.ylo, q.y);
    r.xhi = std::max(r.xhi, q.x);
    r.yhi = std::max(r.yhi, q.y);
  }
  return r;
}

Dbu net_hpwl(const Design& d, Id net) {
  if (d.net_pins(net).size() < 2) return 0;
  const Rect r = net_bbox(d, net);
  return r.width() + r.height();
}

double total_hpwl(const Design& d, bool weighted) {
  const auto nets = d.nets();
  return par::parallel_sum(0, nets.size(), 4096, [&](std::size_t lo, std::size_t hi) {
    double sum = 0.0;
    for (std::size_t i = lo; i < hi; ++i) {
      const double w = weighted ? nets[i].weight : 1.0;
      sum += w * static_cast<double>(net_hpwl(d, static_cast<Id>(i)));
    }
    return sum;
  });
}

double total_hpwl(const Design& d, std::span<const double> w) {
  return par::parallel_sum(0, d.num_nets(), 4096, [&](std::size_t lo, std::size_t hi) {
    double sum = 0.0;
    for (std::size_t i = lo; i < hi; ++i) {
      sum += w[i] * static_cast<double>(net_hpwl(d, static_cast<Id>(i)));
    }
    return sum;
  });
}

std::size_t count_macros(const Design& d) {
  const auto cells = d.cells();
  return static_cast<std::size_t>(std::count_if(
      cells.begin(), cells.end(), [](const Cell& c) { return c.is_macro && !c.removed; }));
}

// ---------------------------------------------------------------------------
// NetBoxCache
// ---------------------------------------------------------------------------

NetBoxCache::NetBoxCache(Design& design) : d_(design) { d_.subscribe(this); }

NetBoxCache::~NetBoxCache() { d_.unsubscribe(this); }

void NetBoxCache::on_flush(const Design& design, const ChangeSet& ch) {
  if (all_) return;
  if (ch.all || design.num_nets() != boxes_.size()) {
    all_ = true;
    dirty_.clear();
    return;
  }
  for (Id n : ch.nets.ids()) dirty_.mark(n);
}

void NetBoxCache::recompute(Id n) {
  const std::size_t i = static_cast<std::size_t>(n);
  total_ -= hpwl_[i];
  boxes_[i] = net_bbox(d_, n);
  hpwl_[i] = d_.net_pins(n).size() < 2 ? 0 : boxes_[i].width() + boxes_[i].height();
  total_ += hpwl_[i];
}

void NetBoxCache::refresh() {
  if (all_) {
    const std::size_t n = d_.num_nets();
    boxes_.assign(n, Rect{});
    hpwl_.assign(n, 0);
    total_ = par::parallel_sum(0, n, 4096, [&](std::size_t lo, std::size_t hi) {
      std::int64_t t = 0;
      for (std::size_t i = lo; i < hi; ++i) {
        boxes_[i] = net_bbox(d_, static_cast<Id>(i));
        hpwl_[i] = d_.net_pins(static_cast<Id>(i)).size() < 2 ? 0
                                                              : boxes_[i].width() + boxes_[i].height();
        t += hpwl_[i];
      }
      return t;
    });
    last_count_ = n;
    all_ = false;
    dirty_.clear();
    return;
  }
  last_count_ = dirty_.size();
  for (Id n : dirty_.ids()) recompute(n);
  dirty_.clear();
}

}  // namespace hermes

#include <stdexcept>

#include "hermes/db/bin_grid.hpp"

namespace hermes {

BinGrid::BinGrid(const Rect& region, int nx, int ny) : region_(region), nx_(nx), ny_(ny) {
  if (nx <= 0 || ny <= 0 || region.width() <= 0 || region.height() <= 0) {
    throw std::invalid_argument("BinGrid: empty grid or region");
  }
  bin_w_ = std::max<Dbu>(1, region.width() / nx);
  bin_h_ = std::max<Dbu>(1, region.height() / ny);
}

}  // namespace hermes
