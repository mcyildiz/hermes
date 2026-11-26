#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "hermes/core/types.hpp"

namespace hermes {

class Design;

/// Set of dirty ids: a bitset for O(1) membership plus the list of marked ids
/// for O(changed) iteration and clearing. Marking an id twice is a no-op.
class DirtySet {
 public:
  void mark(Id id) {
    const std::size_t w = static_cast<std::size_t>(id) >> 6;
    if (w >= bits_.size()) [[unlikely]] bits_.resize(w + 1 + bits_.size() / 2, 0);
    const std::uint64_t m = std::uint64_t{1} << (id & 63);
    if (!(bits_[w] & m)) {
      bits_[w] |= m;
      ids_.push_back(id);
    }
  }
  bool test(Id id) const {
    const std::size_t w = static_cast<std::size_t>(id) >> 6;
    return w < bits_.size() && (bits_[w] >> (id & 63) & 1);
  }
  /// Marked ids in marking order.
  std::span<const Id> ids() const { return ids_; }
  std::size_t size() const { return ids_.size(); }
  bool empty() const { return ids_.empty(); }
  /// O(number of marked ids).
  void clear() {
    for (Id id : ids_) bits_[static_cast<std::size_t>(id) >> 6] = 0;
    ids_.clear();
  }
  /// Drops ids >= n (after objects were popped by an undo).
  void truncate(Id n);
  /// Sizes the bitset for ids < n so mark() never reallocates.
  void reserve(std::size_t n) {
    if ((n + 63) / 64 > bits_.size()) bits_.resize((n + 63) / 64, 0);
  }
  std::size_t memory_bytes() const {
    return bits_.capacity() * sizeof(std::uint64_t) + ids_.capacity() * sizeof(Id);
  }

 private:
  std::vector<std::uint64_t> bits_;
  std::vector<Id> ids_;
};

/// What changed in a Design between two flushes (see Design::flush()).
///
///  * `cells`: moved, resized, fixed/unfixed, added or removed cells.
///  * `pins`:  pins added, (dis)connected, or whose offset changed.
///  * `nets`:  nets whose pin set, weight, or any pin position changed. This
///             includes every net on a dirty cell or dirty pin (derived once
///             per flush, not on every move).
///  * `net_data`: nets whose own record (Net: weight) changed, a subset of
///             `nets`. Readers that copy Net records (snapshots) use it;
///             added/removed nets come with `structure`.
///  * `pin_records` / `net_records`: some Pin / Net record changed without a
///             structural edit (set_pin_offset / set_net_weight). Kept even
///             with tracking off, so a flush with `all` still tells whether
///             only cells changed (e.g. the placer moved them).
///  * `all`:   anything may have changed (fresh design, bulk load, tracking
///             off). The id sets are then empty; listeners rebuild fully.
///  * `structure`: objects were added/removed or connectivity changed; array
///             sizes may differ from the last flush (they can also shrink
///             after an undo). Listeners with per-id arrays resize them.
///  * `floorplan`: die area or row height changed.
///
/// Geometry derived by listeners (bins, regions) is theirs to compute: a
/// congestion map, for example, keeps the bbox it last deposited per net and
/// re-deposits only the dirty nets.
struct ChangeSet {
  std::uint64_t version = 0;  ///< Design version this flush produced.
  bool all = false;
  bool structure = false;
  bool floorplan = false;
  bool pin_records = false;
  bool net_records = false;
  DirtySet cells;
  DirtySet nets;
  DirtySet pins;
  DirtySet net_data;

  bool empty() const {
    return !all && !structure && !floorplan && !pin_records && !net_records && cells.empty() && nets.empty() && pins.empty();
  }
  void clear() {
    all = structure = floorplan = pin_records = net_records = false;
    cells.clear();
    nets.clear();
    pins.clear();
    net_data.clear();
  }
};

/// Receives the accumulated changes on every Design::flush(), on the thread
/// that calls flush() (the design's writer). The design is consistent and
/// must not be modified from inside on_flush(). A listener must unsubscribe
/// before it or the design is destroyed (RAII helpers in hermes do this).
class DesignListener {
 public:
  virtual ~DesignListener() = default;
  virtual void on_flush(const Design& design, const ChangeSet& changes) = 0;
};

}  // namespace hermes
