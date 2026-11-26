#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include "assist/mcp_server.hpp"
#include "assist/tools.hpp"
#include "gui/scene/session.hpp"
#include "hermes/compute/backend.hpp"
#include "hermes/db/geometry.hpp"
#include "hermes/db/snapshot.hpp"
#include "hermes/flow/flow.hpp"
#include "hermes/io/bookshelf.hpp"
#include "hermes/io/design_file.hpp"
#include "hermes/io/lefdef.hpp"
#include "hermes/io/registry.hpp"
#include "hermes/log/log.hpp"
#include "hermes/log/sinks.hpp"
#include "hermes/par/job.hpp"
#include "hermes/par/parallel.hpp"
#include "hermes/qor/qor.hpp"
#include "hermes/region/region.hpp"
#include "hermes/track/run_tracker.hpp"

namespace py = pybind11;
using namespace hermes;

namespace {
std::optional<Id> opt(Id id) { return id == kInvalidId ? std::nullopt : std::optional<Id>(id); }

std::vector<Id> to_list(std::span<const Id> s) { return {s.begin(), s.end()}; }
std::vector<Id> ids(const DirtySet& d) { return to_list(d.ids()); }

// Python subclasses of DesignListener. The design is passed by reference
// (valid during the call); the change set is copied so Python may keep it.
class PyListener : public DesignListener {
 public:
  void on_flush(const Design& d, const ChangeSet& c) override {
    py::gil_scoped_acquire gil;  // flush may run in an engine with the GIL released
    py::function fn = py::get_override(static_cast<const DesignListener*>(this), "on_flush");
    if (!fn) throw std::runtime_error("DesignListener.on_flush is not overridden");
    fn(py::cast(&d, py::return_value_policy::reference), py::cast(c, py::return_value_policy::copy));
  }
};

// ---- logging ---------------------------------------------------------------

log::Level to_level(const py::handle& h) {
  if (py::isinstance<log::Level>(h)) return h.cast<log::Level>();
  if (py::isinstance<py::str>(h)) {
    const auto s = h.cast<std::string>();
    if (auto l = log::parse_level(s)) return *l;
    throw py::value_error("unknown log level '" + s + "'");
  }
  if (py::isinstance<py::int_>(h)) {
    const int v = h.cast<int>();
    if (v >= 0 && v <= 6) return static_cast<log::Level>(v);
  }
  throw py::value_error("log level must be a hermes.log.Level, a name or 0-6");
}

std::uint64_t to_cap(const std::optional<std::uint64_t>& cap) {
  return cap ? *cap : log::kUnlimited;
}

/// Forwards records to a Python callable. The GIL serializes calls, so this
/// sink takes no lock of its own (holding one while waiting for the GIL could
/// deadlock against a thread that holds the GIL and logs).
class PySink : public log::Sink {
 public:
  explicit PySink(py::function fn) : fn_(std::move(fn)) {}
  ~PySink() override {
    if (Py_IsInitialized()) {
      py::gil_scoped_acquire gil;
      fn_ = py::function();
    } else {
      fn_.release();  // interpreter gone: leak rather than touch it
    }
  }
  void write(const log::Record& r) override {
    if (!Py_IsInitialized()) return;
    py::gil_scoped_acquire gil;
    try {
      fn_(log::StoredRecord{r.level, std::string(r.channel), std::string(r.id),
                            std::string(r.text), r.last_of_id, r.elapsed_s, r.thread});
    } catch (py::error_already_set& e) {
      e.discard_as_unraisable("hermes.log callback sink");
    }
  }

