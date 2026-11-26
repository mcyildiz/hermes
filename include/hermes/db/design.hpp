#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "hermes/core/types.hpp"
#include "hermes/db/changes.hpp"
#include "hermes/db/floorplan.hpp"
#include "hermes/db/id_list_pool.hpp"
#include "hermes/db/name_table.hpp"

namespace hermes {

// ---------------------------------------------------------------------------
// Core objects: plain data only.
//
// They hold what every engine reads and nothing an individual algorithm owns.
// Per-algorithm state (net weights from timing, inflation, density charges,
// routes, ...) lives in that algorithm's module, in arrays indexed by the same
// ids (see e.g. PlacerState). Names live in the Design's NameTables, and
// connectivity in flat id-list pools (Design::net_pins / cell_pins), so hot
// loops over these arrays touch neither names nor heap pointers.
// ---------------------------------------------------------------------------

struct Pin {
  Id cell = kInvalidId;     ///< Owning cell (kInvalidId for a top-level I/O pin).
  Id net = kInvalidId;      ///< kInvalidId while unconnected.
  Point offset;             ///< From the cell's lower-left corner; absolute for I/O.
  PinDir dir = PinDir::Input;
};

struct Cell {
  Point loc;                ///< Lower-left corner.
  Dbu width = 0;
  Dbu height = 0;
  Orient orient = Orient::N;
  bool is_macro = false;    ///< Mixed-size placement treats macros specially.
  bool fixed = false;       ///< Fixed cells/macros are never moved by engines.
  bool removed = false;     ///< Tombstone: id stays valid, the cell is gone.
  /// The location is meaningful: given by the input (DEF PLACED/FIXED, a
  /// placed .pl) or written by a placer. Initial placement keeps placed
  /// cells where they are and spreads the others from scratch.
  bool placed = false;

  Rect bbox() const { return {loc.x, loc.y, loc.x + width, loc.y + height}; }
  Point center() const { return {loc.x + width / 2, loc.y + height / 2}; }
  /// Live and not fixed: what placers may move.
  bool movable() const { return !fixed && !removed; }
};

struct Net {
  double weight = 1.0;      ///< Input weight (e.g. DEF NETS + WEIGHT). Engines
                            ///< keep their own working weights (PlacerState).
  bool removed = false;     ///< Tombstone.
};

class Design;
namespace detail {
struct DesignAccess;
}

/// Undo scope. Every change made to the design while a Transaction is open is
/// recorded; rollback() restores the state at construction, commit() keeps
/// it. Destroying an open transaction rolls it back (exception safety).
/// Transactions nest (inner ones must end first). With no transaction open,
/// nothing is recorded and changes cost nothing extra.
///
/// Rollback goes through change tracking like any other edit, so listeners
/// see the restored cells/nets as dirty on the next flush.
class Transaction {
 public:
  explicit Transaction(Design& design);
  ~Transaction();
  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;

  void commit();
  void rollback();
  bool active() const { return active_; }
  /// Undo records held for this transaction so far.
  std::size_t size() const;

 private:
  void end();

  Design* d_;
  std::size_t mark_;
  std::size_t depth_;
  bool active_ = true;
};

/// In-memory netlist + floorplan: the one database every reader fills and
/// every engine reads.
///
/// The Design is deliberately small. It owns flat arrays of cells, nets and
/// pins (ids are indices, stable for the design's lifetime: removal leaves a
/// tombstone) and the name tables, and offers only operations that keep them
/// consistent. Algorithms read through the const accessors and change the
/// design only through the mutators below; queries such as pin locations,
/// net bboxes or HPWL are free functions in "hermes/db/geometry.hpp".
///
/// Change tracking: every mutation records what it touched (see ChangeSet).
/// flush() hands the accumulated set to the subscribed listeners (congestion
/// maps, STA, snapshot publisher, ...) so they update only what changed, then
/// starts a new set and bumps version(). Tracking is off by default (opt in
/// with set_tracking(true)); while off,
/// mutations record nothing and the next flush reports `all`. A fresh design
/// also starts at `all`, so bulk loading never pays for tracking.
///
/// Threading: one writer at a time. The design itself is not safe to read
/// while another thread writes it; concurrent readers (e.g. a GUI) read
/// immutable snapshots instead (see "hermes/db/snapshot.hpp").
class Design {
 public:
  explicit Design(std::string name = "top");
  ~Design();
  /// Copies the data (not listeners, not open transactions, not pending
  /// changes: the copy starts at `all`).
  Design(const Design& o);
  Design& operator=(const Design& o);
  Design(Design&& o) noexcept;
  Design& operator=(Design&& o) noexcept;

