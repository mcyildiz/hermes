// OpenGL 3.3 core renderer (macOS: 4.1 core profile; Linux: Mesa/vendor GL).

#if defined(__APPLE__)
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "gui/app/renderer.hpp"

namespace hermes::gui {

namespace {

const char* kRectVs = R"(#version 330 core
layout(location = 0) in vec4 a_rect;
layout(location = 1) in uint a_style;
uniform vec2 u_scale;
uniform vec2 u_offset;
uniform vec2 u_px2ndc;
uniform vec4 u_palette[32];
uniform uint u_mask;
uniform float u_min_px;
uniform float u_grow_px;
uniform int u_override;
uniform vec4 u_color;
out vec4 v_color;
out vec2 v_local;
out vec2 v_size;
flat out uint v_flags;
void main() {
  uint layer = a_style & 255u;
  v_flags = a_style;
  if (((u_mask >> layer) & 1u) == 0u) {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    v_color = vec4(0.0); v_local = vec2(0.0); v_size = vec2(0.0);
    return;
  }
  vec2 corner = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
  vec2 p0, p1;
  if ((a_style & 256u) != 0u) {
    vec2 c = a_rect.xy * u_scale + u_offset;
    vec2 h = (a_rect.z + u_grow_px) * u_px2ndc;
    p0 = c - h; p1 = c + h;
  } else {
    p0 = a_rect.xy * u_scale + u_offset;
    p1 = a_rect.zw * u_scale + u_offset;
    vec2 grow = max(u_min_px * u_px2ndc - (p1 - p0), vec2(0.0)) * 0.5 + u_grow_px * u_px2ndc;
    p0 -= grow; p1 += grow;
  }
  gl_Position = vec4(mix(p0, p1, corner), 0.0, 1.0);
  v_size = (p1 - p0) / u_px2ndc;
  v_local = corner * v_size;
  v_color = u_override != 0 ? u_color : u_palette[layer];
}
)";

const char* kRectFs = R"(#version 330 core
in vec4 v_color;
in vec2 v_local;
in vec2 v_size;
flat in uint v_flags;
uniform float u_fill_alpha;
uniform float u_border_px;
uniform int u_outline_only;
uniform float u_alpha;
out vec4 o_color;
void main() {
  float d = min(min(v_local.x, v_size.x - v_local.x), min(v_local.y, v_size.y - v_local.y));
  bool marker = (v_flags & 256u) != 0u;
  bool big = !marker && min(v_size.x, v_size.y) > 3.0 * u_border_px + 1.0;
  vec4 c = v_color;
  float a;
  if (big && d < u_border_px) {
    c.rgb = min(c.rgb * 1.25 + 0.06, vec3(1.0));
    a = 1.0;
  } else {
    if (u_outline_only != 0 && big) discard;
    a = big ? u_fill_alpha : 0.92;
    if ((v_flags & 512u) != 0u && big) {  // fixed: diagonal hatch
      float s = mod(gl_FragCoord.x + gl_FragCoord.y, 9.0);
      a *= s < 2.0 ? 1.7 : 0.55;
    }
  }
  if ((v_flags & 1024u) != 0u) a *= 0.45;  // unplaced
  o_color = vec4(c.rgb, c.a * a * u_alpha);
}
)";

const char* kSegVs = R"(#version 330 core
layout(location = 0) in vec4 a_seg;
layout(location = 1) in uint a_style;
uniform vec2 u_scale;
uniform vec2 u_offset;
uniform vec2 u_px2ndc;
uniform vec4 u_palette[32];
uniform uint u_mask;
uniform float u_line_px;
uniform int u_override;
uniform vec4 u_color;
out vec4 v_color;
void main() {
  uint layer = a_style & 255u;
  if (((u_mask >> layer) & 1u) == 0u) {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    v_color = vec4(0.0);
    return;
  }
  vec2 corner = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
  vec2 a = a_seg.xy * u_scale + u_offset;
  vec2 b = a_seg.zw * u_scale + u_offset;
  vec2 dpx = (b - a) / u_px2ndc;
  float len = length(dpx);
  vec2 dir = len > 1e-6 ? dpx / len : vec2(1.0, 0.0);
  vec2 n = vec2(-dir.y, dir.x);
  float hw = 0.5 * u_line_px;
  vec2 cpx = mix(-dir * hw, dpx + dir * hw, corner.x) + n * hw * (corner.y * 2.0 - 1.0);
  gl_Position = vec4(a + cpx * u_px2ndc, 0.0, 1.0);
  v_color = u_override != 0 ? u_color : u_palette[layer];
}
)";

