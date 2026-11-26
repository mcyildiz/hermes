#pragma once

// Read-only queries on the run-tracking database (track/run_tracker.hpp)
// for the assistant's run_history tool.

#include <string>

#include "assist/json.hpp"

namespace hermes::assist {

/// Latest runs (optionally of one design). `db_path` "" = default database.
Json list_runs(const std::string& db_path, const std::string& design, int limit, std::string* err);
/// One run by id (or unique id prefix): settings, stages, results, warnings/errors.
Json run_details(const std::string& db_path, const std::string& run_id, std::string* err);

}  // namespace hermes::assist
