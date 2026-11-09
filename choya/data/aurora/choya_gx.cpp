// Choya Wii Converter: adaptador entre aurora y el motor GLES de Choya. Ver choya_gx.hpp y choya_gx_core.hpp.
#include "choya_gx.hpp"

#include "choya_gx_core.hpp"

#include "../gfx/texture.hpp"
#include "../gx/gx.hpp"
#include "../gx/shader_info.hpp"

#include <algorithm>
#include <atomic>
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
  d.numTexGens = s.numTexGens;
  d.sampledTexCoords = static_cast<uint8_t>(info.sampledTexCoords.to_ulong());

  // TEV.
  d.tevStageCount = static_cast<uint8_t>(std::min<u32>(s.tevStageCount, 16));
  for (u32 i = 0; i < d.tevStageCount; ++i) {
    const auto& st = s.tevStages[i];
    auto& o = d.stages[i];
    o.ca = st.colorPass.a; o.cb = st.colorPass.b; o.cc = st.colorPass.c; o.cd = st.colorPass.d;
    o.cop = st.colorOp.op; o.cbias = st.colorOp.bias; o.cscale = st.colorOp.scale;
    o.cclamp = st.colorOp.clamp ? 1 : 0; o.cout = st.colorOp.outReg;
    o.aa = st.alphaPass.a; o.ab = st.alphaPass.b; o.ac = st.alphaPass.c; o.ad = st.alphaPass.d;
    o.aop = st.alphaOp.op; o.abias = st.alphaOp.bias; o.ascale = st.alphaOp.scale;
    o.aclamp = st.alphaOp.clamp ? 1 : 0; o.aout = st.alphaOp.outReg;
    o.kcSel = st.kcSel; o.kaSel = st.kaSel;
    o.channel = static_cast<uint8_t>(st.channelId);
    o.swapRas = st.tevSwapRas; o.swapTex = st.tevSwapTex;
    o.texCoord = 0xFF;
    o.texMap = 0xFF;
    const auto dep = gx::tev_stage_texture_dependency(s, i);
    if (dep.canSampleTexture && dep.texMapId < 8 && dep.texCoordId < 8 && info.sampledTextures.test(dep.texMapId)) {
      o.texCoord = static_cast<uint8_t>(dep.texCoordId);
      o.texMap = static_cast<uint8_t>(dep.texMapId);
    }
  }
  for (size_t i = 0; i < 4; ++i) {
    const auto& sw = s.tevSwapTable[i];
    d.swap[i][0] = sw.red; d.swap[i][1] = sw.green; d.swap[i][2] = sw.blue; d.swap[i][3] = sw.alpha;
  }
  d.alphaComp0 = s.alphaCompare.comp0;
  d.alphaRef0 = static_cast<uint8_t>(s.alphaCompare.ref0);
  d.alphaOp = s.alphaCompare.op;
  d.alphaComp1 = s.alphaCompare.comp1;
  d.alphaRef1 = static_cast<uint8_t>(s.alphaCompare.ref1);
  for (u32 i = 0; i < 8; ++i) {
    const auto& tcg = s.tcgs[i];
    auto& tg = d.texgens[i];
    tg.type = tcg.type;
    tg.src = tcg.src;
    tg.mtxSlot = 0xFF;
    if (info.indexAttr.test(GX_VA_TEX0MTXIDX + i)) {
      tg.mtxSlot = 0xFE;
    } else if (tcg.mtx != GX_IDENTITY) {
      const u32 slot = static_cast<u32>(tcg.mtx) / 3;
      const u32 idx = slot < gx::MaxPostexMtx ? info.matrixLayout.postexRemap[slot] : slot;
      if (idx < gx::MaxPostexMtx) tg.mtxSlot = static_cast<uint8_t>(idx);
    }
  }

  // Desplazamientos de los uniformes, en el mismo orden y tamaños que gx::build_uniform.
  const auto& gs = gx::g_gxState;
  size_t b = 32 + 4 * gx::MaxIndexAttr;  // vtx_start, pnmtx, 4 flotantes, relleno, array_start[]
  auto word = [&]() { return static_cast<uint16_t>(b / 4); };
  if (s.lineMode != 0) b += 16;
  d.offProj = word();
  b += sizeof(Mat4x4<float>);
  d.offPostex = word();
  b += sizeof(Mat3x4<float>) * (info.matrixLayout.postexCount + info.matrixLayout.nrmCount);
  const auto loads = info.loadsTevRegRgb | info.loadsTevRegAlpha;
  for (size_t i = 0; i < 4; ++i) {
    if (loads.test(i)) {
      d.offTevReg[i] = word();
      b += sizeof(gs.colorRegs[0]);
    }
  }
  if (info.lightingEnabled) b += sizeof(gs.preparedLights) + 4 * sizeof(u32);
  for (size_t i = 0; i < 2; ++i) {
    if (!info.sampledColorChannels.test(i)) continue;
    const auto& cc = gs.colorChannelConfig[i];
    const auto& cca = gs.colorChannelConfig[i + GX_ALPHA0];
    if (cc.lightingEnabled && cc.ambSrc == GX_SRC_REG) b += sizeof(gs.colorChannelState[0].ambColor);
    if (cc.matSrc == GX_SRC_REG) {
      d.offChanMat[i] = word();
      b += sizeof(gs.colorChannelState[0].matColor);
    }
    if (cca.lightingEnabled && cca.ambSrc == GX_SRC_REG) b += sizeof(gs.colorChannelState[0].ambColor);
    if (cca.matSrc == GX_SRC_REG) {
      d.offChanMatA[i] = word();
      b += sizeof(gs.colorChannelState[0].matColor);
    }
  }
  for (size_t i = 0; i < 4; ++i) {
    if (info.sampledKColors.test(i)) {
      d.offKColor[i] = word();
      b += sizeof(gs.kcolors[0]);
    }
  }
  if (info.usesPTTexMtx.any()) b += sizeof(gs.ptTexMtxs);
  if (info.usesFog) b += sizeof(gx::Fog);
  if (info.usedIndTexMtxs.any()) b += gx::MaxIndTexMtxs * 2 * sizeof(Vec4<s32>);
  for (size_t i = 0; i < 8; ++i) {
    if (info.sampledTexCoords.test(i)) {
      d.offTexScale[i] = word();
      b += sizeof(Vec4<float>);
    }
  }
  for (size_t i = 0; i < 8; ++i) {
    if (info.sampledTextures.test(i)) {
      d.offTexSize[i] = word();
      b += sizeof(Vec4<float>);
    }
  }

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

