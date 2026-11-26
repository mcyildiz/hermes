#pragma once

// Native design file (.hdb): saves and restores the whole Design, fast.
//
//   io::save_design(design, "run1/after_place.hdb");
//   Design d = io::load_design("run1/after_place.hdb");
//
// Layout (little-endian):
//   header   64 bytes: "HERMESDB", format major/minor, TOC offset/size/hash
//   sections one per column (cell.x, cell.y, pin.net, names.cell.chars, ...),
//            each 64-byte aligned, cut into ~1 MiB chunks. A chunk is
//            delta- and byte-shuffle-filtered and LZ-compressed (or stored
//            raw with compress=false, so a raw column can be used straight
//            from a memory map).
//   TOC      tag, element size, count, codec, filters, chunk layout and a
//            64-bit checksum per section.
//
// Speed: chunks are filtered, compressed and checksummed in parallel on the
// hermes::par pool on save; on load the file is memory-mapped and every
// chunk of every section decodes in parallel straight into the Design's
// arrays. Name hash tables are rebuilt on load (hashes in parallel) rather
// than stored.
//
// Compatibility: the format major version changes only when old readers
// cannot read new files; they then refuse with a clear message. Minor
// versions add sections: a reader skips sections it does not know (logged
// at verbose) and fills defaults for optional sections a file lacks. Each
// section also carries its own version. Columns (not raw structs) are
// stored, so changing a C++ struct does not change the file.
//
// Checkpoints: algorithm state lives outside the Design, so engines add it
// as extra sections (SaveOptions::extra, read back with load_design's
// `extra`). To checkpoint while an engine keeps running, copy the design at
// a flush point and save the copy in the background (start_save_job).

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "hermes/db/design.hpp"
#include "hermes/par/job.hpp"

namespace hermes::io {

inline constexpr std::uint16_t kDesignFileMajor = 1;
inline constexpr std::uint16_t kDesignFileMinor = 1;  ///< 1.1 added route.layers

struct SaveOptions {
  bool compress = true;
  std::size_t chunk_bytes = std::size_t{1} << 20;
  /// Extra sections, e.g. placer state or routes: stored as "x.<tag>".
  std::map<std::string, std::vector<std::uint8_t>> extra;
  /// Free text stored in the file's metadata (e.g. "after global placement").
  std::string note;
};

struct SaveReport {
  std::size_t raw_bytes = 0;    ///< Column bytes before compression.
  std::size_t file_bytes = 0;
  std::size_t sections = 0, chunks = 0;
  double seconds = 0.0;
};

struct SectionInfo {
  std::string tag;
  std::uint32_t elem_size = 0;
  std::uint64_t count = 0;
  std::uint64_t raw_bytes = 0, stored_bytes = 0;
  bool compressed = false;
  std::uint16_t version = 0;
};

struct DesignFileInfo {
  std::uint16_t major = 0, minor = 0;
  std::map<std::string, std::string> meta;  ///< name, counts, writer, created, note, ...
  std::vector<SectionInfo> sections;
  std::size_t file_bytes = 0;
};

struct LoadOptions {
  bool verify = true;  ///< Check section checksums.
};

/// Saves atomically (writes path.tmp, then renames). Throws on I/O errors.
SaveReport save_design(const Design& d, const std::string& path, const SaveOptions& opts = {});

/// Loads a design file. `info` receives the header/TOC, `extra` the
/// x.<tag> sections. Throws ParseError on a corrupt or incompatible file.
Design load_design(const std::string& path, const LoadOptions& opts = {}, DesignFileInfo* info = nullptr,
                   std::map<std::string, std::vector<std::uint8_t>>* extra = nullptr);

/// Header and section table only (cheap).
DesignFileInfo inspect_design_file(const std::string& path);

/// True if the file starts with the design-file magic.
bool is_design_file(const std::string& path);

/// Saves `copy` on a background job thread (take the copy at a flush point
/// of the running engine: `Design copy = design;`). `report` (optional) must
/// outlive the job.
JobHandle start_save_job(Design copy, std::string path, SaveOptions opts = {},
                         SaveReport* report = nullptr);

}  // namespace hermes::io