const char* kSegFs = R"(#version 330 core
in vec4 v_color;
uniform float u_alpha;
out vec4 o_color;
void main() { o_color = vec4(v_color.rgb, v_color.a * u_alpha); }
)";

const char* kImgVs = R"(#version 330 core
uniform vec4 u_rect;  // world rect relative to the scene origin
uniform vec2 u_scale;
uniform vec2 u_offset;
out vec2 v_uv;
void main() {
  vec2 corner = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
  vec2 p = mix(u_rect.xy, u_rect.zw, corner);
  gl_Position = vec4(p * u_scale + u_offset, 0.0, 1.0);
  v_uv = corner;
}
)";

const char* kImgFs = R"(#version 330 core
in vec2 v_uv;
uniform sampler2D u_tex;
uniform sampler2D u_lut;
uniform float u_vmin;
uniform float u_vmax;
uniform float u_alpha;
uniform int u_mono;
uniform int u_zero;
uniform vec4 u_color;
out vec4 o_color;
void main() {
  float v = texture(u_tex, v_uv).r;
  if (u_zero != 0 && v <= u_vmin) discard;
  float t = clamp((v - u_vmin) / max(1e-12, u_vmax - u_vmin), 0.0, 1.0);
  if (u_mono != 0) {
    o_color = vec4(u_color.rgb, u_alpha * (0.15 + 0.85 * t));
  } else {
    vec4 c = texture(u_lut, vec2(t, 0.5));
    o_color = vec4(c.rgb, u_alpha);
  }
}
)";

GLuint compile(GLenum type, const char* src, std::string* why) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[4096];
    glGetShaderInfoLog(s, sizeof log, nullptr, log);
    if (why) *why = std::string("shader compile failed: ") + log;
    glDeleteShader(s);
    return 0;
  }
  return s;
}

GLuint link(const char* vs, const char* fs, std::string* why) {
  GLuint v = compile(GL_VERTEX_SHADER, vs, why);
  if (!v) return 0;
  GLuint f = compile(GL_FRAGMENT_SHADER, fs, why);
  if (!f) return 0;
  GLuint p = glCreateProgram();
  glAttachShader(p, v);
  glAttachShader(p, f);
  glLinkProgram(p);
  glDeleteShader(v);
  glDeleteShader(f);
  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[4096];
    glGetProgramInfoLog(p, sizeof log, nullptr, log);
    if (why) *why = std::string("shader link failed: ") + log;
    return 0;
  }
  return p;
}

// Turbo-like colormap stops for heatmaps (dark blue -> teal -> yellow -> red).
void make_lut(std::uint8_t* out) {
  struct Stop {
    float t, r, g, b;
  } st[] = {{0.00f, 0.10f, 0.16f, 0.45f}, {0.20f, 0.13f, 0.42f, 0.86f}, {0.40f, 0.12f, 0.75f, 0.80f},
            {0.60f, 0.55f, 0.90f, 0.35f}, {0.75f, 0.98f, 0.82f, 0.22f}, {0.88f, 0.98f, 0.48f, 0.14f},
            {1.00f, 0.86f, 0.10f, 0.18f}};
  for (int i = 0; i < 256; ++i) {
    const float t = i / 255.0f;
    int k = 0;
    while (k < 5 && t > st[k + 1].t) ++k;
    const float u = (t - st[k].t) / (st[k + 1].t - st[k].t);
    auto lerp = [&](float a, float b) { return a + (b - a) * std::clamp(u, 0.0f, 1.0f); };
    out[i * 4 + 0] = static_cast<std::uint8_t>(255 * lerp(st[k].r, st[k + 1].r));
    out[i * 4 + 1] = static_cast<std::uint8_t>(255 * lerp(st[k].g, st[k + 1].g));
    out[i * 4 + 2] = static_cast<std::uint8_t>(255 * lerp(st[k].b, st[k + 1].b));
    out[i * 4 + 3] = 255;
  }
}

