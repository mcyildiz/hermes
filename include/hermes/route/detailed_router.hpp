#pragma once

#include "hermes/db/design.hpp"
#include "hermes/route/global_router.hpp"

namespace hermes {

struct DetailedRouteResult {
  bool completed = false;
  int drc_violations = -1;  ///< -1 = not computed.
};

/// Track-level router that follows the global-route guides.
class DetailedRouter {
 public:
  /// TODO(hermes): track assignment, pin access, search-and-repair DRC fixing.
  DetailedRouteResult route(const Design& design, const GlobalRouteResult& guides);
};

}  // namespace hermes
