#pragma once

#include <algorithm>
#include <cstdint>

namespace hermes {

/// Database units. All geometry is stored as integers to avoid
/// floating-point drift during legalization and routing.
using Dbu = std::int64_t;

/// Index into one of the Design's object vectors.
using Id = std::int32_t;
inline constexpr Id kInvalidId = -1;

struct Point {
  Dbu x = 0;
  Dbu y = 0;

  friend constexpr Point operator+(Point a, Point b) { return {a.x + b.x, a.y + b.y}; }
  friend constexpr Point operator-(Point a, Point b) { return {a.x - b.x, a.y - b.y}; }
  friend constexpr bool operator==(Point a, Point b) = default;
};

struct Rect {
  Dbu xlo = 0;
  Dbu ylo = 0;
  Dbu xhi = 0;
  Dbu yhi = 0;

  constexpr Dbu width() const { return xhi - xlo; }
  constexpr Dbu height() const { return yhi - ylo; }
  constexpr Dbu area() const { return width() * height(); }
  constexpr Point center() const { return {(xlo + xhi) / 2, (ylo + yhi) / 2}; }

  constexpr bool contains(Point p) const {
    return p.x >= xlo && p.x <= xhi && p.y >= ylo && p.y <= yhi;
  }

  constexpr bool overlaps(const Rect& o) const {
    return xlo < o.xhi && o.xlo < xhi && ylo < o.yhi && o.ylo < yhi;
  }

  /// Area of the intersection with another rectangle (0 if disjoint).
  constexpr Dbu overlap_area(const Rect& o) const {
    const Dbu w = std::min(xhi, o.xhi) - std::max(xlo, o.xlo);
    const Dbu h = std::min(yhi, o.yhi) - std::max(ylo, o.ylo);
    return (w > 0 && h > 0) ? w * h : 0;
  }

  friend constexpr bool operator==(const Rect&, const Rect&) = default;
};

enum class Orient : std::uint8_t { N, S, E, W, FN, FS, FE, FW };

enum class PinDir : std::uint8_t { Input, Output, Inout };

}  // namespace hermes