 private:
  py::function fn_;
};

std::mutex g_py_sinks_mu;
std::vector<log::SinkId> g_py_sinks;

void bind_log(py::module_& parent) {
  py::module_ m = parent.def_submodule(
      "log",
      "Hierarchical logging. Channels are dot-separated (route, route.tree); a child\n"
      "inherits its parent's level unless set. Messages with ids can be capped.");

  py::enum_<log::Level>(m, "Level")
      .value("Off", log::Level::Off)
      .value("Error", log::Level::Error)
      .value("Warn", log::Level::Warn)
      .value("Info", log::Level::Info)
      .value("Verbose", log::Level::Verbose)
      .value("Debug", log::Level::Debug)
      .value("Trace", log::Level::Trace);

  py::class_<log::StoredRecord>(m, "Record")
      .def_readonly("level", &log::StoredRecord::level)
      .def_readonly("channel", &log::StoredRecord::channel)
      .def_readonly("id", &log::StoredRecord::id)
      .def_readonly("text", &log::StoredRecord::text)
      .def_readonly("last_of_id", &log::StoredRecord::last_of_id)
      .def_readonly("elapsed", &log::StoredRecord::elapsed_s)
      .def_readonly("thread", &log::StoredRecord::thread)
      .def("__repr__", [](const log::StoredRecord& r) {
        return "Record(" + std::string(log::to_string(r.level)) + ", '" + r.channel + "', '" +
               r.id + "', '" + r.text + "')";
      });

  m.attr("compiled_max_level") = log::kCompiledMaxLevel;

  m.def("set_level", [](std::string_view ch, py::handle l) { log::set_level(ch, to_level(l)); },
        py::arg("channel"), py::arg("level"),
        "Set a channel's level ('*' = root). Children without their own level follow it.");
  m.def("clear_level", &log::clear_level, py::arg("channel"),
        "Make a channel inherit its parent's level again.");
  m.def("level", &log::level, py::arg("channel") = "*", "Effective level of a channel.");
  m.def("explicit_level", &log::explicit_level, py::arg("channel"));
  m.def("configure", [](std::string_view spec) {
          try {
            log::configure(spec);
          } catch (const std::invalid_argument& e) {
            throw py::value_error(e.what());
          }
        },
        py::arg("spec"),
        "Apply a spec such as 'warn,route=debug,route.tree=trace,msg:DEF-0003=off,msg:GRT-1=5'.");
  m.def("channels", [] {
    py::list out;
    for (const auto& c : log::channels()) {
      py::dict d;
      d["name"] = c.name;
      d["level"] = c.level;
      d["explicit"] = c.explicit_level ? py::cast(*c.explicit_level) : py::none();
      out.append(d);
    }
    return out;
  });
  m.def("messages", [] {
    py::list out;
    for (const auto& mi : log::messages()) {
      py::dict d;
      d["id"] = mi.id;
      d["enabled"] = mi.enabled;
      d["cap"] = mi.cap == log::kUnlimited ? py::none() : py::cast(mi.cap);
      d["count"] = mi.count;
      out.append(d);
    }
    return out;
  });
  m.def("enable_message", &log::set_message_enabled, py::arg("id"), py::arg("enabled") = true);
  m.def("disable_message", [](std::string_view id) { log::set_message_enabled(id, false); },
        py::arg("id"));
  m.def("set_message_cap",
        [](std::string_view id, std::optional<std::uint64_t> cap) { log::set_message_cap(id, to_cap(cap)); },
        py::arg("id"), py::arg("cap"), "Print `id` at most `cap` times; None = default cap.");
  m.def("set_default_message_cap",
        [](std::optional<std::uint64_t> cap) { log::set_default_message_cap(to_cap(cap)); },
        py::arg("cap"), "Cap for ids without their own cap; None = unlimited.");
  m.def("reset_message_counts", &log::reset_message_counts);
  m.def("message_count", [](std::string_view id) { return log::message(id).count(); },
        py::arg("id"));

  m.def("log",
        [](std::string_view ch, py::handle level, std::string_view text, std::string_view id) {
          log::write(log::channel(ch), to_level(level), text, id);
        },
        py::arg("channel"), py::arg("level"), py::arg("text"), py::arg("id") = "",
        "Emit a message from Python through the same channels and sinks.");
  m.def("enabled", [](std::string_view ch, py::handle level) {
          return log::channel(ch).enabled(to_level(level));
        },
        py::arg("channel"), py::arg("level"));

  m.def("set_console_level", [](py::handle l) { log::console_sink()->set_level(to_level(l)); },
        py::arg("level"), "Extra filter on the console (e.g. 'warn' to quiet it).");
  m.def("console_level", [] { return log::console_sink()->level(); });
  m.def("set_console_format",
        [](bool time, bool thread, bool location) {
          log::console_sink()->set_format({time, thread, location});
        },
        py::arg("time") = false, py::arg("thread") = false, py::arg("location") = false);

  m.def("add_file_sink",
        [](const std::string& path, py::handle level, bool append) {
          auto s = std::make_shared<log::TextSink>(path, append);
          s->set_level(to_level(level));
          py::gil_scoped_release nogil;
          return log::add_sink(std::move(s));
        },
        py::arg("path"), py::arg("level") = log::Level::Trace, py::arg("append") = false);
  m.def("add_json_sink",
        [](const std::string& path, py::handle level, bool append) {
          auto s = std::make_shared<log::JsonLinesSink>(path, append);
          s->set_level(to_level(level));
          py::gil_scoped_release nogil;
          return log::add_sink(std::move(s));
        },
        py::arg("path"), py::arg("level") = log::Level::Trace, py::arg("append") = false);
  m.def("add_callback_sink",
        [](py::function fn, py::handle level) {
          auto s = std::make_shared<PySink>(std::move(fn));
          s->set_level(to_level(level));
          log::SinkId id;
          {
            py::gil_scoped_release nogil;
            id = log::add_sink(std::move(s));
          }
          std::lock_guard lk(g_py_sinks_mu);
          g_py_sinks.push_back(id);
          return id;
        },
        py::arg("fn"), py::arg("level") = log::Level::Trace,
        "Call fn(record) for every message that reaches this sink. Returns a sink id.");
  m.def("remove_sink",
        [](log::SinkId id) {
          {
            std::lock_guard lk(g_py_sinks_mu);
            std::erase(g_py_sinks, id);
          }
          py::gil_scoped_release nogil;  // a worker may be waiting for the GIL in a sink
          return log::remove_sink(id);
        },
        py::arg("id"));
  m.def("_remove_python_sinks", [] {
    std::vector<log::SinkId> ids;
    {
      std::lock_guard lk(g_py_sinks_mu);
      ids.swap(g_py_sinks);
    }
    py::gil_scoped_release nogil;
    for (auto id : ids) log::remove_sink(id);
  });
  m.def("flush", &log::flush, py::call_guard<py::gil_scoped_release>());
}


void bind_qor(py::module_& m) {
  py::class_<QorOptions>(m, "QorOptions")
      .def(py::init<>())
      .def_readwrite("target_density", &QorOptions::target_density)
      .def_readwrite("bin_rows", &QorOptions::bin_rows)
      .def_readwrite("max_bins_per_side", &QorOptions::max_bins_per_side)
      .def_readwrite("min_free_fraction", &QorOptions::min_free_fraction)
      .def_readwrite("worst_spots", &QorOptions::worst_spots)
      .def_readwrite("maps", &QorOptions::maps);

  py::class_<QorMap>(m, "QorMap", py::buffer_protocol(),
                     "A per-bin map. numpy.asarray(map) gives a (ny, nx) float32 view (row 0 = bottom).")
      .def_readonly("name", &QorMap::name)
      .def_readonly("unit", &QorMap::unit)
      .def_readonly("nx", &QorMap::nx)
      .def_readonly("ny", &QorMap::ny)
      .def_readonly("region", &QorMap::region)
      .def("at", &QorMap::at, py::arg("ix"), py::arg("iy"))
      .def("values", [](const QorMap& q) { return q.values; })
      .def("downsample", &QorMap::downsample, py::arg("max_side"), py::arg("use_max") = true)
      .def_buffer([](QorMap& q) -> py::buffer_info {
        return py::buffer_info(q.values.data(), sizeof(float), py::format_descriptor<float>::format(), 2,
                               {static_cast<py::ssize_t>(q.ny), static_cast<py::ssize_t>(q.nx)},
                               {static_cast<py::ssize_t>(sizeof(float) * q.nx), static_cast<py::ssize_t>(sizeof(float))});
      })
      .def("__repr__", [](const QorMap& q) {
        return "<QorMap " + q.name + " " + std::to_string(q.nx) + "x" + std::to_string(q.ny) + ">";
      });

  py::class_<DensityQor>(m, "DensityQor")
      .def_readonly("nx", &DensityQor::nx).def_readonly("ny", &DensityQor::ny)
      .def_readonly("target", &DensityQor::target)
      .def_readonly("avg_util", &DensityQor::avg_util)
      .def_readonly("peak_util", &DensityQor::peak_util)
      .def_readonly("overflow", &DensityQor::overflow)
      .def_readonly("movable_area", &DensityQor::movable_area)
      .def_readonly("free_area", &DensityQor::free_area)
      .def_readonly("cv", &DensityQor::cv)
      .def_readonly("util_mean", &DensityQor::util_mean)
      .def_readonly("util_std", &DensityQor::util_std)
      .def_readonly("overfull_share", &DensityQor::overfull_share)
      .def_readonly("over100_share", &DensityQor::over100_share)
      .def_readonly("bins_counted", &DensityQor::bins_counted);

  py::class_<LayerQor>(m, "LayerQor")
      .def_readonly("name", &LayerQor::name).def_readonly("dir", &LayerQor::dir)
      .def_readonly("routable", &LayerQor::routable)
      .def_readonly("capacity", &LayerQor::capacity).def_readonly("demand", &LayerQor::demand)
      .def_readonly("overflow", &LayerQor::overflow).def_readonly("max_util", &LayerQor::max_util)
      .def_readonly("avg_util", &LayerQor::avg_util).def_readonly("overflow_gcells", &LayerQor::overflow_gcells)
      .def_readonly("wirelength", &LayerQor::wirelength);

  py::class_<HotSpot>(m, "HotSpot")
      .def_readonly("ix", &HotSpot::ix).def_readonly("iy", &HotSpot::iy).def_readonly("box", &HotSpot::box)
      .def_readonly("util", &HotSpot::util).def_readonly("overflow", &HotSpot::overflow)
      .def_readonly("worst_layer", &HotSpot::worst_layer);

  py::class_<CongestionQor>(m, "CongestionQor")
      .def_readonly("nx", &CongestionQor::nx).def_readonly("ny", &CongestionQor::ny)
      .def_readonly("layer_source", &CongestionQor::layer_source)
      .def_readonly("model", &CongestionQor::model)
      .def_readonly("routed", &CongestionQor::routed)
      .def_readonly("layers", &CongestionQor::layers)
      .def_readonly("total_overflow", &CongestionQor::total_overflow)
      .def_readonly("h_overflow", &CongestionQor::h_overflow)
      .def_readonly("v_overflow", &CongestionQor::v_overflow)
      .def_readonly("overflow_gcells", &CongestionQor::overflow_gcells)
      .def_readonly("max_util", &CongestionQor::max_util)
      .def_readonly("h_max_util", &CongestionQor::h_max_util)
      .def_readonly("v_max_util", &CongestionQor::v_max_util)
      .def_property_readonly("ace", [](const CongestionQor& c) { return std::vector<double>(c.ace, c.ace + 4); })
      .def_readonly("rc", &CongestionQor::rc)
      .def_readonly("hot_spots", &CongestionQor::hot_spots)
      .def_readonly("rudy_h_overflow", &CongestionQor::rudy_h_overflow)
      .def_readonly("rudy_v_overflow", &CongestionQor::rudy_v_overflow)
      .def_readonly("rudy_max_util", &CongestionQor::rudy_max_util);

  py::class_<WirelengthQor>(m, "WirelengthQor")
      .def_readonly("hpwl", &WirelengthQor::hpwl).def_readonly("weighted_hpwl", &WirelengthQor::weighted_hpwl)
      .def_readonly("routed", &WirelengthQor::routed).def_readonly("layer_wl", &WirelengthQor::layer_wl)
      .def_readonly("vias", &WirelengthQor::vias).def_readonly("total_vias", &WirelengthQor::total_vias)
      .def_readonly("segments", &WirelengthQor::segments)
      .def_readonly("layers_estimated", &WirelengthQor::layers_estimated);

  py::class_<QorReport>(m, "QorReport", "End-of-run quality report; str(report) is the text form.")
      .def_readonly("design", &QorReport::design)
      .def_readonly("density", &QorReport::density)
      .def_readonly("congestion", &QorReport::congestion)
      .def_readonly("wirelength", &QorReport::wirelength)
      .def_readonly("maps", &QorReport::maps, py::return_value_policy::reference_internal)
      .def_readonly("notes", &QorReport::notes)
      .def_readonly("seconds", &QorReport::seconds)
      .def_readonly("total_seconds", &QorReport::total_seconds)
      .def("map", &QorReport::map, py::arg("name"), py::return_value_policy::reference_internal)
      .def("map_names", [](const QorReport& q) {
        std::vector<std::string> v;
        for (const auto& mm : q.maps) v.push_back(mm.name);
        return v;
      })
      .def("text", &format_qor)
      .def("to_json", &qor_to_json)
      .def("maps_json", &qor_maps_to_json, py::arg("max_side") = 128)
      .def("__str__", &format_qor);

  m.def(
      "compute_qor",
      [](const Design& d, const QorOptions& o, std::optional<GlobalRouterOptions> router) {
        if (!router) return compute_qor(d, o);
        const RouteUsage3D u = GlobalRouter(*router).route_usage(d);
        return compute_qor(d, o, &u);
      },
      py::arg("design"), py::arg("options") = QorOptions{}, py::arg("router") = std::nullopt,
      py::call_guard<py::gil_scoped_release>(),
      "QoR report for the design's current placement. With router options, congestion uses that "
      "router's routes on its GCell grid; without, a 64x64 grid (or the design's route grid).");
}

void bind_track(py::module_& parent) {
  py::module_ m = parent.def_submodule("track", "Run tracking: runs, progress, metrics and results in the run database.");
  py::class_<track::RunTracker>(m, "Run",
                                "A tracked run. Use as a context manager: the run ends 'done', or 'failed' "
                                "on an exception.")
      .def(py::init([](std::string name, std::string design, std::string input, std::vector<std::string> tags,
                       std::string settings_json, std::string command, std::string db_path, std::string server_url,
                       std::string token, bool capture_log) {
             track::RunInfo info{std::move(name), std::move(design), std::move(input), std::move(command),
                                 std::move(tags), std::move(settings_json)};
             if (info.command.empty()) info.command = "python";
             track::TrackerOptions o;
             o.db_path = std::move(db_path);
             o.server_url = std::move(server_url);
             o.token = std::move(token);
             auto t = std::make_unique<track::RunTracker>(std::move(info), std::move(o));
             if (capture_log) t->attach_log();
             return t;
           }),
           py::arg("name") = "", py::arg("design") = "", py::arg("input") = "",
           py::arg("tags") = std::vector<std::string>{}, py::arg("settings_json") = "", py::arg("command") = "",
           py::arg("db_path") = "", py::arg("server_url") = "", py::arg("token") = "", py::arg("capture_log") = true)
      .def_property_readonly("id", &track::RunTracker::run_id)
      .def_property_readonly("db_path", &track::RunTracker::db_path)
      .def_property_readonly("db_ok", &track::RunTracker::db_ok)
      .def_property_readonly("dropped", &track::RunTracker::dropped)
      .def("set_design", &track::RunTracker::set_design, py::arg("name"))
      .def("stage", &track::RunTracker::stage, py::arg("name"))
      .def("end_stage", &track::RunTracker::end_stage)
      .def("metric", &track::RunTracker::metric, py::arg("name"), py::arg("step"), py::arg("value"))
      .def("summary", py::overload_cast<std::string_view, double>(&track::RunTracker::summary), py::arg("key"),
           py::arg("value"))
      .def("summary", py::overload_cast<std::string_view, std::string_view>(&track::RunTracker::summary),
           py::arg("key"), py::arg("value"))
      .def("set_qor", &track::RunTracker::set_qor, py::arg("report"), py::arg("map_side") = 128)
      .def("sync", &track::RunTracker::sync, py::call_guard<py::gil_scoped_release>())
      .def("finish", &track::RunTracker::finish, py::arg("status") = "done", py::arg("exit_code") = 0,
           py::arg("error") = "", py::call_guard<py::gil_scoped_release>())
      .def("__enter__", [](track::RunTracker& t) -> track::RunTracker& { return t; },
           py::return_value_policy::reference)
      .def("__exit__", [](track::RunTracker& t, py::object exc_type, py::object exc, py::object) {
        if (exc_type.is_none()) {
          t.finish("done");
        } else {
          const std::string msg = py::str(exc);
          py::gil_scoped_release rel;
          t.finish("failed", 1, msg);
        }
        return false;
      });
  m.def("metric", &track::metric, py::arg("name"), py::arg("step"), py::arg("value"),
        "Record a metric in the active run (no-op without one).");
  m.def("default_db_path", &track::default_db_path);
  m.def("build_version", [] { return std::string(track::build_version()); });
  m.def("have_sqlite", &track::have_sqlite);
}
}  // namespace

