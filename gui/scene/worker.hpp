#pragma once

// Background scene builder: turns snapshots into Scenes and view windows
// into Details on its own thread, latest request wins. The UI thread posts
// requests and picks up results without ever waiting.

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "gui/scene/scene.hpp"

namespace hermes::gui {

class SceneWorker {
 public:
  /// `on_ready` runs on the worker thread after each result (e.g. to wake
  /// the UI's event loop).
  explicit SceneWorker(std::function<void()> on_ready = {});
  ~SceneWorker();
  SceneWorker(const SceneWorker&) = delete;
  SceneWorker& operator=(const SceneWorker&) = delete;

  /// Builds a scene for `snap` (replaces a pending scene request).
  void request_scene(std::shared_ptr<const DesignSnapshot> snap, const SceneOptions& o);
  /// Builds detail for a scene (replaces a pending detail request).
  void request_detail(std::shared_ptr<const Scene> scene, std::shared_ptr<const TechInfo> tech,
                      const DetailOptions& o);
  /// Drops pending requests and the results (a new design was opened).
  void reset();

  std::shared_ptr<const Scene> scene() const;
  std::shared_ptr<const Detail> detail() const;
  bool idle() const;
  /// Version of the last snapshot a scene was requested for.
  std::uint64_t requested_version() const;

 private:
  void run();

  std::function<void()> on_ready_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  bool stop_ = false;
  int working_ = 0;
  // Pending requests.
  std::shared_ptr<const DesignSnapshot> want_snap_;
  SceneOptions want_scene_opts_;
  std::shared_ptr<const Scene> want_detail_scene_;
  std::shared_ptr<const TechInfo> want_tech_;
  DetailOptions want_detail_opts_;
  bool want_detail_ = false;
  std::uint64_t requested_version_ = 0;
  std::uint64_t epoch_ = 0;  // bumped by reset(): results of older work are dropped
  // Results.
  std::shared_ptr<const Scene> scene_;
  std::shared_ptr<const Detail> detail_;
  std::thread th_;
};

}  // namespace hermes::gui
