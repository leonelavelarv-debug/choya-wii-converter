// Choya Wii Converter: motor GLES de Choya dentro de aurora (paso 2a). Ver choya_gx.hpp.
//
// Cómo se dibuja un cuadro de aurora en OpenGL ES 3.2 (Mali-G31):
//  - Los vértices crudos del GX (vbuf) y los arreglos indexados (abuf) se suben como texturas R32UI de 4096 de
//    ancho: el driver de la Mali no deja leer buffers de almacenamiento desde el vertex shader
//    (GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS = 0), así que se leen con texelFetch.
//  - Los uniformes de cada dibujo se leen de un UBO enlazado en el desplazamiento del dibujo, con el mismo orden
//    de campos que escribe aurora (gx/shader_info.cpp, build_uniform).
//  - Los índices (uint16, ya en triángulos) van a un buffer de elementos; gl_VertexID es el índice del vértice.
//  - Cada ShaderConfig distinto genera un programa GLSL ES 3.20 propio de Choya.
#include "choya_gx.hpp"

#include "../gx/gx.hpp"
#include "../gx/shader_info.hpp"

#include <absl/container/flat_hash_map.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

// Las da la pantalla de Choya del runtime (choya_gles.cpp). Débiles: otros ejecutables que enlacen aurora sin
// la pantalla de Choya siguen enlazando, y el motor simplemente no arranca.
extern "C" __attribute__((weak)) void* choya_gl_proc(const char* name);
extern "C" __attribute__((weak)) void choya_gles_log(const char* line);

