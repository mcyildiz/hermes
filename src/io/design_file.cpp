#include "hermes/io/design_file.hpp"

#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <limits>

#include "codec.hpp"
#include "hermes/db/design_access.hpp"
#include "hermes/io/common.hpp"
#include "hermes/log/log.hpp"
#include "hermes/par/parallel.hpp"

static_assert(std::endian::native == std::endian::little, "design files assume a little-endian host");
static_assert(sizeof(std::size_t) == 8, "design files assume 64-bit size_t");

namespace hermes::io {

namespace {

HERMES_LOG_CHANNEL(lg_hdb, "io.hdb");
using DA = detail::DesignAccess;

constexpr char kMagic[8] = {'H', 'E', 'R', 'M', 'E', 'S', 'D', 'B'};
constexpr std::size_t kHeaderBytes = 64;
constexpr std::size_t kTagBytes = 40;
constexpr std::size_t kAlign = 64;
constexpr std::uint64_t kRawChunk = std::uint64_t{1} << 63;  // chunk-table flag: stored unfiltered-uncompressed

enum Filter : std::uint8_t { kNone = 0, kDelta = 1, kShuffle = 2 };
enum Codec : std::uint8_t { kRaw = 0, kLz = 1 };

#pragma pack(push, 1)
struct Header {
  char magic[8];
  std::uint16_t major, minor;
  std::uint32_t header_bytes;
  std::uint64_t toc_offset, toc_bytes, toc_hash, file_bytes;
  std::uint32_t num_sections, flags;
  std::uint8_t reserved[8];
};
struct TocEntry {
  char tag[kTagBytes];
  std::uint32_t elem_size;
  std::uint8_t codec, filter;
  std::uint16_t version;
  std::uint64_t count, raw_bytes, offset, stored_bytes;
  std::uint32_t chunk_elems, num_chunks;
  std::uint64_t hash;
};
#pragma pack(pop)
static_assert(sizeof(Header) == kHeaderBytes);
static_assert(sizeof(TocEntry) == 96);

// --- Sections on the save side ---------------------------------------------

struct OutSection {
  std::string tag;
  std::uint32_t elem = 1;
  std::uint8_t filter = kNone;
  std::uint16_t version = 1;
  std::uint64_t count = 0;
  std::function<void(std::size_t lo, std::size_t hi, std::uint8_t* dst)> fill;
};

template <class T, class Get>
OutSection out_col(std::string tag, std::size_t n, std::uint8_t filter, Get get) {
  OutSection s;
  s.tag = std::move(tag);
  s.elem = sizeof(T);
  s.filter = filter;
  s.count = n;
  s.fill = [get](std::size_t lo, std::size_t hi, std::uint8_t* dst) {
    for (std::size_t i = lo; i < hi; ++i) {
      const T v = get(i);
      std::memcpy(dst + (i - lo) * sizeof(T), &v, sizeof(T));
    }
  };
  return s;
}

template <class T>
OutSection out_span(std::string tag, std::span<const T> v, std::uint8_t filter) {
  OutSection s;
  s.tag = std::move(tag);
  s.elem = sizeof(T);
  s.filter = filter;
  s.count = v.size();
  s.fill = [v](std::size_t lo, std::size_t hi, std::uint8_t* dst) {
    std::memcpy(dst, v.data() + lo, (hi - lo) * sizeof(T));
  };
  return s;
}

OutSection out_blob(std::string tag, std::vector<std::uint8_t> bytes) {
  auto owned = std::make_shared<std::vector<std::uint8_t>>(std::move(bytes));
  OutSection s;
  s.tag = std::move(tag);
  s.elem = 1;
  s.count = owned->size();
  s.fill = [owned](std::size_t lo, std::size_t hi, std::uint8_t* dst) {
    std::memcpy(dst, owned->data() + lo, hi - lo);
  };
  return s;
}

void add_names(std::vector<OutSection>& out, const std::string& prefix, const NameTable& t) {
  out.push_back(out_span<char>(prefix + ".chars", t.chars(), kNone));
  out.push_back(out_span<std::size_t>(prefix + ".offsets", t.offsets(), kDelta | kShuffle));
}

// Small binary writer/reader for blobs (route grid).
struct BlobWriter {
  std::vector<std::uint8_t> b;
  template <class T>
  void put(T v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
    b.insert(b.end(), p, p + sizeof(T));
  }
  template <class T>
  void vec(const std::vector<T>& v) {
    put<std::uint64_t>(v.size());
    const auto* p = reinterpret_cast<const std::uint8_t*>(v.data());
    b.insert(b.end(), p, p + v.size() * sizeof(T));
  }
};
struct BlobReader {
  const std::uint8_t* p;
  std::size_t n, pos = 0;
  template <class T>
  T get() {
    if (pos + sizeof(T) > n) throw ParseError("design file: truncated blob");
    T v;
    std::memcpy(&v, p + pos, sizeof(T));
    pos += sizeof(T);
    return v;
  }
  template <class T>
  void vec(std::vector<T>& v) {
    const std::uint64_t k = get<std::uint64_t>();
    if (k > (n - pos) / sizeof(T)) throw ParseError("design file: truncated blob");
    v.resize(k);
    std::memcpy(v.data(), p + pos, k * sizeof(T));
    pos += k * sizeof(T);
  }
};

std::vector<std::uint8_t> route_blob(const RouteGrid& g) {
  BlobWriter w;
  w.put(g.nx);
  w.put(g.ny);
  w.put(g.num_layers);
  w.put(g.origin.x);
  w.put(g.origin.y);
  w.put(g.tile_w);
  w.put(g.tile_h);
  w.put(g.blockage_porosity);
  w.vec(g.vertical_capacity);
  w.vec(g.horizontal_capacity);
  w.vec(g.min_wire_width);
  w.vec(g.min_wire_spacing);
  w.vec(g.via_spacing);
  w.vec(g.pin_layer_cell);
  w.vec(g.pin_layer);
  w.vec(g.blockage_cell);
  w.vec(g.blockage_begin);
  w.vec(g.blockage_layers);
  w.vec(g.adjust_edges);
  w.vec(g.adjust_capacity);
  return std::move(w.b);
}

// Routing layers: count, then per layer: dir, pitch, width, spacing, name.
std::vector<std::uint8_t> layers_blob(std::span<const RoutingLayer> layers) {
  BlobWriter w;
  w.put<std::uint64_t>(layers.size());
  for (const RoutingLayer& l : layers) {
    w.put(static_cast<std::uint8_t>(l.dir));
    w.put(l.pitch);
    w.put(l.width);
    w.put(l.spacing);
    w.vec(std::vector<char>(l.name.begin(), l.name.end()));
  }
  return std::move(w.b);
}

std::vector<RoutingLayer> parse_layers_blob(const std::uint8_t* p, std::size_t n) {
  BlobReader r{p, n};
  const std::uint64_t k = r.get<std::uint64_t>();
  if (k > n) throw ParseError("design file: bad route.layers section");
  std::vector<RoutingLayer> out(k);
  for (RoutingLayer& l : out) {
    l.dir = r.get<std::uint8_t>() ? LayerDir::Vertical : LayerDir::Horizontal;
    l.pitch = r.get<Dbu>();
    l.width = r.get<Dbu>();
    l.spacing = r.get<Dbu>();
    std::vector<char> name;
    r.vec(name);
    l.name.assign(name.begin(), name.end());
  }
  return out;
}

RouteGrid parse_route_blob(const std::uint8_t* p, std::size_t n) {
  BlobReader r{p, n};
  RouteGrid g;
  g.nx = r.get<std::int32_t>();
  g.ny = r.get<std::int32_t>();
  g.num_layers = r.get<std::int32_t>();
  g.origin.x = r.get<Dbu>();
  g.origin.y = r.get<Dbu>();
  g.tile_w = r.get<Dbu>();
  g.tile_h = r.get<Dbu>();
  g.blockage_porosity = r.get<double>();
  r.vec(g.vertical_capacity);
  r.vec(g.horizontal_capacity);
  r.vec(g.min_wire_width);
  r.vec(g.min_wire_spacing);
  r.vec(g.via_spacing);
  r.vec(g.pin_layer_cell);
  r.vec(g.pin_layer);
  r.vec(g.blockage_cell);
  r.vec(g.blockage_begin);
  r.vec(g.blockage_layers);
  r.vec(g.adjust_edges);
  r.vec(g.adjust_capacity);
  return g;
}

std::uint8_t cell_flags(const Cell& c) {
  return static_cast<std::uint8_t>((static_cast<unsigned>(c.orient) & 7) | (c.is_macro << 3) |
                                   (c.fixed << 4) | (c.removed << 5) | (c.placed << 6));
}

std::string meta_text(const Design& d, const SaveOptions& o) {
  std::string m;
  auto kv = [&](std::string_view k, const std::string& v) {
    m += k;
    m += '=';
    for (char c : v) m += (c == '\n' || c == '\r') ? ' ' : c;
    m += '\n';
  };
  kv("writer", "hermes 0.1.0");
  kv("created", std::to_string(std::time(nullptr)));
  kv("name", d.name());
  kv("num_cells", std::to_string(d.num_cells()));
  kv("num_nets", std::to_string(d.num_nets()));
  kv("num_pins", std::to_string(d.num_pins()));
  kv("num_rows", std::to_string(d.rows().size()));
  const Rect& r = d.die();
  kv("die", std::to_string(r.xlo) + " " + std::to_string(r.ylo) + " " + std::to_string(r.xhi) + " " +
                std::to_string(r.yhi));
  kv("row_height", std::to_string(d.row_height()));
  std::string dbu;
  put_double(dbu, d.dbu_per_micron());
  kv("dbu_per_micron", dbu);
  if (!o.note.empty()) kv("note", o.note);
  return m;
}

std::map<std::string, std::string> parse_meta(std::string_view t) {
  std::map<std::string, std::string> m;
  while (!t.empty()) {
    const std::size_t nl = t.find('\n');
    const std::string_view line = t.substr(0, nl);
    const std::size_t eq = line.find('=');
    if (eq != std::string_view::npos) m[std::string(line.substr(0, eq))] = std::string(line.substr(eq + 1));
    if (nl == std::string_view::npos) break;
    t.remove_prefix(nl + 1);
  }
  return m;
}

std::vector<OutSection> design_sections(const Design& d, const SaveOptions& o) {
  std::vector<OutSection> s;
  {
    const std::string m = meta_text(d, o);
    s.push_back(out_blob("meta", std::vector<std::uint8_t>(m.begin(), m.end())));
  }
  const std::span<const Cell> cells = d.cells();
  const std::size_t nc = cells.size();
  s.push_back(out_col<Dbu>("cell.x", nc, kShuffle, [cells](std::size_t i) { return cells[i].loc.x; }));
  s.push_back(out_col<Dbu>("cell.y", nc, kShuffle, [cells](std::size_t i) { return cells[i].loc.y; }));
  s.push_back(out_col<Dbu>("cell.w", nc, kShuffle, [cells](std::size_t i) { return cells[i].width; }));
  s.push_back(out_col<Dbu>("cell.h", nc, kShuffle, [cells](std::size_t i) { return cells[i].height; }));
  s.push_back(out_col<std::uint8_t>("cell.flags", nc, kNone, [cells](std::size_t i) { return cell_flags(cells[i]); }));
  const std::span<const Net> nets = d.nets();
  s.push_back(out_col<double>("net.weight", nets.size(), kShuffle, [nets](std::size_t i) { return nets[i].weight; }));
  s.push_back(out_col<std::uint8_t>("net.flags", nets.size(), kNone,
                                    [nets](std::size_t i) { return static_cast<std::uint8_t>(nets[i].removed); }));
  const std::span<const Pin> pins = d.pins();
  const std::size_t np = pins.size();
  s.push_back(out_col<Id>("pin.cell", np, kDelta | kShuffle, [pins](std::size_t i) { return pins[i].cell; }));
  s.push_back(out_col<Id>("pin.net", np, kDelta | kShuffle, [pins](std::size_t i) { return pins[i].net; }));
  s.push_back(out_col<Dbu>("pin.dx", np, kShuffle, [pins](std::size_t i) { return pins[i].offset.x; }));
  s.push_back(out_col<Dbu>("pin.dy", np, kShuffle, [pins](std::size_t i) { return pins[i].offset.y; }));
  s.push_back(out_col<std::uint8_t>("pin.dir", np, kNone,
                                    [pins](std::size_t i) { return static_cast<std::uint8_t>(pins[i].dir); }));
  s.push_back(out_span<Id>("pin.name", d.pin_name_ids(), kShuffle));
  // Connectivity pools: list sizes + items in list order (holes dropped).
  auto add_pool = [&](const std::string& tag, const IdListPool& pool) {
    const auto ranges = pool.ranges();
    const auto raw = pool.raw_data();
    s.push_back(out_col<std::uint32_t>(tag + ".sizes", ranges.size(), kShuffle,
                                       [ranges](std::size_t i) { return ranges[i].size; }));
    if (pool.wasted_slots() == 0 && raw.size() == pool.num_items()) {
      bool packed = true;  // Ranges in order and contiguous: copy as is.
      std::uint64_t pos = 0;
      for (const auto& r : ranges) {
        if (r.begin != pos) {
          packed = false;
          break;
        }
        pos += r.size;
      }
      if (packed) {
        s.push_back(out_span<Id>(tag + ".data", raw, kDelta | kShuffle));
        return;
      }
    }
    auto flat = std::make_shared<std::vector<Id>>();
    flat->reserve(pool.num_items());
    for (std::size_t l = 0; l < ranges.size(); ++l) {
      const auto li = pool.list(static_cast<Id>(l));
      flat->insert(flat->end(), li.begin(), li.end());
    }
    OutSection o2 = out_span<Id>(tag + ".data", std::span<const Id>(*flat), kDelta | kShuffle);
    auto fill = o2.fill;
    o2.fill = [flat, fill](std::size_t lo, std::size_t hi, std::uint8_t* dst) { fill(lo, hi, dst); };
    s.push_back(std::move(o2));
  };
  add_pool("net_pins", d.net_pin_pool());
  add_pool("cell_pins", d.cell_pin_pool());
  s.push_back(out_span<Id>("io_pins", d.io_pins(), kDelta | kShuffle));
  add_names(s, "names.cell", d.cell_names());
  add_names(s, "names.net", d.net_names());
  add_names(s, "names.pin", d.pin_names());
  add_names(s, "names.master", d.master_names());
  add_names(s, "names.site", d.site_names());
  s.push_back(out_span<Id>("cell.master", d.cell_master_ids(), kShuffle));
  const std::span<const Row> rows = d.rows();
  const std::size_t nr = rows.size();
  s.push_back(out_col<Dbu>("row.x", nr, kShuffle, [rows](std::size_t i) { return rows[i].origin.x; }));
  s.push_back(out_col<Dbu>("row.y", nr, kDelta | kShuffle, [rows](std::size_t i) { return rows[i].origin.y; }));
  s.push_back(out_col<Dbu>("row.site_w", nr, kShuffle, [rows](std::size_t i) { return rows[i].site_width; }));
  s.push_back(out_col<Dbu>("row.site_h", nr, kShuffle, [rows](std::size_t i) { return rows[i].site_height; }));
  s.push_back(out_col<Dbu>("row.step", nr, kShuffle, [rows](std::size_t i) { return rows[i].site_step; }));
  s.push_back(out_col<std::int32_t>("row.num_sites", nr, kShuffle, [rows](std::size_t i) { return rows[i].num_sites; }));
  s.push_back(out_col<std::uint8_t>("row.orient", nr, kNone,
                                    [rows](std::size_t i) { return static_cast<std::uint8_t>(rows[i].orient); }));
  s.push_back(out_col<Id>("row.site", nr, kShuffle, [rows](std::size_t i) { return rows[i].site; }));
  if (!d.route_grid().empty()) s.push_back(out_blob("route.grid", route_blob(d.route_grid())));
  if (!d.routing_layers().empty()) s.push_back(out_blob("route.layers", layers_blob(d.routing_layers())));
  for (const auto& [tag, bytes] : o.extra) {
    if (tag.size() + 2 >= kTagBytes) throw std::invalid_argument("extra section tag too long: " + tag);
    s.push_back(out_blob("x." + tag, bytes));
  }
  return s;
}

class File {
 public:
  explicit File(const std::string& path) : path_(path), f_(std::fopen(path.c_str(), "wb")) {
    if (!f_) throw std::runtime_error("cannot write " + path + ": " + std::strerror(errno));
    std::setvbuf(f_, nullptr, _IOFBF, 1 << 22);
  }
  ~File() {
    if (f_) std::fclose(f_);
  }
  void write(const void* p, std::size_t n) {
    if (n && std::fwrite(p, 1, n, f_) != n) throw std::runtime_error("error writing " + path_);
    pos_ += n;
  }
  void pad_to(std::size_t off) {
    static const std::uint8_t zeros[kAlign] = {};
    while (pos_ < off) write(zeros, std::min(kAlign, off - pos_));
  }
  void seek_start() {
    if (std::fseek(f_, 0, SEEK_SET) != 0) throw std::runtime_error("error writing " + path_);
  }
  void close() {
    const int rc = std::fclose(f_);
    f_ = nullptr;
    if (rc != 0) throw std::runtime_error("error writing " + path_);
  }
  std::size_t pos() const { return pos_; }

