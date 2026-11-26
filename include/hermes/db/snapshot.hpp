#pragma once

// Consistent, immutable views of a Design for concurrent readers.
//
// The Design has one writer (the engine running on it) and is not safe to
// read while it changes. A reader on another thread (the GUI, a report
// writer, a remote viewer) instead reads a DesignSnapshot: an immutable copy
// at one version, published by a SnapshotPublisher on the writer's flushes.
//
//   writer thread                       reader thread (e.g. GUI)
//   ---------------------------------   ----------------------------------
//   SnapshotPublisher pub(design);      auto s = pub.latest();   // any time
//   engine edits design ...             draw(s->cell(i).bbox()) ...
//   design.flush();  // publishes       // s stays valid and unchanged while
//                                       // held; take latest() again later
//
// Cost model: cells, pins and nets live in fixed chunks (4096 objects) shared
// between consecutive snapshots. A publish copies only chunks holding objects
// that changed since the previous one, so moving one cell costs one chunk
// copy (~160 KB), and memory is shared across versions. Moving cells copies
// cell chunks only (Net records hold no geometry). Connectivity and names
// are shared until a structural change (add/remove/connect), which copies
// them once. A flush with `all` (tracking off) copies everything. Chunks are
// copied in parallel on the hermes::par pool.
//
// Cells are stored in id order by default. set_cell_order() stores them in
// region order instead, so a region-local edit (the usual incremental case)
// copies only the chunks of that region.

#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "hermes/db/design.hpp"

namespace hermes {

/// Read-only array split into shared, immutable chunks.
template <class T>
class ChunkedArray {
 public:
  static constexpr int kShift = 12;
  static constexpr std::size_t kChunk = std::size_t{1} << kShift;

  std::size_t size() const { return size_; }
  const T& operator[](std::size_t i) const { return ptrs_[i >> kShift][i & (kChunk - 1)]; }
  std::size_t num_chunks() const { return ptrs_.size(); }
  /// True if chunk `c` is the same memory in both arrays (shared, not copied).
  bool shares_chunk(const ChunkedArray& o, std::size_t c) const {
    return c < ptrs_.size() && c < o.ptrs_.size() && ptrs_[c] == o.ptrs_[c];
  }

 private:
  friend class SnapshotPublisher;
  std::vector<std::shared_ptr<const T[]>> owners_;
  std::vector<const T*> ptrs_;
  std::size_t size_ = 0;
};

/// Immutable design state at one version. Safe to read from any thread.
class DesignSnapshot {
 public:
  std::uint64_t version() const { return version_; }
  const std::string& name() const { return name_; }
  const Rect& die() const { return die_; }
  Dbu row_height() const { return row_height_; }
  double dbu_per_micron() const { return dbu_per_micron_; }
  /// Placement rows and routing layers, as of the last structural change
  /// (readers set them once at load).
  std::span<const Row> rows() const { return topo_->rows; }
  std::span<const RoutingLayer> routing_layers() const { return topo_->routing_layers; }

  std::size_t num_cells() const { return cells_.size(); }
  std::size_t num_nets() const { return nets_.size(); }
  std::size_t num_pins() const { return pins_.size(); }
  const Cell& cell(Id id) const {
    return cells_[layout_ ? layout_->slot[static_cast<std::size_t>(id)] : static_cast<std::size_t>(id)];
  }
  const Net& net(Id id) const { return nets_[static_cast<std::size_t>(id)]; }
  const Pin& pin(Id id) const { return pins_[static_cast<std::size_t>(id)]; }
  std::span<const Id> net_pins(Id net) const { return topo_->net_pins.list(net); }
  std::span<const Id> cell_pins(Id cell) const { return topo_->cell_pins.list(cell); }
  std::span<const Id> io_pins() const { return topo_->io_pins; }

  Point pin_location(Id pin) const {
    const Pin& p = this->pin(pin);
    return p.cell == kInvalidId ? p.offset : cell(p.cell).loc + p.offset;
  }
  Rect net_bbox(Id net) const;

  std::string_view cell_name(Id id) const { return topo_->cell_names.name(id); }
  std::string_view net_name(Id id) const { return topo_->net_names.name(id); }
  std::string_view pin_name(Id id) const {
    return topo_->pin_names.name(topo_->pin_name_ids.at(static_cast<std::size_t>(id)));
  }
  /// LEF master name of a cell (empty if none).
  std::string_view cell_master(Id id) const {
    const std::size_t c = static_cast<std::size_t>(id);
    return c < topo_->cell_master.size() && topo_->cell_master[c] != kInvalidId
               ? topo_->master_names.name(topo_->cell_master[c])
               : std::string_view();
  }
  Id find_cell(std::string_view n) const { return topo_->cell_names.find(n); }
  Id find_net(std::string_view n) const { return topo_->net_names.find(n); }

