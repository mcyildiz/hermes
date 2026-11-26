#pragma once

// Deterministic parallel scatter into a grid, by bands of rows.
//
// Many hot loops scatter items into a bin grid: cells deposit charge into
// density bins, nets deposit RUDY demand into GCells. Done naively in
// parallel, two threads add into the same bin (a race), and atomics or
// per-thread grids make the floating-point sum depend on the thread count.
//
// RowBands instead gives each task a band of grid rows it owns. An item that
// touches rows [r0, r1] is listed in every band it overlaps, and the task of
// a band processes its items in item order, clipped to the band's rows. So
// every bin receives its contributions in item order: the result is
// bit-identical to the serial loop for any number of threads or bands.
//
//   RowBands bands;
//   bands.build(num_items, grid_rows, [&](std::size_t i) { return rows_of(i); });
//   bands.run([&](std::size_t i, int row_lo, int row_hi) { deposit(i, row_lo, row_hi); });
//
// Items spanning several bands are visited once per band (clipped), so bands
// should be taller than typical items; build() picks the band height.

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "hermes/par/parallel.hpp"

namespace hermes::par {

class RowBands {
 public:
  /// rows_of(i) -> std::pair<int, int>: first and last grid row item i
  /// touches (inclusive, clamped to [0, num_rows)); return first > last for
  /// an item that touches nothing. `num_bands` = 0 picks a count.
  template <class RowsOf>
  void build(std::size_t num_items, int num_rows, RowsOf&& rows_of, int num_bands = 0);

  /// Calls f(item, row_lo, row_hi) for every (item, band) pair, bands in
  /// parallel, items in increasing order within a band. [row_lo, row_hi] is
  /// the item's row range clipped to the band.
  template <class F>
  void run(F&& f) const;

  int num_bands() const { return nbands_; }
  int band_rows() const { return band_h_; }
  /// Item visits (>= items with rows; extra visits come from band crossings).
  std::size_t num_entries() const { return entries_.size(); }

 private:
  int nbands_ = 0, band_h_ = 1, rows_ = 0;
  std::vector<std::int32_t> lo_, hi_;       // per item row range
  std::vector<std::size_t> start_;          // per band, into entries_
  std::vector<std::uint32_t> entries_;      // item ids grouped by band
  std::vector<std::uint32_t> counts_;       // per (block, band) scratch
};

template <class RowsOf>
void RowBands::build(std::size_t n, int num_rows, RowsOf&& rows_of, int num_bands) {
  rows_ = num_rows;
  if (num_bands <= 0) {
    // Two bands per thread (slack for imbalance), at least 4 rows tall:
    // every extra band adds a visit for each item crossing it.
    num_bands = std::max(1, std::min(num_rows / 4, 2 * num_threads()));
  }
  nbands_ = std::max(1, std::min(num_bands, std::max(1, num_rows)));
  band_h_ = (std::max(1, num_rows) + nbands_ - 1) / nbands_;
  nbands_ = (std::max(1, num_rows) + band_h_ - 1) / band_h_;
  lo_.resize(n);
  hi_.resize(n);

  const std::size_t nb = std::max<std::size_t>(1, block_count(n, 8192));
  const std::size_t bs = n == 0 ? 1 : (n + nb - 1) / nb;
  const std::size_t B = static_cast<std::size_t>(nbands_);
  counts_.assign(nb * B, 0);
  // Counters live in a thread-local array while a block runs (adjacent
  // blocks' rows of counts_ share cache lines: counting there directly
  // would serialize the threads on false sharing).
  parallel_for_each(0, nb, [&](std::size_t blk) {
    thread_local std::vector<std::uint32_t> cnt;
    cnt.assign(B, 0);
    const std::size_t e = std::min(n, (blk + 1) * bs);
    for (std::size_t i = blk * bs; i < e; ++i) {
      const auto [r0, r1] = rows_of(i);
      lo_[i] = r0;
      hi_[i] = r1;
      if (r0 > r1) continue;
      for (int b = r0 / band_h_; b <= r1 / band_h_; ++b) ++cnt[b];
    }
    std::copy(cnt.begin(), cnt.end(), counts_.begin() + blk * B);
  });
  // Exclusive prefix over (band, block): band-major, then block order.
  start_.assign(B + 1, 0);
  std::size_t total = 0;
  for (std::size_t b = 0; b < B; ++b) {
    start_[b] = total;
    for (std::size_t blk = 0; blk < nb; ++blk) {
      const std::uint32_t c = counts_[blk * B + b];
      counts_[blk * B + b] = static_cast<std::uint32_t>(total - start_[b]);
      total += c;
    }
  }
  start_[B] = total;
  entries_.resize(total);
  parallel_for_each(0, nb, [&](std::size_t blk) {
    thread_local std::vector<std::uint32_t> off;
    off.assign(counts_.begin() + blk * B, counts_.begin() + (blk + 1) * B);
    const std::size_t e = std::min(n, (blk + 1) * bs);
    for (std::size_t i = blk * bs; i < e; ++i) {
      if (lo_[i] > hi_[i]) continue;
      for (int b = lo_[i] / band_h_; b <= hi_[i] / band_h_; ++b) {
        entries_[start_[b] + off[b]++] = static_cast<std::uint32_t>(i);
      }
    }
  });
}

template <class F>
void RowBands::run(F&& f) const {
  parallel_for_each(0, static_cast<std::size_t>(nbands_), [&](std::size_t b) {
    const int blo = static_cast<int>(b) * band_h_;
    const int bhi = std::min(rows_, blo + band_h_) - 1;
    for (std::size_t e = start_[b]; e < start_[b + 1]; ++e) {
      const std::uint32_t i = entries_[e];
      f(static_cast<std::size_t>(i), std::max<int>(lo_[i], blo), std::min<int>(hi_[i], bhi));
    }
  });
}

}  // namespace hermes::par
