// Choya Wii Converter: núcleo del motor GLES de Choya (paso 2a). Ver choya_gx_core.hpp.
//
// Cómo se dibuja un cuadro del GX en OpenGL ES 3.2 (Mali-G31):
//  - Los vértices crudos del GX (vbuf) y los arreglos indexados (abuf) se suben como texturas R32UI de 4096 de
//    ancho: el driver de la Mali no deja leer buffers de almacenamiento desde el vertex shader
//    (GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS = 0), así que se leen con texelFetch.
//  - Los uniformes de cada dibujo se leen de un UBO enlazado en el desplazamiento del dibujo, con el mismo orden
//    de campos que escribe aurora (gx/shader_info.cpp, build_uniform).
//  - Los índices (uint16, ya en triángulos) van a un buffer de elementos; gl_VertexID es el índice del vértice.
//  - Cada combinación de formato de vértice y canales genera un programa GLSL ES 3.20 propio de Choya.
//  - Los pases se dibujan en un framebuffer propio del tamaño del EFB; lo que se ve en pantalla es el EFB al
//    terminar el último pase que se copió (resolve), escalado a la pantalla.
#include "choya_gx_core.hpp"

#include <dolphin/gx/GXEnum.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace choya::gx {
namespace {

using u8 = uint8_t;
using u32 = uint32_t;

LogFn g_logger = nullptr;
ProcFn g_loader = nullptr;
bool g_reversedZ = true;

void log_line(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void log_line(const char* fmt, ...) {
  char line[2048];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  if (g_logger != nullptr) {
    g_logger(line);
  } else {
    std::fprintf(stderr, "[choya-gx] %s\n", line);
  }
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
                 kUniformBufferOffsetAlignment = 0x8A34, kInvalidIndex = 0xFFFFFFFFu, kFramebuffer = 0x8D40,
                 kReadFramebuffer = 0x8CA8, kDrawFramebuffer = 0x8CA9, kColorAttachment0 = 0x8CE0,
                 kDepthStencilAttachment = 0x821A, kRenderbuffer = 0x8D41, kDepth24Stencil8 = 0x88F0, kRgba8 = 0x8058,
                 kFramebufferComplete = 0x8CD5, kLinear = 0x2601;

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
  void (*Uniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
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
  void (*GenFramebuffers)(GLsizei, GLuint*);
  void (*DeleteFramebuffers)(GLsizei, const GLuint*);
  void (*BindFramebuffer)(GLenum, GLuint);
  void (*FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
  void (*GenRenderbuffers)(GLsizei, GLuint*);
  void (*DeleteRenderbuffers)(GLsizei, const GLuint*);
  void (*BindRenderbuffer)(GLenum, GLuint);
  void (*RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
  void (*FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
  GLenum (*CheckFramebufferStatus)(GLenum);
  void (*BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
  void (*GetIntegerv)(GLenum, GLint*);
};
Gl gl{};

template <typename T>
bool load(T& fn, const char* name) {
  fn = g_loader != nullptr ? reinterpret_cast<T>(g_loader(name)) : nullptr;
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
  CHOYA_GL(Uniform4f);
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
  CHOYA_GL(GenFramebuffers);
  CHOYA_GL(DeleteFramebuffers);
  CHOYA_GL(BindFramebuffer);
  CHOYA_GL(FramebufferTexture2D);
  CHOYA_GL(GenRenderbuffers);
  CHOYA_GL(DeleteRenderbuffers);
  CHOYA_GL(BindRenderbuffer);
  CHOYA_GL(RenderbufferStorage);
  CHOYA_GL(FramebufferRenderbuffer);
  CHOYA_GL(CheckFramebufferStatus);
  CHOYA_GL(BlitFramebuffer);
  CHOYA_GL(GetIntegerv);
#undef CHOYA_GL
  return ok;
}

// --- Cuadro capturado -----------------------------------------------------------------------------------------
enum class Op : uint8_t { Pass, Viewport, Scissor, Draw, Clear };
struct Item {
  Op op;
  uint8_t pad[7];
  union {
    struct {
      uint32_t width, height;
      uint8_t clearColor, clearDepth, hasResolve, pad;
      float rgba[4];
      float depth;
    } pass;
    Viewport viewport;
    Rect scissor;
    struct {
      uint64_t pipeline;
      uint32_t idxOffset, indexCount, instanceCount, uniformOffset, dstAlpha, pad;
    } draw;
    struct {
      uint64_t pipeline;
      float rgba[4];
      float depth;
      uint32_t useScissor;
      Rect scissor;
    } clear;
  };
};
static_assert(sizeof(Item) == 56);
struct Frame {
  std::vector<uint8_t> verts, uniforms, indices, storage;
  std::vector<Item> items;
  bool ready = false;
};
Frame g_recording;
Frame g_ready;
Frame g_last;  // el último cuadro dibujado (para volcarlo)
bool g_recordingActive = false;
std::mutex g_frameMutex;

std::mutex g_pipelineMutex;
std::unordered_map<uint64_t, PipelineDesc> g_pipelines;
std::unordered_map<uint64_t, ClearDesc> g_clears;

void copy_bytes(std::vector<uint8_t>& dst, const uint8_t* src, size_t size) {
  dst.resize(size);
  if (size != 0) {
    std::memcpy(dst.data(), src, size);
  }
}

uint64_t shader_key(const PipelineDesc& d) {
  // FNV-1a sobre la parte del descriptor que define el programa.
  const auto* p = reinterpret_cast<const uint8_t*>(&d);
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < kShaderKeyBytes; ++i) {
    h = (h ^ p[i]) * 1099511628211ull;
  }
  return h;
}

int vertex_color_attr(const PipelineDesc& cfg, u32 channel) {
  const bool has0 = cfg.attrs[GX_VA_CLR0].attrType != GX_NONE;
  const bool has1 = cfg.attrs[GX_VA_CLR1].attrType != GX_NONE;
  if (channel == 0) {
    if (has0) return GX_VA_CLR0;
    if (has1) return GX_VA_CLR1;
  } else if (channel == 1 && has0 && has1) {
    return GX_VA_CLR1;
  }
  return -1;
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
void attr_address(const PipelineDesc& cfg, GXAttr attr, char& buf, std::string& offs, bool& le) {
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

std::string vec_attr(const PipelineDesc& cfg, GXAttr attr, u8 cnt) {
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

std::string color_attr(const PipelineDesc& cfg, GXAttr attr) {
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
  GLint debugLoc = -1;
  bool ok = false;
};

bool debug_colors() {
  static const bool on = [] {
    const char* v = std::getenv("CHOYA_GX_COLORES");
    return v != nullptr && std::strcmp(v, "1") == 0;
  }();
  return on;
}

// Desplazamientos (en palabras de 32 bits) de los uniformes que usa el paso 2a, siguiendo build_uniform.
struct UniformLayout {
  u32 proj = 20;
  u32 postex = 36;
  u32 chanMat[2] = {0, 0};    // material de color del canal i (0 = viene del vértice)
  u32 chanMatA[2] = {0, 0};   // material de alfa del canal i
};

UniformLayout uniform_layout(const PipelineDesc& cfg) {
  UniformLayout l;
  u32 w = 20;  // vtx_start, current_pnmtx, 2 tamaños de viewport, pad, array_start[12]
  if (cfg.lineMode != 0) w += 4;
  l.proj = w;
  w += 16;
  l.postex = w;
  w += 12u * cfg.postexCount;
  w += 12u * cfg.nrmCount;
  w += 4u * cfg.tevRegLoads;
  if (cfg.lightingEnabled) w += 20u * 8u + 4u;  // 8 luces de 80 bytes + 4 palabras
  for (int i = 0; i < 2; ++i) {
    if (((cfg.sampledChannels >> i) & 1) == 0) continue;
    const auto& cc = cfg.chans[i];
    const auto& cca = cfg.chans[i + GX_ALPHA0];
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

std::string vertex_color(const PipelineDesc& cfg, u32 channel) {
  const int attr = vertex_color_attr(cfg, channel);
  return attr >= 0 ? color_attr(cfg, static_cast<GXAttr>(attr)) : "vec4(1.0)";
}

void build_sources(const PipelineDesc& cfg, std::string& vs, std::string& fs) {
  const auto l = uniform_layout(cfg);

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
  if ((cfg.sampledChannels & 1) != 0) {
    const auto& cc = cfg.chans[0];
    const auto& cca = cfg.chans[GX_ALPHA0];
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

  if (debug_colors()) {
    fs = "#version 320 es\nprecision highp float;\nin vec4 v_color;\nuniform vec4 u_debug;\n"
         "layout(location = 0) out vec4 o_color;\nvoid main() { o_color = u_debug; }\n";
  } else {
    fs = "#version 320 es\nprecision highp float;\nin vec4 v_color;\nlayout(location = 0) out vec4 o_color;\n"
         "void main() { o_color = clamp(v_color, 0.0, 1.0); }\n";
  }
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

std::unordered_map<uint64_t, Program> g_programs;
int g_programErrors = 0;

const Program& program_for(const PipelineDesc& cfg) {
  const uint64_t key = shader_key(cfg);
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
      prog.debugLoc = gl.GetUniformLocation(prog.id, "u_debug");
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
GLenum compare(u8 f) {
  // aurora usa Z invertida (cerca = 1): las comparaciones ordenadas se invierten igual que en gx.cpp.
  switch (f) {
  case GX_NEVER: return kNever;
  case GX_LESS: return g_reversedZ ? kGreater : kLess;
  case GX_EQUAL: return kEqual;
  case GX_LEQUAL: return g_reversedZ ? kGequal : kLequal;
  case GX_GREATER: return g_reversedZ ? kLess : kGreater;
  case GX_NEQUAL: return kNotequal;
  case GX_GEQUAL: return g_reversedZ ? kLequal : kGequal;
  default: return kAlways;
  }
}

GLenum blend_factor(u8 fac, bool isDst, bool alpha) {
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

u8 without_dst_alpha(u8 fac) {
  if (fac == GX_BL_DSTALPHA) return GX_BL_ONE;
  if (fac == GX_BL_INVDSTALPHA) return GX_BL_ZERO;
  return fac;
}

void apply_state(const PipelineDesc& c, uint32_t dstAlpha) {
  // Profundidad: GL solo escribe profundidad con la prueba activa, así que "sin comparar" es ALWAYS.
  gl.Enable(kDepthTest);
  gl.DepthFunc(c.depthCompare ? compare(c.depthFunc) : kAlways);
  gl.DepthMask(c.depthUpdate ? 1 : 0);
  // Recorte de caras pendiente (paso 2b): en 2a se dibuja todo para ver las formas.
  gl.Disable(kCullFace);
  const bool hasAlpha = c.pixelFmt == GX_PF_RGBA6_Z24;
  gl.ColorMask(c.colorUpdate, c.colorUpdate, c.colorUpdate, c.alphaUpdate && hasAlpha);

  GLenum cs = kOne, cd = kZero, as = kOne, ad = kZero, ceq = kFuncAdd, aeq = kFuncAdd;
  bool blend = true;
  switch (c.blendMode) {
  case GX_BM_BLEND: {
    u8 s = c.blendSrc, d = c.blendDst;
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
constexpr int kRing = 3;  // recursos por cuadro en anillo: la Mali no espera a que la GPU suelte el cuadro anterior
struct WordTexture {
  GLuint id = 0;
  int rows = 0;
};
struct FrameResources {
  WordTexture vtex, atex;
  GLuint ubo = 0, ebo = 0;
  size_t uboSize = 0, eboSize = 0;
};
FrameResources g_ring[kRing];
int g_ringIndex = 0;
GLuint g_vao = 0;
GLint g_uboAlign = 16;
bool g_glReady = false, g_glFailed = false;
std::vector<uint8_t> g_tail;

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
    const int alloc = std::min(std::max(rows * 2, 64), 8192);
    gl.TexStorage2D(kTexture2D, 1, kR32ui, kTexWidth, alloc);
    gl.TexParameteri(kTexture2D, kTextureMinFilter, kNearest);
    gl.TexParameteri(kTexture2D, kTextureMagFilter, kNearest);
    t.rows = alloc;
  }
  gl.BindTexture(kTexture2D, t.id);
  // Filas completas directo de los datos; la última fila (incompleta) desde un búfer pequeño.
  const size_t rowBytes = static_cast<size_t>(kTexWidth) * 4;
  const int fullRows = static_cast<int>(bytes.size() / rowBytes);
  if (fullRows > 0) {
    gl.TexSubImage2D(kTexture2D, 0, 0, 0, kTexWidth, fullRows, kRedInteger, kUnsignedInt, bytes.data());
  }
  const size_t rest = bytes.size() - static_cast<size_t>(fullRows) * rowBytes;
  if (rest > 0) {
    const int restWords = static_cast<int>((rest + 3) / 4);
    g_tail.assign(static_cast<size_t>(restWords) * 4, 0);
    std::memcpy(g_tail.data(), bytes.data() + static_cast<size_t>(fullRows) * rowBytes, rest);
    gl.TexSubImage2D(kTexture2D, 0, 0, fullRows, restWords, 1, kRedInteger, kUnsignedInt, g_tail.data());
  }
  return true;
}

bool init_gl() {
  if (!load_gl()) return false;
  for (auto& r : g_ring) {
    gl.GenBuffers(1, &r.ubo);
    gl.GenBuffers(1, &r.ebo);
  }
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
  uint64_t bytesVerts = 0, bytesUniforms = 0, bytesIndices = 0, bytesStorage = 0;
  double cpuMs = 0, captureMs = 0;
} g_stats;

// Framebuffer del EFB (color RGBA8 + profundidad/estencil).
struct Efb {
  GLuint fbo = 0, color = 0, depth = 0;
  int width = 0, height = 0;
} g_efb;

bool ensure_efb(int width, int height) {
  if (g_efb.fbo != 0 && width <= g_efb.width && height <= g_efb.height) return true;
  if (g_efb.fbo != 0) {
    gl.DeleteFramebuffers(1, &g_efb.fbo);
    gl.DeleteTextures(1, &g_efb.color);
    gl.DeleteRenderbuffers(1, &g_efb.depth);
  }
  g_efb.width = std::max(width, g_efb.width);
  g_efb.height = std::max(height, g_efb.height);
  gl.GenTextures(1, &g_efb.color);
  gl.BindTexture(kTexture2D, g_efb.color);
  gl.TexStorage2D(kTexture2D, 1, kRgba8, g_efb.width, g_efb.height);
  gl.TexParameteri(kTexture2D, kTextureMinFilter, kLinear);
  gl.TexParameteri(kTexture2D, kTextureMagFilter, kLinear);
  gl.GenRenderbuffers(1, &g_efb.depth);
  gl.BindRenderbuffer(kRenderbuffer, g_efb.depth);
  gl.RenderbufferStorage(kRenderbuffer, kDepth24Stencil8, g_efb.width, g_efb.height);
  gl.GenFramebuffers(1, &g_efb.fbo);
  gl.BindFramebuffer(kFramebuffer, g_efb.fbo);
  gl.FramebufferTexture2D(kFramebuffer, kColorAttachment0, kTexture2D, g_efb.color, 0);
  gl.FramebufferRenderbuffer(kFramebuffer, kDepthStencilAttachment, kRenderbuffer, g_efb.depth);
  const GLenum status = gl.CheckFramebufferStatus(kFramebuffer);
  if (status != kFramebufferComplete) {
    log_line("motor: el framebuffer del EFB (%dx%d) no está completo (0x%04X)", g_efb.width, g_efb.height, status);
    return false;
  }
  log_line("motor: EFB de %dx%d", g_efb.width, g_efb.height);
  return true;
}

// Copia la zona (0,0,w,h) del EFB a la pantalla, escalada.
void present_efb(int w, int h, int screenWidth, int screenHeight) {
  gl.Disable(kScissorTest);
  gl.BindFramebuffer(kReadFramebuffer, g_efb.fbo);
  gl.BindFramebuffer(kDrawFramebuffer, 0);
  // El EFB se dibuja con y hacia arriba en GL (como la pantalla): la copia no voltea.
  gl.BlitFramebuffer(0, g_efb.height - h, w, g_efb.height, 0, 0, screenWidth, screenHeight, kColorBufferBit, kLinear);
  gl.BindFramebuffer(kFramebuffer, g_efb.fbo);
}

bool draw_frame_locked(Frame& f, int screenWidth, int screenHeight);

} // namespace

void set_logger(LogFn fn) noexcept { g_logger = fn; }
void set_gl_loader(ProcFn fn) noexcept { g_loader = fn; }
void set_reversed_z(bool reversed) noexcept { g_reversedZ = reversed; }

void register_pipeline(uint64_t ref, const PipelineDesc& desc) noexcept {
  std::lock_guard lock(g_pipelineMutex);
  g_pipelines.try_emplace(ref, desc);
}
bool has_pipeline(uint64_t ref) noexcept {
  std::lock_guard lock(g_pipelineMutex);
  return g_pipelines.contains(ref);
}
void register_clear(uint64_t ref, const ClearDesc& desc) noexcept {
  std::lock_guard lock(g_pipelineMutex);
  g_clears.try_emplace(ref, desc);
}
bool has_clear(uint64_t ref) noexcept {
  std::lock_guard lock(g_pipelineMutex);
  return g_clears.contains(ref);
}

static std::chrono::steady_clock::time_point g_captureStart;

void capture_begin(const uint8_t* verts, size_t vertsSize, const uint8_t* uniforms, size_t uniformsSize,
                   const uint8_t* indices, size_t indicesSize, const uint8_t* storage, size_t storageSize) noexcept {
  g_captureStart = std::chrono::steady_clock::now();
  copy_bytes(g_recording.verts, verts, vertsSize);
  copy_bytes(g_recording.uniforms, uniforms, uniformsSize);
  // Relleno: cada dibujo enlaza 16 KiB de uniformes a partir de su desplazamiento.
  g_recording.uniforms.resize(g_recording.uniforms.size() + 16384, 0);
  copy_bytes(g_recording.indices, indices, indicesSize);
  copy_bytes(g_recording.storage, storage, storageSize);
  g_recording.items.clear();
  g_recordingActive = true;
}

bool capturing() noexcept { return g_recordingActive; }

void capture_pass(uint32_t width, uint32_t height, bool clearColor, bool clearDepth, const float clearRgba[4],
                  float clearDepthValue, bool hasResolve) noexcept {
  if (!g_recordingActive) return;
  Item it{};
  it.op = Op::Pass;
  it.pass.width = width;
  it.pass.height = height;
  it.pass.clearColor = clearColor;
  it.pass.clearDepth = clearDepth;
  it.pass.hasResolve = hasResolve;
  std::memcpy(it.pass.rgba, clearRgba, sizeof(it.pass.rgba));
  it.pass.depth = clearDepthValue;
  g_recording.items.push_back(it);
}

void capture_viewport(const Viewport& vp) noexcept {
  if (!g_recordingActive) return;
  Item it{};
  it.op = Op::Viewport;
  it.viewport = vp;
  g_recording.items.push_back(it);
}

void capture_scissor(const Rect& rect) noexcept {
  if (!g_recordingActive) return;
  Item it{};
  it.op = Op::Scissor;
  it.scissor = rect;
  g_recording.items.push_back(it);
}

void capture_draw(uint64_t pipeline, uint32_t idxOffset, uint32_t indexCount, uint32_t instanceCount,
                  uint32_t uniformOffset, uint32_t dstAlpha) noexcept {
  if (!g_recordingActive) return;
  Item it{};
  it.op = Op::Draw;
  it.draw.pipeline = pipeline;
  it.draw.idxOffset = idxOffset;
  it.draw.indexCount = indexCount;
  it.draw.instanceCount = instanceCount;
  it.draw.uniformOffset = uniformOffset;
  it.draw.dstAlpha = dstAlpha;
  g_recording.items.push_back(it);
}

void capture_clear(uint64_t pipeline, const float rgba[4], float depth, bool useScissor, const Rect& rect) noexcept {
  if (!g_recordingActive) return;
  Item it{};
  it.op = Op::Clear;
  it.clear.pipeline = pipeline;
  std::memcpy(it.clear.rgba, rgba, sizeof(it.clear.rgba));
  it.clear.depth = depth;
  it.clear.useScissor = useScissor;
  it.clear.scissor = rect;
  g_recording.items.push_back(it);
}

void capture_end() noexcept {
  if (!g_recordingActive) return;
  g_recordingActive = false;
  g_stats.captureMs +=
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_captureStart).count();
  std::lock_guard lock(g_frameMutex);
  std::swap(g_ready, g_recording);
  g_ready.ready = true;
}

void capture_discard() noexcept { g_recordingActive = false; }

// --- Volcado ------------------------------------------------------------------------------------------------
namespace {
constexpr char kDumpMagic[8] = {'C', 'H', 'O', 'Y', 'A', 'G', 'X', '1'};

void write_blob(FILE* f, const std::vector<uint8_t>& v) {
  const uint64_t n = v.size();
  std::fwrite(&n, sizeof(n), 1, f);
  if (n) std::fwrite(v.data(), 1, n, f);
}
bool read_blob(FILE* f, std::vector<uint8_t>& v) {
  uint64_t n = 0;
  if (std::fread(&n, sizeof(n), 1, f) != 1 || n > (1ull << 30)) return false;
  v.resize(n);
  return n == 0 || std::fread(v.data(), 1, n, f) == n;
}
} // namespace

bool dump_last_frame(const char* path) noexcept {
  std::lock_guard lock(g_frameMutex);
  const Frame& f = g_last;
  FILE* out = std::fopen(path, "wb");
  if (!out) return false;
  std::fwrite(kDumpMagic, 1, sizeof(kDumpMagic), out);
  const uint32_t header[4] = {static_cast<uint32_t>(sizeof(PipelineDesc)), static_cast<uint32_t>(sizeof(Item)),
                              g_reversedZ ? 1u : 0u, 0};
  std::fwrite(header, sizeof(header), 1, out);
  write_blob(out, f.verts);
  write_blob(out, f.uniforms);
  write_blob(out, f.indices);
  write_blob(out, f.storage);
  const uint64_t items = f.items.size();
  std::fwrite(&items, sizeof(items), 1, out);
  if (items) std::fwrite(f.items.data(), sizeof(Item), items, out);
  std::lock_guard plock(g_pipelineMutex);
  std::vector<std::pair<uint64_t, PipelineDesc>> pipes;
  std::vector<std::pair<uint64_t, ClearDesc>> clears;
  for (const Item& it : f.items) {
    if (it.op == Op::Draw) {
      auto p = g_pipelines.find(it.draw.pipeline);
      if (p != g_pipelines.end()) pipes.emplace_back(p->first, p->second);
    } else if (it.op == Op::Clear) {
      auto c = g_clears.find(it.clear.pipeline);
      if (c != g_clears.end()) clears.emplace_back(c->first, c->second);
    }
  }
  const uint64_t np = pipes.size(), nc = clears.size();
  std::fwrite(&np, sizeof(np), 1, out);
  for (const auto& [ref, d] : pipes) {
    std::fwrite(&ref, sizeof(ref), 1, out);
    std::fwrite(&d, sizeof(d), 1, out);
  }
  std::fwrite(&nc, sizeof(nc), 1, out);
  for (const auto& [ref, d] : clears) {
    std::fwrite(&ref, sizeof(ref), 1, out);
    std::fwrite(&d, sizeof(d), 1, out);
  }
  std::fclose(out);
  return true;
}

bool load_frame(const char* path) noexcept {
  FILE* in = std::fopen(path, "rb");
  if (!in) return false;
  char magic[8];
  uint32_t header[4];
  bool ok = std::fread(magic, 1, 8, in) == 8 && std::memcmp(magic, kDumpMagic, 8) == 0 &&
            std::fread(header, sizeof(header), 1, in) == 1 && header[0] == sizeof(PipelineDesc) &&
            header[1] == sizeof(Item);
  Frame f;
  uint64_t items = 0, np = 0, nc = 0;
  ok = ok && read_blob(in, f.verts) && read_blob(in, f.uniforms) && read_blob(in, f.indices) &&
       read_blob(in, f.storage) && std::fread(&items, sizeof(items), 1, in) == 1 && items < (1u << 24);
  if (ok) {
    f.items.resize(items);
    ok = items == 0 || std::fread(f.items.data(), sizeof(Item), items, in) == items;
  }
  ok = ok && std::fread(&np, sizeof(np), 1, in) == 1;
  for (uint64_t i = 0; ok && i < np; ++i) {
    uint64_t ref;
    PipelineDesc d;
    ok = std::fread(&ref, sizeof(ref), 1, in) == 1 && std::fread(&d, sizeof(d), 1, in) == 1;
    if (ok) register_pipeline(ref, d);
  }
  ok = ok && std::fread(&nc, sizeof(nc), 1, in) == 1;
  for (uint64_t i = 0; ok && i < nc; ++i) {
    uint64_t ref;
    ClearDesc d;
    ok = std::fread(&ref, sizeof(ref), 1, in) == 1 && std::fread(&d, sizeof(d), 1, in) == 1;
    if (ok) register_clear(ref, d);
  }
  std::fclose(in);
  if (!ok) return false;
  g_reversedZ = header[2] != 0;
  f.ready = true;
  std::lock_guard lock(g_frameMutex);
  g_ready = std::move(f);
  return true;
}

bool draw_frame(int screenWidth, int screenHeight) noexcept {
  if (g_glFailed || g_loader == nullptr) return false;
  if (!g_glReady) {
    if (!init_gl()) {
      g_glFailed = true;
      log_line("motor: no pude cargar OpenGL ES; queda la pantalla de prueba");
      return false;
    }
    g_glReady = true;
  }
  std::lock_guard lock(g_frameMutex);
  if (!g_ready.ready) return false;
  g_ready.ready = false;
  const bool drawn = draw_frame_locked(g_ready, screenWidth, screenHeight);
  std::swap(g_last, g_ready);
  return drawn;
}

namespace {
bool draw_frame_locked(Frame& f, int screenWidth, int screenHeight) {
  const auto cpuStart = std::chrono::steady_clock::now();
  // Tamaño del EFB: el pase más grande del cuadro.
  int efbW = 1, efbH = 1;
  for (const Item& pass : f.items) {
    if (pass.op == Op::Pass) {
      efbW = std::max(efbW, static_cast<int>(pass.pass.width));
      efbH = std::max(efbH, static_cast<int>(pass.pass.height));
    }
  }
  if (!ensure_efb(efbW, efbH)) return false;
  gl.BindFramebuffer(kFramebuffer, g_efb.fbo);
  g_stats.bytesVerts += f.verts.size();
  g_stats.bytesUniforms += f.uniforms.size();
  g_stats.bytesIndices += f.indices.size();
  g_stats.bytesStorage += f.storage.size();

  // Subir los datos del cuadro a los recursos que tocan en el anillo.
  FrameResources& res = g_ring[g_ringIndex];
  g_ringIndex = (g_ringIndex + 1) % kRing;
  gl.ActiveTexture(kTexture0);
  if (!upload_words(res.vtex, f.verts)) {
    log_line("motor: cuadro demasiado grande para la textura de vértices");
    return false;
  }
  gl.ActiveTexture(kTexture0 + 1);
  if (!upload_words(res.atex, f.storage)) {
    log_line("motor: cuadro demasiado grande para la textura de arreglos");
    return false;
  }
  // Uniformes y elementos: se "huérfana" el búfer completo (BufferData nuevo) para no esperar a la GPU.
  gl.BindBuffer(kUniformBuffer, res.ubo);
  res.uboSize = std::max(res.uboSize, f.uniforms.size());
  gl.BufferData(kUniformBuffer, static_cast<GLsizeiptr>(res.uboSize), nullptr, kStreamDraw);
  gl.BufferSubData(kUniformBuffer, 0, static_cast<GLsizeiptr>(f.uniforms.size()), f.uniforms.data());
  gl.BindVertexArray(g_vao);
  gl.BindBuffer(kElementArrayBuffer, res.ebo);
  if (!f.indices.empty()) {
    res.eboSize = std::max(res.eboSize, f.indices.size());
    gl.BufferData(kElementArrayBuffer, static_cast<GLsizeiptr>(res.eboSize), nullptr, kStreamDraw);
    gl.BufferSubData(kElementArrayBuffer, 0, static_cast<GLsizeiptr>(f.indices.size()), f.indices.data());
  }
  gl.ActiveTexture(kTexture0);
  gl.BindTexture(kTexture2D, res.vtex.id);
  gl.ActiveTexture(kTexture0 + 1);
  gl.BindTexture(kTexture2D, res.atex.id);
  const GLuint ubo = res.ubo;
  uint32_t drawIndex = 0;


  // Coordenadas del pase (origen arriba a la izquierda) -> GL (origen abajo) dentro del EFB.
  int passW = efbW, passH = efbH;
  auto to_gl_rect = [&](float x, float y, float w, float h, GLint out[4]) {
    out[0] = static_cast<GLint>(x);
    out[1] = static_cast<GLint>(static_cast<float>(g_efb.height) - y - h);
    out[2] = static_cast<GLint>(w + 0.5f);
    out[3] = static_cast<GLint>(h + 0.5f);
  };
  GLint scissorRect[4] = {0, g_efb.height - efbH, efbW, efbH};
  bool presented = false;
  bool passOpen = false;
  bool passResolves = false;
  auto end_pass = [&]() {
    // La copia a pantalla (XFB) es el EFB al terminar el pase que se copia (GXCopyDisp u otra copia).
    if (passOpen && passResolves) {
      present_efb(passW, passH, screenWidth, screenHeight);
      presented = true;
    }
  };
  std::unique_lock pipeLock(g_pipelineMutex);

  for (const Item& it : f.items) {
    switch (it.op) {
    case Op::Pass: {
      end_pass();
      ++g_stats.passes;
      passOpen = true;
      passResolves = it.pass.hasResolve != 0;
      passW = it.pass.width ? static_cast<int>(it.pass.width) : efbW;
      passH = it.pass.height ? static_cast<int>(it.pass.height) : efbH;
      gl.Viewport(0, g_efb.height - passH, passW, passH);
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
      scissorRect[0] = 0;
      scissorRect[1] = g_efb.height - passH;
      scissorRect[2] = passW;
      scissorRect[3] = passH;
      gl.Scissor(scissorRect[0], scissorRect[1], scissorRect[2], scissorRect[3]);
    } break;
    case Op::Viewport: {
      GLint r[4];
      to_gl_rect(it.viewport.left, it.viewport.top, it.viewport.width, it.viewport.height, r);
      gl.Viewport(r[0], r[1], r[2], r[3]);
      const float minDepth = g_reversedZ ? 1.0f - it.viewport.zfar : it.viewport.znear;
      const float maxDepth = g_reversedZ ? 1.0f - it.viewport.znear : it.viewport.zfar;
      gl.DepthRangef(minDepth, maxDepth);
    } break;
    case Op::Scissor: {
      to_gl_rect(static_cast<float>(it.scissor.x), static_cast<float>(it.scissor.y),
                     static_cast<float>(it.scissor.width), static_cast<float>(it.scissor.height), scissorRect);
      gl.Scissor(scissorRect[0], scissorRect[1], std::max(0, scissorRect[2]), std::max(0, scissorRect[3]));
    } break;
    case Op::Clear: {
      auto cit = g_clears.find(it.clear.pipeline);
      if (cit == g_clears.end()) {
        ++g_stats.skippedConfig;
        break;
      }
      const auto& cc = cit->second;
      GLint r[4] = {scissorRect[0], scissorRect[1], scissorRect[2], scissorRect[3]};
      if (it.clear.useScissor) {
        to_gl_rect(static_cast<float>(it.clear.scissor.x), static_cast<float>(it.clear.scissor.y),
                       static_cast<float>(it.clear.scissor.width), static_cast<float>(it.clear.scissor.height), r);
      }
      gl.Scissor(r[0], r[1], std::max(0, r[2]), std::max(0, r[3]));
      gl.ColorMask(cc.color, cc.color, cc.color, cc.alpha);
      gl.DepthMask(cc.depth ? 1 : 0);
      gl.ClearColor(it.clear.rgba[0], it.clear.rgba[1], it.clear.rgba[2], it.clear.rgba[3]);
      gl.ClearDepthf(it.clear.depth);
      GLbitfield bits = 0;
      if (cc.color || cc.alpha) bits |= kColorBufferBit;
      if (cc.depth) bits |= kDepthBufferBit;
      if (bits) gl.Clear(bits);
      gl.Scissor(scissorRect[0], scissorRect[1], std::max(0, scissorRect[2]), std::max(0, scissorRect[3]));
      ++g_stats.clears;
    } break;
    case Op::Draw: {
      auto pit = g_pipelines.find(it.draw.pipeline);
      if (pit == g_pipelines.end()) {
        ++g_stats.skippedConfig;
        break;
      }
      const auto& pc = pit->second;
      if (pc.lineMode != 0 || it.draw.instanceCount != 1 || pc.cullMode == GX_CULL_ALL) {
        ++g_stats.skippedLine;
        break;
      }
      if (it.draw.uniformOffset % static_cast<uint32_t>(g_uboAlign) != 0 ||
          it.draw.uniformOffset + 16384u > f.uniforms.size()) {
        ++g_stats.skippedAlign;
        break;
      }
      const Program& prog = program_for(pc);
      if (!prog.ok) {
        ++g_stats.skippedProgram;
        break;
      }
      gl.UseProgram(prog.id);
      gl.BindBufferRange(kUniformBuffer, 0, ubo, static_cast<GLintptr>(it.draw.uniformOffset), 16384);
      apply_state(pc, it.draw.dstAlpha);
      if (debug_colors()) {
        static const float kPalette[12][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 0}, {1, 0, 1}, {0, 1, 1},
                                              {1, .5f, 0}, {.5f, 0, 1}, {0, .6f, .3f}, {.6f, .3f, 0}, {.3f, .3f, .3f},
                                              {1, 1, 1}};
        const float* c = kPalette[drawIndex % 12];
        gl.Uniform4f(prog.debugLoc, c[0], c[1], c[2], 1.f);
        gl.Disable(kBlend);
        gl.ColorMask(1, 1, 1, 1);
      }
      ++drawIndex;
      gl.DrawElements(kTriangles, static_cast<GLsizei>(it.draw.indexCount), kUnsignedShort,
                      reinterpret_cast<const void*>(static_cast<uintptr_t>(it.draw.idxOffset)));
      ++g_stats.draws;
    } break;
    }
  }
  end_pass();
  pipeLock.unlock();
  if (!presented) {
    // Ningún pase se copió: se muestra cómo quedó el último.
    present_efb(passW, passH, screenWidth, screenHeight);
  }
  gl.BindFramebuffer(kFramebuffer, 0);
  gl.Disable(kScissorTest);
  gl.ColorMask(1, 1, 1, 1);
  gl.DepthMask(1);

  g_stats.cpuMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cpuStart).count();
  if (++g_stats.frames % 300 == 0) {
    log_line("motor: KB por cuadro: vértices %.0f, uniformes %.0f, índices %.0f, arreglos %.0f; CPU del motor %.2f ms "
             "por cuadro (copia %.2f ms)",
             g_stats.bytesVerts / 300.0 / 1024, g_stats.bytesUniforms / 300.0 / 1024, g_stats.bytesIndices / 300.0 / 1024,
             g_stats.bytesStorage / 300.0 / 1024, g_stats.cpuMs / 300.0, g_stats.captureMs / 300.0);
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
} // namespace choya::gx
