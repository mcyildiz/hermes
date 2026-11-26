// Core database tests: editing ops, change tracking, transactions/undo,
// incremental helpers and snapshots for concurrent readers.

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <functional>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "hermes/db/bin_grid.hpp"
#include "hermes/db/design.hpp"
#include "hermes/db/geometry.hpp"
#include "hermes/db/snapshot.hpp"
#include "hermes/flow/flow.hpp"
#include "hermes/log/sinks.hpp"

using namespace hermes;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                          \
    }                                                                        \
  } while (0)

template <class E, class F>
bool throws(F&& f) {
  try {
    f();
  } catch (const E&) {
    return true;
  }
  return false;
}

/// Everything observable about a design, for before/after comparisons.
std::string dump(const Design& d) {
  std::string s;
  auto add = [&](auto... v) { ((s += std::to_string(v) + ","), ...); };
  add(d.die().xlo, d.die().ylo, d.die().xhi, d.die().yhi, d.row_height());
  s += "|cells";
  for (Id i = 0; i < static_cast<Id>(d.num_cells()); ++i) {
    const Cell& c = d.cell(i);
    add(c.loc.x, c.loc.y, c.width, c.height, static_cast<int>(c.orient), c.is_macro, c.fixed,
        c.removed);
    s += std::string(d.cell_name(i)) + (c.removed ? "" : std::to_string(d.find_cell(d.cell_name(i)))) + ":";
    for (Id p : d.cell_pins(i)) add(p);
    s += ";";
  }
  s += "|nets";
  for (Id i = 0; i < static_cast<Id>(d.num_nets()); ++i) {
    add(d.net(i).weight, d.net(i).removed);
    s += std::string(d.net_name(i)) +
         (d.net(i).removed ? "" : std::to_string(d.find_net(d.net_name(i)))) + ":";
    for (Id p : d.net_pins(i)) add(p);
    s += ";";
  }
  s += "|pins";
  for (Id i = 0; i < static_cast<Id>(d.num_pins()); ++i) {
    const Pin& p = d.pin(i);
    add(p.cell, p.net, p.offset.x, p.offset.y, static_cast<int>(p.dir));
    s += std::string(d.pin_name(i)) + ";";
  }
  s += "|io";
  for (Id p : d.io_pins()) add(p);
  return s;
}

/// Net/cell/pin lists agree with each other.
bool consistent(const Design& d) {
  for (Id p = 0; p < static_cast<Id>(d.num_pins()); ++p) {
    const Pin& pin = d.pin(p);
    if (pin.net != kInvalidId) {
      const auto l = d.net_pins(pin.net);
      if (std::count(l.begin(), l.end(), p) != 1) return false;
    }
    const auto owner = pin.cell == kInvalidId ? d.io_pins() : d.cell_pins(pin.cell);
    if (std::count(owner.begin(), owner.end(), p) != 1) return false;
  }
  for (Id n = 0; n < static_cast<Id>(d.num_nets()); ++n) {
    for (Id p : d.net_pins(n))
      if (d.pin(p).net != n) return false;
    if (d.net(n).removed && !d.net_pins(n).empty()) return false;
  }
  for (Id c = 0; c < static_cast<Id>(d.num_cells()); ++c) {
    for (Id p : d.cell_pins(c)) {
      if (d.pin(p).cell != c) return false;
      if (d.cell(c).removed && d.pin(p).net != kInvalidId) return false;
    }
  }
  return true;
}

Design small_design() {
  Design d("small");
  d.set_tracking(true);
  d.set_die({0, 0, 1000, 1000});
  d.set_row_height(10);
  const Id a = d.add_cell("a", 10, 10);
  const Id b = d.add_cell("b", 20, 10);
  const Id c = d.add_cell("c", 30, 10);
  d.move_cell(a, {0, 0});
  d.move_cell(b, {100, 200});
  d.move_cell(c, {500, 500});
  const Id n0 = d.add_net("n0");
  const Id n1 = d.add_net("n1");
  d.connect(a, n0, "Z", {5, 5}, PinDir::Output);
  d.connect(b, n0, "A", {0, 5});
  d.connect(b, n1, "Z", {20, 5}, PinDir::Output);
  d.connect(c, n1, "A", {0, 5});
  d.connect(kInvalidId, n1, "out", {1000, 500});
  return d;
}

