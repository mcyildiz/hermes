# HERMES

**H**olistic **E**ngine for **R**outability-driven **M**ixed-size placement with **E**mbedded **S**TA

Hermes is a timing- and congestion-driven placer for mixed-size designs. Routing
feedback is built into placement, and a full routing stage runs afterward.

## Flow

```
            ┌──────────────────────── global placement loop ────────────────────────┐
 netlist →  │ wirelength + density step                                             │
            │   every N iters: Embedded STA ──► net criticality ──► net re-weighting │
            │   every M iters: GlobalRouter.estimate_congestion ──► cell inflation   │
            └───────────────────────────────────────────────────────────────────────┘
                 → macro legalization → std-cell legalization → detailed placement
                 → global routing → detailed routing → post-route STA
```

The `GlobalRouter` acts as the congestion oracle *inside* placement and also does
the real global route afterward. Both stages share one `CongestionMap`, so they
agree on what counts as congested.

## Layout

```
include/hermes/
  core/types.hpp            Point, Rect, DBU, orientations
  log/log.hpp               Hierarchical logging: channels, levels, message ids/caps, HLOG_* macros
  log/sinks.hpp             Log sinks: console/text file, JSON lines, memory, callback
  db/design.hpp             Core Design: flat cells/nets/pins, editing ops, change tracking, Transaction
  db/changes.hpp            ChangeSet (dirty cells/nets/pins), DesignListener
  db/geometry.hpp           Shared queries: pin_location, net_bbox, HPWL; NetBoxCache (incremental)
  db/bin_grid.hpp           BinGrid shared by congestion/density/region code
  db/snapshot.hpp           Immutable versioned snapshots for concurrent readers (GUI)
  db/id_list_pool.hpp       Flat pool of per-net/per-cell pin lists
  db/name_table.hpp         Compact id <-> name tables (names kept out of hot structs)
  db/floorplan.hpp          Rows and the Bookshelf .route grid (floorplan data in the Design)
  db/design_access.hpp      Bulk-load access for readers (not for algorithms)
  db/synthetic.hpp          Synthetic mixed-size designs (benchmarks, demos, GUI tests)
  io/common.hpp             Reader options (k=v), mmap input, parallel text splitting, ParseError
  io/registry.hpp           Reader/writer plugins, directory detection, hermes.inputs manifest
  io/lefdef.hpp             LEF/DEF reader (parallel) and DEF writer
  io/bookshelf.hpp          Bookshelf reader (.aux .nodes .nets .pl .scl .wts .route) and writers
  io/design_file.hpp        Native .hdb design file: versioned, chunk-compressed, parallel save/load
  par/parallel.hpp          Thread pool, deterministic parallel_for / parallel_reduce, ThreadLimit
  par/row_bands.hpp         Deterministic parallel scatter into grids (density, RUDY)
  par/job.hpp               Background jobs: RunControl (progress, cancel, publish cadence), JobHandle
  compute/backend.hpp       Compute backends (CPU now; GPU behind the same kernel interface)
  region/region.hpp         Region partitioning, RegionProblem/Solution, serialization, executors
  timing/sta.hpp            StaEngine interface + WireDelaySta placeholder
  place/congestion.hpp      CongestionMap, RUDY estimate, macro blockages
  place/placer.hpp          MixedSizePlacer (timing + routability feedback)
  route/global_router.hpp   GlobalRouter (congestion oracle + router)
  route/detailed_router.hpp DetailedRouter
  route/route_usage.hpp     Layer stack + 3-D routing usage (per-layer demand/capacity in tracks, vias)
  route/route_shapes.hpp    Wire shapes of the router's current routes (EstimatedRouteModel, for viewers)
  qor/qor.hpp               End-of-run QoR report: density, evenness, 3-D congestion, 3-D wirelength, maps
  track/run_tracker.hpp     Run tracking: runs, progress, stages, metrics, results -> SQLite / run server
  core/json.hpp             Minimal JSON writer
  flow/flow.hpp             run_flow(): place → route → STA
src/                        Implementations
python/bindings.cpp         pybind11 module `hermes._hermes`
python/hermes/              Python package
apps/hermes_main.cpp        `hermes` CLI
tests/                      C++ (ctest) and Python (pytest) tests
bench/log_bench.cpp         Logging overhead micro-benchmark (`hermes_log_bench`)
bench/db_bench.cpp          Design database benchmark, 1M cells (`hermes_db_bench`)
bench/par_bench.cpp         Speedup vs threads of every parallel kernel (`hermes_par_bench`)
bench/io_bench.cpp          Read/write/save/load timings on a 1M-cell design (`hermes_io_bench`)
bench/qor_bench.cpp         QoR report and run-tracking cost on a 1M-cell design (`hermes_qor_bench`)
bench/gui_bench.cpp         GUI scene build / detail / pick timings on a 1M-cell design (`hermes_gui_bench`)
tools/dashboard/            Run dashboard (Python standard library only): live/past runs, compare
gui/scene/                  GUI model, no graphics: scene + LOD tiles, detail, picking, session (jobs), commands
gui/app/                    hermes_gui: GLFW + Dear ImGui app, OpenGL 3.3 renderer, panels
assist/                     Optional AI assistant: tools, MCP server, model providers, chat loop, config
apps/assist_cli.cpp         `hermes shell`, `hermes mcp`, `hermes ai ...`
third_party/                Dear ImGui 1.91.9b (+ Roboto/Cousine fonts), stb_image_write (vendored)
```

