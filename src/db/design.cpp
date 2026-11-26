#include "hermes/db/design.hpp"
#include "hermes/par/parallel.hpp"

#include <algorithm>
#include <bit>
#include <stdexcept>
#include <string>

namespace hermes {

void DirtySet::truncate(Id n) {
  std::size_t keep = 0;
  for (Id id : ids_) {
    if (id < n) {
      ids_[keep++] = id;
    } else {
      bits_[static_cast<std::size_t>(id) >> 6] &= ~(std::uint64_t{1} << (id & 63));
    }
  }
  ids_.resize(keep);
}

// ---------------------------------------------------------------------------
// Construction, copy, move
// ---------------------------------------------------------------------------

Design::Design(std::string name) : name_(std::move(name)) {
  pending_.all = true;  // Everything is new.
  update_hooks();
}

Design::~Design() = default;

Design::Design(const Design& o)
    : name_(o.name_),
      die_(o.die_),
      row_height_(o.row_height_),
      cells_(o.cells_),
      nets_(o.nets_),
      pins_(o.pins_),
      net_pins_(o.net_pins_),
      cell_pins_(o.cell_pins_),
      io_pins_(o.io_pins_),
      cell_names_(o.cell_names_),
      net_names_(o.net_names_),
      pin_names_(o.pin_names_),
      pin_name_ids_(o.pin_name_ids_),
      dbu_per_micron_(o.dbu_per_micron_),
      rows_(o.rows_),
      site_names_(o.site_names_),
      route_grid_(o.route_grid_),
      routing_layers_(o.routing_layers_),
      master_names_(o.master_names_),
      cell_master_(o.cell_master_),
      tracking_(o.tracking_),
      version_(o.version_) {
  pending_.all = true;
  pending_.structure = pending_.floorplan = true;
  update_hooks();
}

Design& Design::operator=(const Design& o) {
  if (this != &o) {
    Design tmp(o);
    *this = std::move(tmp);
  }
  return *this;
}

Design::Design(Design&& o) noexcept
    : name_(std::move(o.name_)),
      die_(o.die_),
      row_height_(o.row_height_),
      cells_(std::move(o.cells_)),
      nets_(std::move(o.nets_)),
      pins_(std::move(o.pins_)),
      net_pins_(std::move(o.net_pins_)),
      cell_pins_(std::move(o.cell_pins_)),
      io_pins_(std::move(o.io_pins_)),
      cell_names_(std::move(o.cell_names_)),
      net_names_(std::move(o.net_names_)),
      pin_names_(std::move(o.pin_names_)),
      pin_name_ids_(std::move(o.pin_name_ids_)),
      dbu_per_micron_(o.dbu_per_micron_),
      rows_(std::move(o.rows_)),
      site_names_(std::move(o.site_names_)),
      route_grid_(std::move(o.route_grid_)),
      routing_layers_(std::move(o.routing_layers_)),
      master_names_(std::move(o.master_names_)),
      cell_master_(std::move(o.cell_master_)),
      tracking_(o.tracking_),
      version_(o.version_) {
  pending_.all = true;
  pending_.structure = pending_.floorplan = true;
  update_hooks();
}

Design& Design::operator=(Design&& o) noexcept {
  if (this == &o) return *this;
  // Listeners subscribed to *this keep listening: they see `all` next flush.
  name_ = std::move(o.name_);
  die_ = o.die_;
  row_height_ = o.row_height_;
  cells_ = std::move(o.cells_);
  nets_ = std::move(o.nets_);
  pins_ = std::move(o.pins_);
  net_pins_ = std::move(o.net_pins_);
  cell_pins_ = std::move(o.cell_pins_);
  io_pins_ = std::move(o.io_pins_);
  cell_names_ = std::move(o.cell_names_);
  net_names_ = std::move(o.net_names_);
  pin_names_ = std::move(o.pin_names_);
  pin_name_ids_ = std::move(o.pin_name_ids_);
  dbu_per_micron_ = o.dbu_per_micron_;
  rows_ = std::move(o.rows_);
  site_names_ = std::move(o.site_names_);
  route_grid_ = std::move(o.route_grid_);
  routing_layers_ = std::move(o.routing_layers_);
  master_names_ = std::move(o.master_names_);
  cell_master_ = std::move(o.cell_master_);
  tracking_ = o.tracking_;
  undo_.clear();
  undo_marks_.clear();
  undo_rects_.clear();
  pending_.clear();
  pending_.all = true;
  pending_.structure = pending_.floorplan = true;
  update_hooks();
  return *this;
}

void Design::bad_id(const char* what, Id id) {
  throw std::out_of_range(std::string("Design: bad ") + what + " id " + std::to_string(id));
}

// ---------------------------------------------------------------------------
// Floorplan
// ---------------------------------------------------------------------------

void Design::set_die(const Rect& die) {
  if (in_transaction()) {
    undo_rects_.push_back(die_);
    record(kDie, kInvalidId, kInvalidId, static_cast<Dbu>(undo_rects_.size() - 1));
  }
  die_ = die;
  pending_.floorplan = true;
}

void Design::set_row_height(Dbu h) {
  record(kRowHeight, kInvalidId, kInvalidId, row_height_);
  row_height_ = h;
  pending_.floorplan = true;
}

void Design::set_dbu_per_micron(double v) {
  dbu_per_micron_ = v;
  pending_.floorplan = true;
}

void Design::set_rows(std::vector<Row> rows) {
  rows_ = std::move(rows);
  if (!rows_.empty()) {
    // Most common row height (ties: the smaller one).
    std::vector<Dbu> h;
    h.reserve(rows_.size());
    for (const Row& r : rows_) h.push_back(r.site_height);
    std::sort(h.begin(), h.end());
    Dbu best = h[0];
    std::size_t best_n = 0;
    for (std::size_t i = 0; i < h.size();) {
      std::size_t j = i;
      while (j < h.size() && h[j] == h[i]) ++j;
      if (j - i > best_n) {
        best_n = j - i;
        best = h[i];
      }
      i = j;
    }
    set_row_height(best);
  }
  pending_.floorplan = true;
}

void Design::set_route_grid(RouteGrid g) {
  route_grid_ = std::move(g);
  pending_.floorplan = true;
}

void Design::set_routing_layers(std::vector<RoutingLayer> layers) {
  routing_layers_ = std::move(layers);
  pending_.floorplan = true;
}

void Design::set_cell_master(Id cell, std::string_view master) {
  const std::size_t c = check_cell(cell);
  if (cell_master_.size() <= c) cell_master_.resize(cells_.size(), kInvalidId);
  cell_master_[c] = master.empty() ? kInvalidId : master_names_.intern(master);
}

// ---------------------------------------------------------------------------
// Netlist editing
// ---------------------------------------------------------------------------

void Design::mark_structure() { pending_.structure = true; }

Id Design::add_cell(std::string_view name, Dbu width, Dbu height, bool is_macro) {
  if (cell_names_.contains(name)) {
    throw std::invalid_argument("add_cell: duplicate cell name '" + std::string(name) + "'");
  }
  if (cells_.size() >= static_cast<std::size_t>(INT32_MAX)) throw std::length_error("too many cells");
  const Id id = cell_names_.add(name);
  Cell c;
  c.width = width;
  c.height = height;
  c.is_macro = is_macro;
  cells_.push_back(c);
  cell_pins_.add_list();
  record(kAddCell, id);
  if (tracking_active()) pending_.cells.mark(id);
  mark_structure();
  return id;
}

Id Design::add_net(std::string_view name) {
  if (net_names_.contains(name)) {
    throw std::invalid_argument("add_net: duplicate net name '" + std::string(name) + "'");
  }
  if (nets_.size() >= static_cast<std::size_t>(INT32_MAX)) throw std::length_error("too many nets");
  const Id id = net_names_.add(name);
  nets_.emplace_back();
  net_pins_.add_list();
  record(kAddNet, id);
  if (tracking_active()) pending_.nets.mark(id);
  mark_structure();
  return id;
}

Id Design::add_pin(Id cell, std::string_view pin_name, Point offset, PinDir dir) {
  if (cell != kInvalidId) {
    check_cell(cell);
    if (cells_[cell].removed) throw std::logic_error("add_pin: cell is removed");
  }
  if (pins_.size() >= static_cast<std::size_t>(INT32_MAX)) throw std::length_error("too many pins");
  Pin p;
  p.cell = cell;
  p.offset = offset;
  p.dir = dir;
  const Id name_id = pin_names_.intern(pin_name);
  const Id pid = static_cast<Id>(pins_.size());
  pins_.push_back(p);
  pin_name_ids_.push_back(name_id);
  if (cell != kInvalidId) {
    cell_pins_.push_back(cell, pid);
  } else {
    io_pins_.push_back(pid);
  }
  record(kAddPin, pid);
  if (tracking_active()) {
    pending_.pins.mark(pid);
    if (cell != kInvalidId) pending_.cells.mark(cell);
  }
  mark_structure();
  return pid;
}

Id Design::connect(Id cell, Id net, std::string_view pin_name, Point offset, PinDir dir) {
  check_net(net);  // Validate before creating anything.
  const Id pid = add_pin(cell, pin_name, offset, dir);
  connect_pin(pid, net);
  return pid;
}

void Design::connect_pin(Id pin, Id net) {
  check_pin(pin);
  check_net(net);
  Pin& p = pins_[pin];
  if (p.net != kInvalidId) throw std::logic_error("connect_pin: pin is already connected");
  if (nets_[net].removed) throw std::logic_error("connect_pin: net is removed");
  if (p.cell != kInvalidId && cells_[p.cell].removed) {
    throw std::logic_error("connect_pin: pin's cell is removed");
  }
  net_pins_.push_back(net, pin);
  p.net = net;
  record(kConnect, pin, net);
  if (tracking_active()) {
    pending_.pins.mark(pin);
    pending_.nets.mark(net);
  }
  mark_structure();
}

void Design::detach(Id pin, Id net, std::uint32_t pos) {
  net_pins_.erase_at(net, pos);
  pins_[pin].net = kInvalidId;
}

void Design::disconnect_pin(Id pin) {
  check_pin(pin);
  const Id net = pins_[pin].net;
  if (net == kInvalidId) return;
  const std::uint32_t pos = net_pins_.find(net, pin);
  record(kDisconnect, pin, net, pos);
  detach(pin, net, pos);
  if (tracking_active()) {
    pending_.pins.mark(pin);
    pending_.nets.mark(net);
  }
  mark_structure();
}

void Design::remove_cell(Id cell) {
  check_cell(cell);
  if (cells_[cell].removed) throw std::logic_error("remove_cell: already removed");
  // Disconnecting edits net lists only, so the cell's pin list stays valid.
  for (Id p : cell_pins_.list(cell)) disconnect_pin(p);
  cell_names_.erase(cell);
  cells_[cell].removed = true;
  record(kRemoveCell, cell);
  if (tracking_active()) pending_.cells.mark(cell);
  mark_structure();
}

void Design::remove_net(Id net) {
  check_net(net);
  if (nets_[net].removed) throw std::logic_error("remove_net: already removed");
  // Last pin first: each disconnect is then an O(1) erase at the list end.
  while (net_pins_.size(net) > 0) disconnect_pin(net_pins_.list(net).back());
  net_names_.erase(net);
  nets_[net].removed = true;
  record(kRemoveNet, net);
  if (tracking_active()) pending_.nets.mark(net);
  mark_structure();
}

// ---------------------------------------------------------------------------
// Attribute edits
// ---------------------------------------------------------------------------

void Design::on_cell_edit(Id cell, Op op, Dbu a, Dbu b) {
  record(op, cell, kInvalidId, a, b);
  if (tracking_active()) pending_.cells.mark(cell);
}

void Design::set_pin_offset(Id pin, Point offset) {
  check_pin(pin);
  Pin& p = pins_[pin];
  record(kPinOffset, pin, kInvalidId, p.offset.x, p.offset.y);
  p.offset = offset;
  pending_.pin_records = true;
  if (tracking_active()) pending_.pins.mark(pin);
}

void Design::set_net_weight(Id net, double weight) {
  check_net(net);
  record(kNetWeight, net, kInvalidId, std::bit_cast<Dbu>(nets_[net].weight));
  nets_[net].weight = weight;
  pending_.net_records = true;
  if (tracking_active()) {
    pending_.nets.mark(net);
    pending_.net_data.mark(net);
  }
}

void Design::reserve(std::size_t num_cells, std::size_t num_nets, std::size_t num_pins) {
  cells_.reserve(num_cells);
  cell_names_.reserve(num_cells);
  cell_pins_.reserve(num_cells, num_pins);
  nets_.reserve(num_nets);
  net_names_.reserve(num_nets);
  net_pins_.reserve(num_nets, num_pins);
  pins_.reserve(num_pins);
  pin_name_ids_.reserve(num_pins);
}

void Design::compact() {
  net_pins_.compact();
  cell_pins_.compact();
  mark_structure();
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

Id Design::find_pin(Id cell_id, std::string_view pin_name) const {
  const Id nid = pin_names_.find(pin_name);
  if (nid == kInvalidId) return kInvalidId;
  const std::span<const Id> candidates =
      cell_id == kInvalidId ? std::span<const Id>(io_pins_) : cell_pins(cell_id);
  for (Id pid : candidates) {
    if (pin_name_ids_[pid] == nid) return pid;
  }
  return kInvalidId;
}

std::size_t Design::name_memory_bytes() const {
  return cell_names_.memory_bytes() + net_names_.memory_bytes() + pin_names_.memory_bytes() +
         pin_name_ids_.capacity() * sizeof(Id);
}

std::size_t Design::memory_bytes() const {
  return cells_.capacity() * sizeof(Cell) + nets_.capacity() * sizeof(Net) +
         pins_.capacity() * sizeof(Pin) + net_pins_.memory_bytes() + cell_pins_.memory_bytes() +
         io_pins_.capacity() * sizeof(Id) + name_memory_bytes() + pending_.cells.memory_bytes() +
         pending_.nets.memory_bytes() + pending_.pins.memory_bytes() +
         undo_.capacity() * sizeof(UndoRecord);
}

// ---------------------------------------------------------------------------
// Change tracking
// ---------------------------------------------------------------------------

void Design::set_tracking(bool on) {
  tracking_ = on;
  if (!on) {
    pending_.cells.clear();
    pending_.nets.clear();
    pending_.pins.clear();
    pending_.net_data.clear();
    pending_.all = true;
  }
  update_hooks();
}

void Design::move_cells(std::span<const Id> ids, std::span<const Point> locs) {
  if (ids.size() != locs.size()) throw std::invalid_argument("move_cells: size mismatch");
  for (Id id : ids) check_cell(id);
  if (hooks_) {
    for (std::size_t i = 0; i < ids.size(); ++i) move_cell(ids[i], locs[i]);
    return;
  }
  par::parallel_for(0, ids.size(), 16384, [&](std::size_t lo, std::size_t hi) {
    for (std::size_t i = lo; i < hi; ++i) cells_[static_cast<std::size_t>(ids[i])].loc = locs[i];
  });
}

std::uint64_t Design::flush() {
  if (pending_.structure) {
    // An undo may have popped objects that were marked.
    pending_.cells.truncate(static_cast<Id>(cells_.size()));
    pending_.nets.truncate(static_cast<Id>(nets_.size()));
    pending_.net_data.truncate(static_cast<Id>(nets_.size()));
    pending_.pins.truncate(static_cast<Id>(pins_.size()));
  }
  if (!pending_.all) {
    // Nets touched through their pins: once per dirty cell/pin, not per move.
    if (pending_.cells.size() * 8 > cells_.size()) {
      // Many dirty cells: one sequential pass over the pins beats chasing
      // each cell's pins through the (randomly ordered) pin array.
      for (const Pin& p : pins_) {
        if (p.net != kInvalidId && p.cell != kInvalidId && pending_.cells.test(p.cell)) {
          pending_.nets.mark(p.net);
        }
      }
    } else {
      for (Id c : pending_.cells.ids()) {
        for (Id p : cell_pins_.list(c)) {
          const Id n = pins_[p].net;
          if (n != kInvalidId) pending_.nets.mark(n);
        }
      }
    }
    for (Id p : pending_.pins.ids()) {
      const Id n = pins_[p].net;
      if (n != kInvalidId) pending_.nets.mark(n);
    }
  }
  pending_.version = ++version_;
  for (DesignListener* l : listeners_) l->on_flush(*this, pending_);
  pending_.clear();
  pending_.all = !tracking_;
  update_hooks();
  return version_;
}

void Design::subscribe(DesignListener* l) {
  if (std::find(listeners_.begin(), listeners_.end(), l) == listeners_.end()) {
    listeners_.push_back(l);
  }
}

void Design::unsubscribe(DesignListener* l) { std::erase(listeners_, l); }

// ---------------------------------------------------------------------------
// Undo
// ---------------------------------------------------------------------------

void Design::undo_to(std::size_t mark) {
  while (undo_.size() > mark) {
    const UndoRecord r = undo_.back();
    undo_.pop_back();
    undo_one(r);
  }
}

void Design::undo_one(const UndoRecord& r) {
  const bool track = tracking_active();
  auto dirty_cell = [&](Id c) {
    if (track && c != kInvalidId) pending_.cells.mark(c);
  };
  auto dirty_pin_net = [&](Id p, Id n) {
    if (!track) return;
    pending_.pins.mark(p);
    pending_.nets.mark(n);
  };
  switch (r.op) {
    case kMove:
      cells_[r.id].loc = {r.a, r.b};
      dirty_cell(r.id);
      break;
    case kResize:
      cells_[r.id].width = r.a;
      cells_[r.id].height = r.b;
      dirty_cell(r.id);
      break;
    case kFixed:
      cells_[r.id].fixed = r.a != 0;
      dirty_cell(r.id);
      break;
    case kOrient:
      cells_[r.id].orient = static_cast<Orient>(r.a);
      dirty_cell(r.id);
      break;
    case kPlaced:
      cells_[r.id].placed = r.a != 0;
      dirty_cell(r.id);
      break;
    case kPinOffset:
      pins_[r.id].offset = {r.a, r.b};
      pending_.pin_records = true;
      if (track) pending_.pins.mark(r.id);
      break;
    case kNetWeight:
      nets_[r.id].weight = std::bit_cast<double>(r.a);
      pending_.net_records = true;
      if (track) {
        pending_.nets.mark(r.id);
        pending_.net_data.mark(r.id);
      }
      break;
    case kAddCell:
      cells_.pop_back();
      if (cell_master_.size() > cells_.size()) cell_master_.resize(cells_.size());
      cell_pins_.pop_list();
      cell_names_.pop_back();
      mark_structure();
      break;
    case kAddNet:
      nets_.pop_back();
      net_pins_.pop_list();
      net_names_.pop_back();
      mark_structure();
      break;
    case kAddPin: {
      const Pin& p = pins_.back();
      if (p.cell != kInvalidId) {
        cell_pins_.erase_at(p.cell, cell_pins_.size(p.cell) - 1);
        dirty_cell(p.cell);
      } else {
        io_pins_.pop_back();
      }
      pins_.pop_back();
      pin_name_ids_.pop_back();
      mark_structure();
      break;
    }
    case kConnect:
      detach(r.id, r.aux, net_pins_.size(r.aux) - 1);
      dirty_pin_net(r.id, r.aux);
      mark_structure();
      break;
    case kDisconnect:
      net_pins_.insert(r.aux, static_cast<std::uint32_t>(r.a), r.id);
      pins_[r.id].net = r.aux;
      dirty_pin_net(r.id, r.aux);
      mark_structure();
      break;
    case kRemoveCell:
      cells_[r.id].removed = false;
      cell_names_.restore(r.id);
      dirty_cell(r.id);
      mark_structure();
      break;
    case kRemoveNet:
      nets_[r.id].removed = false;
      net_names_.restore(r.id);
      if (track) pending_.nets.mark(r.id);
      mark_structure();
      break;
    case kDie:
      die_ = undo_rects_[static_cast<std::size_t>(r.a)];
      undo_rects_.resize(static_cast<std::size_t>(r.a));
      pending_.floorplan = true;
      break;
    case kRowHeight:
      row_height_ = r.a;
      pending_.floorplan = true;
      break;
  }
}

// ---------------------------------------------------------------------------
// Transaction
// ---------------------------------------------------------------------------

Transaction::Transaction(Design& design) : d_(&design), mark_(design.undo_.size()) {
  d_->undo_marks_.push_back(mark_);
  depth_ = d_->undo_marks_.size();
  d_->update_hooks();
}

Transaction::~Transaction() {
  if (!active_) return;
  try {
    rollback();
  } catch (...) {  // Never throw from a destructor.
  }
}

std::size_t Transaction::size() const { return active_ ? d_->undo_.size() - mark_ : 0; }

void Transaction::end() {
  if (!active_) throw std::logic_error("Transaction: already committed or rolled back");
  if (d_->undo_marks_.size() != depth_) {
    throw std::logic_error("Transaction: an inner transaction is still open");
  }
  d_->undo_marks_.pop_back();
  if (d_->undo_marks_.empty()) {
    d_->undo_.clear();
    d_->undo_rects_.clear();
  }
  active_ = false;
  d_->update_hooks();
}

void Transaction::commit() { end(); }

void Transaction::rollback() {
  if (active_ && d_->undo_marks_.size() == depth_) d_->undo_to(mark_);
  end();
}

}  // namespace hermes