// Threads, compute backends, background jobs, regions.
struct PyFlowJob {
  JobHandle handle;
  std::shared_ptr<FlowReport> report;
  py::object design;  // keeps the Design alive while the job runs
  ~PyFlowJob() {
    // The last handle cancels and joins: do it without the GIL, so a job
    // thread that logs to a Python sink can finish.
    py::gil_scoped_release release;
    handle = JobHandle();
  }
};

io::Options to_options(const std::optional<std::map<std::string, py::object>>& m) {
  io::Options o;
  if (!m) return o;
  for (const auto& [k, v] : *m) {
    if (py::isinstance<py::bool_>(v)) o.set(k, v.cast<bool>() ? "1" : "0");
    else o.set(k, py::str(v).cast<std::string>());
  }
  return o;
}

py::dict load_info_dict(const io::LoadInfo& li) {
  py::dict r;
  r["format"] = li.plan.format;
  r["origin"] = li.plan.origin;
  py::list items;
  for (const auto& it : li.plan.items) items.append(py::make_tuple(it.role, it.path));
  r["items"] = items;
  r["seconds"] = li.seconds;
  r["warnings"] = li.num_warnings;
  py::dict st;
  for (const auto& [k, v] : li.stats) st[py::str(k)] = v;
  r["stats"] = st;
  return r;
}