// ---------------------------------------------------------------------------

void test_id_list_pool() {
  IdListPool p;
  const Id a = p.add_list(), b = p.add_list();
  for (int i = 0; i < 3; ++i) p.push_back(b, i);  // b at the tail: grows in place
  CHECK(p.wasted_slots() == 0);
  p.push_back(a, 10);  // a must move to the end
  p.push_back(a, 11);
  p.insert(a, 0, 9);
  CHECK((std::vector<Id>(p.list(a).begin(), p.list(a).end()) == std::vector<Id>{9, 10, 11}));
  CHECK((std::vector<Id>(p.list(b).begin(), p.list(b).end()) == std::vector<Id>{0, 1, 2}));
  p.erase_at(a, 1);
  CHECK(p.find(a, 11) == 1 && p.find(a, 10) == IdListPool::npos);
  CHECK(p.num_items() == 5);
  p.compact();
  CHECK(p.wasted_slots() == 0);
  CHECK((std::vector<Id>(p.list(a).begin(), p.list(a).end()) == std::vector<Id>{9, 11}));
  CHECK((std::vector<Id>(p.list(b).begin(), p.list(b).end()) == std::vector<Id>{0, 1, 2}));
  const Id c = p.add_list();
  CHECK(c == 2 && p.list(c).empty());
  p.pop_list();
  CHECK(p.num_lists() == 2);
  CHECK(throws<std::logic_error>([&] { p.pop_list(); }));  // b not empty
}

void test_name_table_erase() {
  NameTable t;
  for (int i = 0; i < 1000; ++i) t.add("n" + std::to_string(i));
  std::mt19937 rng(1);
  std::vector<bool> gone(1000, false);
  for (int k = 0; k < 600; ++k) {
    const int i = static_cast<int>(rng() % 1000);
    if (!gone[i]) {
      t.erase(i);
      gone[i] = true;
    } else if (rng() % 2) {
      t.restore(i);
      gone[i] = false;
    }
  }
  bool ok = true;
  for (int i = 0; i < 1000; ++i) {
    const std::string n = "n" + std::to_string(i);
    ok = ok && t.find(n) == (gone[i] ? kInvalidId : i) && t.name(i) == n;
  }
  CHECK(ok);
  const int g = static_cast<int>(std::find(gone.begin(), gone.end(), true) - gone.begin());
  const Id again = t.add("n" + std::to_string(g));  // An erased name is free again.
  CHECK(again == 1000 && t.find("n" + std::to_string(g)) == 1000);
  CHECK(throws<std::invalid_argument>([&] { t.restore(g); }));  // now taken
  t.pop_back();
  CHECK(t.size() == 1000 && t.find("n" + std::to_string(g)) == kInvalidId);
  t.restore(g);
  CHECK(t.find("n" + std::to_string(g)) == g);
}

