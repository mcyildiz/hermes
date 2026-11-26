#include "codec.hpp"

#include <cstring>

namespace hermes::io::codec {

namespace {

inline std::uint32_t read32(const std::uint8_t* p) {
  std::uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}
inline std::uint64_t read64(const std::uint8_t* p) {
  std::uint64_t v;
  std::memcpy(&v, p, 8);
  return v;
}

constexpr int kHashBits = 14;
constexpr std::size_t kMinMatch = 4;
constexpr std::size_t kLastLiterals = 5;   // LZ4 block rules: last 5 bytes are literals,
constexpr std::size_t kMatchSafety = 12;   // and no match starts in the last 12 bytes.

inline std::uint32_t hash4(std::uint32_t v) { return (v * 2654435761u) >> (32 - kHashBits); }

void put_len(std::vector<std::uint8_t>& out, std::size_t len) {
  while (len >= 255) {
    out.push_back(255);
    len -= 255;
  }
  out.push_back(static_cast<std::uint8_t>(len));
}

void emit(std::vector<std::uint8_t>& out, const std::uint8_t* lit, std::size_t lit_n,
          std::size_t offset, std::size_t match_n) {
  const std::size_t ml = match_n ? match_n - kMinMatch : 0;
  const std::uint8_t token = static_cast<std::uint8_t>(((lit_n >= 15 ? 15 : lit_n) << 4) | (ml >= 15 ? 15 : ml));
  out.push_back(token);
  if (lit_n >= 15) put_len(out, lit_n - 15);
  out.insert(out.end(), lit, lit + lit_n);
  if (match_n == 0) return;  // Last sequence: literals only.
  out.push_back(static_cast<std::uint8_t>(offset & 0xff));
  out.push_back(static_cast<std::uint8_t>(offset >> 8));
  if (ml >= 15) put_len(out, ml - 15);
}

}  // namespace

void compress(const std::uint8_t* src, std::size_t n, std::vector<std::uint8_t>& out) {
  out.clear();
  out.reserve(n / 2 + 64);
  std::uint32_t table[1u << kHashBits];
  std::memset(table, 0xff, sizeof(table));  // 0xffffffff = empty
  std::size_t ip = 0, anchor = 0;
  if (n > kMatchSafety) {
    const std::size_t limit = n - kMatchSafety;
    const std::size_t match_end = n - kLastLiterals;
    std::size_t miss = 0;
    while (ip < limit) {
      const std::uint32_t seq = read32(src + ip);
      const std::uint32_t h = hash4(seq);
      const std::uint32_t ref = table[h];
      table[h] = static_cast<std::uint32_t>(ip);
      if (ref != 0xffffffffu && ip - ref <= 65535 && read32(src + ref) == seq) {
        std::size_t r = ref, s = ip;
        // Extend backwards over pending literals.
        while (s > anchor && r > 0 && src[s - 1] == src[r - 1]) {
          --s;
          --r;
        }
        std::size_t len = (ip - s) + kMinMatch;
        std::size_t a = ip + kMinMatch, b = ref + kMinMatch;
        while (a + 8 <= match_end) {
          const std::uint64_t x = read64(src + a) ^ read64(src + b);
          if (x) {
            const std::size_t k = static_cast<std::size_t>(__builtin_ctzll(x)) >> 3;
            len += k;
            a += k;
            goto done;
          }
          a += 8;
          b += 8;
          len += 8;
        }
        while (a < match_end && src[a] == src[b]) {
          ++a;
          ++b;
          ++len;
        }
      done:
        emit(out, src + anchor, s - anchor, s - r, len);
        ip = s + len;
        anchor = ip;
        miss = 0;
        if (ip >= 2 && ip - 2 < limit) table[hash4(read32(src + ip - 2))] = static_cast<std::uint32_t>(ip - 2);
        continue;
      }
      // Skip faster through incompressible data.
      ++miss;
      ip += 1 + (miss >> 5);
    }
  }
  emit(out, src + anchor, n - anchor, 0, 0);
}

bool decompress(const std::uint8_t* src, std::size_t src_n, std::uint8_t* dst, std::size_t n) {
  std::size_t ip = 0, op = 0;
  while (ip < src_n) {
    const std::uint8_t token = src[ip++];
    std::size_t lit = token >> 4;
    if (lit == 15) {
      std::uint8_t b;
      do {
        if (ip >= src_n) return false;
        b = src[ip++];
        lit += b;
      } while (b == 255);
    }
    if (lit > src_n - ip || lit > n - op) return false;
    std::memcpy(dst + op, src + ip, lit);
    ip += lit;
    op += lit;
    if (ip == src_n) break;  // Last sequence.
    if (ip + 2 > src_n) return false;
    const std::size_t off = static_cast<std::size_t>(src[ip]) | (static_cast<std::size_t>(src[ip + 1]) << 8);
    ip += 2;
    std::size_t ml = token & 15;
    if (ml == 15) {
      std::uint8_t b;
      do {
        if (ip >= src_n) return false;
        b = src[ip++];
        ml += b;
      } while (b == 255);
    }
    ml += kMinMatch;
    if (off == 0 || off > op || ml > n - op) return false;
    std::uint8_t* d = dst + op;
    const std::uint8_t* m = d - off;
    if (off >= 8) {
      std::size_t k = 0;
      for (; k + 8 <= ml; k += 8) std::memcpy(d + k, m + k, 8);
      for (; k < ml; ++k) d[k] = m[k];
    } else {
      for (std::size_t k = 0; k < ml; ++k) d[k] = m[k];  // Overlapping copy.
    }
    op += ml;
  }
  return op == n;
}

void delta_encode(std::uint8_t* data, std::size_t n, std::size_t elem) {
  if (n < 2) return;
  if (elem == 4) {
    for (std::size_t i = n - 1; i > 0; --i) {
      std::uint32_t a, b;
      std::memcpy(&a, data + 4 * i, 4);
      std::memcpy(&b, data + 4 * (i - 1), 4);
      a -= b;
      std::memcpy(data + 4 * i, &a, 4);
    }
  } else if (elem == 8) {
    for (std::size_t i = n - 1; i > 0; --i) {
      std::uint64_t a, b;
      std::memcpy(&a, data + 8 * i, 8);
      std::memcpy(&b, data + 8 * (i - 1), 8);
      a -= b;
      std::memcpy(data + 8 * i, &a, 8);
    }
  }
}

void delta_decode(std::uint8_t* data, std::size_t n, std::size_t elem) {
  if (elem == 4) {
    std::uint32_t acc = 0;
    for (std::size_t i = 0; i < n; ++i) {
      std::uint32_t a;
      std::memcpy(&a, data + 4 * i, 4);
      acc = i ? acc + a : a;
      std::memcpy(data + 4 * i, &acc, 4);
    }
  } else if (elem == 8) {
    std::uint64_t acc = 0;
    for (std::size_t i = 0; i < n; ++i) {
      std::uint64_t a;
      std::memcpy(&a, data + 8 * i, 8);
      acc = i ? acc + a : a;
      std::memcpy(data + 8 * i, &acc, 8);
    }
  }
}

void shuffle(const std::uint8_t* src, std::uint8_t* dst, std::size_t n, std::size_t elem) {
  if (elem == 8) {
    for (std::size_t i = 0; i < n; ++i) {
      std::uint64_t v;
      std::memcpy(&v, src + 8 * i, 8);
      for (int j = 0; j < 8; ++j) dst[j * n + i] = static_cast<std::uint8_t>(v >> (8 * j));
    }
    return;
  }
  if (elem == 4) {
    for (std::size_t i = 0; i < n; ++i) {
      std::uint32_t v;
      std::memcpy(&v, src + 4 * i, 4);
      for (int j = 0; j < 4; ++j) dst[j * n + i] = static_cast<std::uint8_t>(v >> (8 * j));
    }
    return;
  }
  for (std::size_t j = 0; j < elem; ++j) {
    std::uint8_t* plane = dst + j * n;
    for (std::size_t i = 0; i < n; ++i) plane[i] = src[i * elem + j];
  }
}

void unshuffle(const std::uint8_t* src, std::uint8_t* dst, std::size_t n, std::size_t elem) {
  if (elem == 8) {
    const std::uint8_t *p0 = src, *p1 = src + n, *p2 = src + 2 * n, *p3 = src + 3 * n, *p4 = src + 4 * n,
                       *p5 = src + 5 * n, *p6 = src + 6 * n, *p7 = src + 7 * n;
    for (std::size_t i = 0; i < n; ++i) {
      const std::uint64_t v = std::uint64_t{p0[i]} | std::uint64_t{p1[i]} << 8 | std::uint64_t{p2[i]} << 16 |
                              std::uint64_t{p3[i]} << 24 | std::uint64_t{p4[i]} << 32 | std::uint64_t{p5[i]} << 40 |
                              std::uint64_t{p6[i]} << 48 | std::uint64_t{p7[i]} << 56;
      std::memcpy(dst + 8 * i, &v, 8);
    }
    return;
  }
  if (elem == 4) {
    const std::uint8_t *p0 = src, *p1 = src + n, *p2 = src + 2 * n, *p3 = src + 3 * n;
    for (std::size_t i = 0; i < n; ++i) {
      const std::uint32_t v = std::uint32_t{p0[i]} | std::uint32_t{p1[i]} << 8 | std::uint32_t{p2[i]} << 16 |
                              std::uint32_t{p3[i]} << 24;
      std::memcpy(dst + 4 * i, &v, 4);
    }
    return;
  }
  for (std::size_t j = 0; j < elem; ++j) {
    const std::uint8_t* plane = src + j * n;
    for (std::size_t i = 0; i < n; ++i) dst[i * elem + j] = plane[i];
  }
}

std::uint64_t hash64(const std::uint8_t* p, std::size_t n, std::uint64_t seed) {
  constexpr std::uint64_t kMul = 0x9E3779B97F4A7C15ull;
  std::uint64_t h0 = seed ^ (n * kMul), h1 = ~seed, h2 = seed + kMul, h3 = seed - kMul;
  std::size_t i = 0;
  for (; i + 32 <= n; i += 32) {
    h0 = (h0 ^ read64(p + i)) * kMul;
    h1 = (h1 ^ read64(p + i + 8)) * kMul;
    h2 = (h2 ^ read64(p + i + 16)) * kMul;
    h3 = (h3 ^ read64(p + i + 24)) * kMul;
    h0 ^= h0 >> 29;
    h1 ^= h1 >> 29;
    h2 ^= h2 >> 29;
    h3 ^= h3 >> 29;
  }
  std::uint64_t h = h0 ^ (h1 * 3) ^ (h2 * 5) ^ (h3 * 7);
  for (; i < n; ++i) h = (h ^ p[i]) * kMul;
  h ^= h >> 31;
  h *= kMul;
  h ^= h >> 29;
  return h;
}

}  // namespace hermes::io::codec