void bind_io(py::module_& m) {
  auto io_m = m.def_submodule("io", "Readers, writers and the native .hdb design file.");
  py::register_exception<io::ParseError>(io_m, "ParseError", PyExc_ValueError);
  py::register_exception<io::InputError>(io_m, "InputError", PyExc_ValueError);
  io_m.attr("MANIFEST_NAME") = std::string(io::kManifestName);

  py::class_<io::InputItem>(io_m, "InputItem")
      .def_readonly("role", &io::InputItem::role)
      .def_readonly("path", &io::InputItem::path)
      .def("__repr__", [](const io::InputItem& i) { return "InputItem(" + i.role + ", " + i.path + ")"; });
  py::class_<io::InputPlan>(io_m, "InputPlan", "What to read: the reader and its files in order.")
      .def_readonly("format", &io::InputPlan::format)
      .def_readonly("items", &io::InputPlan::items)
      .def_readonly("origin", &io::InputPlan::origin)
      .def_property_readonly("options", [](const io::InputPlan& p) {
        std::map<std::string, std::string> r(p.options.all().begin(), p.options.all().end());
        return r;
      })
      .def("paths", &io::InputPlan::paths, py::arg("role"))
      .def("describe", &io::InputPlan::describe);

  io_m.def("plan_input", [](const std::string& path) { return io::plan_input(path); }, py::arg("path"),
           py::call_guard<py::gil_scoped_release>(),
           "Decide what to read from a directory (hermes.inputs manifest wins over detection) or a file.");
  io_m.def("read_manifest", [](const std::string& path) { return io::read_manifest(path); },
           py::arg("path"));
  io_m.def(
      "load_input",
      [](const std::string& path, const std::optional<std::map<std::string, py::object>>& opts) {
        const io::Options o = to_options(opts);
        io::LoadInfo info;
        Design d;
        {
          py::gil_scoped_release nogil;
          d = io::load_input(path, o, &info);
        }
        return py::make_tuple(std::move(d), load_info_dict(info));
      },
      py::arg("path"), py::arg("options") = py::none(),
      "Load a directory, manifest or file. Returns (design, info dict).");
  io_m.def(
      "load_plan",
      [](const io::InputPlan& plan, const std::optional<std::map<std::string, py::object>>& opts) {
        const io::Options o = to_options(opts);
        io::LoadInfo info;
        Design d;
        {
          py::gil_scoped_release nogil;
          d = io::load_plan(plan, o, &info);
        }
        return py::make_tuple(std::move(d), load_info_dict(info));
      },
      py::arg("plan"), py::arg("options") = py::none());
  io_m.def(
      "write_design",
      [](const Design& d, const std::string& path, const std::string& format,
         const std::optional<std::map<std::string, py::object>>& opts) {
        const io::Options o = to_options(opts);
        py::gil_scoped_release nogil;
        io::write_design(d, path, format, o);
      },
      py::arg("design"), py::arg("path"), py::arg("format") = "", py::arg("options") = py::none(),
      "Write with a named writer (hdb, def, pl, bookshelf) or by the path's extension.");
  io_m.def("readers", [] {
    std::vector<std::pair<std::string, std::string>> r;
    for (const auto& x : io::Registry::global().readers()) r.emplace_back(x->name(), x->description());
    return r;
  });
  io_m.def("writers", [] {
    std::vector<std::pair<std::string, std::string>> r;
    for (const auto& x : io::Registry::global().writers()) r.emplace_back(x->name(), x->description());
    return r;
  });

  // Native design file.
  io_m.def(
      "save_design",
      [](const Design& d, const std::string& path, bool compress, const std::string& note,
         const std::map<std::string, py::bytes>& extra) {
        io::SaveOptions so;
        so.compress = compress;
        so.note = note;
        for (const auto& [k, v] : extra) {
          const std::string b = v;
          so.extra[k].assign(b.begin(), b.end());
        }
        io::SaveReport rep;
        {
          py::gil_scoped_release nogil;
          rep = io::save_design(d, path, so);
        }
        py::dict r;
        r["raw_bytes"] = rep.raw_bytes;
        r["file_bytes"] = rep.file_bytes;
        r["sections"] = rep.sections;
        r["chunks"] = rep.chunks;
        r["seconds"] = rep.seconds;
        return r;
      },
      py::arg("design"), py::arg("path"), py::arg("compress") = true, py::arg("note") = "",
      py::arg("extra") = std::map<std::string, py::bytes>{},
      "Save a versioned .hdb snapshot (atomic). `extra` maps tags to bytes stored as x.<tag>.");
  io_m.def(
      "load_design",
      [](const std::string& path, bool verify) {
        io::LoadOptions lo;
        lo.verify = verify;
        std::map<std::string, std::vector<std::uint8_t>> extra;
        Design d;
        {
          py::gil_scoped_release nogil;
          d = io::load_design(path, lo, nullptr, &extra);
        }
        py::dict ex;
        for (const auto& [k, v] : extra)
          ex[py::str(k)] = py::bytes(reinterpret_cast<const char*>(v.data()), v.size());
        return py::make_tuple(std::move(d), ex);
      },
      py::arg("path"), py::arg("verify") = true, "Load a .hdb file. Returns (design, extra sections).");
  io_m.def(
      "inspect_design_file",
      [](const std::string& path) {
        const io::DesignFileInfo i = io::inspect_design_file(path);
        py::dict r;
        r["version"] = py::make_tuple(i.major, i.minor);
        r["meta"] = i.meta;
        r["file_bytes"] = i.file_bytes;
        py::list secs;
        for (const auto& s : i.sections) {
          py::dict e;
          e["tag"] = s.tag;
          e["count"] = s.count;
          e["raw_bytes"] = s.raw_bytes;
          e["stored_bytes"] = s.stored_bytes;
          e["compressed"] = s.compressed;
          secs.append(e);
        }
        r["sections"] = secs;
        return r;
      },
      py::arg("path"));
  io_m.def("is_design_file", &io::is_design_file, py::arg("path"));

  // Format-specific entry points.
  io_m.def(
      "read_lef_def",
      [](const std::vector<std::string>& lefs, const std::string& def) {
        py::gil_scoped_release nogil;
        return io::read_lef_def(lefs, def);
      },
      py::arg("lefs"), py::arg("def_path"));
  io_m.def(
      "write_def",
      [](const Design& d, const std::string& path, bool nets) {
        io::DefWriteOptions o;
        o.nets = nets;
        py::gil_scoped_release nogil;
        io::write_def(d, path, o);
      },
      py::arg("design"), py::arg("path"), py::arg("nets") = true);
  io_m.def(
      "read_bookshelf",
      [](const std::string& aux, const std::optional<std::map<std::string, py::object>>& opts) {
        const io::Options o = to_options(opts);
        py::gil_scoped_release nogil;
        return io::read_bookshelf_aux(aux, o);
      },
      py::arg("aux_path"), py::arg("options") = py::none());
  io_m.def(
      "write_bookshelf",
      [](const Design& d, const std::string& dir, const std::string& base, double scale) {
        py::gil_scoped_release nogil;
        return io::write_bookshelf(d, dir, base, scale);
      },
      py::arg("design"), py::arg("dir"), py::arg("base") = "", py::arg("scale") = 0.0,
      "Write .aux/.nodes/.nets/.pl/.scl/.wts; returns the .aux path.");
  io_m.def(
      "write_bookshelf_pl",
      [](const Design& d, const std::string& path, double scale, bool placed_marker) {
        py::gil_scoped_release nogil;
        io::write_bookshelf_pl(d, path, scale, placed_marker);
      },
      py::arg("design"), py::arg("path"), py::arg("scale") = 0.0, py::arg("placed_marker") = true);
}

