#include "hermes/db/id_list_pool.hpp"

#include <algorithm>
#include <stdexcept>

namespace hermes {

void IdListPool::pop_list() {
  if (ranges_.empty() || ranges_.back().size != 0) {
    throw std::logic_error("IdListPool::pop_list: last list missing or not empty");
  }
  const Range r = ranges_.back();
  ranges_.pop_back();
  if (r.cap > 0 && r.begin + r.cap == data_.size()) data_.resize(r.begin);
}

void IdListPool::grow(Range& r) {
  if (r.begin + r.cap == data_.size()) {  // Last range: grow in place.
    data_.push_back(kInvalidId);
    ++r.cap;
    return;
  }
  const std::uint32_t cap = std::max<std::uint32_t>(4, 2 * r.cap);
  const std::size_t begin = data_.size();
  if (begin + cap > 0xffffffffu) throw std::length_error("IdListPool: more than 2^32 slots");
  data_.resize(begin + cap, kInvalidId);
  std::copy_n(data_.begin() + r.begin, r.size, data_.begin() + static_cast<std::ptrdiff_t>(begin));
  r.begin = static_cast<std::uint32_t>(begin);
  r.cap = cap;
}

void IdListPool::push_back(Id l, Id v) {
  Range& r = ranges_[static_cast<std::size_t>(l)];
  if (r.size == r.cap) grow(r);
  data_[r.begin + r.size++] = v;
  ++items_;
}

void IdListPool::insert(Id l, std::uint32_t pos, Id v) {
  Range& r = ranges_[static_cast<std::size_t>(l)];
  if (pos > r.size) throw std::out_of_range("IdListPool::insert: bad position");
  if (r.size == r.cap) grow(r);
  Id* p = data_.data() + r.begin;
  std::copy_backward(p + pos, p + r.size, p + r.size + 1);
  p[pos] = v;
  ++r.size;
  ++items_;
}

void IdListPool::erase_at(Id l, std::uint32_t pos) {
  Range& r = ranges_[static_cast<std::size_t>(l)];
  if (pos >= r.size) throw std::out_of_range("IdListPool::erase_at: bad position");
  Id* p = data_.data() + r.begin;
  std::copy(p + pos + 1, p + r.size, p + pos);
  --r.size;
  --items_;
}

std::uint32_t IdListPool::find(Id l, Id v) const {
  const auto s = list(l);
  const auto it = std::find(s.begin(), s.end(), v);
  return it == s.end() ? npos : static_cast<std::uint32_t>(it - s.begin());
}

void IdListPool::assign(std::vector<Id> data, std::span<const std::uint32_t> sizes) {
  std::vector<Range> ranges(sizes.size());
  std::uint64_t pos = 0;
  for (std::size_t i = 0; i < sizes.size(); ++i) {
    ranges[i] = {static_cast<std::uint32_t>(pos), sizes[i], sizes[i]};
    pos += sizes[i];
    if (pos > 0xffffffffu) throw std::length_error("IdListPool: more than 2^32 slots");
  }
  if (pos != data.size()) throw std::invalid_argument("IdListPool::assign: sizes do not match data");
  data_ = std::move(data);
  ranges_ = std::move(ranges);
  items_ = data_.size();
}

void IdListPool::reserve(std::size_t lists, std::size_t items) {
  ranges_.reserve(lists);
  data_.reserve(items);
}

void IdListPool::compact() {
  if (wasted_slots() == 0) return;
  std::vector<Id> out;
  out.reserve(items_);
  for (Range& r : ranges_) {
    const std::uint32_t b = static_cast<std::uint32_t>(out.size());
    out.insert(out.end(), data_.begin() + r.begin, data_.begin() + r.begin + r.size);
    r.begin = b;
    r.cap = r.size;
  }
  data_ = std::move(out);
}

}  // namespace hermes