## Build

Requirements: CMake ≥ 3.20, a C++20 compiler, Python ≥ 3.9. pybind11 is used
if it's installed, and downloaded automatically otherwise. SQLite (built into
macOS; `libsqlite3-dev` on Linux) gives the local run database; without it
runs can still be pushed to a dashboard server. libcurl (built into macOS;
`libcurl4-openssl-dev` on Linux) lets the optional chat assistant reach model
APIs; without it the MCP server and the offline mock assistant still work.

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/hermes demo
```

### Python

```bash
# In-tree development (the extension is built into python/hermes/)
PYTHONPATH=python python -m pytest

# Or install as a package
pip install .
```

```python
import hermes

d = hermes.Design("top")
d.die = hermes.Rect(0, 0, 100000, 100000)
ram = d.add_cell("ram0", 20000, 20000, is_macro=True)
u0  = d.add_cell("u0", 2000, 1000)
n   = d.add_net("n0")
d.connect(u0, n, "Z", dir=hermes.PinDir.Output)
d.connect(ram, n, "D")
d.move_cell(u0, hermes.Point(1000, 1000))   # edits go through the design

with d.transaction() as tx:                 # undo scope
    d.move_cell(u0, hermes.Point(5000, 5000))
    if d.total_hpwl() > 1e6:
        tx.rollback()

opts = hermes.FlowOptions()
opts.placer.timing_interval = 25
opts.placer.routability_interval = 25
report = hermes.run_flow(d, opts)
print(report.placement.hpwl, report.global_route.total_overflow)
```

## Design database

The `Design` is small on purpose: flat arrays of plain `Cell`/`Net`/`Pin`
structs (ids are indices), name tables, and only operations that keep them
consistent (`add_cell`, `connect`, `reconnect_pin`, `remove_cell`,
`move_cell`, `resize_cell`, `set_fixed`, ...). Engines read it through const
accessors and change it only through those operations. Algorithm state
(timing-driven net weights, inflation, routes, ...) lives in the algorithm's
own module, in arrays indexed by the same ids (`PlacerState`), never in the
Design. Shared queries (`pin_location`, `net_bbox`, `total_hpwl`, `BinGrid`)
are free functions in `db/geometry.hpp` / `db/bin_grid.hpp`.

```cpp
design.set_tracking(true);             // opt in (default off: flushes then report `all`)
design.move_cell(c, {x, y});           // records c as dirty (a bit + a list entry)
design.flush();                        // ends a batch: nets of dirty cells are derived,
                                       // listeners get the ChangeSet, version()++
struct Congestion : DesignListener {   // e.g. a congestion map or STA
  void on_flush(const Design& d, const ChangeSet& ch) override {
    if (ch.all) rebuild(d); else for (Id n : ch.nets.ids()) update(d, n);
  }
};

{
  Transaction tx(design);              // undo scope (nestable)
  try_local_improvement(design);
  if (worse) tx.rollback(); else tx.commit();   // leaving the scope uncommitted rolls back
}

SnapshotPublisher pub(design);         // GUI thread: auto s = pub.latest();
                                       // immutable, consistent at one version
```

* Removal leaves a tombstone (`Cell::removed`), so ids never shift and
  per-algorithm arrays stay aligned.
* Snapshots share unchanged 4096-object chunks between versions, so a publish
  after a local edit copies only the touched chunks.
* `./build/hermes_db_bench` measures all of this on a 1M-cell design.

## Parallel runtime

Everything heavy runs on one shared thread pool (`hermes/par/parallel.hpp`):

```cpp
hermes::par::set_num_threads(8);           // 0 = all cores; env HERMES_THREADS=8
par::parallel_for(0, n, [&](std::size_t lo, std::size_t hi) { ... });
double s = par::parallel_sum(0, n, [&](std::size_t lo, std::size_t hi) { ...; return part; });
{ par::ThreadLimit lim(2); run_something(); }   // cap the loops this thread starts
```

* **Deterministic.** Ranges are cut into blocks that depend on the range and
  grain only, and reductions combine blocks in order. Grid scatters (density
  charge, RUDY demand) use `par::RowBands`: each task owns a band of rows and
  adds items in item order. The placer therefore gives bit-identical
  results with 1, 4 or 64 threads (tested).
* **Nested and concurrent loops** work: any thread (workers, background jobs,
  the GUI) can start loops, and the caller always works on its own loop, so
  there is no deadlock. No external dependency (oneTBB can be swapped in
  behind the same functions later).
* **Background jobs** (`hermes/par/job.hpp`): an engine runs on its own thread
  with a `RunControl` (progress, cancel, publish cadence). The placer and
  `run_flow` take an optional `RunControl*`; they stop at the next iteration
  when cancelled (design flushed and consistent) and write back + flush at
  the job's publish interval, so `SnapshotPublisher` readers follow progress.

```cpp
SnapshotPublisher pub(design);                       // GUI: auto s = pub.latest();
FlowReport rep;
JobHandle job = start_flow_job(design, opts, &rep);  // returns at once
job.progress();  job.cancel();  job.wait();          // from any thread
```

```python
job = hermes.run_flow_async(d, opts, publish_interval_s=0.25)
while not job.wait(timeout=0.1):
    print(job.progress())        # <JobProgress placement 42%: global placement: iteration 310 ...>