void test_edit_ops() {
  Design d = small_design();
  CHECK(consistent(d));
  const Id b = d.find_cell("b"), n0 = d.find_net("n0"), n1 = d.find_net("n1");
  // Reconnect b/Z from n1 to n0: appended to n0, removed from n1 in order.
  const Id bz = d.find_pin(b, "Z");
  d.reconnect_pin(bz, n0);
  CHECK(d.pin(bz).net == n0 && d.net_pins(n0).back() == bz && d.net_pins(n1).size() == 2);
  CHECK(consistent(d));
  CHECK(throws<std::logic_error>([&] { d.connect_pin(bz, n1); }));  // already connected
  d.disconnect_pin(bz);
  d.disconnect_pin(bz);  // no-op
  CHECK(d.pin(bz).net == kInvalidId && d.net_pins(n0).size() == 2);
  // An unconnected pin can be created and connected later.
  const Id extra = d.add_pin(b, "B", {1, 1});
  CHECK(d.pin(extra).net == kInvalidId && d.find_pin(b, "B") == extra);
  d.connect_pin(extra, n1);
  CHECK(consistent(d));

  // Removing a cell disconnects its pins and frees its name.
  d.remove_cell(b);
  CHECK(d.cell(b).removed && d.find_cell("b") == kInvalidId && d.cell_name(b) == "b");
  CHECK(d.net_pins(n0).size() == 1 && d.net_pins(n1).size() == 2);
  CHECK(consistent(d));
  CHECK(throws<std::logic_error>([&] { d.remove_cell(b); }));
  CHECK(throws<std::logic_error>([&] { d.connect_pin(extra, n1); }));  // cell removed
  const Id b2 = d.add_cell("b", 5, 5);
  CHECK(b2 == 3 && d.find_cell("b") == b2);

  d.remove_net(n1);
  CHECK(d.net(n1).removed && d.net_pins(n1).empty() && d.find_net("n1") == kInvalidId);
  CHECK(consistent(d));

  // Bad ids throw instead of corrupting memory.
  CHECK(throws<std::out_of_range>([&] { d.move_cell(99, {}); }));
  CHECK(throws<std::out_of_range>([&] { d.move_cell(-1, {}); }));
  CHECK(throws<std::out_of_range>([&] { d.connect(0, 42, "X"); }));
  CHECK(throws<std::out_of_range>([&] { d.set_net_weight(-3, 1.0); }));
  CHECK(d.num_pins() == 6);  // the failed connect created nothing
}

class Recorder final : public DesignListener {
 public:
  void on_flush(const Design&, const ChangeSet& c) override {
    ++calls;
    version = c.version;
    all = c.all;
    structure = c.structure;
    floorplan = c.floorplan;
    cells.assign(c.cells.ids().begin(), c.cells.ids().end());
    nets.assign(c.nets.ids().begin(), c.nets.ids().end());
    pins.assign(c.pins.ids().begin(), c.pins.ids().end());
    std::sort(cells.begin(), cells.end());
    std::sort(nets.begin(), nets.end());
  }
  int calls = 0;
  std::uint64_t version = 0;
  bool all = false, structure = false, floorplan = false;
  std::vector<Id> cells, nets, pins;
};

void test_tracking_off_by_default() {
  Design d("off");
  CHECK(!d.tracking());
  const Id a = d.add_cell("a", 1, 1);
  Recorder r;
  d.subscribe(&r);
  d.flush();
  d.move_cell(a, {5, 5});
  CHECK(d.pending().cells.empty() && d.pending().all);
  d.flush();
  CHECK(r.all && r.cells.empty() && r.version == 2);
  d.unsubscribe(&r);
}

void test_change_tracking() {
  Design d = small_design();
  Recorder r;
  d.subscribe(&r);
  CHECK(d.pending().all);  // fresh design: everything is new
  CHECK(d.flush() == 1 && r.calls == 1 && r.all && r.version == 1);
  CHECK(d.pending().empty());

  // Moving a cell marks it once; its nets are derived at flush time.
  const Id b = d.find_cell("b");
  for (int i = 0; i < 5; ++i) d.move_cell(b, {100 + i, 200});
  CHECK(d.pending().cells.size() == 1 && d.pending().nets.empty());
  d.flush();
  CHECK(!r.all && !r.structure && r.cells == std::vector<Id>{b});
  CHECK((r.nets == std::vector<Id>{0, 1}));

  // Pin offset and weight edits mark nets; die marks the floorplan.
  d.set_pin_offset(d.find_pin(kInvalidId, "out"), {990, 500});
  d.set_net_weight(0, 3.0);
  d.set_die({0, 0, 2000, 2000});
  d.flush();
  CHECK(r.cells.empty() && (r.nets == std::vector<Id>{0, 1}) && r.pins.size() == 1 && r.floorplan);

  // Structure: a new cell and a connect.
  const Id e = d.add_cell("e", 1, 1);
  d.connect(e, 0, "A");
  d.flush();
  CHECK(r.structure && r.cells == std::vector<Id>{e} && r.nets == std::vector<Id>{0});

  // An empty flush still bumps the version and notifies.
  d.flush();
  CHECK(r.calls == 5 && r.version == 5 && r.cells.empty() && r.nets.empty() && !r.all);

  // Tracking off: nothing recorded, next flush says `all`.
  d.set_tracking(false);
  d.move_cell(b, {0, 0});
  CHECK(d.pending().cells.empty() && d.pending().all);
  d.flush();
  CHECK(r.all && r.cells.empty());
  d.move_cell(b, {1, 1});
  d.flush();
  CHECK(r.all);  // stays `all` while off
  d.set_tracking(true);
  d.flush();     // the batch that turned it back on is still `all`
  CHECK(r.all);
  d.move_cell(b, {2, 2});
  d.flush();
  CHECK(!r.all && r.cells == std::vector<Id>{b});

  d.unsubscribe(&r);
  d.flush();
  CHECK(r.calls == 9);
}

