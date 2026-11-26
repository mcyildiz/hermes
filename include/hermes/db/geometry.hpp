#pragma once

// Shared geometric queries over a Design. They are free functions (not
// Design members) so the core database stays small; every engine uses the
// same definitions. NetBoxCache is the incremental form of net_bbox/HPWL.

#include <cstdint>
#include <span>
#include <vector>

#include "hermes/db/design.hpp"

namespace hermes {

/// Absolute location of a pin given the current placement.
inline Point pin_location(const Design& d, Id pin) {
  const Pin& p = d.pin(pin);
  if (p.cell == kInvalidId) return p.offset;  // Top-level I/O: absolute.
  return d.cells()[static_cast<std::size_t>(p.cell)].loc + p.offset;
}

/// Bounding box of all pins on a net ({} for a net without pins).
Rect net_bbox(const Design& d, Id net);

/// Half-perimeter wirelength of one net (0 below two pins).
Dbu net_hpwl(const Design& d, Id net);

/// Sum of net HPWL; `weighted` uses Net::weight.
double total_hpwl(const Design& d, bool weighted = false);
/// Sum of w[net] * HPWL(net) with an engine's own weights (size = num_nets).
double total_hpwl(const Design& d, std::span<const double> net_weights);

/// Live macros.
std::size_t count_macros(const Design& d);

/// Per-net bounding boxes and total HPWL, kept up to date incrementally.
///
/// Subscribes to the design; on each flush it marks the dirty nets, and the
/// next query recomputes only those. A flush with `all` (or a structural
/// change in array size) recomputes everything. Must not outlive the design.
class NetBoxCache final : public DesignListener {
 public:
  explicit NetBoxCache(Design& design);
  ~NetBoxCache() override;
  NetBoxCache(const NetBoxCache&) = delete;
  NetBoxCache& operator=(const NetBoxCache&) = delete;

  void on_flush(const Design& design, const ChangeSet& changes) override;

  /// Recomputes dirty nets now (queries do it on demand).
  void refresh();
  const Rect& bbox(Id net) {
    refresh();
    return boxes_[static_cast<std::size_t>(net)];
  }
  /// Exact integer sum of net HPWL.
  std::int64_t total_hpwl() {
    refresh();
    return total_;
  }
  /// Nets recomputed by the last refresh().
  std::size_t last_refresh_count() const { return last_count_; }

 private:
  void recompute(Id net);

  Design& d_;
  std::vector<Rect> boxes_;
  std::vector<Dbu> hpwl_;
  std::int64_t total_ = 0;
  DirtySet dirty_;
  bool all_ = true;
  std::size_t last_count_ = 0;
};

}  // namespace hermes