report = job.result()
```

* **Compute backends** (`hermes/compute/backend.hpp`): the placer's per-
  iteration work (WA wirelength gradient, HPWL, density deposit, Poisson
  solve, density gradient, Nesterov vector updates) are kernels on flat
  arrays (CSR netlist, SoA footprints). `cpu` runs them on the pool; a CUDA
  or Metal backend implements the same interface (`HERMES_BACKEND=name`).
* **Regions** (`hermes/region/region.hpp`): `Partition` tiles the die;
  `extract()` cuts a self-contained `RegionProblem` (region cells, their
  nets, outside pins frozen as terminals); a solver returns a
  `RegionSolution`; `apply()` refuses any move outside the region. Problems
  serialize to bytes, so `RemoteExecutor` can send them to other machines
  over a `Transport` (today: `LoopbackTransport`, the full wire path in
  process). `run_regions()` extracts, solves in parallel and applies; run
  colors one at a time (`regions_of_color`) for Gauss-Seidel style passes.
* **Region-ordered snapshots:** `pub.set_cell_order(part.spatial_order())`
  stores cells by region, so a region-local edit copies one or two chunks
  instead of chunks across the whole id range.

### What can run on a GPU

| Algorithm | GPU fit | Why |
|---|---|---|
| WA / LSE wirelength + gradient | Excellent | One thread per net (or pin), pin gradients gathered per cell: no atomics. DREAMPlace gets 30-40x here. |
| Density deposit (charge map) | Excellent | One thread per cell; row-band or sorted segmented reduction keeps it deterministic. |
| Poisson solve (DCT/DST) | Excellent | Batched 1-D FFTs (cuFFT / MPSGraph / vDSP). Grid sizes are powers of two already. |
| Density gradient, Nesterov updates, HPWL | Excellent | Embarrassingly parallel per cell / per net; memory-bound vector ops. |
| RUDY / pin-density congestion maps | Excellent | Per-net rectangles into a grid (same scatter as density). |
| Global routing (pattern routes, maze with negotiated costs) | Good | Batches of non-overlapping nets in parallel (GAMER, FastGR-style); rip-up loop stays on CPU. |
| STA (graph propagation) | Good | Level-by-level arrival/required propagation over large designs (GPU OpenTimer/INSTA style); small designs are latency-bound, keep CPU. |
| Detailed placement (ISM, global swap) | Partial | Independent sets of cells are matched in parallel (ABCDPlace); sequential parts stay on CPU. |
| Legalization (Abacus, macro LG) | Poor | Sequential clustering per row; parallelize by regions/rows on the CPU instead. |
| Detailed routing, DRC | Poor | Irregular, branchy, memory-heavy; CPU threads per region. |

On Mehmet's Mac mini (Apple Silicon) the GPU path is Metal: CPU and GPU share
memory, so a Metal backend can wrap the placer's host arrays without copies.
CUDA needs device copies (positions per iteration, the netlist once).

Test races with `cmake -DHERMES_SANITIZE=thread` (GCC: libtsan) and run
`hermes_par_tests`. `./build/hermes_par_bench` prints the speedup table.

## Input and output

Readers only fill the Design; algorithms only see the Design. Every reader
and writer is a plugin in `io::Registry` (`hermes formats` lists them), so a
new format, or a newer Bookshelf variant, is one class added to the registry.

### Running on a directory

    hermes path/to/design_dir                 # detect inputs, place, route, save
    hermes detect path/to/design_dir          # only show what would be read
    hermes run --lef tech.lef --lef cells.lef --def top.def    # explicit LEF/DEF

What gets read:

1. If the directory has a `hermes.inputs` manifest, it decides, and wins over detection:

       # role  path (relative to this file; globs allowed)
       lef     tech.lef
       lef     lib/*.lef
       def     top.def
       option  def.allow_unknown_macros=1

   Roles: `lef`, `def` (LEF/DEF); `aux` or `nodes nets pl scl wts route`
   (Bookshelf); `hdb`. `format NAME` names the reader when roles do not.
2. Otherwise hermes detects: LEF+DEF (technology LEFs, those with `LAYER`
   before `MACRO`, are read first), Bookshelf (`.aux`, or the loose files), or a
   saved `.hdb`.
3. If that is ambiguous (two DEFs, two `.aux`, LEF/DEF next to Bookshelf) or
   nothing fits, hermes stops and lists the candidates and the files it saw,
   with a hint to add a manifest.

A single `.def`, `.aux` or `.hdb` file (or a manifest path) works as input too.

Output goes to `--out` (default `hermes_out/<design>`): `<design>.hdb` at the
end of the run, plus `--write FILE` for `.def`, `.pl`, `.aux` or `.hdb`, and
`--checkpoint` for `<design>.placed.hdb` after placement.
`hermes convert <input> out.hdb out.def out.pl` converts without running.

Kept from the input: DEF `PLACED`/`FIXED` locations and orientations are
the initial placement (`place.keep_placed=1` is the default; unplaced cells
start from the usual initial placement), rows with their sites, LEF master
names (written back into DEF), net weights (DEF `+ WEIGHT`, Bookshelf `.wts`),
DBU per micron, and the Bookshelf `.route` grid.

Options (`--opt K=V`, or `option` lines in a manifest; the command line wins):

| Option | Meaning |
|---|---|
| `bookshelf.scale=N` | Bookshelf units to DBU (default 1); the writers divide by it again |
| `bookshelf.pl_placed=0/1` | Treat .pl locations of movable cells as placed (auto: a .pl written by hermes says so) |
| `def.allow_unknown_macros=1` | Warn instead of failing on a COMPONENT without a LEF macro |
| `place.keep_placed=0` | Ignore input locations and place from scratch |

Malformed input throws `ParseError` with `file:line: message`.

### The .hdb design file

The native save/restore format (`hermes info x.hdb` shows its sections):

- A 64-byte header with magic, major/minor version, then a section table.
  Each section is one column of the DB (cell x, cell width, pin offsets,
  name chars, rows, ...) with its own version, element size, codec and checksum.
- Columns are split into ~1 MiB chunks, byte-shuffled (plus delta coding for
  sorted columns) and LZ4-compressed; chunks that do not shrink stay raw.
  Chunks save and load in parallel and the file is memory-mapped on load.
- A newer minor version adds sections; older readers skip unknown ones.
  A different major version is refused with a clear message.
- `x.<tag>` sections carry engine state (`SaveOptions::extra`), so a flow can
  checkpoint mid-run and continue later. `io::start_save_job` saves a copy
  in the background while the engine keeps going.
- Saves are atomic (written to `.tmp`, then renamed).

Bookshelf and DEF are the interchange formats; `.hdb` is the fast,
lossless one (names, masters, removed objects, weights, rows and route grid
included).

### Speed

`hermes_io_bench` on a synthetic design with 1M cells, 1M nets and 4M pins,
on a 4-core cloud machine (times in seconds):

| | 1 thread | 2 threads | 4 threads | File size |
|---|---|---|---|---|
| Read LEF + DEF | 1.69 | 0.94 | 0.70 | 117 MB |
| Read Bookshelf | 1.44 | 1.01 | 0.71 | 147 MB |
| Load .hdb (compressed) | 0.76 | 0.42 | 0.23 | 51 MB |
| Save .hdb (compressed) | 0.41 | 0.31 | 0.19 | 51 MB |

Uncompressed `.hdb` (232 MB) loads in 0.22 s. Writing DEF takes 0.27 s,
the full Bookshelf set 0.52 s and a `.pl` alone 0.08 s. Use
`--log io=debug` to see the time of each reader phase.

From Python:

```python
from hermes import io
d, info = io.load_input("path/to/design_dir")       # or a manifest / .def / .aux / .hdb
io.save_design(d, "run.hdb", note="after GP", extra={"state": b"..."})
d2, extra = io.load_design("run.hdb")
io.write_design(d, "out.def")                       # or .pl, .aux, .hdb
```

## QoR report

Every run ends with a quality report, printed and written to
`<out>/<design>.qor.txt` and `.qor.json` (`--no-qor` skips it;
`FlowReport::qor`, `hermes.compute_qor(design)` in Python):

* **Placement density**: bins of 10 row heights. Utilization = movable cell
  area / free area (free = bin minus fixed cells). Average, peak bin, and the
  ISPD-style density overflow `sum max(0, movable - target * free) / movable area`.
* **Evenness**: coefficient of variation (std / mean) of bin utilization over
  bins with at least 20% free area, and the share of those bins above the
  target density and above 100%.
* **Congestion (3-D)**: one model for everything, the GCell *track* model of
  `route/route_usage.hpp`. Per metal layer and GCell, capacity is the number
  of tracks crossing it (extent / pitch, reduced under blockages) and demand
  is the wire length on that layer inside it divided by the GCell length.
  Per layer: capacity, demand, overflow, average and max utilization,
  overflowed GCells; totals per direction, ACE(0.5/1/2/5%) and RC (DAC 2012
  style, on GCells), the 10 worst GCells, and the RUDY estimate the placer
  uses on the same GCells and capacities, so the two can be compared.
* **Wirelength (3-D)**: HPWL (and weighted), routed length per layer, vias
  per cut layer.
* **Maps** for heatmaps (GUI, dashboard): `density.util`,
  `density.overflow`, `congestion.<layer>.util`, `congestion.h.util`,
  `congestion.v.util`, `congestion.overflow`, `rudy.util`, `vias.total`.
  In Python `numpy.asarray(report.map("congestion.h.util"))` is a (ny, nx) view.

What is real and what is estimated (the report says so in its notes):

| | Source |
|---|---|
| Density, evenness, HPWL | Exact |
| Total routed length, 2-D (per direction) congestion | The global router's real routes (star + L-shapes; the router is still a stub) |
| Per-layer length and congestion, vias | Estimated: a greedy length-based layer assignment of those routes (short runs low, long runs high), until the router assigns layers |
| Layer stack | Technology LEF (`LAYER ... TYPE ROUTING`, now stored in the Design and the `.hdb`), else a Bookshelf `.route` file, else an assumed 6-layer stack (noted in the report) |

Known disagreement: the log lines "placement ... routing overflow" (RUDY) and
"global route ... overflow" use the old 2-D map and are not comparable (RUDY
spreads HPWL over the net box; the router counts whole GCells crossed and
nothing inside a GCell). The report states this and uses the 3-D track model
for both; the placer and router move to that model with the real router.

Cost (`hermes_qor_bench`, 1M cells / 4M pins, 4-core cloud machine, 4 threads):

| GCell grid | 3-D route usage | Report (density, congestion, RUDY, wirelength, maps) |
|---|---|---|
| 64 x 64 | 0.25 s | 0.06 s |
| 256 x 256 | 0.14 s | 0.07 s |
| 1024 x 1024 | 0.23 s | 0.26 s |

With long (random-placement) nets: 0.3-0.45 s for the usage and 0.08-0.4 s
for the report. Everything is parallel and deterministic (integer
accumulation; same report for any thread count, tested).

## Run tracking and dashboard

Every `hermes` run (and `hermes demo`) is registered in a local SQLite
database, `~/.hermes/runs.db` (`--track-db PATH`, env `HERMES_RUNS_DB`,
`--no-track` to skip): settings, input, command line, source version (`git
describe`, or a hash of the sources), host, OS, CPU, cores, memory, threads,
live progress and stage, stage timings, metrics (global placement HPWL and
overflow per iteration), log messages (info and up), the QoR report and its
maps, peak memory, exit status and error. A run that dies without finishing
shows as "lost" once its heartbeat stops. Ctrl-C cancels at the next safe
point and records the run as cancelled.

```bash
hermes design_dir --run-name "td 0.65" --tag sweep --tag adaptec2
python3 tools/dashboard/hermes_dashboard.py           # http://localhost:8765
```

The dashboard lists live and past runs (filter by design, name, host, tag),
shows a run's progress, stage timeline, metric charts, QoR tables,
heatmaps, log and settings, and compares runs (results with deltas,
stage times, settings that differ, overlaid charts). It needs only Python's
standard library.

Watching remotely:

* **One machine** (e.g. the Mac mini): start the dashboard there with
  `--host 0.0.0.0 --token SECRET` and open `http://mac-mini:8765/?token=SECRET`
  from another device on the network, or over a VPN such as Tailscale, or
  through an SSH tunnel (`ssh -L 8765:localhost:8765 mac-mini`, then
  `http://localhost:8765`).