void bind_parallel(py::module_& m) {
  m.def("set_num_threads", &par::set_num_threads, py::arg("n"),
        "Threads used by parallel loops (0 = all hardware threads).");
  m.def("num_threads", &par::num_threads);
  m.def("hardware_threads", &par::hardware_threads);
  m.def("compute_backends", &compute::backends, "Registered compute backends.");
  m.def("compute_backend", [] { return std::string(compute::backend().name()); });
  m.def("set_compute_backend", &compute::set_backend, py::arg("name"));

  py::enum_<JobState>(m, "JobState")
      .value("Running", JobState::Running)
      .value("Done", JobState::Done)
      .value("Cancelled", JobState::Cancelled)
      .value("Failed", JobState::Failed);

  py::class_<JobProgress>(m, "JobProgress")
      .def_readonly("stage", &JobProgress::stage)
      .def_readonly("fraction", &JobProgress::fraction)
      .def_readonly("message", &JobProgress::message)
      .def_readonly("updates", &JobProgress::updates)
      .def_readonly("elapsed_s", &JobProgress::elapsed_s)
      .def("__repr__", [](const JobProgress& p) {
        return "<JobProgress " + p.stage + " " + std::to_string(static_cast<int>(p.fraction * 100)) +
               "%: " + p.message + ">";
      });

  py::class_<PyFlowJob, std::shared_ptr<PyFlowJob>>(m, "FlowJob",
                                                    "A run_flow running in the background.")
      .def_property_readonly("state", [](const PyFlowJob& j) { return j.handle.state(); })
      .def_property_readonly("running", [](const PyFlowJob& j) { return j.handle.running(); })
      .def("progress", [](const PyFlowJob& j) { return j.handle.progress(); })
      .def("cancel", [](PyFlowJob& j) { j.handle.cancel(); })
      .def(
          "wait",
          [](PyFlowJob& j, std::optional<double> timeout) {
            py::gil_scoped_release r;
            if (!timeout) {
              j.handle.wait();
              return true;
            }
            return j.handle.wait_for(*timeout);
          },
          py::arg("timeout") = std::nullopt,
          "Block until the job ends (True) or the timeout passes (False).")
      .def("result", [](PyFlowJob& j) {
        {
          py::gil_scoped_release r;
          j.handle.wait();
        }
        j.handle.rethrow_if_failed();
        return *j.report;
      }, "Wait, then return the FlowReport (raises if the flow failed).");

  m.def(
      "run_flow_async",
      [](py::object design, const FlowOptions& o, double publish_interval_s, int max_threads) {
        auto job = std::make_shared<PyFlowJob>();
        job->design = design;
        job->report = std::make_shared<FlowReport>();
        Design& d = design.cast<Design&>();
        JobOptions jo;
        jo.publish_interval_s = publish_interval_s;
        jo.max_threads = max_threads;
        auto rep = job->report;
        job->handle = start_job(
            "flow", [&d, o, rep](RunControl& ctl) { *rep = run_flow(d, o, &ctl); }, jo);
        return job;
      },
      py::arg("design"), py::arg("options") = FlowOptions{}, py::arg("publish_interval_s") = 0.25,
      py::arg("max_threads") = 0,
      "Start run_flow in the background. Read the design only through a SnapshotPublisher "
      "while it runs.");

  // --- Regions ---------------------------------------------------------------
  auto r = m.def_submodule("region", "Region partitioning and region-local work.");
  py::class_<region::RegionGrid>(r, "RegionGrid")
      .def(py::init<const Rect&, int, int>(), py::arg("area"), py::arg("nx"), py::arg("ny"))
      .def_property_readonly("nx", &region::RegionGrid::nx)
      .def_property_readonly("ny", &region::RegionGrid::ny)
      .def_property_readonly("size", &region::RegionGrid::size)
      .def("rect", &region::RegionGrid::rect)
      .def("region_of", &region::RegionGrid::region_of)
      .def("color", &region::RegionGrid::color);
  py::class_<region::Partition>(r, "Partition")
      .def(py::init<const Design&, const region::RegionGrid&>(), py::arg("design"), py::arg("grid"),
           py::call_guard<py::gil_scoped_release>())
      .def_property_readonly("grid", &region::Partition::grid)
      .def_property_readonly("num_regions", &region::Partition::num_regions)
      .def_property_readonly("num_cut_nets", &region::Partition::num_cut_nets)
      .def("region_of_cell", &region::Partition::region_of_cell)
      .def("cells", [](const region::Partition& p, int reg) {
        auto c = p.cells(reg);
        return std::vector<Id>(c.begin(), c.end());
      })
      .def("is_cut", &region::Partition::is_cut)
      .def("spatial_order", &region::Partition::spatial_order)
      .def("all_regions", &region::Partition::all_regions)
      .def("regions_of_color", &region::Partition::regions_of_color);
  py::class_<region::RegionRunStats>(r, "RegionRunStats")
      .def_readonly("regions", &region::RegionRunStats::regions)
      .def_readonly("cells_moved", &region::RegionRunStats::cells_moved)
      .def_readonly("hpwl_before", &region::RegionRunStats::hpwl_before)
      .def_readonly("hpwl_after", &region::RegionRunStats::hpwl_after);
  r.def(
      "run_regions",
      [](Design& d, const region::Partition& p, const std::vector<int>& regions,
         const std::string& solver, const std::string& executor, int workers) {
        py::gil_scoped_release rel;
        if (executor == "local") {
          region::LocalExecutor ex;
          return region::run_regions(d, p, regions, solver, ex);
        }
        if (executor == "loopback") {
          std::vector<std::shared_ptr<region::Transport>> ws;
          for (int i = 0; i < std::max(1, workers); ++i)
            ws.push_back(std::make_shared<region::LoopbackTransport>());
          region::RemoteExecutor ex(std::move(ws));
          return region::run_regions(d, p, regions, solver, ex);
        }
        throw std::invalid_argument("executor must be 'local' or 'loopback'");
      },
      py::arg("design"), py::arg("partition"), py::arg("regions"), py::arg("solver") = "median",
      py::arg("executor") = "local", py::arg("workers") = 2,
      "Solve regions in parallel and apply the results (flushes once). 'loopback' sends "
      "every problem through the serialized remote-worker protocol.");
  r.def("solvers", &region::solver_names);
}


// --- hermes.Console: the console/tool layer the GUI, `hermes shell` and the
// AI assistant use (same commands, same tools), for scripts and notebooks.
struct PyConsole {
  hermes::gui::Session session;
  hermes::assist::HeadlessView view{session};
  hermes::gui::CommandRegistry commands;
  hermes::assist::ToolRegistry tools;
  hermes::assist::ToolContext ctx{session, view, commands, ""};
  hermes::assist::EditPolicy edits = hermes::assist::EditPolicy::Allow;
  std::unique_ptr<hermes::assist::McpServer> mcp;
};