void test_transaction_rollback_all_ops() {
  Design d = small_design();
  d.flush();
  const std::string before = dump(d);
  {
    Transaction tx(d);
    CHECK(d.in_transaction());
    d.move_cell(0, {7, 7});
    d.resize_cell(1, 40, 20);
    d.set_fixed(2, true);
    d.set_orient(2, Orient::FS);
    d.set_net_weight(1, 5.0);
    d.set_pin_offset(0, {1, 2});
    d.set_die({0, 0, 50, 50});
    d.set_row_height(7);
    const Id x = d.add_cell("x", 3, 3);
    const Id m = d.add_net("m");
    d.connect(x, m, "A");
    d.connect(0, m, "Q");
    d.reconnect_pin(d.find_pin(1, "Z"), m);
    d.remove_cell(2);
    d.remove_net(0);
    const Id y = d.add_cell("c", 1, 1);  // reuses the removed cell's name
    d.move_cell(y, {3, 3});
    CHECK(consistent(d));
    CHECK(dump(d) != before);
    CHECK(tx.size() > 15);
    tx.rollback();
  }
  CHECK(!d.in_transaction());
  CHECK(dump(d) == before);
  CHECK(consistent(d));
  // The rollback shows up as changes (with popped ids dropped).
  Recorder r;
  d.subscribe(&r);
  d.flush();
  CHECK(r.structure && r.floorplan && (r.cells == std::vector<Id>{0, 1, 2}));
  CHECK(std::all_of(r.pins.begin(), r.pins.end(), [&](Id p) { return p < static_cast<Id>(d.num_pins()); }));
  d.unsubscribe(&r);
}

void test_transaction_nesting_and_raii() {
  Design d = small_design();
  const std::string s0 = dump(d);
  {
    Transaction outer(d);
    d.move_cell(0, {1, 1});
    const std::string s1 = dump(d);
    {
      Transaction inner(d);
      d.move_cell(0, {2, 2});
      d.add_cell("tmp", 1, 1);
      CHECK(throws<std::logic_error>([&] { outer.commit(); }));  // inner still open
      inner.rollback();
    }
    CHECK(dump(d) == s1);
    {
      Transaction inner(d);
      d.move_cell(1, {3, 3});
      inner.commit();  // kept, but still undoable by the outer one
    }
    CHECK(dump(d) != s1);
    // outer goes out of scope without commit: rolled back
  }
  CHECK(dump(d) == s0);
  {
    Transaction tx(d);
    d.move_cell(0, {9, 9});
    tx.commit();
    CHECK(throws<std::logic_error>([&] { tx.rollback(); }));
  }
  CHECK(d.cell(0).loc == (Point{9, 9}));
  // Exception safety: an exception inside the scope rolls back.
  try {
    Transaction tx(d);
    d.move_cell(0, {4, 4});
    d.move_cell(77, {0, 0});  // throws
    tx.commit();
  } catch (const std::out_of_range&) {
  }
  CHECK(d.cell(0).loc == (Point{9, 9}) && !d.in_transaction());
}