struct Program {
  GLuint id = 0;
  GLint scale = -1, offset = -1, px2ndc = -1, palette = -1, mask = -1, min_px = -1, grow_px = -1, override_ = -1,
        color = -1, fill_alpha = -1, border_px = -1, outline_only = -1, line_px = -1, alpha = -1, rect = -1, tex = -1,
        lut = -1, vmin = -1, vmax = -1, mono = -1, zero = -1;
  void locate() {
    auto L = [&](const char* n) { return glGetUniformLocation(id, n); };
    scale = L("u_scale");
    offset = L("u_offset");
    px2ndc = L("u_px2ndc");
    palette = L("u_palette");
    mask = L("u_mask");
    min_px = L("u_min_px");
    grow_px = L("u_grow_px");
    override_ = L("u_override");
    color = L("u_color");
    fill_alpha = L("u_fill_alpha");
    border_px = L("u_border_px");
    outline_only = L("u_outline_only");
    line_px = L("u_line_px");
    alpha = L("u_alpha");
    rect = L("u_rect");
    tex = L("u_tex");
    lut = L("u_lut");
    vmin = L("u_vmin");
    vmax = L("u_vmax");
    mono = L("u_mono");
    zero = L("u_zero");
  }
};

class GlRenderer final : public Renderer {
  struct Buf {
    GLuint vao = 0, vbo = 0;
    std::size_t count = 0, capacity = 0;
    std::size_t bound_first = SIZE_MAX;
  };
  struct Img {
    GLuint tex = 0;
    Rect region;
    bool ok = false;
    std::size_t bytes = 0;
  };
 public:
  ~GlRenderer() override {
    if (!ready_) return;
    for (auto& b : bufs_) glDeleteBuffers(1, &b.vbo);
    for (auto& b : bufs_) glDeleteVertexArrays(1, &b.vao);
    for (auto& t : imgs_) glDeleteTextures(1, &t.tex);
    glDeleteTextures(1, &lut_);
    glDeleteVertexArrays(1, &empty_vao_);
    glDeleteProgram(rect_.id);
    glDeleteProgram(seg_.id);
    glDeleteProgram(img_.id);
    glDeleteQueries(3, queries_);
  }

  bool init(std::string* why) override {
    rect_.id = link(kRectVs, kRectFs, why);
    seg_.id = link(kSegVs, kSegFs, why);
    img_.id = link(kImgVs, kImgFs, why);
    if (!rect_.id || !seg_.id || !img_.id) return false;
    rect_.locate();
    seg_.locate();
    img_.locate();
    for (auto& b : bufs_) {
      glGenVertexArrays(1, &b.vao);
      glGenBuffers(1, &b.vbo);
    }
    glGenVertexArrays(1, &empty_vao_);
    for (auto& t : imgs_) glGenTextures(1, &t.tex);
    glGenTextures(1, &lut_);
    std::uint8_t lut[256 * 4];
    make_lut(lut);
    glBindTexture(GL_TEXTURE_2D, lut_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, lut);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenQueries(3, queries_);
    const char* ver = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const char* ren = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    desc_ = std::string("OpenGL ") + (ver ? ver : "?") + " (" + (ren ? ren : "?") + ")";
    ready_ = true;
    return true;
  }

  std::string describe() const override { return desc_; }