* **Several machines**: run one dashboard as the central server and start
  engines with `--track-server http://server:8765 --track-token SECRET` (env
  `HERMES_TRACK_SERVER`, `HERMES_TRACK_TOKEN`). Each engine keeps its local
  database and also pushes the run live; an unreachable server only costs a
  warning and retries.

Speed: the engine never waits for I/O. Tracking calls append to an
in-memory queue (50-130 ns per call) and a writer thread commits to SQLite
every 0.5 s and pushes to the server. With no active run, `track::metric()`
costs one load and branch (2 ns). If the queue overflows (more than 200k
events between flushes), events are dropped and counted, never blocked on.

```cpp
track::metric("route.ripup.overflow", iteration, overflow);   // from any engine
```

```python
with hermes.track.Run(name="sweep", design=d.name, tags=["py"], settings_json=opts.to_json()) as run:
    report = hermes.run_flow(d, opts, tracker=run)    # progress, stages, metrics, QoR
    run.metric("my.metric", 1, 0.5)
print(report.qor)
```

## GUI (hermes_gui)

A viewer and front end that opens in well under a second and stays usable
while engines run. C++ with Dear ImGui on OpenGL 3.3 (GPU instancing). The
engine builds without it (`-DHERMES_BUILD_GUI=OFF`).