/// Random edit sequences: rollback restores exactly; NetBoxCache tracks HPWL.
void test_random_edits() {
  std::mt19937 rng(7);
  Design d("rnd");
  d.set_tracking(true);
  d.set_die({0, 0, 10000, 10000});
  for (int i = 0; i < 200; ++i) d.add_cell("c" + std::to_string(i), 10, 10);
  for (int i = 0; i < 100; ++i) {
    const Id n = d.add_net("n" + std::to_string(i));
    for (int k = 0; k < 3; ++k) d.connect(static_cast<Id>(rng() % 200), n, "p" + std::to_string(k));
  }
  d.flush();
  NetBoxCache boxes(d);
  CHECK(boxes.total_hpwl() == static_cast<std::int64_t>(total_hpwl(d)));

  auto random_op = [&](int name_seq) {
    const Id c = static_cast<Id>(rng() % d.num_cells());
    const Id n = static_cast<Id>(rng() % d.num_nets());
    const Id p = static_cast<Id>(rng() % d.num_pins());
    switch (rng() % 10) {
      case 0: case 1: case 2: case 3:
        d.move_cell(c, {static_cast<Dbu>(rng() % 9990), static_cast<Dbu>(rng() % 9990)});
        break;
      case 4: d.resize_cell(c, 5 + rng() % 20, 10); break;
      case 5: d.set_pin_offset(p, {static_cast<Dbu>(rng() % 10), 0}); break;
      case 6:
        if (!d.net(n).removed && (d.pin(p).cell == kInvalidId || !d.cell(d.pin(p).cell).removed))
          d.reconnect_pin(p, n);
        break;
      case 7:
        if (!d.cell(c).removed && rng() % 4 == 0) d.remove_cell(c);
        break;
      case 8:
        if (!d.cell(c).removed && !d.net(n).removed) d.connect(c, n, "q");
        break;
      case 9:
        d.add_cell("new" + std::to_string(name_seq), 4, 4);
        break;
    }
  };

  for (int round = 0; round < 20; ++round) {
    const std::string before = dump(d);
    {
      Transaction tx(d);
      for (int k = 0; k < 50; ++k) random_op(round * 100 + k);
      CHECK(consistent(d));
      if (round % 2 == 0) tx.rollback();
      else tx.commit();
    }
    if (round % 2 == 0) CHECK(dump(d) == before);
    CHECK(consistent(d));
    d.flush();
    CHECK(boxes.total_hpwl() == static_cast<std::int64_t>(total_hpwl(d)));
  }
  // A local move recomputes only that cell's nets.
  d.flush();
  boxes.refresh();
  Id c = 0;
  while (d.cell(c).removed || d.cell_pins(c).empty()) ++c;
  d.move_cell(c, d.cell(c).loc + Point{3, 0});
  d.flush();
  CHECK(boxes.total_hpwl() == static_cast<std::int64_t>(total_hpwl(d)));
  CHECK(boxes.last_refresh_count() <= d.cell_pins(c).size());
}

void test_bin_grid() {
  BinGrid g({0, 0, 100, 100}, 4, 4);
  CHECK(g.bin_width() == 25 && g.bin_of({99, 0}) == (std::pair<int, int>{3, 0}));
  CHECK(g.bin_of({-5, 500}) == (std::pair<int, int>{0, 3}));
  DirtySet bins;
  g.mark_bins({10, 10, 60, 30}, bins);  // x bins 0..2, y bins 0..1
  CHECK(bins.size() == 6 && bins.test(static_cast<Id>(g.index(2, 1))) && !bins.test(static_cast<Id>(g.index(3, 1))));
  CHECK(throws<std::invalid_argument>([] { BinGrid({0, 0, 0, 10}, 1, 1); }));
}