  void upload(BufferId id, std::span<const Instance> data) override {
    Buf& b = bufs_[static_cast<int>(id)];
    glBindVertexArray(b.vao);
    glBindBuffer(GL_ARRAY_BUFFER, b.vbo);
    const std::size_t bytes = data.size() * sizeof(Instance);
    if (bytes > b.capacity || bytes < b.capacity / 4) {
      glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(std::max<std::size_t>(bytes, 64)), data.data(),
                   GL_DYNAMIC_DRAW);
      b.capacity = std::max<std::size_t>(bytes, 64);
    } else if (bytes) {
      glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(bytes), data.data());
    }
    b.count = data.size();
    b.bound_first = SIZE_MAX;
    glBindVertexArray(0);
  }

  std::size_t size(BufferId id) const override { return bufs_[static_cast<int>(id)].count; }

  void upload_image(ImageId id, const ScalarImage& img) override {
    Img& t = imgs_[static_cast<int>(id)];
    glBindTexture(GL_TEXTURE_2D, t.tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, img.w, img.h, 0, GL_RED, GL_FLOAT, img.values.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    t.region = img.region;
    t.ok = !img.values.empty();
    t.bytes = img.values.size() * 4;
  }

  bool has_image(ImageId id) const override { return imgs_[static_cast<int>(id)].ok; }

  void begin_frame(const ViewXform& v, const Color& bg) override {
    v_ = v;
    // ndc = rel * s + o, with rel = world - origin.
    sx_ = static_cast<float>(2.0 * v.scale / v.fb_w);
    sy_ = static_cast<float>(2.0 * v.scale / v.fb_h);
    ox_ = static_cast<float>((static_cast<double>(v.origin.x) - v.cx) * 2.0 * v.scale / v.fb_w);
    oy_ = static_cast<float>((static_cast<double>(v.origin.y) - v.cy) * 2.0 * v.scale / v.fb_h);
    glViewport(0, 0, v.fb_w, v.fb_h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(bg.r, bg.g, bg.b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    // GPU timing: query ring of 3, read the oldest (no stall).
    const int q = static_cast<int>(frame_ % 3);
    if (frame_ >= 3) {
      GLint avail = 0;
      glGetQueryObjectiv(queries_[q], GL_QUERY_RESULT_AVAILABLE, &avail);
      if (avail) {
        GLuint64 ns = 0;
        glGetQueryObjectui64v(queries_[q], GL_QUERY_RESULT, &ns);
        gpu_ms_ = static_cast<double>(ns) / 1e6;
      }
    }
    glBeginQuery(GL_TIME_ELAPSED, queries_[q]);
  }

  void common(const Program& p, const DrawStyle& s) {
    glUseProgram(p.id);
    glUniform2f(p.scale, sx_, sy_);
    glUniform2f(p.offset, ox_, oy_);
    glUniform2f(p.px2ndc, 2.0f / v_.fb_w, 2.0f / v_.fb_h);
    glUniform4fv(p.palette, LayerSet::kMaxLayers, &s.palette[0].r);
    glUniform1ui(p.mask, s.visible_mask);
    glUniform1i(p.override_, s.override_color ? 1 : 0);
    glUniform4f(p.color, s.color.r, s.color.g, s.color.b, s.color.a);
    glUniform1f(p.alpha, s.alpha);
  }

  void bind(Buf& b, std::size_t first) {
    glBindVertexArray(b.vao);
    if (b.bound_first == first) return;
    glBindBuffer(GL_ARRAY_BUFFER, b.vbo);
    const auto off = static_cast<std::uintptr_t>(first * sizeof(Instance));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(Instance), reinterpret_cast<const void*>(off));
    glVertexAttribDivisor(0, 1);
    glEnableVertexAttribArray(1);
    glVertexAttribIPointer(1, 1, GL_UNSIGNED_INT, sizeof(Instance), reinterpret_cast<const void*>(off + 16));
    glVertexAttribDivisor(1, 1);
    b.bound_first = first;
  }

  void draw_rects(BufferId id, std::size_t first, std::size_t count, const DrawStyle& s) override {
    Buf& b = bufs_[static_cast<int>(id)];
    if (first >= b.count) return;
    count = std::min(count, b.count - first);
    if (!count) return;
    common(rect_, s);
    glUniform1f(rect_.min_px, s.min_px);
    glUniform1f(rect_.grow_px, s.grow_px);
    glUniform1f(rect_.fill_alpha, s.fill_alpha);
    glUniform1f(rect_.border_px, s.border_px);
    glUniform1i(rect_.outline_only, s.outline_only ? 1 : 0);
    bind(b, first);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(count));
    instances_ += count;
    ++draws_;
  }

  void draw_segments(BufferId id, std::size_t first, std::size_t count, const DrawStyle& s) override {
    Buf& b = bufs_[static_cast<int>(id)];
    if (first >= b.count) return;
    count = std::min(count, b.count - first);
    if (!count) return;
    common(seg_, s);
    glUniform1f(seg_.line_px, s.line_px);
    bind(b, first);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(count));
    instances_ += count;
    ++draws_;
  }

  void draw_image(ImageId id, const ImageStyle& s) override {
    Img& t = imgs_[static_cast<int>(id)];
    if (!t.ok) return;
    glUseProgram(img_.id);
    glUniform2f(img_.scale, sx_, sy_);
    glUniform2f(img_.offset, ox_, oy_);
    glUniform4f(img_.rect, static_cast<float>(t.region.xlo - v_.origin.x), static_cast<float>(t.region.ylo - v_.origin.y),
                static_cast<float>(t.region.xhi - v_.origin.x), static_cast<float>(t.region.yhi - v_.origin.y));
    glUniform1f(img_.vmin, s.vmin);
    glUniform1f(img_.vmax, s.vmax);
    glUniform1f(img_.alpha, s.alpha);
    glUniform1i(img_.mono, s.mono ? 1 : 0);
    glUniform1i(img_.zero, s.transparent_zero ? 1 : 0);
    glUniform4f(img_.color, s.color.r, s.color.g, s.color.b, s.color.a);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t.tex);
    const GLint f = s.linear ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
    glUniform1i(img_.tex, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, lut_);
    glUniform1i(img_.lut, 1);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(empty_vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    ++draws_;
  }

  void end_frame() override {
    glEndQuery(GL_TIME_ELAPSED);
    ++frame_;
    glBindVertexArray(0);
    glUseProgram(0);
    last_instances_ = instances_;
    last_draws_ = draws_;
    instances_ = draws_ = 0;
  }

  void read_pixels(int w, int h, std::vector<std::uint8_t>& rgba) override {
    rgba.resize(static_cast<std::size_t>(w) * h * 4);
    std::vector<std::uint8_t> tmp(rgba.size());
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, tmp.data());
    const std::size_t row = static_cast<std::size_t>(w) * 4;
    for (int y = 0; y < h; ++y)
      std::memcpy(rgba.data() + static_cast<std::size_t>(y) * row, tmp.data() + static_cast<std::size_t>(h - 1 - y) * row,
                  row);
    for (std::size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
  }

  double gpu_ms() const override { return gpu_ms_; }
  std::size_t gpu_bytes() const override {
    std::size_t b = 0;
    for (const auto& x : bufs_) b += x.capacity;
    for (const auto& x : imgs_) b += x.bytes;
    return b;
  }

 private:
  bool ready_ = false;
  std::string desc_;
  Program rect_, seg_, img_;
  Buf bufs_[static_cast<int>(BufferId::Count)];
  Img imgs_[static_cast<int>(ImageId::Count)];
  GLuint lut_ = 0, empty_vao_ = 0;
  GLuint queries_[3] = {0, 0, 0};
  std::uint64_t frame_ = 0;
  double gpu_ms_ = -1;
  ViewXform v_;
  float sx_ = 1, sy_ = 1, ox_ = 0, oy_ = 0;
  std::size_t instances_ = 0, draws_ = 0, last_instances_ = 0, last_draws_ = 0;
};

}  // namespace

std::unique_ptr<Renderer> make_gl_renderer() { return std::make_unique<GlRenderer>(); }

}  // namespace hermes::gui