uint8_t wrap_mode(wgpu::AddressMode m) {
  return m == wgpu::AddressMode::Repeat ? 1 : m == wgpu::AddressMode::MirrorRepeat ? 2 : 0;
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
    // Los volcados llevan las texturas: hay que guardarlas desde que se crean.
    const char* dumps = std::getenv("CHOYA_GX_VOLCADO");
    core::set_keep_textures(dumps != nullptr && *dumps != '\0');
    return true;
  }();
  return on;
}

void texture_uploaded(const gfx::TextureRef& ref, const uint8_t* data, size_t size) noexcept {
  if (!enabled() || ref.format != wgpu::TextureFormat::RGBA8Unorm) return;  // índices de paleta: todavía no
  core::texture_data(reinterpret_cast<uintptr_t>(&ref), ref.size.width, ref.size.height, ref.mipCount, data, size);
}

void texture_released(const void* ref) noexcept {
  if (!enabled()) return;
  core::texture_released(reinterpret_cast<uintptr_t>(ref));
}

void register_texture_bind_group(gfx::BindGroupRef ref, const gx::ShaderInfo& info) noexcept {
  if (!enabled()) return;
  core::BindDesc d{};
  for (u32 i = 0; i < gx::MaxTextures && i < 8; ++i) {
    const auto& tex = gx::g_gxState.textures[i];
    if (!tex || !(info.sampledTextures[i] || info.sampledIndTextures[i])) continue;
    d.tex[i] = reinterpret_cast<uintptr_t>(tex.ref.get());
    const auto sd = tex.get_descriptor();
    auto& o = d.samp[i];
    o.wrapS = wrap_mode(sd.addressModeU);
    o.wrapT = wrap_mode(sd.addressModeV);
    o.minFilter = sd.minFilter == wgpu::FilterMode::Linear ? 1 : 0;
    o.magFilter = sd.magFilter == wgpu::FilterMode::Linear ? 1 : 0;
    o.mipFilter = sd.mipmapFilter == wgpu::MipmapFilterMode::Linear    ? 2
                  : sd.mipmapFilter == wgpu::MipmapFilterMode::Nearest ? 1
                                                                       : 0;
    o.minLod = sd.lodMinClamp;
    o.maxLod = sd.lodMaxClamp;
  }
  core::register_bind_group(ref, d);
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
                  float clearDepthValue, bool isDisplayCopy, const gfx::ClipRect& copyRect) noexcept {
  const core::Rect rect = isDisplayCopy ? core::Rect{copyRect.x, copyRect.y, copyRect.width, copyRect.height}
                                        : core::Rect{0, 0, 0, 0};
  core::capture_pass(width, height, clearColor, clearDepth, clearRgba, clearDepthValue, isDisplayCopy, rect);
}