  /// Direct chunk access (e.g. to test or exploit sharing between versions).
  /// With a cell order (SnapshotPublisher::set_cell_order), element i of
  /// the array is cell cell_order()[i].
  const ChunkedArray<Cell>& cell_array() const { return cells_; }
  /// Cell ids in storage order (empty: identity, storage index = id). A
  /// region-ordered layout keeps each region's cells in a few chunks, which
  /// a GUI can also use to draw only the chunks in view.
  std::span<const Id> cell_order() const {
    return layout_ ? std::span<const Id>(layout_->order) : std::span<const Id>();
  }

 private:
  friend class SnapshotPublisher;
  struct Topology {
    IdListPool net_pins;
    IdListPool cell_pins;
    std::vector<Id> io_pins;
    NameTable cell_names;
    NameTable net_names;
    NameTable pin_names;
    std::vector<Id> pin_name_ids;
    NameTable master_names;
    std::vector<Id> cell_master;
    std::vector<Row> rows;
    std::vector<RoutingLayer> routing_layers;
  };

  std::uint64_t version_ = 0;
  std::string name_;
  Rect die_;
  Dbu row_height_ = 0;
  double dbu_per_micron_ = 0.0;
  ChunkedArray<Cell> cells_;
  ChunkedArray<Net> nets_;
  ChunkedArray<Pin> pins_;
  std::shared_ptr<const Topology> topo_;
  struct Layout {
    std::vector<Id> order;            // storage index -> cell id
    std::vector<std::uint32_t> slot;  // cell id -> storage index
  };
  std::shared_ptr<const Layout> layout_;  // null: identity
};

/// Publishes snapshots of a design for concurrent readers.
///
/// Subscribes to the design and, by default, publishes on every flush (the
/// engine's batch boundary), so readers always see the last consistent
/// state. With auto_publish off, call publish() when a new view is wanted
/// (e.g. at most every 50 ms). publish() and the flushes run on the writer
/// thread; latest() may be called from any thread.
/// Must not outlive the design.
class SnapshotPublisher final : public DesignListener {
 public:
  explicit SnapshotPublisher(Design& design, bool auto_publish = true);
  ~SnapshotPublisher() override;
  SnapshotPublisher(const SnapshotPublisher&) = delete;
  SnapshotPublisher& operator=(const SnapshotPublisher&) = delete;

  void on_flush(const Design& design, const ChangeSet& changes) override;

  /// Writer thread: flushes pending changes if any, then publishes the
  /// current state (no-op if nothing changed since the last publish).
  void publish();
  /// Any thread: the most recently published snapshot (never null).
  std::shared_ptr<const DesignSnapshot> latest() const;

  /// Chunks copied by the last publish (cells + nets + pins).
  std::size_t last_copied_chunks() const { return last_copied_; }

  /// Stores cells in snapshots in this order (a permutation of the cell
  /// ids, e.g. region::Partition::spatial_order()) instead of id order. Then
  /// edits confined to one region copy only the chunks holding that region,
  /// instead of chunks all over the id range. Cells added later are
  /// appended. Takes effect at the next publish, which copies all cells.
  /// An empty order goes back to id order.
  void set_cell_order(std::vector<Id> order);

 private:
  void build();
  template <class T>
  std::size_t fill(ChunkedArray<T>& out, const ChunkedArray<T>* prev, std::span<const T> src,
                   const DirtySet* dirty, const std::vector<Id>* order = nullptr);
  void sync_layout();

  Design& d_;
  bool auto_publish_;
  mutable std::mutex mu_;
  std::shared_ptr<const DesignSnapshot> current_;
  // Accumulated since the last publish (writer thread only).
  bool all_ = true;
  bool topo_ = true;
  bool net_records_ = true, pin_records_ = true;  // since last publish
  bool dirty_ = true;
  DirtySet cell_chunks_, net_chunks_, pin_chunks_;
  std::size_t last_copied_ = 0;
  std::shared_ptr<const DesignSnapshot::Layout> layout_;  // current cell order (null: ids)
  bool layout_changed_ = false;
};

}  // namespace hermes
