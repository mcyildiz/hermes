#include "hermes/db/snapshot.hpp"

#include <algorithm>
#include <limits>

#include "hermes/par/parallel.hpp"

namespace hermes {

Rect DesignSnapshot::net_bbox(Id net) const {
  const auto pins = net_pins(net);
  if (pins.empty()) return {};
  Rect r{std::numeric_limits<Dbu>::max(), std::numeric_limits<Dbu>::max(),
         std::numeric_limits<Dbu>::min(), std::numeric_limits<Dbu>::min()};
  for (Id p : pins) {
    const Point q = pin_location(p);
    r.xlo = std::min(r.xlo, q.x);
    r.ylo = std::min(r.ylo, q.y);
    r.xhi = std::max(r.xhi, q.x);
    r.yhi = std::max(r.yhi, q.y);
  }
  return r;
}

SnapshotPublisher::SnapshotPublisher(Design& design, bool auto_publish)
    : d_(design), auto_publish_(auto_publish) {
  build();  // Full copy of the current state.
  d_.subscribe(this);
}

SnapshotPublisher::~SnapshotPublisher() { d_.unsubscribe(this); }

void SnapshotPublisher::on_flush(const Design&, const ChangeSet& ch) {
  dirty_ = true;
  if (ch.all) {
    all_ = true;
  } else if (!all_) {
    constexpr int s = ChunkedArray<Cell>::kShift;
    const std::vector<std::uint32_t>* slot = layout_ ? &layout_->slot : nullptr;
    for (Id id : ch.cells.ids()) {
      const std::size_t i = static_cast<std::size_t>(id);
      cell_chunks_.mark(static_cast<Id>((slot && i < slot->size() ? (*slot)[i] : i) >> s));
    }
    // Net records change only through set_net_weight (net_data) or
    // structural edits; nets that merely have moved pins are not copied.
    for (Id id : (ch.structure ? ch.nets : ch.net_data).ids()) net_chunks_.mark(id >> s);
    for (Id id : ch.pins.ids()) pin_chunks_.mark(id >> s);
  }
  if (ch.structure) topo_ = true;
  if (ch.structure || ch.net_records) net_records_ = true;
  if (ch.structure || ch.pin_records) pin_records_ = true;
  if (auto_publish_) build();
}

void SnapshotPublisher::publish() {
  if (!d_.pending().empty()) {
    d_.flush();  // Records the changes (and builds, if auto_publish).
    if (auto_publish_) return;
  }
  if (dirty_) build();
}

std::shared_ptr<const DesignSnapshot> SnapshotPublisher::latest() const {
  std::lock_guard lk(mu_);
  return current_;
}

template <class T>
std::size_t SnapshotPublisher::fill(ChunkedArray<T>& out, const ChunkedArray<T>* prev,
                                    std::span<const T> src, const DirtySet* dirty,
                                    const std::vector<Id>* order) {
  constexpr std::size_t K = ChunkedArray<T>::kChunk;
  const std::size_t n = src.size();
  const std::size_t nc = (n + K - 1) / K;
  out.size_ = n;
  out.owners_.resize(nc);
  out.ptrs_.resize(nc);
  std::vector<std::size_t> copy;  // chunks to copy
  for (std::size_t c = 0; c < nc; ++c) {
    const std::size_t lo = c * K, len = std::min(K, n - lo);
    // Reuse the previous chunk when it holds the same range and is clean.
    if (prev && dirty && c < prev->num_chunks() && std::min(K, prev->size_ - lo) == len &&
        !dirty->test(static_cast<Id>(c))) {
      out.owners_[c] = prev->owners_[c];
      out.ptrs_[c] = prev->ptrs_[c];
      continue;
    }
    copy.push_back(c);
  }
  // Copy the dirty chunks in parallel (memory bound: helps most when many
  // chunks changed). Each block owns whole chunks, so no two threads write
  // the same memory.
  par::parallel_for(0, copy.size(), 4, [&](std::size_t klo, std::size_t khi) {
    for (std::size_t k = klo; k < khi; ++k) {
      const std::size_t c = copy[k];
      const std::size_t lo = c * K, len = std::min(K, n - lo);
      std::shared_ptr<T[]> chunk(new T[len]);
      if (order) {
        for (std::size_t i = 0; i < len; ++i) chunk[i] = src[static_cast<std::size_t>((*order)[lo + i])];
      } else {
        std::copy_n(src.begin() + static_cast<std::ptrdiff_t>(lo), len, chunk.get());
      }
      out.ptrs_[c] = chunk.get();
      out.owners_[c] = std::move(chunk);
    }
  });
  return copy.size();
}

void SnapshotPublisher::set_cell_order(std::vector<Id> order) {
  if (order.empty()) {
    layout_.reset();
  } else {
    auto l = std::make_shared<DesignSnapshot::Layout>();
    l->order = std::move(order);
    layout_ = std::move(l);
  }
  layout_changed_ = true;
  dirty_ = true;
}

// Brings the layout in line with the design's cell count (cells added since
// it was set are appended; ids popped by an undo are dropped).
void SnapshotPublisher::sync_layout() {
  if (!layout_) return;
  const std::size_t n = d_.num_cells();
  if (!layout_changed_ && layout_->slot.size() == n) return;
  auto l = std::make_shared<DesignSnapshot::Layout>();
  l->slot.assign(n, std::numeric_limits<std::uint32_t>::max());
  l->order.reserve(n);
  for (Id id : layout_->order) {
    const std::size_t i = static_cast<std::size_t>(id);
    if (i < n && l->slot[i] == std::numeric_limits<std::uint32_t>::max()) {
      l->slot[i] = static_cast<std::uint32_t>(l->order.size());
      l->order.push_back(id);
    }
  }
  for (std::size_t i = 0; i < n; ++i) {
    if (l->slot[i] == std::numeric_limits<std::uint32_t>::max()) {
      l->slot[i] = static_cast<std::uint32_t>(l->order.size());
      l->order.push_back(static_cast<Id>(i));
    }
  }
  layout_ = std::move(l);
  layout_changed_ = true;
}

void SnapshotPublisher::build() {
  auto s = std::make_shared<DesignSnapshot>();
  const DesignSnapshot* prev = current_.get();  // Only this thread writes current_.
  const bool full = all_ || !prev;
  sync_layout();
  const bool full_cells = full || layout_changed_;
  s->version_ = d_.version();
  s->name_ = d_.name();
  s->die_ = d_.die();
  s->row_height_ = d_.row_height();
  s->dbu_per_micron_ = d_.dbu_per_micron();
  s->layout_ = layout_;
  last_copied_ =
      fill(s->cells_, full_cells ? nullptr : &prev->cells_, d_.cells(), &cell_chunks_,
           layout_ ? &layout_->order : nullptr) +
      // With `all` (tracking off) nets/pins are still shared when no Net/Pin
      // record changed: the placer's publishes copy cells only.
      fill(s->nets_, !prev || (full && net_records_) ? nullptr : &prev->nets_, d_.nets(),
           &net_chunks_) +
      fill(s->pins_, !prev || (full && pin_records_) ? nullptr : &prev->pins_, d_.pins(),
           &pin_chunks_);
  // The structure flag is kept even with tracking off, so a flush with
  // `all` after only moves (the placer's case) still shares the topology.
  if (!prev || topo_) {
    auto t = std::make_shared<DesignSnapshot::Topology>();
    par::parallel_invoke([&] { t->net_pins = d_.net_pin_pool(); },
                         [&] { t->cell_pins = d_.cell_pin_pool(); },
                         [&] { t->io_pins.assign(d_.io_pins().begin(), d_.io_pins().end()); },
                         [&] { t->cell_names = d_.cell_names(); },
                         [&] { t->net_names = d_.net_names(); },
                         [&] { t->pin_names = d_.pin_names(); },
                         [&] {
                           t->pin_name_ids.assign(d_.pin_name_ids().begin(),
                                                  d_.pin_name_ids().end());
                         },
                         [&] {
                           t->master_names = d_.master_names();
                           t->cell_master.assign(d_.cell_master_ids().begin(), d_.cell_master_ids().end());
                           t->rows.assign(d_.rows().begin(), d_.rows().end());
                           t->routing_layers.assign(d_.routing_layers().begin(), d_.routing_layers().end());
                         });
    s->topo_ = std::move(t);
  } else {
    s->topo_ = prev->topo_;
  }
  {
    std::lock_guard lk(mu_);
    current_ = std::move(s);
  }
  all_ = topo_ = dirty_ = layout_changed_ = net_records_ = pin_records_ = false;
  cell_chunks_.clear();
  net_chunks_.clear();
  pin_chunks_.clear();
}

}  // namespace hermes