void test_snapshot_sharing() {
  Design d("snap");
  d.set_tracking(true);
  d.set_die({0, 0, 100000, 100000});
  const int n = 3 * 4096 + 17;  // 4 cell chunks
  for (int i = 0; i < n; ++i) d.add_cell("c" + std::to_string(i), 10, 10);
  const Id net = d.add_net("n");
  d.connect(0, net, "A", {1, 1});
  d.connect(n - 1, net, "B", {2, 2});
  d.flush();

  SnapshotPublisher pub(d);
  auto s1 = pub.latest();
  CHECK(s1->num_cells() == static_cast<std::size_t>(n) && s1->cell_array().num_chunks() == 4);
  CHECK(s1->cell_name(5) == "c5" && s1->find_cell("c7") == 7 && s1->net_pins(net).size() == 2);

  // One move: one cell chunk copied, the rest shared; topology shared.
  d.move_cell(5000, {77, 77});
  d.flush();
  auto s2 = pub.latest();
  CHECK(s2->version() == d.version() && s2->version() == s1->version() + 1);
  CHECK(pub.last_copied_chunks() == 1);
  CHECK(s2->cell(5000).loc == (Point{77, 77}) && s1->cell(5000).loc == (Point{0, 0}));
  CHECK(s2->cell_array().shares_chunk(s1->cell_array(), 0) &&
        !s2->cell_array().shares_chunk(s1->cell_array(), 1) &&
        s2->cell_array().shares_chunk(s1->cell_array(), 3));

  // Moving a connected cell copies only its cell chunk: Net records hold no
  // geometry, so the net chunk is shared (the net's bbox still updates).
  d.move_cell(n - 1, {500, 600});
  d.flush();
  auto s3 = pub.latest();
  CHECK(pub.last_copied_chunks() == 1);
  CHECK(s3->net_bbox(net) == (Rect{1, 1, 502, 602}));
  CHECK(s2->net_bbox(net) == (Rect{1, 1, 2, 2}));  // old snapshot unchanged

  // Structural change: new topology, names visible.
  const Id extra = d.add_cell("extra", 1, 1);
  d.flush();
  auto s4 = pub.latest();
  CHECK(s4->num_cells() == static_cast<std::size_t>(n + 1) && s4->cell_name(extra) == "extra");
  CHECK(s3->num_cells() == static_cast<std::size_t>(n) && s3->find_cell("extra") == kInvalidId);

  // Manual publishing: nothing new until publish().
  SnapshotPublisher manual(d, /*auto_publish=*/false);
  const auto m1 = manual.latest();
  d.move_cell(1, {5, 5});
  d.flush();
  CHECK(manual.latest() == m1);
  d.move_cell(2, {6, 6});  // not flushed yet: publish() flushes first
  manual.publish();
  CHECK(manual.latest()->cell(1).loc == (Point{5, 5}) && manual.latest()->cell(2).loc == (Point{6, 6}));
  const auto m2 = manual.latest();
  manual.publish();  // nothing changed
  CHECK(manual.latest() == m2);
}

/// A reader thread sees only whole batches: within any snapshot every cell
/// has the same x (the writer moves all cells per batch and then flushes).
void test_snapshot_concurrent_reader() {
  Design d("mt");
  d.set_tracking(true);
  d.set_die({0, 0, 1 << 20, 1 << 20});
  const int n = 20000;
  for (int i = 0; i < n; ++i) d.add_cell("c" + std::to_string(i), 1, 1);
  d.flush();
  SnapshotPublisher pub(d);
  std::atomic<bool> done{false};
  std::atomic<int> reads{0}, torn{0};
  std::thread reader([&] {
    std::uint64_t last = 0;
    while (!done.load()) {
      auto s = pub.latest();
      if (s->version() < last) ++torn;  // versions never go back
      last = s->version();
      const Dbu x = s->cell(0).loc.x;
      for (Id i = 0; i < static_cast<Id>(s->num_cells()); i += 7) {
        if (s->cell(i).loc.x != x) {
          ++torn;
          break;
        }
      }
      ++reads;
    }
  });
  for (int batch = 1; batch <= 200; ++batch) {
    // Every other batch moves only part of the design, so publishes mix
    // shared and copied chunks.
    const int count = batch % 2 ? n : n / 3;
    for (Id i = 0; i < count; ++i) d.move_cell(i, {batch, 0});
    for (Id i = count; i < n; ++i) d.move_cell(i, {batch, 0});
    d.flush();
  }
  // Keep the reader going until it has seen a few snapshots.
  while (reads.load() < 20) std::this_thread::yield();
  done = true;
  reader.join();
  CHECK(torn.load() == 0);
  CHECK(pub.latest()->cell(n - 1).loc.x == 200);
}