namespace aurora::choya {
namespace {

void log_line(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void log_line(const char* fmt, ...) {
  char line[1024];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  if (choya_gles_log != nullptr) choya_gles_log(line);
}

// --- OpenGL ES 3.2: solo lo que usa el motor ---------------------------------------------------------------
using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLboolean = unsigned char;
using GLbitfield = unsigned int;
using GLfloat = float;
using GLchar = char;
using GLintptr = std::intptr_t;
using GLsizeiptr = std::intptr_t;

constexpr GLenum kVertexShader = 0x8B31, kFragmentShader = 0x8B30, kCompileStatus = 0x8B81, kLinkStatus = 0x8B82,
                 kInfoLogLength = 0x8B84, kUniformBuffer = 0x8A11, kElementArrayBuffer = 0x8893, kStreamDraw = 0x88E0, kTexture2D = 0x0DE1, kR32ui = 0x8236,
                 kRedInteger = 0x8D94, kUnsignedInt = 0x1405, kUnsignedShort = 0x1403, kTriangles = 0x0004,
                 kTextureMinFilter = 0x2801, kTextureMagFilter = 0x2800, kNearest = 0x2600, kTexture0 = 0x84C0,
                 kDepthTest = 0x0B71, kBlend = 0x0BE2, kCullFace = 0x0B44, kScissorTest = 0x0C11,
                 kColorBufferBit = 0x4000, kDepthBufferBit = 0x0100, kNever = 0x0200, kLess = 0x0201,
                 kEqual = 0x0202, kLequal = 0x0203, kGreater = 0x0204, kNotequal = 0x0205, kGequal = 0x0206,
                 kAlways = 0x0207, kZero = 0, kOne = 1, kSrcColor = 0x0300, kOneMinusSrcColor = 0x0301,
                 kSrcAlpha = 0x0302, kOneMinusSrcAlpha = 0x0303, kDstAlpha = 0x0304, kOneMinusDstAlpha = 0x0305,
                 kDstColor = 0x0306, kOneMinusDstColor = 0x0307, kConstantAlpha = 0x8003, kFuncAdd = 0x8006,
                 kFuncReverseSubtract = 0x800B, kMax = 0x8008, kUnpackAlignment = 0x0CF5,
                 kUniformBufferOffsetAlignment = 0x8A34, kInvalidIndex = 0xFFFFFFFFu;

struct Gl {
  GLuint (*CreateShader)(GLenum);
  void (*ShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*);
  void (*CompileShader)(GLuint);
  void (*GetShaderiv)(GLuint, GLenum, GLint*);
  void (*GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
  void (*DeleteShader)(GLuint);
  GLuint (*CreateProgram)();
  void (*AttachShader)(GLuint, GLuint);
  void (*LinkProgram)(GLuint);
  void (*GetProgramiv)(GLuint, GLenum, GLint*);
  void (*GetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
  void (*UseProgram)(GLuint);
  GLint (*GetUniformLocation)(GLuint, const GLchar*);
  void (*Uniform1i)(GLint, GLint);
  GLuint (*GetUniformBlockIndex)(GLuint, const GLchar*);
  void (*UniformBlockBinding)(GLuint, GLuint, GLuint);
  void (*GenBuffers)(GLsizei, GLuint*);
  void (*BindBuffer)(GLenum, GLuint);
  void (*BufferData)(GLenum, GLsizeiptr, const void*, GLenum);
  void (*BufferSubData)(GLenum, GLintptr, GLsizeiptr, const void*);
  void (*BindBufferRange)(GLenum, GLuint, GLuint, GLintptr, GLsizeiptr);
  void (*GenTextures)(GLsizei, GLuint*);
  void (*DeleteTextures)(GLsizei, const GLuint*);
  void (*BindTexture)(GLenum, GLuint);
  void (*TexStorage2D)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
  void (*TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*);
  void (*TexParameteri)(GLenum, GLenum, GLint);
  void (*ActiveTexture)(GLenum);
  void (*PixelStorei)(GLenum, GLint);
  void (*GenVertexArrays)(GLsizei, GLuint*);
  void (*BindVertexArray)(GLuint);
  void (*DrawElements)(GLenum, GLsizei, GLenum, const void*);
  void (*Viewport)(GLint, GLint, GLsizei, GLsizei);
  void (*Scissor)(GLint, GLint, GLsizei, GLsizei);
  void (*Enable)(GLenum);
  void (*Disable)(GLenum);
  void (*DepthFunc)(GLenum);
  void (*DepthMask)(GLboolean);
  void (*DepthRangef)(GLfloat, GLfloat);
  void (*ColorMask)(GLboolean, GLboolean, GLboolean, GLboolean);
  void (*BlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum);
  void (*BlendEquationSeparate)(GLenum, GLenum);
  void (*BlendColor)(GLfloat, GLfloat, GLfloat, GLfloat);
  void (*ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
  void (*ClearDepthf)(GLfloat);
  void (*Clear)(GLbitfield);
  GLenum (*GetError)();
  void (*GetIntegerv)(GLenum, GLint*);
};
Gl gl{};

template <typename T>
bool load(T& fn, const char* name) {
  fn = choya_gl_proc != nullptr ? reinterpret_cast<T>(choya_gl_proc(name)) : nullptr;
  if (fn == nullptr) {
    log_line("motor: falta la función GL %s", name);
  }
  return fn != nullptr;
}

bool load_gl() {
  bool ok = true;
#define CHOYA_GL(name) ok &= load(gl.name, "gl" #name)
  CHOYA_GL(CreateShader);
  CHOYA_GL(ShaderSource);
  CHOYA_GL(CompileShader);
  CHOYA_GL(GetShaderiv);
  CHOYA_GL(GetShaderInfoLog);
  CHOYA_GL(DeleteShader);
  CHOYA_GL(CreateProgram);
  CHOYA_GL(AttachShader);
  CHOYA_GL(LinkProgram);
  CHOYA_GL(GetProgramiv);
  CHOYA_GL(GetProgramInfoLog);
  CHOYA_GL(UseProgram);
  CHOYA_GL(GetUniformLocation);
  CHOYA_GL(Uniform1i);
  CHOYA_GL(GetUniformBlockIndex);
  CHOYA_GL(UniformBlockBinding);
  CHOYA_GL(GenBuffers);
  CHOYA_GL(BindBuffer);
  CHOYA_GL(BufferData);
  CHOYA_GL(BufferSubData);
  CHOYA_GL(BindBufferRange);
  CHOYA_GL(GenTextures);
  CHOYA_GL(DeleteTextures);
  CHOYA_GL(BindTexture);
  CHOYA_GL(TexStorage2D);
  CHOYA_GL(TexSubImage2D);
  CHOYA_GL(TexParameteri);
  CHOYA_GL(ActiveTexture);
  CHOYA_GL(PixelStorei);
  CHOYA_GL(GenVertexArrays);
  CHOYA_GL(BindVertexArray);
  CHOYA_GL(DrawElements);
  CHOYA_GL(Viewport);
  CHOYA_GL(Scissor);
  CHOYA_GL(Enable);
  CHOYA_GL(Disable);
  CHOYA_GL(DepthFunc);
  CHOYA_GL(DepthMask);
  CHOYA_GL(DepthRangef);
  CHOYA_GL(ColorMask);
  CHOYA_GL(BlendFuncSeparate);
  CHOYA_GL(BlendEquationSeparate);
  CHOYA_GL(BlendColor);
  CHOYA_GL(ClearColor);
  CHOYA_GL(ClearDepthf);
  CHOYA_GL(Clear);
  CHOYA_GL(GetError);
  CHOYA_GL(GetIntegerv);
#undef CHOYA_GL
  return ok;
}

// --- Cuadro capturado -----------------------------------------------------------------------------------------
enum class Op : uint8_t { Pass, Viewport, Scissor, Draw, Clear };
struct Item {
  Op op;
  union {
    struct {
      uint32_t width, height;
      bool clearColor, clearDepth;
      float rgba[4];
      float depth;
    } pass;
    gfx::Viewport viewport;
    gfx::ClipRect scissor;
    struct {
      gfx::PipelineRef pipeline;
      uint32_t idxOffset, indexCount, instanceCount, uniformOffset, dstAlpha;
    } draw;
    struct {
      gfx::PipelineRef pipeline;
      float rgba[4];
      float depth;
      bool useScissor;
      gfx::ClipRect scissor;
    } clear;
  };
};
struct Frame {
  std::vector<uint8_t> verts, uniforms, indices, storage;
  std::vector<Item> items;
  bool ready = false;
};
Frame g_recording;
Frame g_ready;
bool g_recordingActive = false;
std::mutex g_frameMutex;

std::mutex g_pipelineMutex;
absl::flat_hash_map<gfx::PipelineRef, gx::PipelineConfig> g_gxConfigs;
absl::flat_hash_map<gfx::PipelineRef, gfx::clear::PipelineConfig> g_clearConfigs;

void copy_bytes(std::vector<uint8_t>& dst, const uint8_t* src, size_t size) {
  dst.resize(size);
  if (size != 0) {
    std::memcpy(dst.data(), src, size);
  }
}

// --- Generador GLSL ES 3.20 ---------------------------------------------------------------------------------
constexpr const char* kHelpers = R"(
uint lw_V(uint i) { return texelFetch(vbuf, ivec2(int(i & 4095u), int(i >> 12u)), 0).r; }
uint lw_A(uint i) { return texelFetch(abuf, ivec2(int(i & 4095u), int(i >> 12u)), 0).r; }
uint lb_V(uint o) { return (lw_V(o >> 2u) >> ((o & 3u) * 8u)) & 0xFFu; }
uint lb_A(uint o) { return (lw_A(o >> 2u) >> ((o & 3u) * 8u)) & 0xFFu; }
uint lh_V(uint o, bool le) { uint a = lb_V(o), b = lb_V(o + 1u); return le ? (a | (b << 8u)) : ((a << 8u) | b); }
uint lh_A(uint o, bool le) { uint a = lb_A(o), b = lb_A(o + 1u); return le ? (a | (b << 8u)) : ((a << 8u) | b); }
uint l24_V(uint o, bool le) { uint a = lb_V(o), b = lb_V(o + 1u), c = lb_V(o + 2u); return le ? (a | (b << 8u) | (c << 16u)) : ((a << 16u) | (b << 8u) | c); }
uint l24_A(uint o, bool le) { uint a = lb_A(o), b = lb_A(o + 1u), c = lb_A(o + 2u); return le ? (a | (b << 8u) | (c << 16u)) : ((a << 16u) | (b << 8u) | c); }
uint l32_V(uint o, bool le) { uint a = lb_V(o), b = lb_V(o + 1u), c = lb_V(o + 2u), d = lb_V(o + 3u); return le ? (a | (b << 8u) | (c << 16u) | (d << 24u)) : ((a << 24u) | (b << 16u) | (c << 8u) | d); }
uint l32_A(uint o, bool le) { uint a = lb_A(o), b = lb_A(o + 1u), c = lb_A(o + 2u), d = lb_A(o + 3u); return le ? (a | (b << 8u) | (c << 16u) | (d << 24u)) : ((a << 24u) | (b << 16u) | (c << 8u) | d); }
float sext8(uint v) { return float(int(v << 24u) >> 24); }
float sext16(uint v) { return float(int(v << 16u) >> 16); }
uint uw(uint w) { return u[w >> 2u][w & 3u]; }
vec4 uv4(uint w) { return uintBitsToFloat(u[w >> 2u]); }
)";

// Expresión GLSL de un componente escalar del atributo (offset en bytes `o`).
std::string comp_expr(char buf, u8 compType, const std::string& o, u8 frac, bool le) {
  const std::string le_s = le ? "true" : "false";
  const std::string div = frac != 0 ? " / " + std::to_string(1u << frac) + ".0" : "";
  switch (compType) {
  case GX_U8:
    return "(float(lb_" + std::string(1, buf) + "(" + o + "))" + div + ")";
  case GX_S8:
    return "(sext8(lb_" + std::string(1, buf) + "(" + o + "))" + div + ")";
  case GX_U16:
    return "(float(lh_" + std::string(1, buf) + "(" + o + ", " + le_s + "))" + div + ")";
  case GX_S16:
    return "(sext16(lh_" + std::string(1, buf) + "(" + o + ", " + le_s + "))" + div + ")";
  case GX_F32:
    return "uintBitsToFloat(l32_" + std::string(1, buf) + "(" + o + ", " + le_s + "))";
  default:
    return "0.0";
  }
}

u32 comp_size(u8 compType) {
  switch (compType) {
  case GX_U8:
  case GX_S8:
    return 1;
  case GX_U16:
  case GX_S16:
    return 2;
  default:
    return 4;
  }
}

std::string plus(const std::string& o, u32 add) { return add == 0 ? o : "(" + o + " + " + std::to_string(add) + "u)"; }

// Dirección (buffer + offset en bytes) de un atributo para el vértice `vidx`.
void attr_address(const gx::ShaderConfig& cfg, GXAttr attr, char& buf, std::string& offs, bool& le) {
  const auto& m = cfg.attrs[attr];
  buf = 'V';
  le = false;  // el buffer de vértices siempre es big endian
  offs = "(vtx_start + vidx * " + std::to_string(cfg.vtxStride) + "u + " + std::to_string(m.offset) + "u)";
  const std::string arrayStart = "uw(" + std::to_string(8 + (attr - GX_VA_POS)) + "u)";
  if (m.attrType == GX_INDEX8) {
    offs = "(" + arrayStart + " + lb_V(" + offs + ") * " + std::to_string(m.stride) + "u)";
    buf = 'A';
    le = m.le;
  } else if (m.attrType == GX_INDEX16) {
    offs = "(" + arrayStart + " + lh_V(" + offs + ", false) * " + std::to_string(m.stride) + "u)";
    buf = 'A';
    le = m.le;
  }
}

std::string vec_attr(const gx::ShaderConfig& cfg, GXAttr attr, u8 cnt) {
  const auto& m = cfg.attrs[attr];
  char buf;
  std::string offs;
  bool le;
  attr_address(cfg, attr, buf, offs, le);
  const u32 size = comp_size(m.compType);
  std::string s = "vec" + std::to_string(cnt) + "(";
  for (u8 i = 0; i < cnt; ++i) {
    s += (i ? ", " : "") + comp_expr(buf, m.compType, plus(offs, i * size), m.frac, le);
  }
  return s + ")";
}

std::string color_attr(const gx::ShaderConfig& cfg, GXAttr attr) {
  const auto& m = cfg.attrs[attr];
  char buf;
  std::string offs;
  bool le;
  attr_address(cfg, attr, buf, offs, le);
  const std::string b(1, buf);
  const std::string le_s = le ? "true" : "false";
  auto byte = [&](u32 i) { return "float(lb_" + b + "(" + plus(offs, i) + "))"; };
  switch (m.compType) {
  case GX_RGB565:
    return "(vec4(vec3(float((lh_" + b + "(" + offs + ", " + le_s + ") >> 11u) & 31u) / 31.0, float((lh_" + b + "(" +
           offs + ", " + le_s + ") >> 5u) & 63u) / 63.0, float(lh_" + b + "(" + offs + ", " + le_s +
           ") & 31u) / 31.0), 1.0))";
  case GX_RGB8:
  case GX_RGBX8:
    return "(vec4(" + byte(0) + ", " + byte(1) + ", " + byte(2) + ", 255.0) / 255.0)";
  case GX_RGBA4:
    return "(vec4(float((lh_" + b + "(" + offs + ", " + le_s + ") >> 12u) & 15u), float((lh_" + b + "(" + offs + ", " +
           le_s + ") >> 8u) & 15u), float((lh_" + b + "(" + offs + ", " + le_s + ") >> 4u) & 15u), float(lh_" + b +
           "(" + offs + ", " + le_s + ") & 15u)) / 15.0)";
  case GX_RGBA6:
    return "(vec4(float((l24_" + b + "(" + offs + ", " + le_s + ") >> 18u) & 63u), float((l24_" + b + "(" + offs +
           ", " + le_s + ") >> 12u) & 63u), float((l24_" + b + "(" + offs + ", " + le_s + ") >> 6u) & 63u), float(l24_" +
           b + "(" + offs + ", " + le_s + ") & 63u)) / 63.0)";
  case GX_RGBA8:
  default:
    return "(vec4(" + byte(0) + ", " + byte(1) + ", " + byte(2) + ", " + byte(3) + ") / 255.0)";
  }
}

struct Program {
  GLuint id = 0;
  bool ok = false;
};

// Desplazamientos (en palabras de 32 bits) de los uniformes que usa el paso 2a, siguiendo build_uniform.
struct UniformLayout {
  u32 proj = 20;
  u32 postex = 36;
  u32 chanMat[2] = {0, 0};    // material de color del canal i (0 = viene del vértice)
  u32 chanMatA[2] = {0, 0};   // material de alfa del canal i
};

UniformLayout uniform_layout(const gx::ShaderConfig& cfg, const gx::ShaderInfo& info) {
  UniformLayout l;
  u32 w = 20;  // vtx_start, current_pnmtx, 2 tamaños de viewport, pad, array_start[12]
  if (cfg.lineMode != 0) w += 4;
  l.proj = w;
  w += 16;
  l.postex = w;
  w += 12u * info.matrixLayout.postexCount;
  w += 12u * info.matrixLayout.nrmCount;
  const auto loads = info.loadsTevRegRgb | info.loadsTevRegAlpha;
  w += 4u * static_cast<u32>(loads.count());
  if (info.lightingEnabled) w += 20u * ::GX::MaxLights + 4u;
  for (int i = 0; i < 2; ++i) {
    if (!info.sampledColorChannels.test(i)) continue;
    const auto& cc = cfg.colorChannels[i];
    const auto& cca = cfg.colorChannels[i + GX_ALPHA0];
    if (cc.lightingEnabled && cc.ambSrc == GX_SRC_REG) w += 4;
    if (cc.matSrc == GX_SRC_REG) {
      l.chanMat[i] = w;
      w += 4;
    }
    if (cca.lightingEnabled && cca.ambSrc == GX_SRC_REG) w += 4;
    if (cca.matSrc == GX_SRC_REG) {
      l.chanMatA[i] = w;
      w += 4;
    }
  }
  return l;
}

std::string vertex_color(const gx::ShaderConfig& cfg, u32 channel) {
  const int attr = gx::shader_vertex_color_attr(cfg, channel);
  return attr >= 0 ? color_attr(cfg, static_cast<GXAttr>(attr)) : "vec4(1.0)";
}

void build_sources(const gx::ShaderConfig& cfg, std::string& vs, std::string& fs) {
  const auto info = gx::build_shader_info(cfg);
  const auto l = uniform_layout(cfg, info);

  vs = "#version 320 es\nprecision highp float;\nprecision highp int;\n"
       "uniform highp usampler2D vbuf;\nuniform highp usampler2D abuf;\n"
       "layout(std140) uniform U { uvec4 u[1024]; };\nout vec4 v_color;\n";
  vs += kHelpers;
  vs += "void main() {\n  uint vidx = uint(gl_VertexID);\n  uint vtx_start = uw(0u);\n";
  if (cfg.attrs[GX_VA_PNMTXIDX].attrType != GX_NONE) {
    char buf;
    std::string offs;
    bool le;
    attr_address(cfg, GX_VA_PNMTXIDX, buf, offs, le);
    vs += "  uint pnmtx = lb_" + std::string(1, buf) + "(" + offs + ") / 3u;\n";
  } else {
    vs += "  uint pnmtx = uw(1u);\n";
  }
  const auto& pos = cfg.attrs[GX_VA_POS];
  if (pos.cnt == 2) {
    vs += "  vec3 in_pos = vec3(" + vec_attr(cfg, GX_VA_POS, 2) + ", 0.0);\n";
  } else {
    vs += "  vec3 in_pos = " + vec_attr(cfg, GX_VA_POS, 3) + ";\n";
  }
  vs += "  uint m = " + std::to_string(l.postex) + "u + pnmtx * 12u;\n"
        "  vec4 p = vec4(in_pos, 1.0);\n"
        "  vec4 mv = vec4(dot(p, uv4(m)), dot(p, uv4(m + 4u)), dot(p, uv4(m + 8u)), 1.0);\n"
        "  gl_Position = vec4(dot(mv, uv4(" + std::to_string(l.proj) + "u)), dot(mv, uv4(" +
        std::to_string(l.proj + 4) + "u)), dot(mv, uv4(" + std::to_string(l.proj + 8) + "u)), dot(mv, uv4(" +
        std::to_string(l.proj + 12) + "u)));\n";

  // Color del rasterizador del canal 0 (sin iluminación todavía): material o color del vértice.
  if (info.sampledColorChannels.test(0)) {
    const auto& cc = cfg.colorChannels[0];
    const auto& cca = cfg.colorChannels[GX_ALPHA0];
    const bool needVtx = cc.matSrc == GX_SRC_VTX || cca.matSrc == GX_SRC_VTX;
    if (needVtx) vs += "  vec4 vc = " + vertex_color(cfg, 0) + ";\n";
    const std::string rgb = cc.matSrc == GX_SRC_VTX ? "vc.rgb" : "uv4(" + std::to_string(l.chanMat[0]) + "u).rgb";
    const std::string a = cca.matSrc == GX_SRC_VTX ? "vc.a" : "uv4(" + std::to_string(l.chanMatA[0]) + "u).a";
    vs += "  v_color = vec4(" + rgb + ", " + a + ");\n";
  } else {
    // El TEV no lee el rasterizador (normalmente solo textura): gris para ver la forma.
    vs += "  v_color = vec4(0.6, 0.6, 0.6, 1.0);\n";
  }
  vs += "}\n";

  fs = "#version 320 es\nprecision highp float;\nin vec4 v_color;\nlayout(location = 0) out vec4 o_color;\n"
       "void main() { o_color = clamp(v_color, 0.0, 1.0); }\n";
}

GLuint compile(GLenum type, const std::string& src, std::string& err) {
  const GLuint s = gl.CreateShader(type);
  const char* text = src.c_str();
  gl.ShaderSource(s, 1, &text, nullptr);
  gl.CompileShader(s);
  GLint ok = 0;
  gl.GetShaderiv(s, kCompileStatus, &ok);
  if (!ok) {
    GLint len = 0;
    gl.GetShaderiv(s, kInfoLogLength, &len);
    err.assign(static_cast<size_t>(len > 1 ? len : 1), '\0');
    gl.GetShaderInfoLog(s, len, nullptr, err.data());
    gl.DeleteShader(s);
    return 0;
  }
  return s;
}

absl::flat_hash_map<HashType, Program> g_programs;
int g_programErrors = 0;

const Program& program_for(const gx::ShaderConfig& cfg) {
  const HashType key = xxh3_hash(cfg);
  auto it = g_programs.find(key);
  if (it != g_programs.end()) return it->second;
  Program prog;
  std::string vs, fs, err;
  build_sources(cfg, vs, fs);
  const GLuint v = compile(kVertexShader, vs, err);
  const GLuint f = v ? compile(kFragmentShader, fs, err) : 0;
  if (v && f) {
    prog.id = gl.CreateProgram();
    gl.AttachShader(prog.id, v);
    gl.AttachShader(prog.id, f);
    gl.LinkProgram(prog.id);
    GLint ok = 0;
    gl.GetProgramiv(prog.id, kLinkStatus, &ok);
    if (!ok) {
      GLint len = 0;
      gl.GetProgramiv(prog.id, kInfoLogLength, &len);
      err.assign(static_cast<size_t>(len > 1 ? len : 1), '\0');
      gl.GetProgramInfoLog(prog.id, len, nullptr, err.data());
    } else {
      prog.ok = true;
      gl.UseProgram(prog.id);
      gl.Uniform1i(gl.GetUniformLocation(prog.id, "vbuf"), 0);
      gl.Uniform1i(gl.GetUniformLocation(prog.id, "abuf"), 1);
      const GLuint block = gl.GetUniformBlockIndex(prog.id, "U");
      if (block != kInvalidIndex) gl.UniformBlockBinding(prog.id, block, 0);
    }
  }
  if (v) gl.DeleteShader(v);
  if (f) gl.DeleteShader(f);
  if (!prog.ok && g_programErrors++ < 3) {
    log_line("motor: programa %016llx no compiló: %s", static_cast<unsigned long long>(key), err.c_str());
    log_line("motor: vertex shader:\n%s", vs.c_str());
  }
  return g_programs.emplace(key, prog).first->second;
}

// --- Estado de render -------------------------------------------------------------------------------------
GLenum compare(GXCompare f) {
  // aurora usa Z invertida (cerca = 1): las comparaciones ordenadas se invierten igual que en gx.cpp.
  switch (f) {
  case GX_NEVER: return kNever;
  case GX_LESS: return gx::UseReversedZ ? kGreater : kLess;
  case GX_EQUAL: return kEqual;
  case GX_LEQUAL: return gx::UseReversedZ ? kGequal : kLequal;
  case GX_GREATER: return gx::UseReversedZ ? kLess : kGreater;
  case GX_NEQUAL: return kNotequal;
  case GX_GEQUAL: return gx::UseReversedZ ? kLequal : kGequal;
  default: return kAlways;
  }
}

GLenum blend_factor(GXBlendFactor fac, bool isDst, bool alpha) {
  switch (fac) {
  case GX_BL_ZERO: return kZero;
  case GX_BL_ONE: return kOne;
  case GX_BL_SRCCLR:
    if (alpha) return isDst ? kSrcAlpha : kDstAlpha;
    return isDst ? kSrcColor : kDstColor;
  case GX_BL_INVSRCCLR:
    if (alpha) return isDst ? kOneMinusSrcAlpha : kOneMinusDstAlpha;
    return isDst ? kOneMinusSrcColor : kOneMinusDstColor;
  case GX_BL_SRCALPHA: return kSrcAlpha;
  case GX_BL_INVSRCALPHA: return kOneMinusSrcAlpha;
  case GX_BL_DSTALPHA: return kDstAlpha;
  case GX_BL_INVDSTALPHA: return kOneMinusDstAlpha;
  default: return kOne;
  }
}

GXBlendFactor without_dst_alpha(GXBlendFactor fac) {
  if (fac == GX_BL_DSTALPHA) return GX_BL_ONE;
  if (fac == GX_BL_INVDSTALPHA) return GX_BL_ZERO;
  return fac;
}

void apply_state(const gx::PipelineConfig& c, uint32_t dstAlpha) {
  // Profundidad: GL solo escribe profundidad con la prueba activa, así que "sin comparar" es ALWAYS.
  gl.Enable(kDepthTest);
  gl.DepthFunc(c.depthCompare ? compare(c.depthFunc) : kAlways);
  gl.DepthMask(c.depthUpdate ? 1 : 0);
  // Recorte de caras pendiente (paso 2b): en 2a se dibuja todo para ver las formas.
  gl.Disable(kCullFace);
  const bool hasAlpha = gx::render_target_has_alpha(c.pixelFmt);
  gl.ColorMask(c.colorUpdate, c.colorUpdate, c.colorUpdate, c.alphaUpdate && hasAlpha);

  GLenum cs = kOne, cd = kZero, as = kOne, ad = kZero, ceq = kFuncAdd, aeq = kFuncAdd;
  bool blend = true;
  switch (c.blendMode) {
  case GX_BM_BLEND: {
    GXBlendFactor s = c.blendFacSrc, d = c.blendFacDst;
    if (!hasAlpha) {
      s = without_dst_alpha(s);
      d = without_dst_alpha(d);
    }
    cs = blend_factor(s, false, false);
    cd = blend_factor(d, true, false);
    as = blend_factor(s, false, true);
    ad = blend_factor(d, true, true);
  } break;
  case GX_BM_SUBTRACT:
    cs = cd = as = ad = kOne;
    ceq = aeq = kFuncReverseSubtract;
    break;
  case GX_BM_LOGIC:
    switch (c.blendOp) {
    case GX_LO_CLEAR: cs = cd = as = ad = kZero; break;
    case GX_LO_INVAND: cs = as = kZero; cd = ad = kOneMinusSrcColor; break;
    case GX_LO_NOOP: cs = as = kZero; cd = ad = kOne; break;
    case GX_LO_OR: cs = cd = as = ad = kOne; ceq = aeq = kMax; break;
    default: blend = false; break;
    }
    break;
  default:
    blend = false;
    break;
  }
  if (dstAlpha != UINT32_MAX) {
    as = kConstantAlpha;
    ad = kZero;
    aeq = kFuncAdd;
    blend = true;
    gl.BlendColor(0.f, 0.f, 0.f, static_cast<float>(dstAlpha) / 255.f);
  }
  if (blend) {
    gl.Enable(kBlend);
    gl.BlendFuncSeparate(cs, cd, as, ad);
    gl.BlendEquationSeparate(ceq, aeq);
  } else {
    gl.Disable(kBlend);
  }
}

// --- Recursos -------------------------------------------------------------------------------------------------
constexpr int kTexWidth = 4096;
struct WordTexture {
  GLuint id = 0;
  int rows = 0;
};
WordTexture g_vtex, g_atex;
GLuint g_ubo = 0, g_ebo = 0, g_vao = 0;
size_t g_uboSize = 0, g_eboSize = 0;
GLint g_uboAlign = 16;
bool g_glReady = false, g_glFailed = false;
std::vector<uint8_t> g_padded;

bool upload_words(WordTexture& t, const std::vector<uint8_t>& bytes) {
  const size_t words = (bytes.size() + 3) / 4;
  const int rows = static_cast<int>((words + kTexWidth - 1) / kTexWidth);
  if (rows == 0) return true;
  if (rows > 8192) return false;
  if (t.id == 0 || rows > t.rows) {
    // TexStorage es inmutable: si no cabe, textura nueva y más alta (la vieja se libera).
    if (t.id != 0) gl.DeleteTextures(1, &t.id);
    gl.GenTextures(1, &t.id);
    gl.BindTexture(kTexture2D, t.id);
    const int alloc = std::max(rows * 2, 64);
    gl.TexStorage2D(kTexture2D, 1, kR32ui, kTexWidth, std::min(alloc, 8192));
    gl.TexParameteri(kTexture2D, kTextureMinFilter, kNearest);
    gl.TexParameteri(kTexture2D, kTextureMagFilter, kNearest);
    t.rows = std::min(alloc, 8192);
  }
  g_padded.assign(static_cast<size_t>(rows) * kTexWidth * 4, 0);
  std::memcpy(g_padded.data(), bytes.data(), bytes.size());
  gl.BindTexture(kTexture2D, t.id);
  gl.TexSubImage2D(kTexture2D, 0, 0, 0, kTexWidth, rows, kRedInteger, kUnsignedInt, g_padded.data());
  return true;
}

bool init_gl() {
  if (!load_gl()) return false;
  gl.GenBuffers(1, &g_ubo);
  gl.GenBuffers(1, &g_ebo);
  gl.GenVertexArrays(1, &g_vao);
  gl.GetIntegerv(kUniformBufferOffsetAlignment, &g_uboAlign);
  if (g_uboAlign <= 0) g_uboAlign = 256;
  gl.PixelStorei(kUnpackAlignment, 4);
  log_line("motor: listo (alineación de UBO %d)", g_uboAlign);
  return true;
}

struct Stats {
  uint64_t frames = 0, draws = 0, clears = 0, skippedLine = 0, skippedConfig = 0, skippedAlign = 0,
           skippedProgram = 0, passes = 0;
} g_stats;

} // namespace

bool enabled() noexcept {
  static const bool on = [] {
    const char* v = std::getenv("CHOYA_GLES");
    return v != nullptr && std::strcmp(v, "1") == 0;
  }();
  return on;
}

void register_gx_pipeline(gfx::PipelineRef ref, const gx::PipelineConfig& config) noexcept {
  if (!enabled()) return;
  std::lock_guard lock(g_pipelineMutex);
  g_gxConfigs.try_emplace(ref, config);
}

void register_clear_pipeline(gfx::PipelineRef ref, const gfx::clear::PipelineConfig& config) noexcept {
  if (!enabled()) return;
  std::lock_guard lock(g_pipelineMutex);
  g_clearConfigs.try_emplace(ref, config);
}

void capture_begin(const uint8_t* verts, size_t vertsSize, const uint8_t* uniforms, size_t uniformsSize,
                   const uint8_t* indices, size_t indicesSize, const uint8_t* storage, size_t storageSize) noexcept {
  if (!enabled()) return;
  copy_bytes(g_recording.verts, verts, vertsSize);
  copy_bytes(g_recording.uniforms, uniforms, uniformsSize);
  // Relleno: cada dibujo enlaza 16 KiB de uniformes a partir de su desplazamiento.
  g_recording.uniforms.resize(g_recording.uniforms.size() + 16384, 0);
  copy_bytes(g_recording.indices, indices, indicesSize);
  copy_bytes(g_recording.storage, storage, storageSize);
  g_recording.items.clear();
  g_recordingActive = true;
}

void capture_pass(uint32_t width, uint32_t height, bool clearColor, bool clearDepth, const float clearRgba[4],
                  float clearDepthValue) noexcept {
  if (!g_recordingActive) return;
  Item it{};
  it.op = Op::Pass;
  it.pass.width = width;
  it.pass.height = height;
  it.pass.clearColor = clearColor;
  it.pass.clearDepth = clearDepth;
  std::memcpy(it.pass.rgba, clearRgba, sizeof(it.pass.rgba));
  it.pass.depth = clearDepthValue;
  g_recording.items.push_back(it);
}

void capture_viewport(const gfx::Viewport& vp) noexcept {
  if (!g_recordingActive) return;
  Item it{};
  it.op = Op::Viewport;
  it.viewport = vp;
  g_recording.items.push_back(it);
}

void capture_scissor(const gfx::ClipRect& rect) noexcept {
  if (!g_recordingActive) return;
  Item it{};
  it.op = Op::Scissor;
  it.scissor = rect;
  g_recording.items.push_back(it);
}

void capture_gx_draw(const gx::DrawData& draw) noexcept {
  if (!g_recordingActive) return;
  Item it{};
  it.op = Op::Draw;
  it.draw.pipeline = draw.pipeline;
  it.draw.idxOffset = draw.idxRange.offset;
  it.draw.indexCount = draw.indexCount;
  it.draw.instanceCount = draw.instanceCount;
  it.draw.uniformOffset = draw.uniformRange.offset;
  it.draw.dstAlpha = draw.dstAlpha;
  g_recording.items.push_back(it);
}

void capture_clear(const gfx::clear::DrawData& draw) noexcept {
  if (!g_recordingActive) return;
  Item it{};
  it.op = Op::Clear;
  it.clear.pipeline = draw.pipeline;
  it.clear.rgba[0] = static_cast<float>(draw.color.r);
  it.clear.rgba[1] = static_cast<float>(draw.color.g);
  it.clear.rgba[2] = static_cast<float>(draw.color.b);
  it.clear.rgba[3] = static_cast<float>(draw.color.a);
  it.clear.depth = draw.depth;
  it.clear.useScissor = draw.useScissor;
  it.clear.scissor = draw.scissor;
  g_recording.items.push_back(it);
}

void capture_end() noexcept {
  if (!g_recordingActive) return;
  g_recordingActive = false;
  std::lock_guard lock(g_frameMutex);
  std::swap(g_ready, g_recording);
  g_ready.ready = true;
}

void capture_discard() noexcept { g_recordingActive = false; }

} // namespace aurora::choya

namespace aurora::choya {
namespace {
bool draw_frame_impl(int screenWidth, int screenHeight) {
  if (g_glFailed || choya_gl_proc == nullptr) return false;
  if (!g_glReady) {
    if (!init_gl()) {
      g_glFailed = true;
      log_line("motor: no pude cargar OpenGL ES; queda la pantalla de prueba");
      return false;
    }
    g_glReady = true;
  }

  std::lock_guard lock(g_frameMutex);
  Frame& f = g_ready;
  if (!f.ready) return false;
  f.ready = false;

  // Subir los datos del cuadro.
  if (!upload_words(g_vtex, f.verts) || !upload_words(g_atex, f.storage)) {
    log_line("motor: cuadro demasiado grande para las texturas de vértices");
    return false;
  }
  gl.BindBuffer(kUniformBuffer, g_ubo);
  if (f.uniforms.size() > g_uboSize) {
    g_uboSize = f.uniforms.size() * 2;
    gl.BufferData(kUniformBuffer, static_cast<GLsizeiptr>(g_uboSize), nullptr, kStreamDraw);
  }
  gl.BufferSubData(kUniformBuffer, 0, static_cast<GLsizeiptr>(f.uniforms.size()), f.uniforms.data());
  gl.BindVertexArray(g_vao);
  gl.BindBuffer(kElementArrayBuffer, g_ebo);
  if (f.indices.size() > g_eboSize) {
    g_eboSize = std::max<size_t>(f.indices.size() * 2, 4096);
    gl.BufferData(kElementArrayBuffer, static_cast<GLsizeiptr>(g_eboSize), nullptr, kStreamDraw);
  }
  if (!f.indices.empty()) {
    gl.BufferSubData(kElementArrayBuffer, 0, static_cast<GLsizeiptr>(f.indices.size()), f.indices.data());
  }
  gl.ActiveTexture(kTexture0);
  gl.BindTexture(kTexture2D, g_vtex.id);
  gl.ActiveTexture(kTexture0 + 1);
  gl.BindTexture(kTexture2D, g_atex.id);

  // Pase actual: escala de su tamaño al de la pantalla (y hacia arriba en GL).
  float sx = 1.f, sy = 1.f;
  uint32_t targetH = static_cast<uint32_t>(screenHeight);
  auto to_screen_rect = [&](float x, float y, float w, float h, GLint out[4]) {
    out[0] = static_cast<GLint>(x * sx);
    out[1] = static_cast<GLint>((static_cast<float>(targetH) - y - h) * sy);
    out[2] = static_cast<GLint>(w * sx + 0.5f);
    out[3] = static_cast<GLint>(h * sy + 0.5f);
  };
  GLint scissorRect[4] = {0, 0, screenWidth, screenHeight};
  auto* gxConfigs = &g_gxConfigs;
  std::unique_lock pipeLock(g_pipelineMutex);

  for (const Item& it : f.items) {
    switch (it.op) {
    case Op::Pass: {
      ++g_stats.passes;
      targetH = it.pass.height ? it.pass.height : static_cast<uint32_t>(screenHeight);
      sx = it.pass.width ? static_cast<float>(screenWidth) / static_cast<float>(it.pass.width) : 1.f;
      sy = static_cast<float>(screenHeight) / static_cast<float>(targetH);
      gl.Viewport(0, 0, screenWidth, screenHeight);
      gl.Disable(kScissorTest);
      GLbitfield bits = 0;
      if (it.pass.clearColor) {
        gl.ColorMask(1, 1, 1, 1);
        gl.ClearColor(it.pass.rgba[0], it.pass.rgba[1], it.pass.rgba[2], it.pass.rgba[3]);
        bits |= kColorBufferBit;
      }
      if (it.pass.clearDepth) {
        gl.DepthMask(1);
        gl.ClearDepthf(it.pass.depth);
        bits |= kDepthBufferBit;
      }
      if (bits) gl.Clear(bits);
      gl.Enable(kScissorTest);
      gl.Scissor(0, 0, screenWidth, screenHeight);
    } break;
    case Op::Viewport: {
      GLint r[4];
      to_screen_rect(it.viewport.left, it.viewport.top, it.viewport.width, it.viewport.height, r);
      gl.Viewport(r[0], r[1], r[2], r[3]);
      const float minDepth = aurora::gx::UseReversedZ ? 1.0f - it.viewport.zfar : it.viewport.znear;
      const float maxDepth = aurora::gx::UseReversedZ ? 1.0f - it.viewport.znear : it.viewport.zfar;
      gl.DepthRangef(minDepth, maxDepth);
    } break;
    case Op::Scissor: {
      to_screen_rect(static_cast<float>(it.scissor.x), static_cast<float>(it.scissor.y),
                     static_cast<float>(it.scissor.width), static_cast<float>(it.scissor.height), scissorRect);
      gl.Scissor(scissorRect[0], scissorRect[1], std::max(0, scissorRect[2]), std::max(0, scissorRect[3]));
    } break;
    case Op::Clear: {
      auto cit = g_clearConfigs.find(it.clear.pipeline);
      if (cit == g_clearConfigs.end()) {
        ++g_stats.skippedConfig;
        break;
      }
      const auto& cc = cit->second;
      GLint r[4] = {scissorRect[0], scissorRect[1], scissorRect[2], scissorRect[3]};
      if (it.clear.useScissor) {
        to_screen_rect(static_cast<float>(it.clear.scissor.x), static_cast<float>(it.clear.scissor.y),
                       static_cast<float>(it.clear.scissor.width), static_cast<float>(it.clear.scissor.height), r);
      }
      gl.Scissor(r[0], r[1], std::max(0, r[2]), std::max(0, r[3]));
      gl.ColorMask(cc.clearColor, cc.clearColor, cc.clearColor, cc.clearAlpha);
      gl.DepthMask(cc.clearDepth ? 1 : 0);
      gl.ClearColor(it.clear.rgba[0], it.clear.rgba[1], it.clear.rgba[2], it.clear.rgba[3]);
      gl.ClearDepthf(it.clear.depth);
      GLbitfield bits = 0;
      if (cc.clearColor || cc.clearAlpha) bits |= kColorBufferBit;
      if (cc.clearDepth) bits |= kDepthBufferBit;
      if (bits) gl.Clear(bits);
      gl.Scissor(scissorRect[0], scissorRect[1], std::max(0, scissorRect[2]), std::max(0, scissorRect[3]));
      ++g_stats.clears;
    } break;
    case Op::Draw: {
      auto pit = gxConfigs->find(it.draw.pipeline);
      if (pit == gxConfigs->end()) {
        ++g_stats.skippedConfig;
        break;
      }
      const auto& pc = pit->second;
      if (pc.shaderConfig.lineMode != 0 || it.draw.instanceCount != 1 || pc.cullMode == GX_CULL_ALL) {
        ++g_stats.skippedLine;
        break;
      }
      if (it.draw.uniformOffset % static_cast<uint32_t>(g_uboAlign) != 0 ||
          it.draw.uniformOffset + 16384u > f.uniforms.size()) {
        ++g_stats.skippedAlign;
        break;
      }
      const Program& prog = program_for(pc.shaderConfig);
      if (!prog.ok) {
        ++g_stats.skippedProgram;
        break;
      }
      gl.UseProgram(prog.id);
      gl.BindBufferRange(kUniformBuffer, 0, g_ubo, static_cast<GLintptr>(it.draw.uniformOffset), 16384);
      apply_state(pc, it.draw.dstAlpha);
      gl.DrawElements(kTriangles, static_cast<GLsizei>(it.draw.indexCount), kUnsignedShort,
                      reinterpret_cast<const void*>(static_cast<uintptr_t>(it.draw.idxOffset)));
      ++g_stats.draws;
    } break;
    }
  }
  pipeLock.unlock();
  gl.Disable(kScissorTest);
  gl.ColorMask(1, 1, 1, 1);
  gl.DepthMask(1);

  if (++g_stats.frames % 300 == 0) {
    log_line("motor: %llu cuadros, por cuadro: %.1f dibujos, %.1f borrados, %.1f pases; saltados: %llu sin config, "
         "%llu líneas/puntos, %llu alineación, %llu sin programa; programas %zu; error GL 0x%04X",
         static_cast<unsigned long long>(g_stats.frames), g_stats.draws / 300.0, g_stats.clears / 300.0,
         g_stats.passes / 300.0, static_cast<unsigned long long>(g_stats.skippedConfig),
         static_cast<unsigned long long>(g_stats.skippedLine), static_cast<unsigned long long>(g_stats.skippedAlign),
         static_cast<unsigned long long>(g_stats.skippedProgram), g_programs.size(), gl.GetError());
    g_stats = Stats{.frames = g_stats.frames};
  }
  return true;
}
} // namespace
} // namespace aurora::choya

extern "C" bool choya_gx_draw_frame(int screenWidth, int screenHeight) {
  return aurora::choya::draw_frame_impl(screenWidth, screenHeight);
}