```bash
# macOS: GLFW from Homebrew (else CMake downloads GLFW 3.4)
brew install glfw
cmake -S . -B build && cmake --build build -j --target hermes_gui

./build/hermes_gui                                  # empty window: open or generate a design
./build/hermes_gui path/to/design_dir               # anything `hermes <path>` reads (.hdb is fastest)
./build/hermes_gui --synth 1000000                  # synthetic 1M-cell design
./build/hermes_gui d.hdb -c "run; heatmap density"  # console commands after start
./build/hermes_gui --bench                          # frame-time benchmark on 1M cells (writes hermes_gui_bench.json)
./build/hermes_gui d.hdb --screenshot view.png      # render once, save a PNG, quit (add --hidden for no window)
```

Mouse: drag pans, wheel zooms at the cursor, click selects, double-click zooms
to the selection, Shift-drag zooms to a box, Ctrl/Cmd-drag moves the selected
cell (Ctrl/Cmd-Z undoes). Keys: F fit, Z zoom to selection, Esc clear,
`` ` `` console, L layers panel, I inspector, A assistant, +/- zoom, arrows pan. Dropping a
directory or file on the window opens it.

Panels float over the full-window canvas: a top bar (open, run flow, place,
QoR, cancel, progress), **Layers** (show / pick per layer, colors, counts,
heatmap overlay with opacity, range and legend), **Inspector** (find by name;
cell, net and pin details with clickable pins and nets; last run and QoR
summary with map buttons), an optional **Console**, an optional
**Assistant** chat (see [AI assistant](#ai-assistant-optional)), and a status bar
(coordinates in um, zoom, level of detail, timings).

### How it stays fast

* **The window comes first.** It opens before anything is read (first frame
  in ~70 ms); loading runs as a background job with a progress bar.
* **Level of detail.** The scene worker turns each design snapshot into
  (1) a std-cell density image of the whole die, drawn while cells are under
  ~2.5 px (a 1M-cell chip shows up as soon as it is loaded), (2) one
  20-byte instance per std cell, sorted into tiles so a frame draws only the
  tile rows in view with one instanced draw each, plus macros and I/O pins,
  and (3) on zoom-in, a **detail** window of pins, wires and vias around the
  view, built in the background and replaced as you pan.
* **Frames don't depend on design size.** Geometry goes to the GPU once per
  scene; a frame sets the view and issues a few dozen instanced draws. When
  nothing changes the app sleeps (no busy loop).

### Non-blocking

The GUI never reads the Design while an engine runs. `gui::Session` owns the
Design and runs every engine (load, flow, placement, QoR, save) as a
`JobHandle` job on all but one hardware thread. The UI thread reads only the
latest `DesignSnapshot`; the scene worker rebuilds the scene whenever a newer
snapshot is published (the placer publishes every 0.2 s), so placement plays
out live. While a job runs: view, zoom, pan, layers, select, inspect,
heatmaps and read-only console commands all work; edits (move, undo, open,
run) are refused with a message. Cancel stops the job at its next safe point.

### Wires

The global router does not store routes yet. `route/route_shapes.hpp`'s
`EstimatedRouteModel` reproduces its routes (star from the first pin, L per
sink, greedy length-tier layer assignment) and a test checks that the drawn
wires add up exactly to the per-layer wirelength and via counts of the QoR
report. The Layers panel says the wires are estimated. When the router keeps
real routes, the detail builder reads those instead.

### Heatmaps

*Cell density (live)* and *Pin density (live)* are computed from each
snapshot, so they follow a running engine. The QoR maps of step 5
(density.util, congestion per layer and direction, overflow, RUDY, vias)
appear after **QoR** or a flow; the panel warns when the design has changed
since that report.

### Console and scripting

One command registry serves the console, `-c`/`--script`, `hermes shell`,
Python (`hermes.Console`) and the AI assistant's tools. `help` lists commands: `open synth run place cancel wait
settle qor report save select find clear zoom zoomfrac layer layers heatmap
heatmaps move undo info threads screenshot panel frames sleep bench_frames
bench_report echo quit`. Commands that change the design are marked and
refused while a job runs. There is no embedded Python interpreter yet; the
Python API (`import hermes`) remains the scripting route for engines.

### Rendering backends

`gui/app/renderer.hpp` is the backend interface (instanced rectangles,
segments, markers, colormapped images). OpenGL 3.3 core is the only
implementation (deprecated on macOS but supported, 4.1 core). A Metal backend
implements the same interface; Dear ImGui already has a Metal backend.

### Measured (1M cells, 4-core Linux cloud box, software OpenGL)

The cloud box has no GPU (Mesa llvmpipe renders on the same 4 CPU cores
the engine uses), so GPU-side numbers here are worst cases. Run
`hermes_gui --bench` on a real GPU for the true frame times.

| | |
|---|---|
| First frame (window + UI, before loading) | 65-73 ms |
| 1M-cell .hdb to full-chip view | 0.61 s (load + snapshot 0.47 s, scene 44 ms) |
| Scene build, 1M cells (`hermes_gui_bench`) | 27 ms at 4 threads, 70 ms at 1 |
| Detail (pins + wires) for 1% / 5% / 20% of the die | 18 / 34 / 106 ms, background |
| Pick (click) with wires selectable | 0.3 ms |
| Live update after a placer publish (all cells moved) | 31 ms (publish 3.5 + scene 27), background |
| Full-chip view, 1M cells: frame CPU p50 / p95 wall | 0.43 ms / 7 ms |
| Close-up with wires and pins: frame CPU p50 | 7.7 ms (llvmpipe) |
| During 1M-cell placement (3 threads busy): frame wall p95 | 18 ms |

## AI assistant (optional)

An engineer can ask an AI model about the open design, have it run analyses
and reports, and (only if allowed) have it run commands. It is **off by
default**, **model-agnostic**, and **read-only by default**. There are two
ways in, and both use the same hermes tool layer (`assist/tools.hpp`), which
acts through the same session and console commands as the GUI, so a model
can do nothing an engineer at the console could not:

1. **MCP server** (`hermes mcp`). Any MCP client drives hermes: Claude Code,
   Claude desktop, or any other agent or model that speaks the Model Context
   Protocol. The model, its account and its permission prompts live in the
   client.
2. **Built-in chat** in `hermes_gui` (Assistant panel, key A) and in
   `hermes shell` (`ai <question>` or `? <question>`). hermes calls the model
   itself through a provider interface: Anthropic (Claude), any
   OpenAI-compatible endpoint (OpenAI, gateways like OpenRouter/vLLM, and local
   models on Ollama, llama.cpp `llama-server`, LM Studio), or a mock provider
   (offline demo and tests). Adding a provider is one class
   (`assist/provider.hpp`).

### Tools

| Read (always available) | Change (hidden unless edits are allowed) |
|---|---|
| `design_summary` counts, die, layers, job, last flow, QoR headline | `load_design` path + reader options (waits) |
| `job_status`, `wait_for_job` | `run_flow` place (+route) with flow options, optional wait |
| `compute_qor` (background job, does not change the design), `qor_report` | `cancel_job` |
| `congestion_report` per-layer demand/capacity/overflow, hot spots in um, ACE/RC, RUDY | `move_cell`, `undo` |
| `timing_report` WNS/TNS of the last flow, longest nets with estimated delay | `save_design` (.hdb) |
| `query_cell`, `query_net`, `find_objects` (glob), `cells_in_region` | `run_command` lines that edit or write files |
| `read_log` (level / text filters), `run_history` (run DB, one run's details) | |
| `list_commands`, `run_command` (view/report commands), `get_selection`, `show_in_view` | |

Tool results are JSON or report text, in microns. Long waits (loads, flows,
QoR) run as session jobs; tools return as soon as they finish or time out,
and the engine never blocks on the model.

### MCP: Claude Code, Claude desktop, other clients

```bash
# Read-only (default): the model inspects and analyzes, cannot change anything.
claude mcp add hermes -- /path/to/build/hermes mcp /path/to/design_dir
# Allow changes (load, run_flow, move, save, ...). Claude Code asks you before each such call
# unless you pre-approve the tool; tools are annotated readOnlyHint / destructiveHint.
claude mcp add hermes-rw -- /path/to/build/hermes mcp /path/to/design_dir --allow-edits