 private:
  std::string path_;
  std::FILE* f_;
  std::size_t pos_ = 0;
};

std::size_t align_up(std::size_t v) { return (v + kAlign - 1) / kAlign * kAlign; }

struct ParsedFile {
  Header h{};
  std::vector<TocEntry> toc;
};

ParsedFile parse_header(const InputFile& f, bool verify) {
  ParsedFile pf;
  const std::string& path = f.path();
  if (f.size() < kHeaderBytes || std::memcmp(f.data(), kMagic, 8) != 0) {
    throw ParseError(path + ": not a hermes design file");
  }
  std::memcpy(&pf.h, f.data(), kHeaderBytes);
  if (pf.h.major != kDesignFileMajor) {
    throw ParseError(path + ": design file format " + std::to_string(pf.h.major) + "." +
                     std::to_string(pf.h.minor) + " is not supported by this hermes (reads format " +
                     std::to_string(kDesignFileMajor) + ".x)");
  }
  if (pf.h.toc_offset > f.size() || pf.h.toc_bytes > f.size() - pf.h.toc_offset ||
      pf.h.toc_bytes != std::uint64_t{pf.h.num_sections} * sizeof(TocEntry)) {
    throw ParseError(path + ": corrupt section table (file truncated?)");
  }
  const auto* toc = reinterpret_cast<const std::uint8_t*>(f.data()) + pf.h.toc_offset;
  if (verify && codec::hash64(toc, pf.h.toc_bytes) != pf.h.toc_hash) {
    throw ParseError(path + ": section table checksum mismatch");
  }
  pf.toc.resize(pf.h.num_sections);
  std::memcpy(pf.toc.data(), toc, pf.h.toc_bytes);
  for (const TocEntry& e : pf.toc) {
    if (e.offset > f.size() || e.stored_bytes > f.size() - e.offset || e.elem_size == 0 ||
        e.raw_bytes != e.count * e.elem_size) {
      throw ParseError(path + ": corrupt section '" + std::string(e.tag, strnlen(e.tag, kTagBytes)) + "'");
    }
  }
  return pf;
}

std::string tag_of(const TocEntry& e) { return std::string(e.tag, strnlen(e.tag, kTagBytes)); }

}  // namespace

// ---------------------------------------------------------------------------
// Save
// ---------------------------------------------------------------------------

SaveReport save_design(const Design& d, const std::string& path, const SaveOptions& opts) {
  const auto t0 = std::chrono::steady_clock::now();
  std::vector<OutSection> secs = design_sections(d, opts);

  struct Job {
    std::size_t sec, chunk;
  };
  std::vector<TocEntry> toc(secs.size());
  std::vector<std::vector<Job>> sec_jobs(secs.size());
  std::vector<Job> jobs;
  for (std::size_t i = 0; i < secs.size(); ++i) {
    const OutSection& s = secs[i];
    TocEntry& e = toc[i];
    std::memset(&e, 0, sizeof(e));
    if (s.tag.size() >= kTagBytes) throw std::logic_error("section tag too long");
    std::memcpy(e.tag, s.tag.data(), s.tag.size());
    e.elem_size = s.elem;
    e.codec = opts.compress ? kLz : kRaw;
    e.filter = opts.compress ? s.filter : static_cast<std::uint8_t>(kNone);
    e.version = s.version;
    e.count = s.count;
    e.raw_bytes = s.count * s.elem;
    e.chunk_elems = static_cast<std::uint32_t>(std::max<std::size_t>(1, opts.chunk_bytes / s.elem));
    e.num_chunks = static_cast<std::uint32_t>((s.count + e.chunk_elems - 1) / e.chunk_elems);
    for (std::size_t c = 0; c < e.num_chunks; ++c) jobs.push_back({i, c});
  }

  // Fill, filter, compress and hash every chunk in parallel.
  std::vector<std::vector<std::uint8_t>> out(jobs.size());
  std::vector<std::uint64_t> chunk_hash(jobs.size());
  std::vector<std::uint8_t> chunk_raw(jobs.size(), 0);
  par::parallel_for_each(0, jobs.size(), [&](std::size_t j) {
    const OutSection& s = secs[jobs[j].sec];
    const TocEntry& e = toc[jobs[j].sec];
    const std::size_t lo = jobs[j].chunk * e.chunk_elems;
    const std::size_t hi = std::min<std::size_t>(s.count, lo + e.chunk_elems);
    const std::size_t n = (hi - lo) * s.elem;
    std::vector<std::uint8_t> buf(n);
    s.fill(lo, hi, buf.data());
    if (e.codec == kRaw) {
      out[j] = std::move(buf);
    } else {
      if (e.filter & kDelta) codec::delta_encode(buf.data(), hi - lo, s.elem);
      if ((e.filter & kShuffle) && s.elem > 1) {
        std::vector<std::uint8_t> sh(n);
        codec::shuffle(buf.data(), sh.data(), hi - lo, s.elem);
        buf.swap(sh);
      }
      codec::compress(buf.data(), n, out[j]);
      if (out[j].size() >= n) {  // Incompressible: keep the filtered bytes.
        out[j] = std::move(buf);
        chunk_raw[j] = 1;
      }
    }
    chunk_hash[j] = codec::hash64(out[j].data(), out[j].size());
  });

  // Layout.
  std::size_t off = kHeaderBytes, j0 = 0, raw_total = 0;
  for (std::size_t i = 0; i < secs.size(); ++i) {
    TocEntry& e = toc[i];
    off = align_up(off);
    e.offset = off;
    std::size_t bytes = e.codec == kRaw ? 0 : std::size_t{e.num_chunks} * 8;
    std::vector<std::uint64_t> hs(e.num_chunks);
    for (std::size_t c = 0; c < e.num_chunks; ++c) {
      bytes += out[j0 + c].size();
      hs[c] = chunk_hash[j0 + c];
    }
    e.stored_bytes = bytes;
    e.hash = codec::hash64(reinterpret_cast<const std::uint8_t*>(hs.data()), hs.size() * 8, i);
    off += bytes;
    raw_total += e.raw_bytes;
    j0 += e.num_chunks;
  }
  const std::size_t toc_off = align_up(off);

  namespace fs = std::filesystem;
  if (const fs::path parent = fs::path(path).parent_path(); !parent.empty()) fs::create_directories(parent);
  const std::string tmp = path + ".tmp";
  {
    File f(tmp);
    Header h{};
    f.write(&h, sizeof(h));  // Placeholder, rewritten at the end.
    j0 = 0;
    for (std::size_t i = 0; i < secs.size(); ++i) {
      const TocEntry& e = toc[i];
      f.pad_to(e.offset);
      if (e.codec != kRaw) {
        std::vector<std::uint64_t> table(e.num_chunks);
        for (std::size_t c = 0; c < e.num_chunks; ++c) {
          table[c] = out[j0 + c].size() | (chunk_raw[j0 + c] ? kRawChunk : 0);
        }
        f.write(table.data(), table.size() * 8);
      }
      for (std::size_t c = 0; c < e.num_chunks; ++c) {
        f.write(out[j0 + c].data(), out[j0 + c].size());
        std::vector<std::uint8_t>().swap(out[j0 + c]);
      }
      j0 += e.num_chunks;
    }
    f.pad_to(toc_off);
    f.write(toc.data(), toc.size() * sizeof(TocEntry));
    std::memcpy(h.magic, kMagic, 8);
    h.major = kDesignFileMajor;
    h.minor = kDesignFileMinor;
    h.header_bytes = kHeaderBytes;
    h.toc_offset = toc_off;
    h.toc_bytes = toc.size() * sizeof(TocEntry);
    h.toc_hash = codec::hash64(reinterpret_cast<const std::uint8_t*>(toc.data()), h.toc_bytes);
    h.file_bytes = f.pos();
    h.num_sections = static_cast<std::uint32_t>(toc.size());
    f.seek_start();
    f.write(&h, sizeof(h));
    f.close();
    SaveReport rep;
    rep.raw_bytes = raw_total;
    rep.file_bytes = h.file_bytes;
    rep.sections = secs.size();
    rep.chunks = jobs.size();
    fs::rename(tmp, path);
    rep.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    HLOG_INFO(lg_hdb, "saved {}: {} cells, {} nets, {} pins, {:.1f} MB ({:.1f} MB raw) in {:.3f} s", path,
              d.num_cells(), d.num_nets(), d.num_pins(), rep.file_bytes / 1e6, rep.raw_bytes / 1e6, rep.seconds);
    return rep;
  }
}

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------

namespace {

struct Sink {
  std::uint32_t elem = 1;
  bool required = true;
  /// Called once (serially) with the element count before decoding.
  std::function<void(std::uint64_t count)> prepare;
  /// Called per decoded chunk (in parallel): elements [lo, hi).
  std::function<void(std::size_t lo, std::size_t hi, const std::uint8_t* src)> put;
};

template <class T>
Sink vec_sink(std::vector<T>& v, bool required = true) {
  Sink s;
  s.elem = sizeof(T);
  s.required = required;
  s.prepare = [&v](std::uint64_t n) { v.resize(n); };
  s.put = [&v](std::size_t lo, std::size_t hi, const std::uint8_t* src) {
    std::memcpy(v.data() + lo, src, (hi - lo) * sizeof(T));
  };
  return s;
}

template <class T, class Obj, class Set>
Sink field_sink(std::vector<Obj>& v, std::uint64_t expect, const char* tag, Set set, bool required = true) {
  Sink s;
  s.elem = sizeof(T);
  s.required = required;
  s.prepare = [expect, tag](std::uint64_t n) {
    if (n != expect) throw ParseError(std::string("design file: section ") + tag + " has the wrong length");
  };
  s.put = [&v, set](std::size_t lo, std::size_t hi, const std::uint8_t* src) {
    for (std::size_t i = lo; i < hi; ++i) {
      T x;
      std::memcpy(&x, src + (i - lo) * sizeof(T), sizeof(T));
      set(v[i], x);
    }
  };
  return s;
}

std::uint64_t meta_u64(const std::map<std::string, std::string>& m, const char* key) {
  const auto it = m.find(key);
  long long v = 0;
  if (it == m.end() || !parse_int(it->second, v) || v < 0) {
    throw ParseError(std::string("design file: bad or missing meta '") + key + "'");
  }
  return static_cast<std::uint64_t>(v);
}

}  // namespace

DesignFileInfo inspect_design_file(const std::string& path) {
  const InputFile f(path);
  const ParsedFile pf = parse_header(f, true);
  DesignFileInfo info;
  info.major = pf.h.major;
  info.minor = pf.h.minor;
  info.file_bytes = f.size();
  for (const TocEntry& e : pf.toc) {
    SectionInfo si;
    si.tag = tag_of(e);
    si.elem_size = e.elem_size;
    si.count = e.count;
    si.raw_bytes = e.raw_bytes;
    si.stored_bytes = e.stored_bytes;
    si.compressed = e.codec != kRaw;
    si.version = e.version;
    info.sections.push_back(si);
    if (si.tag == "meta" && e.codec == kRaw) {
      info.meta = parse_meta({f.data() + e.offset, e.raw_bytes});
    }
  }
  if (info.meta.empty()) {
    // Compressed meta: decode just that section.
    for (const TocEntry& e : pf.toc) {
      if (tag_of(e) != "meta" || e.codec == kRaw) continue;
      std::vector<std::uint8_t> raw(e.raw_bytes);
      const auto* base = reinterpret_cast<const std::uint8_t*>(f.data()) + e.offset;
      const auto* table = reinterpret_cast<const std::uint64_t*>(base);
      std::size_t pos = std::size_t{e.num_chunks} * 8, lo = 0;
      for (std::uint32_t c = 0; c < e.num_chunks; ++c) {
        const std::size_t n = std::min<std::size_t>(e.chunk_elems, e.count - lo);
        const std::uint64_t sz = table[c] & ~kRawChunk;
        if (table[c] & kRawChunk) {
          std::memcpy(raw.data() + lo, base + pos, n);
        } else if (!codec::decompress(base + pos, sz, raw.data() + lo, n)) {
          throw ParseError(path + ": corrupt meta section");
        }
        pos += sz;
        lo += n;
      }
      info.meta = parse_meta({reinterpret_cast<const char*>(raw.data()), raw.size()});
    }
  }
  return info;
}

bool is_design_file(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  char m[8] = {};
  const bool ok = std::fread(m, 1, 8, f) == 8 && std::memcmp(m, kMagic, 8) == 0;
  std::fclose(f);
  return ok;
}

Design load_design(const std::string& path, const LoadOptions& opts, DesignFileInfo* info_out,
                   std::map<std::string, std::vector<std::uint8_t>>* extra) {
  const auto t0 = std::chrono::steady_clock::now();
  const InputFile f(path);
  const ParsedFile pf = parse_header(f, opts.verify);
  const auto* base = reinterpret_cast<const std::uint8_t*>(f.data());

  // Decodes one section's chunk c into dst (raw bytes, unfiltered).
  struct SecLayout {
    const TocEntry* e = nullptr;
    std::vector<std::uint64_t> pos, size;  // per chunk: file offset, stored bytes
    std::vector<std::uint8_t> raw;         // per chunk: stored without codec
  };
  std::map<std::string, SecLayout> layout;
  for (const TocEntry& e : pf.toc) {
    SecLayout L;
    L.e = &e;
    if (e.codec == kRaw) {
      std::uint64_t p = e.offset;
      for (std::uint32_t c = 0; c < e.num_chunks; ++c) {
        const std::uint64_t n = std::min<std::uint64_t>(e.chunk_elems, e.count - std::uint64_t{c} * e.chunk_elems) * e.elem_size;
        L.pos.push_back(p);
        L.size.push_back(n);
        L.raw.push_back(1);
        p += n;
      }
      if (p - e.offset != e.stored_bytes) throw ParseError(path + ": corrupt section '" + tag_of(e) + "'");
    } else {
      if (std::uint64_t{e.num_chunks} * 8 > e.stored_bytes) throw ParseError(path + ": corrupt section '" + tag_of(e) + "'");
      std::uint64_t p = e.offset + std::uint64_t{e.num_chunks} * 8;
      for (std::uint32_t c = 0; c < e.num_chunks; ++c) {
        std::uint64_t t;
        std::memcpy(&t, base + e.offset + c * 8, 8);
        L.pos.push_back(p);
        L.size.push_back(t & ~kRawChunk);
        L.raw.push_back((t & kRawChunk) != 0);
        p += t & ~kRawChunk;
      }
      if (p != e.offset + e.stored_bytes) throw ParseError(path + ": corrupt section '" + tag_of(e) + "'");
    }
    if (e.version > 1) {
      HLOG_VERBOSE(lg_hdb, "{}: section {} has version {} (newer than this reader)", path, tag_of(e), e.version);
    }
    layout.emplace(tag_of(e), std::move(L));
  }

  auto decode_chunk = [&](const SecLayout& L, std::uint32_t c, std::vector<std::uint8_t>& tmp,
                          std::vector<std::uint8_t>& out) -> const std::uint8_t* {
    const TocEntry& e = *L.e;
    const std::size_t lo = std::size_t{c} * e.chunk_elems;
    const std::size_t n_el = std::min<std::size_t>(e.chunk_elems, e.count - lo);
    const std::size_t n = n_el * e.elem_size;
    const std::uint8_t* src = base + L.pos[c];
    if (e.codec == kRaw) return src;  // Straight from the mapping.
    tmp.resize(n);
    if (L.raw[c]) {
      if (L.size[c] != n) throw ParseError(path + ": corrupt chunk in '" + tag_of(e) + "'");
      std::memcpy(tmp.data(), src, n);
    } else if (!codec::decompress(src, L.size[c], tmp.data(), n)) {
      throw ParseError(path + ": corrupt chunk in '" + tag_of(e) + "'");
    }
    const std::uint8_t* p = tmp.data();
    if ((e.filter & kShuffle) && e.elem_size > 1) {
      out.resize(n);
      codec::unshuffle(tmp.data(), out.data(), n_el, e.elem_size);
      p = out.data();
    }
    if (e.filter & kDelta) {
      auto* w = const_cast<std::uint8_t*>(p);
      codec::delta_decode(w, n_el, e.elem_size);
    }
    return p;
  };
  auto read_whole = [&](const std::string& tag) {
    const SecLayout& L = layout.at(tag);
    std::vector<std::uint8_t> all(L.e->raw_bytes), tmp, out;
    for (std::uint32_t c = 0; c < L.e->num_chunks; ++c) {
      const std::uint8_t* p = decode_chunk(L, c, tmp, out);
      const std::size_t lo = std::size_t{c} * L.e->chunk_elems;
      const std::size_t n = std::min<std::size_t>(L.e->chunk_elems, L.e->count - lo) * L.e->elem_size;
      std::memcpy(all.data() + lo * L.e->elem_size, p, n);
    }
    return all;
  };

  if (!layout.count("meta")) throw ParseError(path + ": no meta section");
  const std::vector<std::uint8_t> meta_bytes = read_whole("meta");
  const auto meta = parse_meta({reinterpret_cast<const char*>(meta_bytes.data()), meta_bytes.size()});
  const std::uint64_t nc = meta_u64(meta, "num_cells"), nn = meta_u64(meta, "num_nets"),
                      np = meta_u64(meta, "num_pins");

  Design d(meta.count("name") ? meta.at("name") : std::string("top"));
  std::vector<Cell>& cells = DA::cells(d);
  std::vector<Net>& nets = DA::nets(d);
  std::vector<Pin>& pins = DA::pins(d);
  par::parallel_invoke([&] { cells.resize(nc); }, [&] { nets.resize(nn); }, [&] { pins.resize(np); });

  std::vector<std::uint32_t> net_sizes, cell_sizes;
  std::vector<Id> net_data, cell_data;
  struct Names {
    std::vector<char> chars;
    std::vector<std::size_t> offsets;
  } n_cell, n_net, n_pin, n_master, n_site;
  std::vector<Dbu> row_x, row_y, row_w, row_h, row_step;
  std::vector<std::int32_t> row_n;
  std::vector<std::uint8_t> row_o;
  std::vector<Id> row_site;

  std::map<std::string, Sink> sinks;
  sinks["cell.x"] = field_sink<Dbu>(cells, nc, "cell.x", [](Cell& c, Dbu v) { c.loc.x = v; });
  sinks["cell.y"] = field_sink<Dbu>(cells, nc, "cell.y", [](Cell& c, Dbu v) { c.loc.y = v; });
  sinks["cell.w"] = field_sink<Dbu>(cells, nc, "cell.w", [](Cell& c, Dbu v) { c.width = v; });
  sinks["cell.h"] = field_sink<Dbu>(cells, nc, "cell.h", [](Cell& c, Dbu v) { c.height = v; });
  sinks["cell.flags"] = field_sink<std::uint8_t>(cells, nc, "cell.flags", [](Cell& c, std::uint8_t f) {
    c.orient = static_cast<Orient>(f & 7);
    c.is_macro = f & 8;
    c.fixed = f & 16;
    c.removed = f & 32;
    c.placed = f & 64;
  });
  sinks["net.weight"] = field_sink<double>(nets, nn, "net.weight", [](Net& n, double w) { n.weight = w; });
  sinks["net.flags"] = field_sink<std::uint8_t>(nets, nn, "net.flags", [](Net& n, std::uint8_t f) { n.removed = f & 1; });
  sinks["pin.cell"] = field_sink<Id>(pins, np, "pin.cell", [](Pin& p, Id v) { p.cell = v; });
  sinks["pin.net"] = field_sink<Id>(pins, np, "pin.net", [](Pin& p, Id v) { p.net = v; });
  sinks["pin.dx"] = field_sink<Dbu>(pins, np, "pin.dx", [](Pin& p, Dbu v) { p.offset.x = v; });
  sinks["pin.dy"] = field_sink<Dbu>(pins, np, "pin.dy", [](Pin& p, Dbu v) { p.offset.y = v; });
  sinks["pin.dir"] = field_sink<std::uint8_t>(pins, np, "pin.dir", [](Pin& p, std::uint8_t v) { p.dir = static_cast<PinDir>(v); });
  sinks["pin.name"] = vec_sink(DA::pin_name_ids(d));
  sinks["net_pins.sizes"] = vec_sink(net_sizes);
  sinks["net_pins.data"] = vec_sink(net_data);
  sinks["cell_pins.sizes"] = vec_sink(cell_sizes);
  sinks["cell_pins.data"] = vec_sink(cell_data);
  sinks["io_pins"] = vec_sink(DA::io_pins(d));
  auto names = [&](const std::string& p, Names& n, bool req) {
    sinks[p + ".chars"] = vec_sink(n.chars, req);
    sinks[p + ".offsets"] = vec_sink(n.offsets, req);
  };
  names("names.cell", n_cell, true);
  names("names.net", n_net, true);
  names("names.pin", n_pin, true);
  names("names.master", n_master, false);
  names("names.site", n_site, false);
  sinks["cell.master"] = vec_sink(DA::cell_master(d), false);
  sinks["row.x"] = vec_sink(row_x, false);
  sinks["row.y"] = vec_sink(row_y, false);
  sinks["row.site_w"] = vec_sink(row_w, false);
  sinks["row.site_h"] = vec_sink(row_h, false);
  sinks["row.step"] = vec_sink(row_step, false);
  sinks["row.num_sites"] = vec_sink(row_n, false);
  sinks["row.orient"] = vec_sink(row_o, false);
  sinks["row.site"] = vec_sink(row_site, false);

  // Match sections to sinks; collect the chunk jobs.
  struct Job {
    const SecLayout* L;
    const Sink* sink;
    std::uint32_t chunk;
  };
  std::vector<Job> jobs;
  std::vector<std::function<void()>> prepares;
  for (auto& [tag, sink] : sinks) {
    const auto it = layout.find(tag);
    if (it == layout.end()) {
      if (sink.required) throw ParseError(path + ": missing section '" + tag + "'");
      continue;
    }
    const TocEntry& e = *it->second.e;
    if (e.elem_size != sink.elem) throw ParseError(path + ": section '" + tag + "' has an unexpected element size");
    prepares.push_back([&sink, n = e.count] { sink.prepare(n); });
    for (std::uint32_t c = 0; c < e.num_chunks; ++c) jobs.push_back({&it->second, &sink, c});
  }
  // Allocate the destinations concurrently (first touch is most of the cost).
  par::parallel_for_each(0, prepares.size(), [&](std::size_t i) { prepares[i](); });
  for (const auto& [tag, L] : layout) {
    if (!sinks.count(tag) && tag != "meta" && tag != "route.grid" && tag != "route.layers" && tag.rfind("x.", 0) != 0) {
      HLOG_VERBOSE(lg_hdb, "{}: skipping unknown section '{}' (written by a newer hermes?)", path, tag);
    }
  }
  std::vector<std::uint64_t> hashes(opts.verify ? jobs.size() : 0);
  par::parallel_for_each(0, jobs.size(), [&](std::size_t j) {
    thread_local std::vector<std::uint8_t> tmp, out;
    const Job& job = jobs[j];
    if (opts.verify) hashes[j] = codec::hash64(base + job.L->pos[job.chunk], job.L->size[job.chunk]);
    const std::uint8_t* p = decode_chunk(*job.L, job.chunk, tmp, out);
    const std::size_t lo = std::size_t{job.chunk} * job.L->e->chunk_elems;
    const std::size_t hi = std::min<std::size_t>(job.L->e->count, lo + job.L->e->chunk_elems);
    job.sink->put(lo, hi, p);
  });
  if (opts.verify) {
    // Section checksum = hash of its chunk hashes (jobs are in chunk order per section).
    std::size_t j = 0;
    while (j < jobs.size()) {
      const SecLayout* L = jobs[j].L;
      std::vector<std::uint64_t> hs;
      while (j < jobs.size() && jobs[j].L == L) hs.push_back(hashes[j++]);
      const std::size_t idx = static_cast<std::size_t>(L->e - pf.toc.data());
      if (codec::hash64(reinterpret_cast<const std::uint8_t*>(hs.data()), hs.size() * 8, idx) != L->e->hash) {
        throw ParseError(path + ": checksum mismatch in section '" + tag_of(*L->e) + "'");
      }
    }
  }

  // Assemble pools and names.
  try {
    DA::net_pins(d).assign(std::move(net_data), net_sizes);
    DA::cell_pins(d).assign(std::move(cell_data), cell_sizes);
    std::vector<std::uint8_t> cell_unmapped(nc), net_unmapped(nn);
    for (std::size_t i = 0; i < nc; ++i) cell_unmapped[i] = cells[i].removed;
    for (std::size_t i = 0; i < nn; ++i) net_unmapped[i] = nets[i].removed;
    auto assign = [](NameTable& t, Names& n, std::span<const std::uint8_t> um) {
      if (n.offsets.empty()) n.offsets.push_back(0);
      t.assign(std::move(n.chars), std::move(n.offsets), um);
    };
    par::parallel_invoke([&] { assign(DA::cell_names(d), n_cell, cell_unmapped); },
                         [&] { assign(DA::net_names(d), n_net, net_unmapped); },
                         [&] { assign(DA::pin_names(d), n_pin, {}); },
                         [&] { assign(DA::master_names(d), n_master, {}); },
                         [&] { assign(DA::site_names(d), n_site, {}); });
  } catch (const std::invalid_argument& e) {
    throw ParseError(path + ": " + e.what());
  }
  for (std::size_t p = 0; p < np; ++p) {
    if (pins[p].cell == kInvalidId) continue;
    if (pins[p].cell < 0 || static_cast<std::uint64_t>(pins[p].cell) >= nc) throw ParseError(path + ": bad pin cell id");
  }
  try {
    DA::finish(d);
  } catch (const std::exception& e) {
    throw ParseError(path + ": " + e.what());
  }

  // Floorplan.
  {
    long long v[4] = {};
    if (meta.count("die") && std::sscanf(meta.at("die").c_str(), "%lld %lld %lld %lld", &v[0], &v[1], &v[2], &v[3]) == 4) {
      d.set_die({v[0], v[1], v[2], v[3]});
    }
    std::vector<Row> rows(row_x.size());
    if (row_y.size() != rows.size() || row_w.size() != rows.size() || row_h.size() != rows.size() ||
        row_step.size() != rows.size() || row_n.size() != rows.size() || row_o.size() != rows.size() ||
        row_site.size() != rows.size()) {
      throw ParseError(path + ": row sections disagree");
    }
    for (std::size_t i = 0; i < rows.size(); ++i) {
      rows[i] = {{row_x[i], row_y[i]}, row_w[i], row_h[i], row_step[i], row_n[i], static_cast<Orient>(row_o[i] & 7), row_site[i]};
    }
    DA::rows(d) = std::move(rows);
    d.set_row_height(static_cast<Dbu>(meta_u64(meta, "row_height")));
    double dbu = 0;
    if (meta.count("dbu_per_micron")) parse_double(meta.at("dbu_per_micron"), dbu);
    d.set_dbu_per_micron(dbu);
    if (layout.count("route.grid")) {
      const auto b = read_whole("route.grid");
      d.set_route_grid(parse_route_blob(b.data(), b.size()));
    }
    if (layout.count("route.layers")) {
      const auto b = read_whole("route.layers");
      d.set_routing_layers(parse_layers_blob(b.data(), b.size()));
    }
  }
  if (extra) {
    extra->clear();
    for (const auto& [tag, L] : layout) {
      if (tag.rfind("x.", 0) == 0) (*extra)[tag.substr(2)] = read_whole(tag);
    }
  }
  d.flush();
  if (info_out) {
    info_out->major = pf.h.major;
    info_out->minor = pf.h.minor;
    info_out->meta = meta;
    info_out->file_bytes = f.size();
    info_out->sections.clear();
    for (const TocEntry& e : pf.toc) {
      info_out->sections.push_back({tag_of(e), e.elem_size, e.count, e.raw_bytes, e.stored_bytes, e.codec != kRaw, e.version});
    }
  }
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  HLOG_INFO(lg_hdb, "loaded {}: {} cells, {} nets, {} pins in {:.3f} s", path, nc, nn, np, secs);
  return d;
}

JobHandle start_save_job(Design copy, std::string path, SaveOptions opts, SaveReport* report) {
  auto d = std::make_shared<Design>(std::move(copy));
  return start_job("save " + path, [d, path = std::move(path), opts = std::move(opts), report](RunControl& ctl) {
    ctl.progress(0.0, "saving");
    const SaveReport r = save_design(*d, path, opts);
    if (report) *report = r;
    ctl.progress(1.0, "saved");
  });
}

}  // namespace hermes::io
