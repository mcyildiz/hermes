#include "gui/scene/worker.hpp"

namespace hermes::gui {

SceneWorker::SceneWorker(std::function<void()> on_ready) : on_ready_(std::move(on_ready)) {
  th_ = std::thread([this] { run(); });
}

SceneWorker::~SceneWorker() {
  {
    std::lock_guard lk(mu_);
    stop_ = true;
  }
  cv_.notify_all();
  th_.join();
}

void SceneWorker::request_scene(std::shared_ptr<const DesignSnapshot> snap, const SceneOptions& o) {
  {
    std::lock_guard lk(mu_);
    requested_version_ = snap ? snap->version() : 0;
    want_snap_ = std::move(snap);
    want_scene_opts_ = o;
  }
  cv_.notify_all();
}

void SceneWorker::request_detail(std::shared_ptr<const Scene> scene, std::shared_ptr<const TechInfo> tech,
                                 const DetailOptions& o) {
  {
    std::lock_guard lk(mu_);
    want_detail_scene_ = std::move(scene);
    want_tech_ = std::move(tech);
    want_detail_opts_ = o;
    want_detail_ = true;
  }
  cv_.notify_all();
}

void SceneWorker::reset() {
  std::lock_guard lk(mu_);
  ++epoch_;
  want_snap_.reset();
  want_detail_ = false;
  want_detail_scene_.reset();
  scene_.reset();
  detail_.reset();
  requested_version_ = 0;
}

std::shared_ptr<const Scene> SceneWorker::scene() const {
  std::lock_guard lk(mu_);
  return scene_;
}
std::shared_ptr<const Detail> SceneWorker::detail() const {
  std::lock_guard lk(mu_);
  return detail_;
}
bool SceneWorker::idle() const {
  std::lock_guard lk(mu_);
  return working_ == 0 && !want_snap_ && !want_detail_;
}
std::uint64_t SceneWorker::requested_version() const {
  std::lock_guard lk(mu_);
  return requested_version_;
}

void SceneWorker::run() {
  std::unique_lock lk(mu_);
  for (;;) {
    cv_.wait(lk, [&] { return stop_ || want_snap_ || want_detail_; });
    if (stop_) return;
    const std::uint64_t epoch = epoch_;
    // Scenes first: a detail of a stale scene is wasted work.
    if (want_snap_) {
      auto snap = std::move(want_snap_);
      want_snap_.reset();
      const SceneOptions o = want_scene_opts_;
      ++working_;
      lk.unlock();
      std::shared_ptr<const Scene> sc = build_scene(std::move(snap), o);
      lk.lock();
      --working_;
      if (epoch == epoch_) scene_ = std::move(sc);
    } else if (want_detail_) {
      auto sc = std::move(want_detail_scene_);
      auto tech = std::move(want_tech_);
      const DetailOptions o = want_detail_opts_;
      want_detail_ = false;
      ++working_;
      lk.unlock();
      std::shared_ptr<const Detail> d;
      if (sc && tech) d = build_detail(*sc, *tech, o);
      lk.lock();
      --working_;
      if (epoch == epoch_ && d) detail_ = std::move(d);
    }
    if (on_ready_) {
      lk.unlock();
      on_ready_();
      lk.lock();
    }
  }
}

}  // namespace hermes::gui