  const std::string& name() const { return name_; }

  // --- Floorplan -----------------------------------------------------------
  void set_die(const Rect& die);
  const Rect& die() const { return die_; }
  void set_row_height(Dbu h);
  Dbu row_height() const { return row_height_; }
  /// Database units per micron (DEF UNITS DISTANCE MICRONS). 0 = unknown.
  /// Bookshelf inputs have no physical unit: there it is the reader's scale.
  void set_dbu_per_micron(double v);
  double dbu_per_micron() const { return dbu_per_micron_; }
  /// Placement rows. Readers set them; set_rows also sets row_height to the
  /// most common row height when rows are given.
  void set_rows(std::vector<Row> rows);
  std::span<const Row> rows() const { return rows_; }
  /// Site names referenced by Row::site.
  Id intern_site_name(std::string_view name) { return site_names_.intern(name); }
  std::string_view site_name(Id id) const { return id == kInvalidId ? std::string_view() : site_names_.name(id); }
  const NameTable& site_names() const { return site_names_; }
  /// Optional input routing grid (empty() if the input had none).
  void set_route_grid(RouteGrid g);
  const RouteGrid& route_grid() const { return route_grid_; }
  /// Routing layers bottom to top (from a technology LEF; may be empty).
  void set_routing_layers(std::vector<RoutingLayer> layers);
  std::span<const RoutingLayer> routing_layers() const { return routing_layers_; }

  // --- Netlist construction / editing --------------------------------------
  /// Cell and net names must be unique among live objects of their kind;
  /// a duplicate throws std::invalid_argument.
  Id add_cell(std::string_view name, Dbu width, Dbu height, bool is_macro = false);
  Id add_net(std::string_view name);
  /// Creates an unconnected pin on `cell` (kInvalidId for a top-level I/O
  /// pin). Pin names repeat across cells ("A", "Z"), so each distinct name is
  /// stored once.
  Id add_pin(Id cell, std::string_view pin_name, Point offset = {},
             PinDir dir = PinDir::Input);
  /// add_pin() + connect_pin(): creates a pin on `cell` attached to `net`.
  Id connect(Id cell, Id net, std::string_view pin_name, Point offset = {},
             PinDir dir = PinDir::Input);
  /// Attaches an unconnected pin to `net` (appended to the net's pin list).
  void connect_pin(Id pin, Id net);
  /// Detaches a pin from its net (no-op if unconnected). The pin stays on
  /// its cell.
  void disconnect_pin(Id pin);
  /// Moves a pin to another net (disconnect + connect).
  void reconnect_pin(Id pin, Id net) {
    disconnect_pin(pin);
    connect_pin(pin, net);
  }
  /// Disconnects all its pins and tombstones the cell; its name is free again.
  void remove_cell(Id cell);
  /// Disconnects all its pins and tombstones the net; its name is free again.
  void remove_net(Id net);

  // --- Attribute edits (hot path: inline, O(1)) ----------------------------
  void move_cell(Id cell, Point loc) {
    Cell& c = cells_[check_cell(cell)];
    if (hooks_) on_cell_edit(cell, kMove, c.loc.x, c.loc.y);
    c.loc = loc;
  }
  /// move_cell for many cells: cells[i] goes to locs[i] (spans of equal
  /// size; ids distinct). Runs on the thread pool when nothing records
  /// edits (tracking off, no transaction), serially otherwise.
  void move_cells(std::span<const Id> cells, std::span<const Point> locs);
  void resize_cell(Id cell, Dbu width, Dbu height) {
    Cell& c = cells_[check_cell(cell)];
    if (hooks_) on_cell_edit(cell, kResize, c.width, c.height);
    c.width = width;
    c.height = height;
  }
  void set_fixed(Id cell, bool fixed) {
    Cell& c = cells_[check_cell(cell)];
    if (c.fixed == fixed) return;
    if (hooks_) on_cell_edit(cell, kFixed, c.fixed, 0);
    c.fixed = fixed;
  }
  /// Records the orientation only. Readers bake orientation into the cell's
  /// size and pin offsets; an engine that flips a cell updates those with
  /// resize_cell / set_pin_offset in the same batch.
  void set_orient(Id cell, Orient o) {
    Cell& c = cells_[check_cell(cell)];
    if (hooks_) on_cell_edit(cell, kOrient, static_cast<Dbu>(c.orient), 0);
    c.orient = o;
  }
  void set_placed(Id cell, bool placed) {
    Cell& c = cells_[check_cell(cell)];
    if (c.placed == placed) return;
    if (hooks_) on_cell_edit(cell, kPlaced, c.placed, 0);
    c.placed = placed;
  }
  void set_pin_offset(Id pin, Point offset);
  void set_net_weight(Id net, double weight);

