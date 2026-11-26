#include "assist/run_history.hpp"

#include <format>

#include "hermes/track/run_tracker.hpp"

#if HERMES_HAVE_SQLITE
#include <sqlite3.h>
#endif

namespace hermes::assist {

#if HERMES_HAVE_SQLITE
namespace {

struct Db {
  sqlite3* db = nullptr;
  ~Db() {
    if (db) sqlite3_close(db);
  }
};

bool open_ro(const std::string& path_in, Db& d, std::string* err) {
  const std::string path = path_in.empty() ? track::default_db_path() : path_in;
  if (sqlite3_open_v2(path.c_str(), &d.db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    *err = std::format("cannot open run database {} (no runs tracked yet?)", path);
    return false;
  }
  sqlite3_busy_timeout(d.db, 2000);
  return true;
}

Json col(sqlite3_stmt* st, int i) {
  switch (sqlite3_column_type(st, i)) {
    case SQLITE_INTEGER: return Json(static_cast<long long>(sqlite3_column_int64(st, i)));
    case SQLITE_FLOAT: return Json(sqlite3_column_double(st, i));
    case SQLITE_TEXT: return Json(std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, i))));
    default: return Json();
  }
}

// JSON text columns come back parsed (summary, settings, results).
Json json_col(sqlite3_stmt* st, int i, std::size_t max_chars = 6000) {
  Json v = col(st, i);
  if (!v.is_string()) return v;
  if (v.str_ref().size() > max_chars) return Json(v.str_ref().substr(0, max_chars) + "... [cut]");
  Json p = Json::parse(v.str_ref());
  return p.is_null() ? v : p;
}

Json query(sqlite3* db, const char* sql, const std::vector<std::string>& binds, std::string* err,
           const std::vector<std::string>& json_cols = {}) {
  sqlite3_stmt* st = nullptr;
  Json rows = Json::array();
  if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
    *err = std::string("run database: ") + sqlite3_errmsg(db);
    return rows;
  }
  for (std::size_t i = 0; i < binds.size(); ++i)
    sqlite3_bind_text(st, static_cast<int>(i) + 1, binds[i].c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(st) == SQLITE_ROW) {
    Json r = Json::object();
    for (int c = 0; c < sqlite3_column_count(st); ++c) {
      const std::string name = sqlite3_column_name(st, c);
      bool js = false;
      for (const auto& j : json_cols) js |= j == name;
      r.set(name, js ? json_col(st, c) : col(st, c));
    }
    rows.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  return rows;
}

}  // namespace

Json list_runs(const std::string& db_path, const std::string& design, int limit, std::string* err) {
  Db d;
  if (!open_ro(db_path, d, err)) return {};
  const std::string sql = std::format(
      "SELECT id, name, status, datetime(started, 'unixepoch', 'localtime') AS started,"
      " round(coalesce(ended, heartbeat) - started, 3) AS seconds, design, threads, version, tags, summary, error"
      " FROM runs {} ORDER BY started DESC LIMIT {}",
      design.empty() ? "" : "WHERE design = ?1", limit);
  Json rows = query(d.db, sql.c_str(), design.empty() ? std::vector<std::string>{} : std::vector<std::string>{design},
                    err, {"summary", "tags"});
  return Json::object({{"database", db_path.empty() ? track::default_db_path() : db_path}, {"runs", std::move(rows)}});
}

Json run_details(const std::string& db_path, const std::string& run_id, std::string* err) {
  Db d;
  if (!open_ro(db_path, d, err)) return {};
  const std::string like = run_id + "%";
  Json rows = query(d.db,
                    "SELECT id, name, status, datetime(started, 'unixepoch', 'localtime') AS started,"
                    " round(coalesce(ended, heartbeat) - started, 3) AS seconds, design, input, command, host, cpu,"
                    " cores, threads, version, tags, settings, summary, results, exit_code, error, peak_rss"
                    " FROM runs WHERE id LIKE ?1 ORDER BY started DESC LIMIT 2",
                    {like}, err, {"settings", "summary", "results", "tags"});
  if (!err->empty()) return {};
  if (rows.size() == 0) {
    *err = "no run with id '" + run_id + "'";
    return {};
  }
  if (rows.size() > 1) {
    *err = "run id prefix '" + run_id + "' is ambiguous; give more characters";
    return {};
  }
  Json r = rows[0];
  const std::string id = r["id"].as_string();
  r.set("stages", query(d.db,
                        "SELECT name, round(ended - started, 3) AS seconds FROM stages WHERE run_id = ?1 ORDER BY seq",
                        {id}, err));
  r.set("warnings_and_errors",
        query(d.db,
              "SELECT round(t, 3) AS t, level, channel, msg_id, text FROM events WHERE run_id = ?1 AND kind = 'log'"
              " AND lower(level) IN ('warn', 'warning', 'error') ORDER BY seq DESC LIMIT 30",
              {id}, err));
  return r;
}

#else

Json list_runs(const std::string&, const std::string&, int, std::string* err) {
  *err = "this hermes build has no SQLite, so there is no local run database";
  return {};
}
Json run_details(const std::string&, const std::string&, std::string* err) {
  *err = "this hermes build has no SQLite, so there is no local run database";
  return {};
}

#endif

}  // namespace hermes::assist