void capture_copy(const gfx::TextureRef* tex, const gfx::ClipRect& rect, uint32_t fmt, bool opaque) noexcept {
  if (tex == nullptr) return;
  core::capture_copy(reinterpret_cast<uintptr_t>(tex), core::Rect{rect.x, rect.y, rect.width, rect.height},
                     tex->size.width, tex->size.height, fmt, opaque);
}

void capture_viewport(const gfx::Viewport& vp) noexcept {
  core::capture_viewport(core::Viewport{vp.left, vp.top, vp.width, vp.height, vp.znear, vp.zfar});
}

void capture_scissor(const gfx::ClipRect& rect) noexcept {
  core::capture_scissor(core::Rect{rect.x, rect.y, rect.width, rect.height});
}

void capture_gx_draw(const gx::DrawData& draw) noexcept {
  core::capture_draw(draw.pipeline, draw.idxRange.offset, draw.indexCount, draw.instanceCount,
                     draw.uniformRange.offset, draw.dstAlpha, draw.bindGroups.textureBindGroup);
}

void capture_clear(const gfx::clear::DrawData& draw) noexcept {
  const float rgba[4] = {static_cast<float>(draw.color.r), static_cast<float>(draw.color.g),
                         static_cast<float>(draw.color.b), static_cast<float>(draw.color.a)};
  core::capture_clear(draw.pipeline, rgba, draw.depth, draw.useScissor,
                      core::Rect{draw.scissor.x, draw.scissor.y, draw.scissor.width, draw.scissor.height});
}

void capture_end(bool present) noexcept { core::capture_end(present); }

void capture_discard() noexcept { core::capture_discard(); }

} // namespace aurora::choya

// Volcado a pedido (MENU + R1 en modo diagnóstico): el siguiente cuadro dibujado va a cuadro-manual-<n>.bin.
static std::atomic<bool> g_choyaDumpRequest{false};
extern "C" void choya_gx_request_dump() { g_choyaDumpRequest.store(true); }

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
    if (g_choyaDumpRequest.exchange(false)) {
      char path[64];
      std::snprintf(path, sizeof(path), "cuadro-manual-%lu.bin", frames);
      char line[160];
      std::snprintf(line, sizeof(line), "motor: volcado a pedido del cuadro %lu en %s: %s", frames, path,
                    core::dump_last_frame(path) ? "listo" : "falló (solo en modo diagnóstico)");
      aurora::choya::log_to_runtime(line);
    }
  }
  return drawn;
}

// Aviso de liberación de texturas (declarado en gfx/texture.hpp por el parche de Choya).
aurora::gfx::TextureRef::~TextureRef() { aurora::choya::texture_released(this); }