./build/hermes ai mcp-config design_dir      # prints both snippets with this binary's absolute path
```

Claude desktop (`~/Library/Application Support/Claude/claude_desktop_config.json`
on macOS), or any MCP client that launches stdio servers:

```json
{ "mcpServers": { "hermes": { "command": "/path/to/build/hermes",
                              "args": ["mcp", "/path/to/design_dir"] } } }
```

The server speaks MCP over stdio (protocol 2025-06-18; also accepts
2025-03-26 and 2024-11-05). stdout carries protocol messages only; hermes logs
go to stderr. Options: `--opt K=V` (reader options), `--threads N`,
`--runs-db PATH`. Without a design argument, the model can load one with
`load_design` (needs `--allow-edits`). What the client's model sees is
whatever the tools return; nothing else leaves the machine.

### Built-in chat

```bash
./build/hermes ai init        # writes ~/.hermes/assistant.conf (commented template, enabled = false)
# edit it: enabled = true, provider/model; then put the key in the environment:
export ANTHROPIC_API_KEY=...  # or OPENAI_API_KEY, or HERMES_AI_API_KEY; never stored in the config
./build/hermes ai status      # provider, model, policy, ready or why not
./build/hermes ai privacy     # exactly what each message sends, and where
./build/hermes_gui design_dir # Assistant button / key A
./build/hermes shell design_dir
hermes> ? which layers overflow and where are the hot spots
```

`~/.hermes/assistant.conf` (or `$HERMES_AI_CONFIG`), `key = value`:

| Key | Default | |
|---|---|---|
| `enabled` | `false` | Nothing is contacted unless true (and a key or a local endpoint is set) |
| `provider` | `anthropic` | `anthropic`, `openai` (any OpenAI-compatible endpoint), `ollama`, `llamacpp`, `mock` |
| `model` | `claude-opus-5-5` / `gpt-5` | Any model the endpoint serves (e.g. `qwen3:32b` on Ollama) |
| `base_url` | provider's public API | `ollama` → `http://localhost:11434/v1`, `llamacpp` → `http://localhost:8080/v1` |
| `api_key_env`, `api_key_file` | `ANTHROPIC_API_KEY` / `OPENAI_API_KEY` | Where the key comes from; `HERMES_AI_API_KEY` wins. Local endpoints need none |
| `edits` | `read-only` | `read-only`: change tools are not offered and are refused. `ask`: every change shows Allow / Deny (GUI) or y/N (shell). `allow`: no confirmation |
| `send_design_summary`, `send_qor`, `send_selection` | `true` | Context attached to each message |
| `send_log_lines` | `0` | Recent log lines attached to each message |
| `effort`, `fallbacks` | model default, `auto` | Anthropic: `output_config.effort`; server-side refusal fallbacks on the models that support them |
| `max_tool_calls`, `max_tokens`, `timeout_s` | 25, 8192, 180 | Per question / answer / request |

