#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "hermes/core/types.hpp"

namespace hermes {

/// Many small, growable id lists (the pins of each net, the pins of each
/// cell) packed into one flat array, so the design holds no per-object heap
/// allocations and copying it (e.g. for a snapshot) is a few memcpy's.
///
/// Each list owns a contiguous slot range [begin, begin + cap). A list that
/// outgrows its range moves to the end of the array with twice the capacity;
/// a list that already sits at the end grows in place, so lists built one
/// after another (a DEF NETS section) end up tightly packed. Moving leaves
/// holes; compact() removes them.
///
/// Spans returned by list() are invalidated by any change to the pool.
class IdListPool {
 public:
  struct Range {
    std::uint32_t begin = 0;
    std::uint32_t size = 0;
    std::uint32_t cap = 0;
  };

  std::size_t num_lists() const { return ranges_.size(); }
  /// Appends an empty list and returns its index.
  Id add_list() {
    // Starts at the end of the array, so it can grow in place.
    ranges_.push_back({static_cast<std::uint32_t>(data_.size()), 0, 0});
    return static_cast<Id>(ranges_.size() - 1);
  }
  /// Removes the last list (which must be empty).
  void pop_list();

  std::span<const Id> list(Id l) const {
    const Range& r = ranges_[static_cast<std::size_t>(l)];
    return {data_.data() + r.begin, r.size};
  }
  std::uint32_t size(Id l) const { return ranges_[static_cast<std::size_t>(l)].size; }

  void push_back(Id l, Id v);
  /// Inserts `v` at position `pos` (order is preserved; driver-first etc.).
  void insert(Id l, std::uint32_t pos, Id v);
  /// Removes the element at `pos`, keeping the order of the rest.
  void erase_at(Id l, std::uint32_t pos);
  /// Position of `v` in list `l`, or npos.
  std::uint32_t find(Id l, Id v) const;
  static constexpr std::uint32_t npos = ~std::uint32_t{0};

  void reserve(std::size_t lists, std::size_t items);
  /// Repacks all lists tightly in list order (capacity == size).
  void compact();

  /// Replaces the contents with tightly packed lists: list i is
  /// data[sum(sizes[0..i)) .. + sizes[i]). Throws std::invalid_argument if
  /// the sizes do not add up to data.size().
  void assign(std::vector<Id> data, std::span<const std::uint32_t> sizes);
  /// Raw slots (lists in range order, may contain holes unless compact).
  std::span<const Id> raw_data() const { return data_; }
  std::span<const Range> ranges() const { return ranges_; }

  std::size_t num_items() const { return items_; }
  /// Slots not holding a list element (holes + spare capacity).
  std::size_t wasted_slots() const { return data_.size() - items_; }
  std::size_t memory_bytes() const {
    return data_.capacity() * sizeof(Id) + ranges_.capacity() * sizeof(Range);
  }

 private:
  void grow(Range& r);

  std::vector<Id> data_;
  std::vector<Range> ranges_;
  std::size_t items_ = 0;
};

}  // namespace hermes
