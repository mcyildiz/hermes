#include "gui/scene/session.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

#include "hermes/io/design_file.hpp"
#include "hermes/io/registry.hpp"
#include "hermes/log/log.hpp"
#include "hermes/par/parallel.hpp"
#include "hermes/route/global_router.hpp"

namespace hermes::gui {

HERMES_LOG_CHANNEL(log_gui, "gui");

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
  // Same GCell grid as build_route_usage().
  BinGrid g;
  const RouteGrid& rg = d.route_grid();
  if (!rg.empty() && o.use_route_grid) {
    g = BinGrid({rg.origin.x, rg.origin.y, rg.origin.x + rg.nx * rg.tile_w, rg.origin.y + rg.ny * rg.tile_h}, rg.nx,
                rg.ny);
  } else {
    g = BinGrid(d.die(), std::max(1, o.grid_x), std::max(1, o.grid_y));
  }
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

bool Session::start_flow(const FlowOptions& opts, std::string* why) {
  if (!has_design()) {
    if (why) *why = "no design loaded";
    return false;
  }
  if (!check_free(why)) return false;
  undo_.clear();  // The flow changes everything; old undo entries would fight it.
  job_kind_ = opts.run_routing ? "flow" : "placement";
  flow_report_ = std::make_shared<FlowReport>();
  JobOptions jo;
  jo.max_threads = job_threads();
  jo.publish_interval_s = 0.2;
  job_ = start_flow_job(*design_, opts, flow_report_.get(), jo);
  return true;
}

bool Session::start_qor(std::string* why) {
  if (!has_design()) {
    if (why) *why = "no design loaded";
    return false;
  }
  if (!check_free(why)) return false;
  job_kind_ = "qor";
  qor_pending_ = std::make_shared<QorReport>();
  JobOptions jo;
  jo.max_threads = job_threads();
  auto out = qor_pending_;
  Design* d = design_.get();
  job_ = start_job(
      "qor",
      [d, out](RunControl& ctl) {
        ctl.begin_stage("qor", 0.0, 1.0);
        *out = compute_qor(*d);  // Reads the design; no writer runs meanwhile.
      },
      jo);
  return true;
}

bool Session::save(const std::string& path, std::string* why) {
  if (!has_design()) {
    if (why) *why = "no design loaded";
    return false;
  }
  if (!check_free(why)) return false;
  job_kind_ = "save";
  JobOptions jo;
  jo.max_threads = job_threads();
  Design* d = design_.get();
  job_ = start_job(
      "save " + path,
      [d, path](RunControl&) {
        const auto r = io::save_design(*d, path);
        HLOG_INFO(log_gui, "saved {} ({:.1f} MB) in {:.3f} s", path, static_cast<double>(r.file_bytes) / 1e6,
                  r.seconds);
      },
      jo);
  return true;
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
  if (st == JobState::Failed) {
    std::string msg;
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
  if ((kind == "flow" || kind == "placement") && flow_report_) {
    if (st == JobState::Done) {
      flow_report_done_ = flow_report_;
      if (flow_report_->has_qor) {
        qor_ = std::make_shared<QorReport>(flow_report_->qor);
        qor_version_ = design_ ? design_->version() : 0;
      }
      note(kind + " finished", log::Level::Info);
    }
    flow_report_.reset();
  }
  if (kind == "qor" && qor_pending_) {
    if (st == JobState::Done) {
      qor_ = qor_pending_;
      qor_version_ = design_ ? design_->version() : 0;
      note("QoR report ready", log::Level::Info);
    }
    qor_pending_.reset();
  }
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
