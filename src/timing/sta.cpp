#include "hermes/timing/sta.hpp"

#include <algorithm>

#include "hermes/db/geometry.hpp"
#include "hermes/par/parallel.hpp"

namespace hermes {

void WireDelaySta::update(const Design& design) {
  // TODO(hermes): replace with graph-based STA (Liberty cell delays, SDC
  // constraints, arrival/required propagation). This model treats every net
  // as an independent single-stage path launched and captured at the clock.
  // A graph STA parallelizes by levels (all nodes of one topological level
  // at once), which is also how GPU timers (e.g. GPU-accelerated OpenTimer
  // variants, INSTA) work.
  const std::size_t n = design.num_nets();
  criticality_.resize(n);
  struct Acc {
    double worst = 0.0, wns = 0.0, tns = 0.0;
  };
  // Pass 1: delays (kept in criticality_) and slack statistics, reduced in
  // block order (deterministic).
  const Acc acc = par::parallel_reduce(
      0, n, 4096, Acc{},
      [&](std::size_t lo, std::size_t hi) {
        Acc a;
        for (std::size_t i = lo; i < hi; ++i) {
          const double delay = ps_per_dbu_ * static_cast<double>(net_hpwl(design, static_cast<Id>(i)));
          criticality_[i] = delay;
          a.worst = std::max(a.worst, delay);
          const double slack = period_ps_ - delay;
          if (slack < 0.0) {
            a.wns = std::min(a.wns, slack);
            a.tns += slack;
          }
        }
        return a;
      },
      [](Acc a, const Acc& b) {
        return Acc{std::max(a.worst, b.worst), std::min(a.wns, b.wns), a.tns + b.tns};
      });
  wns_ = acc.wns;
  tns_ = acc.tns;
  // Pass 2: criticality = delay / worst delay.
  par::parallel_for(0, n, 16384, [&](std::size_t lo, std::size_t hi) {
    for (std::size_t i = lo; i < hi; ++i) criticality_[i] = acc.worst > 0.0 ? criticality_[i] / acc.worst : 0.0;
  });
}

double WireDelaySta::net_criticality(Id net) const {
  if (net < 0 || net >= static_cast<Id>(criticality_.size())) return 0.0;
  return criticality_[net];
}

}  // namespace hermes
