#include "gui/scene/session.hpp"

#include <algorithm>
#include <chrono>
#include <thread>
#include <filesystem>
#include <format>

#include "hermes/io/design_file.hpp"
#include "hermes/io/registry.hpp"
#include "hermes/log/log.hpp"
#include "hermes/par/parallel.hpp"
#include "hermes/route/global_router.hpp"

namespace hermes::gui {

HERMES_LOG_CHANNEL(log_gui, "gui");
HERMES_LOG_CHANNEL(log_cmd, "cmd");

namespace {
double now_s() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

struct Session::Loaded {
  std::unique_ptr<Design> design;
  std::unique_ptr<SnapshotPublisher> pub;
  std::shared_ptr<TechInfo> tech;
  std::string source;
  double seconds = 0.0;
};

std::shared_ptr<TechInfo> make_tech_info(const Design& d, const GlobalRouterOptions& router) {
  RouteUsageOptions o;
  o.grid_x = router.grid_x;
  o.grid_y = router.grid_y;
  o.first_routing_layer = router.first_routing_layer;
  o.macro_blocked_layers = router.macro_blocked_layers;
  LayerStack st = layer_stack_for(d, o);
  // Same GCell grid as the global router.
  const BinGrid g = GlobalRouter(router).gcell_grid(d);
  auto t = std::make_shared<TechInfo>();
  t->wires_source = "Estimated: global router stub (star + L-shape), greedy layer assignment; layers from " + st.source;
  t->routes = EstimatedRouteModel(std::move(st), g);
  return t;
}

Session::Session() {
  sink_id_ = log::add_sink(std::make_shared<log::CallbackSink>([this](const log::Record& r) {
    if (static_cast<int>(r.level) > static_cast<int>(log::Level::Info)) return;
    std::lock_guard lk(log_mu_);
    log_.push_back({r.level, std::string(r.channel), std::string(r.text), r.elapsed_s});
    if (log_.size() > 5000) log_.pop_front();
    log_count_.fetch_add(1, std::memory_order_relaxed);
  }));
}

Session::~Session() {
  if (job_.valid()) {
    job_.cancel();
    job_.wait();
  }
  job_ = {};
  log::remove_sink(sink_id_);
  pub_.reset();
  design_.reset();
}

void Session::note(std::string text, log::Level level) {
  std::lock_guard lk(log_mu_);
  log_.push_back({level, "gui", std::move(text), 0.0});
  if (log_.size() > 5000) log_.pop_front();
  log_count_.fetch_add(1, std::memory_order_relaxed);
}

std::vector<LogLine> Session::log_lines(std::size_t max) const {
  std::lock_guard lk(log_mu_);
  const std::size_t n = std::min(max, log_.size());
  return {log_.end() - static_cast<std::ptrdiff_t>(n), log_.end()};
}

std::shared_ptr<const DesignSnapshot> Session::snapshot() const { return pub_ ? pub_->latest() : nullptr; }

bool Session::busy() const { return job_.valid() && job_.running(); }

JobProgress Session::job_progress() const { return job_.valid() ? job_.progress() : JobProgress{}; }

int Session::job_threads() const {
  if (job_threads_ > 0) return job_threads_;
  return std::max(1, par::hardware_threads() - 1);
}

bool Session::check_free(std::string* why) const {
  if (busy()) {
    if (why) *why = "a job is running (" + job_kind_ + "); wait for it or cancel it";
    return false;
  }
  return true;
}

void Session::install(std::unique_ptr<Loaded> l) {
  // The previous design's snapshots stay valid for whoever holds them.
  pub_.reset();
  design_ = std::move(l->design);
  pub_ = std::move(l->pub);
  tech_ = std::move(l->tech);
  source_ = std::move(l->source);
  load_seconds_ = l->seconds;
  flow_report_done_.reset();
  qor_.reset();
  placement_.reset();
  legalize_.reset();
  route_.reset();
  timing_.reset();
  engines_ = std::make_unique<FlowEngines>();
  undo_.clear();
  ++generation_;
}

bool Session::open(const std::string& path, const io::Options& opts, std::string* why) {
  if (!check_free(why)) return false;
  job_kind_ = "load";
  JobOptions jo;
  jo.max_threads = job_threads();
  job_ = start_job(
      "load " + path,
      [this, path, opts](RunControl& ctl) {
        const double t0 = now_s();
        try {
          ctl.begin_stage("read", 0.0, 0.8);
          io::LoadInfo info;
          Design d = io::load_input(path, opts, &info);
          ctl.check_cancelled();
          ctl.begin_stage("snapshot", 0.8, 1.0);
          auto l = std::make_unique<Loaded>();
          l->design = std::make_unique<Design>(std::move(d));
          l->design->set_tracking(true);
          l->tech = make_tech_info(*l->design);
          l->pub = std::make_unique<SnapshotPublisher>(*l->design);
          l->source = path;
          l->seconds = now_s() - t0;
          HLOG_INFO(log_gui, "loaded {} ({} cells, {} nets) in {:.3f} s", path, l->design->num_cells(),
                    l->design->num_nets(), l->seconds);
          std::lock_guard lk(mu_);
          loaded_ = std::move(l);
        } catch (const Cancelled&) {
          throw;
        } catch (const std::exception& e) {
          std::lock_guard lk(mu_);
          load_error_ = e.what();
          throw;
        }
      },
      jo);
  return true;
}

bool Session::open_synthetic(const SyntheticOptions& o, std::string* why) {
  if (!check_free(why)) return false;
  job_kind_ = "load";
  JobOptions jo;
  jo.max_threads = job_threads();
  job_ = start_job(
      "synthetic",
      [this, o](RunControl& ctl) {
        const double t0 = now_s();
        ctl.begin_stage("generate", 0.0, 0.9);
        auto l = std::make_unique<Loaded>();
        l->design = std::make_unique<Design>(make_synthetic_design(o));
        l->design->set_tracking(true);
        l->tech = make_tech_info(*l->design);
        l->pub = std::make_unique<SnapshotPublisher>(*l->design);
        l->source = "synthetic " + std::to_string(o.cells);
        l->seconds = now_s() - t0;
        HLOG_INFO(log_gui, "generated a synthetic design with {} cells in {:.3f} s", l->design->num_cells(),
                  l->seconds);
        std::lock_guard lk(mu_);
        loaded_ = std::move(l);
      },
      jo);
  return true;
}

void Session::adopt(Design design, std::string source) {
  wait();
  auto l = std::make_unique<Loaded>();
  l->design = std::make_unique<Design>(std::move(design));
  l->design->set_tracking(true);
  l->tech = make_tech_info(*l->design);
  l->pub = std::make_unique<SnapshotPublisher>(*l->design);
  l->source = std::move(source);
  install(std::move(l));
}

bool Session::start_task(std::string kind, SessionTask task, bool edits, std::string* why) {
  if (!has_design()) {
    if (why) *why = "no design loaded";
    return false;
  }
  if (!check_free(why)) return false;
  if (edits) undo_.clear();  // The task moves cells; old undo entries would fight it.
  job_kind_ = kind;
  job_edits_ = edits;
  auto out = std::make_shared<TaskOutput>();
  task_out_ = out;
  JobOptions jo;
  jo.max_threads = job_threads();
  jo.publish_interval_s = 0.2;
  Design* d = design_.get();
  FlowEngines* eng = engines_.get();
  job_ = start_job(
      kind,
      [d, eng, out, task = std::move(task)](RunControl& ctl) {
        task(*d, *eng, ctl, *out);
        d->flush();  // Whatever the task left unflushed reaches the snapshots.
      },
      jo);
  return true;
}

bool Session::start_flow(const FlowOptions& opts, std::string* why) {
  const std::string kind = opts.run_routing ? "flow" : "placement";
  return start_task(
      kind,
      [opts](Design& d, FlowEngines& eng, RunControl& ctl, TaskOutput& out) {
        auto r = std::make_shared<FlowReport>(run_flow(d, eng, opts, &ctl));
        out.placement = r->placement;
        if (opts.run_routing && !r->cancelled) {
          RouteStepResult rs;
          rs.stats = r->global_route;
          out.route = rs;
        }
        if (!r->cancelled) {
          TimingStepResult t;
          t.wns_ps = r->final_wns_ps;
          t.tns_ps = r->final_tns_ps;
          t.clock_period_ps = eng.clock_period_ps;
          out.timing = t;
        }
        if (r->has_qor) out.qor = std::make_shared<QorReport>(r->qor);
        out.flow = std::move(r);
      },
      true, why);
}

bool Session::start_qor(std::string* why, QorOptions opts) {
  return start_task(
      "qor",
      [opts](Design& d, FlowEngines& eng, RunControl& ctl, TaskOutput& out) {
        ctl.begin_stage("qor", 0.0, 1.0);
        out.qor = std::make_shared<QorReport>(qor_design(d, eng, opts));  // Reads the design only.
        out.summary = "QoR report ready ('report' prints it)";
      },
      false, why);
}

bool Session::save(const std::string& path, std::string* why, const io::Options& opts) {
  return start_task(
      "save",
      [path, opts](Design& d, FlowEngines&, RunControl&, TaskOutput& out) {
        if (std::filesystem::path(path).extension() == ".hdb") {
          const auto r = io::save_design(d, path);
          out.summary = std::format("saved {} ({:.1f} MB) in {:.3f} s", path, static_cast<double>(r.file_bytes) / 1e6,
                                    r.seconds);
        } else {
          const double t0 = now_s();
          io::write_design(d, path, {}, opts);
          out.summary = std::format("wrote {} in {:.3f} s", path, now_s() - t0);
        }
      },
      false, why);
}

void Session::cancel() {
  if (job_.valid()) job_.cancel();
}

void Session::wait() {
  if (job_.valid()) job_.wait();
  poll();
}

void Session::poll() {
  if (!job_.valid() || job_.running()) return;
  const JobState st = job_.state();
  const std::string kind = job_kind_;
  std::unique_ptr<Loaded> l;
  {
    std::lock_guard lk(mu_);
    l = std::move(loaded_);
  }
  if (l) install(std::move(l));
  std::string fail_msg;
  if (st == JobState::Failed) {
    std::string& msg = fail_msg;
    try {
      job_.rethrow_if_failed();
    } catch (const std::exception& e) {
      msg = e.what();
    } catch (...) {
      msg = "unknown error";
    }
    note(kind + " failed: " + msg, log::Level::Error);
  } else if (st == JobState::Cancelled) {
    note(kind + " cancelled", log::Level::Warn);
  }
  std::shared_ptr<TaskOutput> out = std::move(task_out_);
  if (out && st == JobState::Done) {
    if (out->placement) placement_ = std::make_shared<PlacementResult>(*out->placement);
    if (out->legalize) legalize_ = std::make_shared<LegalizeStepResult>(*out->legalize);
    if (out->route) route_ = std::make_shared<RouteStepResult>(*out->route);
    if (out->timing) timing_ = std::make_shared<TimingStepResult>(*out->timing);
    if (out->flow) flow_report_done_ = out->flow;
    if (out->qor) {
      qor_ = out->qor;
      qor_version_ = design_ ? design_->version() : 0;
    }
    if (!out->summary.empty()) HLOG_INFO(log_cmd, "{}", out->summary);
    else if (kind != "load") HLOG_INFO(log_cmd, "{} finished", kind);
  } else if (out && st == JobState::Cancelled && out->placement) {
    placement_ = std::make_shared<PlacementResult>(*out->placement);  // partial, marked cancelled
  }
  last_end_.kind = kind;
  last_end_.state = st;
  last_end_.error = fail_msg;
  ++last_end_.serial;
  job_ = {};
  job_kind_.clear();
}

bool Session::move_cell(Id cell, Point loc, std::string* why) {
  if (!has_design()) {
    if (why) *why = "no design loaded";
    return false;
  }
  if (!check_free(why)) return false;
  if (cell < 0 || static_cast<std::size_t>(cell) >= design_->num_cells() || design_->cell(cell).removed) {
    if (why) *why = "no such cell";
    return false;
  }
  if (design_->cell(cell).fixed) {
    if (why) *why = "cell is fixed";
    return false;
  }
  undo_.emplace_back(cell, design_->cell(cell).loc);
  design_->move_cell(cell, loc);
  design_->set_placed(cell, true);
  design_->flush();  // Publishes a snapshot (one chunk with tracking on).
  return true;
}

bool Session::undo(std::string* why) {
  if (!check_free(why)) return false;
  if (undo_.empty()) {
    if (why) *why = "nothing to undo";
    return false;
  }
  const auto [cell, loc] = undo_.back();
  undo_.pop_back();
  design_->move_cell(cell, loc);
  design_->flush();
  return true;
}

}  // namespace hermes::gui