void test_placer_state_stays_outside_design() {
  Design d("pst");
  d.set_die({0, 0, 1000, 1000});
  const Id pad = d.add_cell("pad", 10, 10);
  d.move_cell(pad, {0, 0});
  d.set_fixed(pad, true);
  std::vector<Id> cs;
  for (int i = 0; i < 20; ++i) cs.push_back(d.add_cell("u" + std::to_string(i), 10, 10));
  for (int i = 0; i < 20; ++i) {
    const Id n = d.add_net("n" + std::to_string(i));
    d.connect(i == 0 ? pad : cs[i - 1], n, "Z", {}, PinDir::Output);
    d.connect(cs[i], n, "A");
  }
  d.set_net_weight(3, 2.0);
  WireDelaySta sta(10.0, 1.0);  // everything critical
  PlacerOptions o;
  o.max_iterations = 30;
  o.timing_interval = 10;
  o.stop_overflow = 0.0;
  MixedSizePlacer placer(o);
  placer.run(d, &sta, nullptr);
  // Timing-driven weights live in the placer's state; the design keeps its
  // input weights.
  CHECK(d.net(3).weight == 2.0 && d.net(4).weight == 1.0);
  CHECK(placer.state().net_weight.size() == d.num_nets());
  CHECK(placer.state().net_weight[4] > 1.0);
  CHECK(placer.state().net_weight[3] > 2.0);  // input weight scaled, not overwritten
  CHECK(d.version() > 0 && !d.in_transaction());
}

/// Removed cells are ignored by engines.
void test_engines_skip_removed() {
  Design d("rm");
  d.set_die({0, 0, 1000, 1000});
  d.set_row_height(10);
  const Id a = d.add_cell("a", 10, 10);
  const Id b = d.add_cell("b", 10, 10);
  const Id gone = d.add_cell("gone", 500, 500, /*is_macro=*/true);
  const Id n = d.add_net("n");
  d.connect(a, n, "Z");
  d.connect(b, n, "A");
  d.connect(gone, n, "A");
  d.remove_cell(gone);
  CHECK(count_macros(d) == 0);
  FlowOptions o;
  o.placer.max_iterations = 20;
  o.router.grid_x = o.router.grid_y = 8;
  run_flow(d, o);
  CHECK(d.cell(gone).loc == (Point{0, 0}) && !d.cell(gone).fixed);
  CHECK(check_legal(d, 10));
}

}  // namespace

int main() {
  hermes::log::console_sink()->set_level(hermes::log::Level::Error);
  const std::vector<std::pair<std::string, std::function<void()>>> tests = {
      {"id_list_pool", test_id_list_pool},
      {"name_table_erase", test_name_table_erase},
      {"edit_ops", test_edit_ops},
      {"tracking_off_by_default", test_tracking_off_by_default},
      {"change_tracking", test_change_tracking},
      {"transaction_rollback_all_ops", test_transaction_rollback_all_ops},
      {"transaction_nesting_and_raii", test_transaction_nesting_and_raii},
      {"random_edits", test_random_edits},
      {"bin_grid", test_bin_grid},
      {"snapshot_sharing", test_snapshot_sharing},
      {"snapshot_concurrent_reader", test_snapshot_concurrent_reader},
      {"placer_state_stays_outside_design", test_placer_state_stays_outside_design},
      {"engines_skip_removed", test_engines_skip_removed},
  };
  for (const auto& [name, fn] : tests) {
    const int before = g_failures;
    fn();
    std::printf("%s %s\n", g_failures == before ? "PASS" : "FAIL", name.c_str());
  }
  std::printf("%d failure(s)\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
