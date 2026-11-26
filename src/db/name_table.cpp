#include "hermes/db/name_table.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <limits>
#include <memory>
#include <functional>
#include <stdexcept>
#include <string>

#include "hermes/par/parallel.hpp"

namespace hermes {

namespace {
// Grow when more than 3/4 of the slots are used.
constexpr std::size_t kMinSlots = 16;
bool over_load(std::size_t used, std::size_t slots) { return 4 * used > 3 * slots; }
}  // namespace

std::uint32_t NameTable::hash_of(std::string_view s) {
  const std::uint64_t h = std::hash<std::string_view>{}(s);
  return static_cast<std::uint32_t>(h ^ (h >> 32));
}

std::size_t NameTable::probe(std::string_view name, std::uint32_t h) const {
  const std::size_t mask = slots_.size() - 1;
  for (std::size_t i = h & mask;; i = (i + 1) & mask) {
    const Slot& s = slots_[i];
    if (s.id == kInvalidId) return i;
    if (s.hash == h && this->name(s.id) == name) return i;
  }
}

void NameTable::rehash(std::size_t num_slots) {
  std::vector<Slot> old = std::move(slots_);
  slots_.assign(num_slots, Slot{});
  const std::size_t mask = num_slots - 1;
  for (const Slot& s : old) {
    if (s.id == kInvalidId) continue;
    std::size_t i = s.hash & mask;
    while (slots_[i].id != kInvalidId) i = (i + 1) & mask;
    slots_[i] = s;
  }
}

Id NameTable::append(std::string_view name, std::uint32_t h, std::size_t slot) {
  const Id id = static_cast<Id>(size());
  chars_.insert(chars_.end(), name.begin(), name.end());
  offsets_.push_back(chars_.size());
  slots_[slot] = Slot{id, h};
  if (over_load(size(), slots_.size())) rehash(slots_.size() * 2);
  return id;
}

Id NameTable::add(std::string_view name) {
  if (slots_.empty()) rehash(kMinSlots);
  const std::uint32_t h = hash_of(name);
  const std::size_t slot = probe(name, h);
  if (slots_[slot].id != kInvalidId) {
    throw std::invalid_argument("duplicate name '" + std::string(name) + "'");
  }
  return append(name, h, slot);
}

Id NameTable::intern(std::string_view name) {
  if (slots_.empty()) rehash(kMinSlots);
  const std::uint32_t h = hash_of(name);
  const std::size_t slot = probe(name, h);
  if (slots_[slot].id != kInvalidId) return slots_[slot].id;
  return append(name, h, slot);
}

Id NameTable::find(std::string_view name) const {
  if (slots_.empty()) return kInvalidId;
  return slots_[probe(name, hash_of(name))].id;
}

void NameTable::erase_slot(std::size_t i) {
  // Backward-shift deletion keeps linear probing correct without tombstones.
  const std::size_t mask = slots_.size() - 1;
  for (std::size_t j = (i + 1) & mask; slots_[j].id != kInvalidId; j = (j + 1) & mask) {
    const std::size_t home = slots_[j].hash & mask;
    // Entry j may move to i unless its home lies cyclically in (i, j].
    const bool stays = (i <= j) ? (i < home && home <= j) : (i < home || home <= j);
    if (stays) continue;
    slots_[i] = slots_[j];
    i = j;
  }
  slots_[i] = Slot{};
}

void NameTable::erase(Id id) {
  const std::string_view n = name(id);
  if (slots_.empty()) return;
  const std::size_t s = probe(n, hash_of(n));
  if (slots_[s].id == id) erase_slot(s);
}

void NameTable::restore(Id id) {
  const std::string_view n = name(id);
  if (slots_.empty()) rehash(kMinSlots);
  const std::uint32_t h = hash_of(n);
  const std::size_t s = probe(n, h);
  if (slots_[s].id == id) return;
  if (slots_[s].id != kInvalidId) {
    throw std::invalid_argument("NameTable::restore: name '" + std::string(n) + "' is taken");
  }
  slots_[s] = Slot{id, h};
  // size() is unchanged, so the load factor still holds.
}

void NameTable::pop_back() {
  if (empty()) throw std::logic_error("NameTable::pop_back on empty table");
  const Id id = static_cast<Id>(size() - 1);
  erase(id);
  offsets_.pop_back();
  chars_.resize(offsets_.back());
}

std::string_view NameTable::name(Id id) const {
  if (id < 0 || static_cast<std::size_t>(id) >= size()) {
    throw std::out_of_range("NameTable: bad id " + std::to_string(id));
  }
  const std::size_t b = offsets_[id];
  return {chars_.data() + b, offsets_[id + 1] - b};
}

void NameTable::reserve(std::size_t num_names, std::size_t num_chars) {
  offsets_.reserve(num_names + 1);
  if (num_chars) chars_.reserve(num_chars);
  std::size_t want = std::max(kMinSlots, slots_.size());
  while (over_load(num_names, want)) want *= 2;
  if (want != slots_.size()) rehash(std::bit_ceil(want));
}

void NameTable::assign(std::vector<char> chars, std::vector<std::size_t> offsets,
                       std::span<const std::uint8_t> unmapped) {
  if (offsets.empty() || offsets.front() != 0 || offsets.back() != chars.size()) {
    throw std::invalid_argument("NameTable::assign: bad offsets");
  }
  const std::size_t n = offsets.size() - 1;
  for (std::size_t i = 0; i < n; ++i) {
    if (offsets[i] > offsets[i + 1]) throw std::invalid_argument("NameTable::assign: bad offsets");
  }
  if (!unmapped.empty() && unmapped.size() != n) {
    throw std::invalid_argument("NameTable::assign: unmapped has the wrong size");
  }
  chars_ = std::move(chars);
  offsets_ = std::move(offsets);
  std::vector<std::uint32_t> hashes(n);
  par::parallel_for(0, n, 8192, [&](std::size_t lo, std::size_t hi) {
    for (std::size_t i = lo; i < hi; ++i) {
      hashes[i] = hash_of({chars_.data() + offsets_[i], offsets_[i + 1] - offsets_[i]});
    }
  });
  std::size_t want = kMinSlots;
  while (over_load(n, want)) want *= 2;
  const std::size_t mask = want - 1;
  // Lock-free parallel insert: a slot is one 64-bit word {id, hash}, claimed
  // with a CAS. Two inserts of the same name follow the same probe sequence,
  // so they meet at the same slot and the loser sees the duplicate. The slot
  // layout may depend on timing; ids and lookups do not.
  static_assert(sizeof(Slot) == 8);
  auto pack = [](Slot s) {
    std::uint64_t v;
    std::memcpy(&v, &s, 8);
    return v;
  };
  auto unpack = [](std::uint64_t v) {
    Slot s;
    std::memcpy(&s, &v, 8);
    return s;
  };
  const std::uint64_t empty = pack(Slot{});
  std::unique_ptr<std::atomic<std::uint64_t>[]> table(new std::atomic<std::uint64_t>[want]);
  par::parallel_for(0, want, 1 << 16, [&](std::size_t lo, std::size_t hi) {
    for (std::size_t i = lo; i < hi; ++i) table[i].store(empty, std::memory_order_relaxed);
  });
  std::atomic<std::size_t> dup{std::numeric_limits<std::size_t>::max()};
  par::parallel_for(0, n, 8192, [&](std::size_t lo, std::size_t hi) {
    for (std::size_t i = lo; i < hi; ++i) {
      if (!unmapped.empty() && unmapped[i]) continue;
      const std::uint32_t h = hashes[i];
      const std::string_view nm{chars_.data() + offsets_[i], offsets_[i + 1] - offsets_[i]};
      const std::uint64_t mine = pack(Slot{static_cast<Id>(i), h});
      for (std::size_t k = h & mask;; k = (k + 1) & mask) {
        std::uint64_t cur = table[k].load(std::memory_order_acquire);
        if (cur == empty) {
          if (table[k].compare_exchange_strong(cur, mine, std::memory_order_acq_rel)) break;
          // Lost the race: `cur` now holds the winner; check it below.
        }
        const Slot sl = unpack(cur);
        if (sl.hash == h) {
          const Id o = sl.id;
          const std::string_view on{chars_.data() + offsets_[o], offsets_[o + 1] - offsets_[o]};
          if (on == nm) {
            std::size_t first = dup.load();
            const std::size_t d = std::min(i, static_cast<std::size_t>(o));
            while (d < first && !dup.compare_exchange_weak(first, d)) {}
            break;
          }
        }
      }
    }
  });
  if (dup.load() != std::numeric_limits<std::size_t>::max()) {
    const std::size_t i = dup.load();
    const std::string nm(chars_.data() + offsets_[i], offsets_[i + 1] - offsets_[i]);
    slots_.clear();
    chars_.clear();
    offsets_.assign(1, 0);
    throw std::invalid_argument("duplicate name '" + nm + "'");
  }
  slots_.resize(want);
  par::parallel_for(0, want, 1 << 16, [&](std::size_t lo, std::size_t hi) {
    for (std::size_t i = lo; i < hi; ++i) slots_[i] = unpack(table[i].load(std::memory_order_relaxed));
  });
}

void NameTable::assign(std::span<const std::string_view> names) {
  std::vector<std::size_t> offsets(names.size() + 1, 0);
  for (std::size_t i = 0; i < names.size(); ++i) offsets[i + 1] = offsets[i] + names[i].size();
  std::vector<char> chars(offsets.back());
  par::parallel_for(0, names.size(), 8192, [&](std::size_t lo, std::size_t hi) {
    for (std::size_t i = lo; i < hi; ++i) {
      std::copy(names[i].begin(), names[i].end(), chars.begin() + static_cast<std::ptrdiff_t>(offsets[i]));
    }
  });
  assign(std::move(chars), std::move(offsets));
}

std::size_t NameTable::memory_bytes() const {
  return chars_.capacity() + offsets_.capacity() * sizeof(std::size_t) +
         slots_.capacity() * sizeof(Slot);
}

}  // namespace hermes