void bind_assist(py::module_& m) {
  using namespace hermes::assist;
  py::class_<PyConsole>(m, "Console",
                        "A headless hermes session with the console commands and the assistant tools.\n\n"
                        "    c = hermes.Console('designs/aes')      # loads (waits)\n"
                        "    print(c.run('qor'))                    # console command, waits for jobs\n"
                        "    c.call_tool('congestion_report', hot_spots=5)   # JSON text\n"
                        "    c.mcp('{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}')")
      .def(py::init([](std::optional<std::string> path, const std::string& edits, const std::string& runs_db) {
             auto c = std::make_unique<PyConsole>();
             if (!parse_policy(edits, &c->edits) || c->edits == EditPolicy::Ask)
               throw std::invalid_argument("edits must be 'read-only' or 'allow'");
             c->ctx.runs_db = runs_db;
             if (path) {
               std::string why;
               if (!c->session.open(*path, {}, &why)) throw std::runtime_error(why);
               py::gil_scoped_release nogil;
               c->session.wait();
             }
             McpOptions o;
             o.edits = c->edits;
             c->mcp = std::make_unique<McpServer>(c->tools, c->ctx, o);
             return c;
           }),
           py::arg("path") = py::none(), py::arg("edits") = "allow", py::arg("runs_db") = "")
      .def(
          "run",
          [](PyConsole& c, const std::string& line) {
            hermes::gui::CommandContext cc{c.session, c.view};
            const auto w = hermes::gui::split_command_line(line);
            if (!w.empty() && c.edits == EditPolicy::ReadOnly)
              if (const auto* cmd = c.commands.find(w[0]); cmd && (cmd->edits_design || cmd->writes_files))
                throw std::runtime_error("'" + w[0] + "' changes the design; this Console is read-only");
            hermes::gui::CommandResult r = c.commands.run(cc, line);
            if (!r.ok) throw std::runtime_error(r.text);
            {
              py::gil_scoped_release nogil;
              if (r.wait_until)
                while (!r.wait_until()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
              if (c.session.busy()) c.session.wait();
              c.session.poll();
            }
            return r.text;
          },
          py::arg("line"), "Runs a console command line and waits for the job it starts; returns its text.")
      .def(
          "call_tool",
          [](PyConsole& c, const std::string& name, py::kwargs kw) {
            const std::string args = py::str(py::module_::import("json").attr("dumps")(kw));
            const Json a = Json::parse(args);
            if (std::string why = policy_refusal(c.tools, c.ctx, name, a, c.edits); !why.empty())
              throw std::runtime_error(why);
            ToolResult r;
            {
              py::gil_scoped_release nogil;
              r = call_tool_blocking(c.tools, c.ctx, name, a);
            }
            if (!r.ok) throw std::runtime_error(r.text);
            return r.text;
          },
          "Calls an assistant tool with keyword arguments; returns its text (often JSON).")
      .def("tools",
           [](PyConsole& c) {
             py::list out;
             for (const Tool* t : c.tools.list(true)) {
               py::dict d;
               d["name"] = t->name;
               d["title"] = t->title;
               d["access"] = t->access_for ? "mixed" : access_name(t->access);
               d["description"] = t->description;
               d["schema"] = t->schema.dump();
               out.append(d);
             }
             return out;
           })
      .def(
          "mcp",
          [](PyConsole& c, const std::string& line) -> std::optional<std::string> {
            py::gil_scoped_release nogil;
            return c.mcp->handle_line(line);
          },
          py::arg("line"), "Handles one MCP JSON-RPC message; returns the reply (None for notifications).")
      .def_property_readonly("has_design", [](PyConsole& c) { return c.session.has_design(); })
      .def("summary", [](PyConsole& c) { return design_summary(c.session).dump(1); });
}

PYBIND11_MODULE(_hermes, m) {
  bind_assist(m);
  m.doc() = "Hermes: Holistic Engine for Routability-driven Mixed-size placement with Embedded STA";

  py::class_<Point>(m, "Point")
      .def(py::init<>())
      .def(py::init([](Dbu x, Dbu y) { return Point{x, y}; }), py::arg("x"), py::arg("y"))
      .def_readwrite("x", &Point::x)
      .def_readwrite("y", &Point::y)
      .def("__repr__", [](const Point& p) {
        return "Point(" + std::to_string(p.x) + ", " + std::to_string(p.y) + ")";
      });

  py::class_<Rect>(m, "Rect")
      .def(py::init<>())
      .def(py::init([](Dbu xlo, Dbu ylo, Dbu xhi, Dbu yhi) { return Rect{xlo, ylo, xhi, yhi}; }),
           py::arg("xlo"), py::arg("ylo"), py::arg("xhi"), py::arg("yhi"))
      .def_readwrite("xlo", &Rect::xlo)
      .def_readwrite("ylo", &Rect::ylo)
      .def_readwrite("xhi", &Rect::xhi)
      .def_readwrite("yhi", &Rect::yhi)
      .def_property_readonly("width", &Rect::width)
      .def_property_readonly("height", &Rect::height)
      .def_property_readonly("area", &Rect::area);

  py::enum_<PinDir>(m, "PinDir")
      .value("Input", PinDir::Input)
      .value("Output", PinDir::Output)
      .value("Inout", PinDir::Inout);

  py::enum_<Orient>(m, "Orient")
      .value("N", Orient::N).value("S", Orient::S).value("E", Orient::E).value("W", Orient::W)
      .value("FN", Orient::FN).value("FS", Orient::FS).value("FE", Orient::FE)
      .value("FW", Orient::FW);

  // Cell/Net/Pin are read-only value copies: change the design through its
  // methods (move_cell, set_fixed, ...) so change tracking and undo see it.
  py::class_<Cell>(m, "Cell")
      .def_readonly("loc", &Cell::loc)
      .def_readonly("width", &Cell::width)
      .def_readonly("height", &Cell::height)
      .def_readonly("orient", &Cell::orient)
      .def_readonly("is_macro", &Cell::is_macro)
      .def_readonly("fixed", &Cell::fixed)
      .def_readonly("placed", &Cell::placed)
      .def_readonly("removed", &Cell::removed)
      .def_property_readonly("bbox", &Cell::bbox);

  py::enum_<LayerDir>(m, "LayerDir")
      .value("Horizontal", LayerDir::Horizontal)
      .value("Vertical", LayerDir::Vertical);
  py::class_<RoutingLayer>(m, "RoutingLayer")
      .def(py::init([](std::string name, LayerDir dir, Dbu pitch, Dbu width, Dbu spacing) {
             return RoutingLayer{std::move(name), dir, pitch, width, spacing};
           }),
           py::arg("name"), py::arg("dir") = LayerDir::Horizontal, py::arg("pitch") = 0, py::arg("width") = 0,
           py::arg("spacing") = 0)
      .def_readwrite("name", &RoutingLayer::name)
      .def_readwrite("dir", &RoutingLayer::dir)
      .def_readwrite("pitch", &RoutingLayer::pitch)
      .def_readwrite("width", &RoutingLayer::width)
      .def_readwrite("spacing", &RoutingLayer::spacing)
      .def("__repr__", [](const RoutingLayer& l) {
        return "<RoutingLayer " + l.name + (l.dir == LayerDir::Horizontal ? " H" : " V") + " pitch " +
               std::to_string(l.pitch) + ">";
      });

  py::class_<Row>(m, "Row")
      .def_readonly("origin", &Row::origin)
      .def_readonly("site_width", &Row::site_width)
      .def_readonly("site_height", &Row::site_height)
      .def_readonly("site_step", &Row::site_step)
      .def_readonly("num_sites", &Row::num_sites)
      .def_readonly("orient", &Row::orient)
      .def_property_readonly("bbox", &Row::bbox);

  py::class_<Net>(m, "Net")
      .def_readonly("weight", &Net::weight)
      .def_readonly("removed", &Net::removed);

  py::class_<Pin>(m, "Pin")
      .def_property_readonly("cell", [](const Pin& p) { return opt(p.cell); })
      .def_property_readonly("net", [](const Pin& p) { return opt(p.net); })
      .def_readonly("offset", &Pin::offset)
      .def_readonly("dir", &Pin::dir);

  py::class_<ChangeSet>(m, "ChangeSet", "What changed between two Design.flush() calls.")
      .def_readonly("version", &ChangeSet::version)
      .def_readonly("all", &ChangeSet::all)
      .def_readonly("structure", &ChangeSet::structure)
      .def_readonly("floorplan", &ChangeSet::floorplan)
      .def_property_readonly("cells", [](const ChangeSet& c) { return ids(c.cells); })
      .def_property_readonly("nets", [](const ChangeSet& c) { return ids(c.nets); })
      .def_property_readonly("pins", [](const ChangeSet& c) { return ids(c.pins); })
      .def("empty", &ChangeSet::empty);

  py::class_<DesignListener, PyListener>(
      m, "DesignListener",
      "Subclass and override on_flush(design, changes); register with Design.subscribe().")
      .def(py::init<>())
      .def("on_flush", &DesignListener::on_flush);

  py::class_<Transaction>(m, "Transaction",
                          "Undo scope; use as a context manager via Design.transaction().")
      .def(py::init<Design&>(), py::keep_alive<1, 2>())
      .def("commit", &Transaction::commit)
      .def("rollback", &Transaction::rollback)
      .def_property_readonly("active", &Transaction::active)
      .def("__len__", &Transaction::size)
      .def("__enter__", [](Transaction& t) -> Transaction& { return t; },
           py::return_value_policy::reference)
      .def("__exit__", [](Transaction& t, py::object exc_type, py::object, py::object) {
        if (!t.active()) return false;
        if (exc_type.is_none()) t.commit();
        else t.rollback();
        return false;  // re-raise exceptions
      });

  py::class_<Design>(m, "Design")
      .def(py::init<std::string>(), py::arg("name") = "top")
      .def_property_readonly("name", &Design::name)
      .def_property("die", &Design::die, &Design::set_die)
      .def_property("row_height", &Design::row_height, &Design::set_row_height)
      .def("add_cell", &Design::add_cell, py::arg("name"), py::arg("width"),
           py::arg("height"), py::arg("is_macro") = false)
      .def("add_net", &Design::add_net, py::arg("name"))
      .def("add_pin",
           [](Design& d, std::optional<Id> cell, std::string_view n, Point off, PinDir dir) {
             return d.add_pin(cell.value_or(kInvalidId), n, off, dir);
           },
           py::arg("cell"), py::arg("pin_name"), py::arg("offset") = Point{},
           py::arg("dir") = PinDir::Input, "Unconnected pin; cell=None for a top-level I/O pin.")
      .def("connect", &Design::connect, py::arg("cell"), py::arg("net"),
           py::arg("pin_name"), py::arg("offset") = Point{}, py::arg("dir") = PinDir::Input)
      .def("connect_pin", &Design::connect_pin, py::arg("pin"), py::arg("net"))
      .def("disconnect_pin", &Design::disconnect_pin, py::arg("pin"))
      .def("reconnect_pin", &Design::reconnect_pin, py::arg("pin"), py::arg("net"))
      .def("remove_cell", &Design::remove_cell, py::arg("cell"))
      .def("remove_net", &Design::remove_net, py::arg("net"))
      .def("move_cell", &Design::move_cell, py::arg("cell"), py::arg("loc"))
      .def("resize_cell", &Design::resize_cell, py::arg("cell"), py::arg("width"), py::arg("height"))
      .def("set_fixed", &Design::set_fixed, py::arg("cell"), py::arg("fixed") = true)
      .def("set_placed", &Design::set_placed, py::arg("cell"), py::arg("placed") = true)
      .def("set_cell_master", &Design::set_cell_master, py::arg("cell"), py::arg("master"))
      .def("cell_master", [](const Design& d, Id c) { return std::string(d.cell_master(c)); }, py::arg("cell"),
           "LEF master name ('' when unknown, e.g. read from Bookshelf).")
      .def_property("dbu_per_micron", &Design::dbu_per_micron, &Design::set_dbu_per_micron)
      .def_property_readonly("rows", [](const Design& d) { return std::vector<Row>(d.rows().begin(), d.rows().end()); })
      .def_property(
          "routing_layers",
          [](const Design& d) { return std::vector<RoutingLayer>(d.routing_layers().begin(), d.routing_layers().end()); },
          [](Design& d, std::vector<RoutingLayer> v) { d.set_routing_layers(std::move(v)); },
          "Metal layers bottom to top (from a technology LEF).")
      .def("set_orient", &Design::set_orient, py::arg("cell"), py::arg("orient"))
      .def("set_pin_offset", &Design::set_pin_offset, py::arg("pin"), py::arg("offset"))
      .def("set_net_weight", &Design::set_net_weight, py::arg("net"), py::arg("weight"))
      .def("compact", &Design::compact)
      // Names live in the Design, not in Cell/Net/Pin. Lookups return None when absent.
      .def("cell_name", &Design::cell_name, py::arg("cell"))
      .def("net_name", &Design::net_name, py::arg("net"))
      .def("pin_name", &Design::pin_name, py::arg("pin"))
      .def("find_cell", [](const Design& d, std::string_view n) { return opt(d.find_cell(n)); },
           py::arg("name"))
      .def("find_net", [](const Design& d, std::string_view n) { return opt(d.find_net(n)); },
           py::arg("name"))
      .def("find_pin",
           [](const Design& d, std::optional<Id> cell, std::string_view n) {
             return opt(d.find_pin(cell.value_or(kInvalidId), n));
           },
           py::arg("cell"), py::arg("pin_name"),
           "Pin `pin_name` of `cell`; pass cell=None for a top-level I/O pin.")
      .def("cell", &Design::cell, py::arg("cell"), py::return_value_policy::copy)
      .def("net", &Design::net, py::arg("net"), py::return_value_policy::copy)
      .def("pin", &Design::pin, py::arg("pin"), py::return_value_policy::copy)
      .def("net_pins", [](const Design& d, Id n) { return to_list(d.net_pins(n)); }, py::arg("net"))
      .def("cell_pins", [](const Design& d, Id c) { return to_list(d.cell_pins(c)); }, py::arg("cell"))
      .def_property_readonly("io_pins", [](const Design& d) { return to_list(d.io_pins()); })
      .def_property_readonly("num_cells", &Design::num_cells)
      .def_property_readonly("num_nets", &Design::num_nets)
      .def_property_readonly("num_pins", &Design::num_pins)
      .def_property_readonly("num_macros", [](const Design& d) { return count_macros(d); })
      .def("pin_location", [](const Design& d, Id p) { return pin_location(d, p); }, py::arg("pin"))
      .def("net_bbox", [](const Design& d, Id n) { return net_bbox(d, n); }, py::arg("net"))
      .def("net_hpwl", [](const Design& d, Id n) { return net_hpwl(d, n); }, py::arg("net"))
      .def("total_hpwl", [](const Design& d, bool w) { return total_hpwl(d, w); },
           py::arg("weighted") = false)
      .def_property_readonly("memory_bytes", &Design::memory_bytes)
      // Change tracking and undo.
      .def_property("tracking", &Design::tracking, &Design::set_tracking)
      .def_property_readonly("version", &Design::version)
      .def_property_readonly("pending", &Design::pending, py::return_value_policy::copy,
                             "Changes since the last flush (a copy).")
      .def("flush", &Design::flush,
           "End a batch: hand pending changes to listeners; returns the new version.")
      .def("subscribe", &Design::subscribe, py::arg("listener"), py::keep_alive<1, 2>())
      .def("unsubscribe", &Design::unsubscribe, py::arg("listener"))
      .def_property_readonly("in_transaction", &Design::in_transaction)
      .def("transaction", [](Design& d) { return std::make_unique<Transaction>(d); },
           py::keep_alive<0, 1>(),
           "with d.transaction() as tx: ... (commits on success, rolls back on exception;\n"
           "call tx.rollback() to undo explicitly).");

  py::class_<DesignSnapshot, std::shared_ptr<DesignSnapshot>>(
      m, "DesignSnapshot", "Immutable design state at one version; safe to read from any thread.")
      .def_property_readonly("version", &DesignSnapshot::version)
      .def_property_readonly("die", &DesignSnapshot::die)
      .def_property_readonly("num_cells", &DesignSnapshot::num_cells)
      .def_property_readonly("num_nets", &DesignSnapshot::num_nets)
      .def_property_readonly("num_pins", &DesignSnapshot::num_pins)
      .def("cell", &DesignSnapshot::cell, py::return_value_policy::copy)
      .def("net", &DesignSnapshot::net, py::return_value_policy::copy)
      .def("pin", &DesignSnapshot::pin, py::return_value_policy::copy)
      .def("net_pins", [](const DesignSnapshot& s, Id n) { return to_list(s.net_pins(n)); })
      .def("pin_location", &DesignSnapshot::pin_location)
      .def("net_bbox", &DesignSnapshot::net_bbox)
      .def("cell_name", &DesignSnapshot::cell_name)
      .def("net_name", &DesignSnapshot::net_name)
      .def("find_cell", [](const DesignSnapshot& s, std::string_view n) { return opt(s.find_cell(n)); });

  py::class_<SnapshotPublisher>(m, "SnapshotPublisher",
                                "Publishes DesignSnapshots on each flush (or on publish()).")
      .def(py::init<Design&, bool>(), py::arg("design"), py::arg("auto_publish") = true,
           py::keep_alive<1, 2>())
      .def("publish", &SnapshotPublisher::publish)
      .def("latest", [](const SnapshotPublisher& p) {
        return std::const_pointer_cast<DesignSnapshot>(p.latest());
      })
      .def_property_readonly("last_copied_chunks", &SnapshotPublisher::last_copied_chunks)
      .def("set_cell_order", &SnapshotPublisher::set_cell_order, py::arg("order"),
           "Store cells in this order (e.g. Partition.spatial_order()) so region-local edits "
           "copy only that region's chunks.");

  py::class_<NetBoxCache>(m, "NetBoxCache", "Incremental per-net bboxes and total HPWL.")
      .def(py::init<Design&>(), py::arg("design"), py::keep_alive<1, 2>())
      .def("total_hpwl", &NetBoxCache::total_hpwl)
      .def("bbox", &NetBoxCache::bbox, py::arg("net"))
      .def_property_readonly("last_refresh_count", &NetBoxCache::last_refresh_count);

  py::class_<PlacerOptions>(m, "PlacerOptions")
      .def(py::init<>())
      .def_readwrite("max_iterations", &PlacerOptions::max_iterations)
      .def_readwrite("stop_overflow", &PlacerOptions::stop_overflow)
      .def_readwrite("log_interval", &PlacerOptions::log_interval)
      .def_readwrite("target_density", &PlacerOptions::target_density)
      .def_readwrite("timing_interval", &PlacerOptions::timing_interval)
      .def_readwrite("timing_weight", &PlacerOptions::timing_weight)
      .def_readwrite("timing_exponent", &PlacerOptions::timing_exponent)
      .def_readwrite("routability_interval", &PlacerOptions::routability_interval)
      .def_readwrite("inflation_threshold", &PlacerOptions::inflation_threshold)
      .def_readwrite("max_inflation", &PlacerOptions::max_inflation)
      .def_readwrite("legalize", &PlacerOptions::legalize)
      .def_readwrite("detailed_placement", &PlacerOptions::detailed_placement)
      .def_readwrite("keep_placed", &PlacerOptions::keep_placed,
                     "Start cells marked placed (e.g. DEF PLACED) from their current location.");

  py::class_<GlobalRouterOptions>(m, "GlobalRouterOptions")
      .def(py::init<>())
      .def_readwrite("grid_x", &GlobalRouterOptions::grid_x)
      .def_readwrite("grid_y", &GlobalRouterOptions::grid_y)
      .def_readwrite("capacity_per_gcell", &GlobalRouterOptions::capacity_per_gcell)
      .def_readwrite("macro_blockage", &GlobalRouterOptions::macro_blockage)
      .def_readwrite("first_routing_layer", &GlobalRouterOptions::first_routing_layer)
      .def_readwrite("macro_blocked_layers", &GlobalRouterOptions::macro_blocked_layers);

  bind_qor(m);

  py::class_<FlowOptions>(m, "FlowOptions")
      .def(py::init<>())
      .def_readwrite("placer", &FlowOptions::placer)
      .def_readwrite("router", &FlowOptions::router)
      .def_readwrite("clock_period_ps", &FlowOptions::clock_period_ps)
      .def_readwrite("wire_delay_ps_per_dbu", &FlowOptions::wire_delay_ps_per_dbu)
      .def_readwrite("run_detailed_route", &FlowOptions::run_detailed_route)
      .def_readwrite("run_routing", &FlowOptions::run_routing)
      .def_readwrite("report_qor", &FlowOptions::report_qor)
      .def_readwrite("qor", &FlowOptions::qor)
      .def("to_json", &flow_options_json);

  py::class_<PlacementResult>(m, "PlacementResult")
      .def_readonly("iterations", &PlacementResult::iterations)
      .def_readonly("hpwl", &PlacementResult::hpwl)
      .def_readonly("wns_ps", &PlacementResult::wns_ps)
      .def_readonly("tns_ps", &PlacementResult::tns_ps)
      .def_readonly("overflow", &PlacementResult::overflow)
      .def_readonly("max_utilization", &PlacementResult::max_utilization)
      .def_readonly("density_overflow", &PlacementResult::density_overflow)
      .def_readonly("global_hpwl", &PlacementResult::global_hpwl)
      .def_readonly("cancelled", &PlacementResult::cancelled);

  py::class_<GlobalRouteResult>(m, "GlobalRouteResult")
      .def_readonly("total_overflow", &GlobalRouteResult::total_overflow)
      .def_readonly("max_utilization", &GlobalRouteResult::max_utilization)
      .def_readonly("wirelength", &GlobalRouteResult::wirelength);

  py::class_<DetailedRouteResult>(m, "DetailedRouteResult")
      .def_readonly("completed", &DetailedRouteResult::completed)
      .def_readonly("drc_violations", &DetailedRouteResult::drc_violations);

  py::class_<FlowReport>(m, "FlowReport")
      .def_readonly("placement", &FlowReport::placement)
      .def_readonly("global_route", &FlowReport::global_route)
      .def_readonly("detailed_route", &FlowReport::detailed_route)
      .def_readonly("final_wns_ps", &FlowReport::final_wns_ps)
      .def_readonly("final_tns_ps", &FlowReport::final_tns_ps)
      .def_readonly("cancelled", &FlowReport::cancelled)
      .def_property_readonly("qor", [](const FlowReport& r) -> py::object {
        return r.has_qor ? py::cast(r.qor) : py::none();
      }, "End-of-run QoR report (None if FlowOptions.report_qor was off).")
      .def_readonly("stage_seconds", &FlowReport::stage_seconds);

  // The GIL is released so engine threads can log to Python sinks.
  m.def(
      "run_flow",
      [](Design& d, const FlowOptions& o, track::RunTracker* tracker) {
        if (!tracker) return run_flow(d, o);
        RunControl ctl(0.0);
        tracker->attach(ctl);
        FlowReport r = run_flow(d, o, &ctl);
        tracker->summary("iterations", static_cast<double>(r.placement.iterations));
        tracker->summary("wns_ps", r.final_wns_ps);
        if (r.has_qor) tracker->set_qor(r.qor);
        return r;
      },
      py::arg("design"), py::arg("options") = FlowOptions{}, py::arg("tracker") = nullptr,
      py::call_guard<py::gil_scoped_release>(),
      "Run placement (timing- and routability-driven) followed by routing. With a hermes.track.Run, "
      "progress, stages, metrics and the QoR report are recorded in that run.");

  bind_track(m);

  bind_parallel(m);

  bind_io(m);

  bind_log(m);
}
