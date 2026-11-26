#include "hermes/route/detailed_router.hpp"

#include "hermes/log/log.hpp"

namespace hermes {

namespace {
HERMES_LOG_CHANNEL(log_drt, "route.detailed");
}  // namespace

DetailedRouteResult DetailedRouter::route(const Design& /*design*/,
                                          const GlobalRouteResult& /*guides*/) {
  // Not implemented yet. Returning completed=false keeps the flow honest.
  HLOG_INFO_ID(log_drt, "DRT-0001", "detailed routing is not implemented yet; skipped");
  return {};
}

}  // namespace hermes
