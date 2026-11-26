#pragma once

// Reader / writer plugins and directory input.
//
// Every input format is a Reader plugin and every output format a Writer
// plugin, registered by name in the Registry. Readers only fill a Design;
// engines only see the Design, never a file format. Built-ins:
//
//   readers  lefdef     LEF (tech + cells) + one DEF
//            bookshelf  .aux (+ .nodes .nets .pl .scl .wts .route)
//            hdb        hermes design file (save/restore)
//   writers  hdb        hermes design file (.hdb)
//            def        DEF 5.8 (.def)
//            pl         Bookshelf placement (.pl)
//            bookshelf  full Bookshelf set (path is a directory or .aux)
//
// Directory input:  load_input("path/to/dir")
//   1. If the directory has a manifest file `hermes.inputs`, it decides
//      what is read (and in which order). Syntax, one entry per line:
//          # comment
//          format lefdef              (optional: inferred from the roles)
//          lef    tech.lef            (role + path relative to the manifest;
//          lef    cells/*.lef          '*' and '?' allowed in the file name)
//          def    design.def
//          option def.allow_unknown_macros=1
//   2. Otherwise every reader looks at the files and proposes what it would
//      read (LEF: technology LEFs first, then the others by name). Exactly
//      one proposal is read. None, or several (e.g. two DEFs, or a DEF and a
//      .aux side by side), stops with an InputError that lists what was
//      found, so the user can add a manifest or point at a file.
//   A file path works too: a .def (LEFs from its directory), a .aux, a .hdb
//   or a manifest.
//
// Adding a format: implement Reader (detect + read) or Writer, and call
// Registry::global().add_reader / add_writer at startup.

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "hermes/db/design.hpp"
#include "hermes/io/common.hpp"

namespace hermes::io {

/// Input that cannot be resolved (nothing found, ambiguous, bad manifest).
class InputError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

inline constexpr std::string_view kManifestName = "hermes.inputs";

struct InputItem {
  std::string role;  ///< Reader-defined: "lef", "def", "aux", "nodes", "hdb", ...
  std::string path;
};

/// What to read: the reader and its files in order.
struct InputPlan {
  std::string format;
  std::vector<InputItem> items;
  Options options;                 ///< From manifest "option" lines.
  std::string origin;              ///< "manifest ...", "detected in ...", "file ..."
  std::vector<std::string> paths(std::string_view role) const;
  std::string describe() const;    ///< One line per item, for logs.
};

struct LoadInfo {
  InputPlan plan;
  double seconds = 0.0;
  std::vector<std::pair<std::string, std::string>> stats;  ///< Reader-specific summary.
  std::size_t num_warnings = 0;
};

class Reader {
 public:
  virtual ~Reader() = default;
  virtual std::string_view name() const = 0;
  virtual std::string_view description() const = 0;
  /// Roles this reader accepts in a manifest.
  virtual std::vector<std::string_view> roles() const = 0;
  /// Looks at the (sorted) regular files of a directory. Returns the plans
  /// it could read: none (not its files), one, or several (ambiguous).
  /// `problem` explains files it recognized but cannot use as they are.
  virtual std::vector<InputPlan> detect(const std::vector<std::filesystem::path>& files,
                                        std::string& problem) const = 0;
  /// A plan for one file given on the command line, if this reader takes it.
  virtual bool accepts_file(const std::filesystem::path& file, InputPlan& plan) const = 0;
  virtual Design read(const InputPlan& plan, const Options& opts, LoadInfo& info) const = 0;
};

class Writer {
 public:
  virtual ~Writer() = default;
  virtual std::string_view name() const = 0;
  virtual std::string_view description() const = 0;
  /// File extensions that select this writer (".def"); may be empty.
  virtual std::vector<std::string_view> extensions() const = 0;
  virtual void write(const Design& d, const std::string& path, const Options& opts) const = 0;
};

class Registry {
 public:
  /// The process-wide registry with the built-in plugins.
  static Registry& global();

  void add_reader(std::unique_ptr<Reader> r);
  void add_writer(std::unique_ptr<Writer> w);
  const Reader* reader(std::string_view name) const;
  const Writer* writer(std::string_view name) const;
  /// Writer chosen by the file extension, or nullptr.
  const Writer* writer_for(std::string_view path) const;
  const std::vector<std::unique_ptr<Reader>>& readers() const { return readers_; }
  const std::vector<std::unique_ptr<Writer>>& writers() const { return writers_; }

 private:
  std::vector<std::unique_ptr<Reader>> readers_;
  std::vector<std::unique_ptr<Writer>> writers_;
};

/// Parses a manifest file into a plan (paths resolved against its dir).
InputPlan read_manifest(const std::string& path, const Registry& reg = Registry::global());

/// Decides what to read from a directory, manifest or single file. Throws
/// InputError (listing what was found) when nothing or more than one
/// candidate fits. The choice and the files are logged (channel io).
InputPlan plan_input(const std::string& path, const Registry& reg = Registry::global());

/// Reads a plan. `opts` override the plan's manifest options.
Design load_plan(const InputPlan& plan, const Options& opts = {}, LoadInfo* info = nullptr,
                 const Registry& reg = Registry::global());

/// plan_input + load_plan.
Design load_input(const std::string& path, const Options& opts = {}, LoadInfo* info = nullptr,
                  const Registry& reg = Registry::global());

/// Writes with the named writer, or the one picked by the path's extension
/// when `format` is empty. Throws std::invalid_argument for an unknown one.
void write_design(const Design& d, const std::string& path, std::string_view format = {},
                  const Options& opts = {}, const Registry& reg = Registry::global());

}  // namespace hermes::io
