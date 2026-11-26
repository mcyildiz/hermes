#pragma once

// Bulk access to a Design's arrays, for readers and the design-file loader
// only. Engines never include this: they go through the Design's edit ops.
//
// A bulk loader fills the arrays directly (in parallel), then calls
// finish(), which checks that the arrays agree with each other and marks
// the whole design as changed. Undo is not available for bulk loads (no
// transaction may be open).

#include <stdexcept>

#include "hermes/db/design.hpp"

namespace hermes::detail {

struct DesignAccess {
  static std::string& name(Design& d) { return d.name_; }
  static std::vector<Cell>& cells(Design& d) { return d.cells_; }
  static std::vector<Net>& nets(Design& d) { return d.nets_; }
  static std::vector<Pin>& pins(Design& d) { return d.pins_; }
  static IdListPool& net_pins(Design& d) { return d.net_pins_; }
  static IdListPool& cell_pins(Design& d) { return d.cell_pins_; }
  static std::vector<Id>& io_pins(Design& d) { return d.io_pins_; }
  static NameTable& cell_names(Design& d) { return d.cell_names_; }
  static NameTable& net_names(Design& d) { return d.net_names_; }
  static NameTable& pin_names(Design& d) { return d.pin_names_; }
  static std::vector<Id>& pin_name_ids(Design& d) { return d.pin_name_ids_; }
  static NameTable& site_names(Design& d) { return d.site_names_; }
  static NameTable& master_names(Design& d) { return d.master_names_; }
  static std::vector<Id>& cell_master(Design& d) { return d.cell_master_; }
  static std::vector<Row>& rows(Design& d) { return d.rows_; }

  /// Validates sizes and marks everything changed. Throws
  /// std::runtime_error if the arrays are inconsistent.
  static void finish(Design& d) {
    if (d.in_transaction()) throw std::logic_error("bulk load inside a transaction");
    auto bad = [](const char* what) { throw std::runtime_error(std::string("bulk load: ") + what); };
    if (d.cell_names_.size() != d.cells_.size()) bad("cell names do not match cells");
    if (d.net_names_.size() != d.nets_.size()) bad("net names do not match nets");
    if (d.cell_pins_.num_lists() != d.cells_.size()) bad("cell pin lists do not match cells");
    if (d.net_pins_.num_lists() != d.nets_.size()) bad("net pin lists do not match nets");
    if (d.pin_name_ids_.size() != d.pins_.size()) bad("pin names do not match pins");
    if (d.cell_master_.size() > d.cells_.size()) bad("more masters than cells");
    d.pending_.clear();
    d.pending_.all = true;
    d.pending_.structure = d.pending_.floorplan = true;
    d.update_hooks();
  }
};

}  // namespace hermes::detail
