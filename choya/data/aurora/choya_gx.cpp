// Choya Wii Converter: adaptador entre aurora y el motor GLES de Choya. Ver choya_gx.hpp y choya_gx_core.hpp.
#include "choya_gx.hpp"

#include "choya_gx_core.hpp"

#include "../gx/gx.hpp"
#include "../gx/shader_info.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Las da la pantalla de Choya del runtime (choya_gles.cpp). Débiles: otros ejecutables que enlacen aurora sin
// la pantalla de Choya siguen enlazando, y el motor simplemente no arranca.
extern "C" __attribute__((weak)) void* choya_gl_proc(const char* name);
extern "C" __attribute__((weak)) void choya_gles_log(const char* line);

namespace aurora::choya {
namespace core = ::choya::gx;
namespace {

core::PipelineDesc describe(const gx::PipelineConfig& c) {
  core::PipelineDesc d{};
  const auto& s = c.shaderConfig;
  static_assert(gx::MaxVtxAttr <= 26, "PipelineDesc.attrs se quedó corto");
  for (size_t i = 0; i < gx::MaxVtxAttr; ++i) {
    const auto& a = s.attrs[i];
    d.attrs[i] = {a.attrType, a.cnt, a.compType, a.offset, a.stride, a.frac, static_cast<uint8_t>(a.le ? 1 : 0), 0};
  }
  for (size_t i = 0; i < 4; ++i) {
    const auto& cc = s.colorChannels[i];
    d.chans[i] = {static_cast<uint8_t>(cc.matSrc), static_cast<uint8_t>(cc.ambSrc),
                  static_cast<uint8_t>(cc.lightingEnabled ? 1 : 0), 0};
  }
  const auto info = gx::build_shader_info(s);
  d.vtxStride = s.vtxStride;
  d.lineMode = s.lineMode;
  d.sampledChannels = static_cast<uint8_t>(info.sampledColorChannels.to_ulong());
  d.postexCount = info.matrixLayout.postexCount;
  d.nrmCount = info.matrixLayout.nrmCount;
  d.tevRegLoads = static_cast<uint8_t>((info.loadsTevRegRgb | info.loadsTevRegAlpha).count());
  d.lightingEnabled = info.lightingEnabled ? 1 : 0;
  d.depthCompare = c.depthCompare;
  d.depthUpdate = c.depthUpdate;
  d.colorUpdate = c.colorUpdate;
  d.alphaUpdate = c.alphaUpdate;
  d.depthFunc = static_cast<uint8_t>(c.depthFunc);
  d.cullMode = static_cast<uint8_t>(c.cullMode);
  d.blendMode = static_cast<uint8_t>(c.blendMode);
  d.blendSrc = static_cast<uint8_t>(c.blendFacSrc);
  d.blendDst = static_cast<uint8_t>(c.blendFacDst);
  d.blendOp = static_cast<uint8_t>(c.blendOp);
  d.pixelFmt = static_cast<uint8_t>(c.pixelFmt);
  return d;
}

// CHOYA_GX_VOLCADO="600,1800": vuelca esos cuadros a cuadro-<n>.bin para revisarlos en la PC.
std::vector<unsigned long> dump_frames() {
  std::vector<unsigned long> out;
  if (const char* v = std::getenv("CHOYA_GX_VOLCADO")) {
    std::string s(v);
    size_t pos = 0;
    while (pos < s.size()) {
      const size_t comma = s.find(',', pos);
      const std::string part = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
      if (!part.empty()) out.push_back(std::strtoul(part.c_str(), nullptr, 10));
      if (comma == std::string::npos) break;
      pos = comma + 1;
    }
  }
  return out;
}

void log_to_runtime(const char* line) {
  if (choya_gles_log != nullptr) {
    choya_gles_log(line);
  } else {
    std::fprintf(stderr, "[choya-gx] %s\n", line);
  }
}

} // namespace

bool enabled() noexcept {
  static const bool on = [] {
    const char* v = std::getenv("CHOYA_GLES");
    if (v == nullptr || std::strcmp(v, "1") != 0) return false;
    core::set_reversed_z(gx::UseReversedZ);
    core::set_logger(log_to_runtime);
    return true;
  }();
  return on;
}

void register_gx_pipeline(gfx::PipelineRef ref, const gx::PipelineConfig& config) noexcept {
  if (!enabled() || core::has_pipeline(ref)) return;
  core::register_pipeline(ref, describe(config));
}

void register_clear_pipeline(gfx::PipelineRef ref, const gfx::clear::PipelineConfig& config) noexcept {
  if (!enabled() || core::has_clear(ref)) return;
  core::register_clear(ref, core::ClearDesc{static_cast<uint8_t>(config.clearColor ? 1 : 0),
                                           static_cast<uint8_t>(config.clearAlpha ? 1 : 0),
                                           static_cast<uint8_t>(config.clearDepth ? 1 : 0), 0});
}

void capture_begin(const uint8_t* verts, size_t vertsSize, const uint8_t* uniforms, size_t uniformsSize,
                   const uint8_t* indices, size_t indicesSize, const uint8_t* storage, size_t storageSize) noexcept {
  if (!enabled()) return;
  core::capture_begin(verts, vertsSize, uniforms, uniformsSize, indices, indicesSize, storage, storageSize);
}

void capture_pass(uint32_t width, uint32_t height, bool clearColor, bool clearDepth, const float clearRgba[4],
                  float clearDepthValue, bool isDisplayCopy) noexcept {
  core::capture_pass(width, height, clearColor, clearDepth, clearRgba, clearDepthValue, isDisplayCopy);
}

void capture_viewport(const gfx::Viewport& vp) noexcept {
  core::capture_viewport(core::Viewport{vp.left, vp.top, vp.width, vp.height, vp.znear, vp.zfar});
}

void capture_scissor(const gfx::ClipRect& rect) noexcept {
  core::capture_scissor(core::Rect{rect.x, rect.y, rect.width, rect.height});
}

void capture_gx_draw(const gx::DrawData& draw) noexcept {
  core::capture_draw(draw.pipeline, draw.idxRange.offset, draw.indexCount, draw.instanceCount,
                     draw.uniformRange.offset, draw.dstAlpha);
}

void capture_clear(const gfx::clear::DrawData& draw) noexcept {
  const float rgba[4] = {static_cast<float>(draw.color.r), static_cast<float>(draw.color.g),
                         static_cast<float>(draw.color.b), static_cast<float>(draw.color.a)};
  core::capture_clear(draw.pipeline, rgba, draw.depth, draw.useScissor,
                      core::Rect{draw.scissor.x, draw.scissor.y, draw.scissor.width, draw.scissor.height});
}

void capture_end() noexcept { core::capture_end(); }

void capture_discard() noexcept { core::capture_discard(); }

} // namespace aurora::choya

extern "C" bool choya_gx_draw_frame(int screenWidth, int screenHeight) {
  namespace core = ::choya::gx;
  if (choya_gl_proc == nullptr) return false;
  static bool configured = false;
  static std::vector<unsigned long> dumps;
  static unsigned long frames = 0;
  if (!configured) {
    configured = true;
    core::set_gl_loader(choya_gl_proc);
    core::set_logger(aurora::choya::log_to_runtime);
    dumps = aurora::choya::dump_frames();
  }
  const bool drawn = core::draw_frame(screenWidth, screenHeight);
  if (drawn) {
    ++frames;
    for (unsigned long n : dumps) {
      if (n == frames) {
        char path[64];
        std::snprintf(path, sizeof(path), "cuadro-%lu.bin", n);
        char line[128];
        std::snprintf(line, sizeof(line), "motor: volcado del cuadro %lu en %s: %s", n, path,
                      core::dump_last_frame(path) ? "listo" : "falló");
        aurora::choya::log_to_runtime(line);
      }
    }
  }
  return drawn;
}