Environment overrides: `HERMES_AI_ENABLED`, `HERMES_AI_PROVIDER`,
`HERMES_AI_MODEL`, `HERMES_AI_BASE_URL`, `HERMES_AI_EDITS`,
`HERMES_AI_EFFORT`. In a session: `ai on|off`, `ai set <key> <value>`,
`ai status`, `ai privacy`, `ai tools`, `ai stop`, `ai reset`, `ai yes|no`.
Quick look without any model: `ai set provider mock` then `ai on` (the mock
picks one read tool by keyword and shows its output).

**What is sent.** Nothing, until the assistant is enabled and you send a
message. Then each request carries your text and the conversation so far, the
tool list (names and descriptions, no design data), the context you allowed
(design name, input path, counts, die size, layer names, job state, last flow
and QoR headline numbers; the selected object's name; log lines if enabled),
and the output of the tools the model calls (reports and the names and
coordinates of the cells and nets it asks about). Never the whole design and
never files. `ai privacy` (and the panel's "What is sent") prints this for the
current settings. With a local endpoint (Ollama, llama.cpp) nothing leaves the
machine.

**Non-blocking.** The model request runs on the assistant's thread. Its tool
calls are handed to the GUI thread and run there once per frame (the Design
has one owner), so the GUI keeps drawing while the model thinks or a flow runs.
Stop interrupts the request at once; a hermes job the model started keeps
running until you cancel it. Measured in the tests: with a 300 ms model reply
the owner loop kept turning (59 frames) and the longest `pump()` took ~1 µs.

**Anthropic notes.** Messages API with `anthropic-version: 2026-10-21`,
automatic prompt caching (`cache_control`), assistant turns replayed unchanged
(thinking blocks included), `stop_reason: refusal` shown as a decline, and
`fallbacks: "default"` (beta `server-side-fallback-2026-07-01`) on
`claude-opus-5-5`, `claude-opus-5`, `claude-sonnet-5-5`, `claude-fable-5-1/5-2`
(`fallbacks = off` turns it off).

### Python

```python
c = hermes.Console("design_dir")              # headless session; edits="read-only" to forbid changes
print(c.run("qor"))                           # any console command; waits for the job it starts
print(c.call_tool("congestion_report", hot_spots=5))
c.mcp('{"jsonrpc":"2.0","id":1,"method":"tools/list"}')   # embed the MCP server in a Python host
```

## Logging

Every message goes to a **channel**: a dot-separated name such as `place.gp`,
`route.global` or `io.def`. A channel without its own level follows its
parent, so `route=debug` turns on `route.tree` too unless `route.tree` has its
own level. The root (`*`) defaults to `info`.
Levels: `off error warn info verbose debug trace`.

```cpp
#include "hermes/log/log.hpp"
namespace { HERMES_LOG_CHANNEL(log_tree, "route.tree"); }

HLOG_DEBUG(log_tree, "net {} -> {} segments", net, segs.size());   // std::format syntax
HLOG_WARN_ID(log_tree, "GRT-0001", "net {} is unroutable", net);    // id: can be switched off/capped
if (HLOG_ENABLED(log_tree, Debug)) { /* work done only for the log */ }
```

* **Off costs nothing measurable.** A disabled call is one load, one compare and
  one predicted branch; arguments are not evaluated. `trace` is compiled out of
  Release builds (set `-DHERMES_LOG_MAX_LEVEL=0..6` to change that). Run
  `./build/hermes_log_bench` to measure. One caveat: any call in a loop the compiler
  would otherwise vectorize stops vectorization, so keep logs out of the
  innermost vectorizable loops (or branch on `HLOG_ENABLED` outside the loop).
* **Thread-safe.** No locks on the disabled path. Enabled messages are formatted
  per thread and written as whole lines, so lines never interleave. Levels and
  caps can change at runtime while other threads log.
* **Message ids** (`DEF-0003`, `PLC-0001`, ...) can be switched off or limited to N
  prints; the last allowed print says that further ones are suppressed.
* **Sinks:** console (stdout), text file, JSON lines (one object per message, for
  run tracking), memory, callback. Add your own by subclassing `log::Sink`.

Control it from the CLI, the environment, C++ or Python:

```bash
./build/hermes --log "warn,place.gp=verbose,route=debug" --log-msg DEF-0002=5 \
               --log-file run.log --log-json run.jsonl demo
HERMES_LOG="info,io.def=off,msg:*=20" ./build/hermes demo   # msg:*=N caps every id
./build/hermes --log-channels                                # list channels
```

```python
import hermes.log as hl
hl.configure("warn,place.gp=debug")
hl.set_message_cap("DEF-0002", 5)
with hl.capture(console=False) as recs:   # list of Record(level, channel, id, text, ...)
    hermes.run_flow(d)
hl.to_python_logging()                    # forward to Python's logging module
```

## Status

What exists today: a compiling, tested skeleton with the full flow running from
start to finish. The real algorithms go into these stubs (marked `TODO(hermes)`):

| Area | Current placeholder | Planned |
|---|---|---|
| Global placement | Jacobi centroid pull | Nesterov analytic (WA wirelength + electrostatic density) |
| Initial placement | Die center | Quadratic / B2B |
| Timing | HPWL-proportional wire delay | Graph-based STA (Liberty + SDC) |
| Congestion estimate | RUDY + macro blockage | Pattern-route / probabilistic |
| Macro legalization | Clamp + fix | Overlap removal, grid snapping, orientation |
| Cell legalization | Row snapping | Abacus / Tetris around macros |
| Detailed placement | – | Timing-aware swaps, ISM, local reorder |
| Global routing | Star + L-shape | RSMT + maze, rip-up-and-reroute, layer assignment |
| Detailed routing | – | Track assignment, pin access, DRC repair |
| I/O | LEF/DEF (incl. routing layers), Bookshelf, native .hdb (directory input, manifest) | Verilog, Liberty, SDC readers |
| Layer assignment (QoR) | Greedy by run length, congestion-unaware | From the 3-D global router |
| Run tracking / QoR | SQLite run DB, dashboard, push to a server; end-of-run report | – |
| GUI | Viewer, live jobs, layers, inspector, heatmaps, console (OpenGL) | Look and feel per Mehmet's spec, Metal backend, routed wires from the router, embedded Python |
| AI assistant | MCP server (read-only / --allow-edits), built-in chat (Anthropic, OpenAI-compatible, local, mock), confirmations | Streaming answers, MCP resources (QoR maps as images), heatmap snapshots for vision models |