  /// Library cell (LEF MACRO) a cell instantiates, "" if none (e.g. Bookshelf
  /// nodes). Cold data in a side table; writers (DEF) and timing use it.
  void set_cell_master(Id cell, std::string_view master);
  std::string_view cell_master(Id cell) const {
    const std::size_t c = check_cell(cell);
    return c < cell_master_.size() && cell_master_[c] != kInvalidId ? master_names_.name(cell_master_[c])
                                                                   : std::string_view();
  }
  /// Distinct master names, and per cell its master name id (may be shorter
  /// than num_cells(); missing entries mean "no master").
  const NameTable& master_names() const { return master_names_; }
  std::span<const Id> cell_master_ids() const { return cell_master_; }

  /// Pre-sizes arrays, pools and name tables (e.g. from DEF section counts).
  void reserve(std::size_t num_cells, std::size_t num_nets, std::size_t num_pins);
  /// Repacks the connectivity pools (removes holes left by growing lists).
  /// Readers call it once after loading. Counts as a structural change.
  void compact();

  // --- Read access ---------------------------------------------------------
  // Spans stay valid until the next structural change (add/remove/connect).
  // Counts include tombstoned objects: ids are [0, size()).
  std::span<const Cell> cells() const { return cells_; }
  std::span<const Net> nets() const { return nets_; }
  std::span<const Pin> pins() const { return pins_; }
  std::size_t num_cells() const { return cells_.size(); }
  std::size_t num_nets() const { return nets_.size(); }
  std::size_t num_pins() const { return pins_.size(); }
  const Cell& cell(Id id) const { return cells_.at(static_cast<std::size_t>(id)); }
  const Net& net(Id id) const { return nets_.at(static_cast<std::size_t>(id)); }
  const Pin& pin(Id id) const { return pins_.at(static_cast<std::size_t>(id)); }

  /// Pins of a net (driver first when built that way) / of a cell.
  std::span<const Id> net_pins(Id net) const { return net_pins_.list(check_net(net)); }
  std::span<const Id> cell_pins(Id cell) const { return cell_pins_.list(check_cell(cell)); }
  /// Top-level I/O pins (cell == kInvalidId).
  std::span<const Id> io_pins() const { return io_pins_; }
  /// The connectivity pools behind net_pins / cell_pins (for bulk copies).
  const IdListPool& net_pin_pool() const { return net_pins_; }
  const IdListPool& cell_pin_pool() const { return cell_pins_; }

  // --- Names (cold data, kept out of Cell/Net/Pin) -------------------------
  std::string_view cell_name(Id id) const { return cell_names_.name(id); }
  std::string_view net_name(Id id) const { return net_names_.name(id); }
  std::string_view pin_name(Id id) const { return pin_names_.name(pin_name_ids_.at(id)); }
  /// Id lookups by name among live objects; kInvalidId if absent.
  Id find_cell(std::string_view name) const { return cell_names_.find(name); }
  Id find_net(std::string_view name) const { return net_names_.find(name); }
  /// Pin `pin_name` of `cell`, or of the top-level I/O pins when `cell` is
  /// kInvalidId. Returns the first match if a cell has two such pins.
  Id find_pin(Id cell, std::string_view pin_name) const;
  const NameTable& cell_names() const { return cell_names_; }
  const NameTable& net_names() const { return net_names_; }
  /// Distinct pin names (a dictionary, not one entry per pin).
  const NameTable& pin_names() const { return pin_names_; }
  std::span<const Id> pin_name_ids() const { return pin_name_ids_; }
  /// Heap bytes held by the name tables.
  std::size_t name_memory_bytes() const;
  /// Heap bytes held by the whole design (arrays, pools, names, tracking).
  std::size_t memory_bytes() const;

