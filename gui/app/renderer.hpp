#pragma once

// Rendering backend interface. The viewer only talks to this; OpenGL 3.3
// is the first implementation (gl_renderer.cpp). A Metal or Vulkan backend
// implements the same calls: instanced rectangles, instanced segments,
// markers, and scalar images through a colormap.
//
// All geometry goes up once per scene / detail build into GPU buffers; a
// frame only sets the view transform and issues a handful of instanced
// draws (one per visible tile row for cells), so frame cost does not
// depend on the design size.

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "gui/scene/layers.hpp"
#include "gui/scene/scene.hpp"

namespace hermes::gui {

/// World -> normalized device coordinates for the current view.
struct ViewXform {
  double cx = 0, cy = 0;     ///< View center (world DBU).
  double scale = 1;          ///< Framebuffer pixels per DBU.
  int fb_w = 1, fb_h = 1;    ///< Framebuffer size in pixels.
  Point origin;              ///< Scene origin (float coordinates are relative to it).
  double world_per_px() const { return 1.0 / scale; }
};

enum class BufferId : int { Cells, Macros, IoPins, Rows, Pins, Wires, Vias, Grid, Die, SelRects, SelWires, SelMarkers, Ghost, Count };
enum class ImageId : int { Density, Heatmap, Count };

struct DrawStyle {
  std::array<Color, LayerSet::kMaxLayers> palette{};
  std::uint32_t visible_mask = 0xffffffffu;
  float fill_alpha = 0.55f;     ///< Interior alpha of rectangles big enough to have a border.
  float border_px = 1.0f;
  float min_px = 0.0f;          ///< Rectangles are drawn at least this big.
  float grow_px = 0.0f;         ///< Inflate rectangles (selection halo).
  float line_px = 1.0f;         ///< Segment width.
  float alpha = 1.0f;           ///< Global alpha multiplier.
  bool outline_only = false;
  bool override_color = false;
  Color color;                  ///< With override_color.
};

struct ImageStyle {
  float vmin = 0, vmax = 1;
  float alpha = 0.7f;
  bool mono = false;            ///< Use `color` with alpha = value (density LOD) instead of the colormap.
  Color color;
  bool linear = false;          ///< Bilinear filtering (density LOD) instead of crisp bins.
  bool transparent_zero = true; ///< Values <= vmin draw nothing.
};

class Renderer {
 public:
  virtual ~Renderer() = default;
  virtual bool init(std::string* why) = 0;
  virtual std::string describe() const = 0;  ///< e.g. "OpenGL 4.1 Metal - 89.4 (Apple M4)"

  virtual void upload(BufferId b, std::span<const Instance> data) = 0;
  virtual std::size_t size(BufferId b) const = 0;
  virtual void upload_image(ImageId i, const ScalarImage& img) = 0;
  virtual bool has_image(ImageId i) const = 0;

  virtual void begin_frame(const ViewXform& v, const Color& background) = 0;
  /// Rectangles (and markers) [first, first + count) of buffer b.
  virtual void draw_rects(BufferId b, std::size_t first, std::size_t count, const DrawStyle& s) = 0;
  virtual void draw_segments(BufferId b, std::size_t first, std::size_t count, const DrawStyle& s) = 0;
  virtual void draw_image(ImageId i, const ImageStyle& s) = 0;
  virtual void end_frame() = 0;

  /// RGBA8 of the framebuffer, top row first.
  virtual void read_pixels(int w, int h, std::vector<std::uint8_t>& rgba) = 0;
  /// GPU time of the last finished frame (ms), or < 0 if unknown.
  virtual double gpu_ms() const = 0;
  virtual std::size_t gpu_bytes() const = 0;
};

std::unique_ptr<Renderer> make_gl_renderer();

}  // namespace hermes::gui
