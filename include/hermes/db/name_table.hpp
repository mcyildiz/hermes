#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "hermes/core/types.hpp"

namespace hermes {

/// Bidirectional id <-> name table, kept apart from the hot Cell/Net/Pin
/// structs so placement and timing loops never touch name bytes.
///
/// Ids are dense and assigned in insertion order (0, 1, 2, ...), so a table
/// can mirror a Design object vector one to one.
///
/// Layout, chosen for memory and locality:
///  * id -> name: all characters live in one contiguous arena; `offsets_[id]`
///    and `offsets_[id + 1]` bound the name. No per-name heap allocation.
///  * name -> id: open-addressing hash table of {id, hash} slots (8 bytes,
///    linear probing). Slots hold ids, never pointers, so the table stays
///    valid when the arena grows and when the table is copied or moved.
class NameTable {
 public:
  /// Adds a new name and returns its id. Throws std::invalid_argument if the
  /// name is already present.
  Id add(std::string_view name);
  /// Returns the id of `name`, adding it first if absent.
  Id intern(std::string_view name);
  /// Id of `name`, or kInvalidId if absent.
  Id find(std::string_view name) const;
  bool contains(std::string_view name) const { return find(name) != kInvalidId; }

  /// Makes `id`'s name unfindable (find() returns kInvalidId and the name may
  /// be added again). The id and name(id) stay valid. No-op if not mapped.
  void erase(Id id);
  /// Maps `id`'s name to it again after erase(). Throws std::invalid_argument
  /// if another id now holds the name.
  void restore(Id id);
  /// Removes the last id completely (undo of add()).
  void pop_back();

  /// Name of `id`. Throws std::out_of_range on a bad id. The view stays valid
  /// until the next add/intern (the arena may reallocate).
  std::string_view name(Id id) const;

  std::size_t size() const { return offsets_.size() - 1; }
  bool empty() const { return size() == 0; }

  /// Pre-sizes for `num_names` names totalling about `num_chars` characters.
  void reserve(std::size_t num_names, std::size_t num_chars = 0);

  /// Heap bytes held by this table (capacity, not just size).
  std::size_t memory_bytes() const;

  // --- Bulk access (readers, design files) ---------------------------------
  /// Replaces the contents: name i is chars[offsets[i], offsets[i + 1]).
  /// `offsets` has size()+1 entries starting at 0. Ids with unmapped[i] != 0
  /// keep their name but are not findable (erased, e.g. tombstoned cells).
  /// Hashes are computed on the thread pool. Throws std::invalid_argument on
  /// a duplicate mapped name (or malformed offsets).
  void assign(std::vector<char> chars, std::vector<std::size_t> offsets,
              std::span<const std::uint8_t> unmapped = {});
  /// assign() from a list of names (copied in parallel).
  void assign(std::span<const std::string_view> names);
  std::span<const char> chars() const { return chars_; }
  std::span<const std::size_t> offsets() const { return offsets_; }
  /// True if `id` is findable by its name (not erased).
  bool mapped(Id id) const { return find(name(id)) == id; }

 private:
  struct Slot {
    Id id = kInvalidId;
    std::uint32_t hash = 0;
  };

  static std::uint32_t hash_of(std::string_view s);
  /// Slot index holding `name`, or the empty slot where it would go.
  std::size_t probe(std::string_view name, std::uint32_t h) const;
  Id append(std::string_view name, std::uint32_t h, std::size_t slot);
  void erase_slot(std::size_t slot);
  void rehash(std::size_t num_slots);

  std::vector<char> chars_;
  std::vector<std::size_t> offsets_{0};
  std::vector<Slot> slots_;  ///< Power-of-two size, or empty.
};

}  // namespace hermes
