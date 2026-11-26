#pragma once

// LEF/DEF readers that fill a hermes::Design.
//
// Scope: what placement needs from ISPD 2015 / ICCAD 2015 style benchmarks.
//   LEF: UNITS, SITE (size), MACRO (class, size, origin, pin direction/use and
//        pin shapes). Technology sections (LAYER, VIA, VIARULE, SPACING, ...)
//        and OBS are skipped.
//   DEF: DESIGN, UNITS, DIEAREA, ROW, COMPONENTS, PINS, NETS. Routing-related
//        and other sections (TRACKS, GCELLGRID, VIAS, SPECIALNETS, BLOCKAGES,
//        REGIONS, GROUPS, ...) are skipped. Routed wiring inside NETS is
//        skipped too.
//
// Conventions in the resulting Design:
//   * All geometry is in DEF database units (DEF `UNITS DISTANCE MICRONS`).
//     LEF microns are converted with that factor.
//   * Cell width/height and pin offsets reflect the component's DEF
//     orientation (E/W/FE/FW swap width and height).
//   * MACRO CLASS BLOCK becomes `is_macro`. FIXED/COVER components are `fixed`.
//   * Top-level I/O pins have `cell == kInvalidId` and an absolute location.
//     Their direction is stored from the net's point of view: a DEF INPUT
//     port drives its net, so it is stored as PinDir::Output (and vice versa).
//   * Row height is the height of the site used by most ROWs.
//   * Only pins that appear in NETS are created (the DB has no unconnected
//     pins). Power/ground pins are therefore usually absent.

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "hermes/db/design.hpp"
#include "hermes/io/common.hpp"

namespace hermes::io {

/// Hash for string-keyed maps that accept std::string_view lookups.
struct StringHash {
  using is_transparent = void;
  std::size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
};
template <class V>
using StringMap = std::unordered_map<std::string, V, StringHash, std::equal_to<>>;

enum class PinUse : unsigned char { Signal, Clock, Power, Ground, Analog, Other };

struct LefPin {
  std::string name;
  PinDir dir = PinDir::Input;
  PinUse use = PinUse::Signal;
  /// Center of the bounding box of the pin's PORT shapes, in microns,
  /// relative to the macro's lower-left corner (ORIGIN already applied).
  double x = 0.0;
  double y = 0.0;
  bool has_shape = false;
};

struct LefMacro {
  std::string name;
  std::string cls;          ///< e.g. "CORE", "BLOCK", "PAD", "CORE TIEHIGH".
  double width = 0.0;       ///< Microns.
  double height = 0.0;
  bool is_block() const { return cls.rfind("BLOCK", 0) == 0; }

  const std::vector<LefPin>& pins() const { return pins_; }
  const LefPin* find_pin(std::string_view pin) const;
  /// Adds a pin, replacing an existing pin of the same name.
  void add_pin(LefPin p);

 private:
  std::vector<LefPin> pins_;
  StringMap<std::size_t> pin_index_;
};

struct LefSite {
  std::string name;
  double width = 0.0;       ///< Microns.
  double height = 0.0;
};

/// A routing layer from a technology LEF (LAYER ... TYPE ROUTING). Microns.
struct LefLayer {
  std::string name;
  bool horizontal = true;   ///< DIRECTION HORIZONTAL (else VERTICAL).
  double pitch = 0.0;       ///< PITCH (first value when two are given).
  double width = 0.0;       ///< WIDTH
  double spacing = 0.0;     ///< First plain SPACING value.
};

/// Cell library accumulated from one or more LEF files (e.g. tech.lef then
/// cells.lef). Later definitions of the same name replace earlier ones.
class LefLibrary {
 public:
  double database_microns = 0.0;   ///< LEF `UNITS DATABASE MICRONS` (0 if absent).
  /// Routing layers in LEF order (bottom to top). A later definition of the
  /// same name replaces the earlier one in place.
  std::vector<LefLayer> routing_layers;
  void add_routing_layer(LefLayer l);

  const LefMacro* find_macro(std::string_view name) const;
  const LefSite* find_site(std::string_view name) const;
  std::size_t num_macros() const { return macros_.size(); }
  std::size_t num_sites() const { return sites_.size(); }

  LefMacro& add_macro(LefMacro m);
  template <class F>
  void for_each_macro(F&& f) const {
    for (const auto& [name, m] : macros_) f(m);
  }
  LefSite& add_site(LefSite s);

 private:
  StringMap<LefMacro> macros_;
  StringMap<LefSite> sites_;
};

/// Parse LEF text into `lib`. `source` is used in error messages.
void read_lef_string(std::string_view text, LefLibrary& lib,
                     std::string_view source = "<lef>");
/// Parse a LEF file into `lib`.
void read_lef(const std::string& path, LefLibrary& lib);

struct DefReadReport {
  std::string design_name;
  double dbu_per_micron = 0.0;
  std::size_t num_components = 0;
  std::size_t num_macros = 0;
  std::size_t num_fixed = 0;
  std::size_t num_unplaced = 0;
  std::size_t num_io_pins = 0;
  std::size_t num_nets = 0;
  std::size_t num_pins = 0;          ///< Pins created in the Design.
  std::size_t num_rows = 0;
  Dbu site_width = 0;                ///< Width of the majority row site.
  std::vector<std::string> warnings; ///< Capped at max_warnings.
  std::size_t num_warnings = 0;      ///< Total, including ones not stored.
};

struct DefReadOptions {
  /// If false, a component whose macro is not in the library is an error.
  /// If true, it becomes a zero-size cell and a warning is recorded.
  bool allow_unknown_macros = false;
  std::size_t max_warnings = 50;
};

/// Parse DEF text against `lib` and build a Design.
Design read_def_string(std::string_view text, const LefLibrary& lib,
                       DefReadReport* report = nullptr,
                       const DefReadOptions& opts = {},
                       std::string_view source = "<def>");
/// Parse a DEF file against `lib` and build a Design.
Design read_def(const std::string& path, const LefLibrary& lib,
                DefReadReport* report = nullptr, const DefReadOptions& opts = {});

/// Convenience: read every LEF in order, then the DEF.
Design read_lef_def(const std::vector<std::string>& lef_paths,
                    const std::string& def_path, DefReadReport* report = nullptr,
                    const DefReadOptions& opts = {});

struct DefWriteOptions {
  /// Master written for cells without one (e.g. read from Bookshelf):
  /// "<prefix><width>x<height>" in DBU.
  std::string missing_master_prefix = "HERMES_";
  /// Write nets (COMPONENTS and PINS only when false: a placement DEF).
  bool nets = true;
};

/// Writes the design as DEF 5.8: DIEAREA, ROW, COMPONENTS (PLACED / FIXED /
/// UNPLACED with orientation), PINS (top-level I/O pins, PLACED at their
/// location), NETS (connectivity, + WEIGHT for weights != 1). Removed objects
/// and unconnected pins are skipped. Formatting runs in parallel.
/// Reading the result back with the same LEF gives the same placement.
void write_def(const Design& d, const std::string& path, const DefWriteOptions& opts = {});

}  // namespace hermes::io
