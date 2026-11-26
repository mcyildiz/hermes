#pragma once

// Internal byte codecs for the design file: an LZ77 block compressor in the
// LZ4 block layout (fast to decode, no external dependency) plus the delta
// and byte-shuffle filters that make columns of integers compress well.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hermes::io::codec {

/// Upper bound of compress() output for n input bytes.
inline std::size_t compress_bound(std::size_t n) { return n + n / 255 + 16; }
/// Compresses src[0, n) into dst (resized to the compressed size).
void compress(const std::uint8_t* src, std::size_t n, std::vector<std::uint8_t>& dst);
/// Decompresses exactly `n` bytes into dst. Returns false on corrupt input.
bool decompress(const std::uint8_t* src, std::size_t src_n, std::uint8_t* dst, std::size_t n);

/// In place: v[i] -= v[i-1] (from the end; elements of `elem` bytes, 4 or 8).
void delta_encode(std::uint8_t* data, std::size_t n_elems, std::size_t elem);
void delta_decode(std::uint8_t* data, std::size_t n_elems, std::size_t elem);
/// Transposes n_elems elements of `elem` bytes into byte planes (dst must
/// not alias src).
void shuffle(const std::uint8_t* src, std::uint8_t* dst, std::size_t n_elems, std::size_t elem);
void unshuffle(const std::uint8_t* src, std::uint8_t* dst, std::size_t n_elems, std::size_t elem);

/// Fast 64-bit checksum.
std::uint64_t hash64(const std::uint8_t* p, std::size_t n, std::uint64_t seed = 0);

}  // namespace hermes::io::codec