  // --- Change tracking -----------------------------------------------------
  /// Off: mutations record nothing and the next flush reports `all`. Use it
  /// for full (non-incremental) runs. Default off: incremental engines, the
  /// snapshot publisher with partial copies, etc. call set_tracking(true).
  void set_tracking(bool on);
  bool tracking() const { return tracking_; }
  /// Changes accumulated since the last flush. Nets on dirty cells/pins are
  /// added only when flushing.
  const ChangeSet& pending() const { return pending_; }
  /// Ends a batch: completes the change set, hands it to every listener, bumps
  /// version() and starts a new set. Engines call it at consistent points
  /// (end of an iteration, a legalization pass, a reroute batch).
  std::uint64_t flush();
  /// Number of flushes so far. Changes only in flush().
  std::uint64_t version() const { return version_; }
  void subscribe(DesignListener* l);
  void unsubscribe(DesignListener* l);

  /// True while a Transaction is open.
  bool in_transaction() const { return !undo_marks_.empty(); }

 private:
  friend class Transaction;
  friend struct detail::DesignAccess;

  enum Op : std::uint8_t {
    kMove, kResize, kFixed, kOrient, kPlaced,  // cell attribute (a, b = old values)
    kPinOffset,                       // pin (a, b = old offset)
    kNetWeight,                       // net (a = bit pattern of old weight)
    kAddCell, kAddNet, kAddPin,       // undo pops the object
    kConnect,                         // pin, aux = net; undo detaches (last in net list)
    kDisconnect,                      // pin, aux = net, a = position in net list
    kRemoveCell, kRemoveNet,          // undo clears the tombstone
    kDie,                             // a = index into undo_rects_
    kRowHeight,                       // a = old row height
  };
  struct UndoRecord {
    Op op;
    Id id;
    Id aux;
    Dbu a;
    Dbu b;
  };

  [[noreturn]] static void bad_id(const char* what, Id id);
  std::size_t check_cell(Id id) const {
    if (static_cast<std::size_t>(static_cast<std::uint32_t>(id)) >= cells_.size()) bad_id("cell", id);
    return static_cast<std::size_t>(id);
  }
  Id check_net(Id id) const {
    if (static_cast<std::size_t>(static_cast<std::uint32_t>(id)) >= nets_.size()) bad_id("net", id);
    return id;
  }
  Id check_pin(Id id) const {
    if (static_cast<std::size_t>(static_cast<std::uint32_t>(id)) >= pins_.size()) bad_id("pin", id);
    return id;
  }

  void on_cell_edit(Id cell, Op op, Dbu a, Dbu b);
  void record(Op op, Id id, Id aux = kInvalidId, Dbu a = 0, Dbu b = 0) {
    if (!undo_marks_.empty()) undo_.push_back({op, id, aux, a, b});
  }
  bool tracking_active() const { return tracking_ && !pending_.all; }
  void mark_structure();
  void update_hooks() { hooks_ = tracking_active() || !undo_marks_.empty(); }
  void undo_to(std::size_t mark);
  void undo_one(const UndoRecord& r);
  void detach(Id pin, Id net, std::uint32_t pos);

  std::string name_;
  Rect die_;
  Dbu row_height_ = 0;
  std::vector<Cell> cells_;
  std::vector<Net> nets_;
  std::vector<Pin> pins_;
  IdListPool net_pins_;             ///< list index == net id
  IdListPool cell_pins_;            ///< list index == cell id
  std::vector<Id> io_pins_;         ///< pins with cell == kInvalidId

  NameTable cell_names_;            ///< id == cell id
  NameTable net_names_;             ///< id == net id
  NameTable pin_names_;             ///< distinct pin names
  std::vector<Id> pin_name_ids_;    ///< pin id -> pin_names_ id

  // Floorplan / library side data (cold).
  double dbu_per_micron_ = 0.0;
  std::vector<Row> rows_;
  NameTable site_names_;
  RouteGrid route_grid_;
  std::vector<RoutingLayer> routing_layers_;
  NameTable master_names_;
  std::vector<Id> cell_master_;     ///< cell id -> master_names_ id (may be short)

  // Session state: not copied with the design.
  bool tracking_ = false;
  bool hooks_ = false;              ///< tracking_active() || in_transaction()
  std::uint64_t version_ = 0;
  ChangeSet pending_;
  std::vector<DesignListener*> listeners_;
  std::vector<UndoRecord> undo_;
  std::vector<std::size_t> undo_marks_;  ///< undo_ size at each open transaction
  std::vector<Rect> undo_rects_;
};

}  // namespace hermes
