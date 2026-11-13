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
#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <deque>
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <iterator>
#if defined(__linux__)
#include <sched.h>
#include <sys/syscall.h>
#endif
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
                 kDepthStencilAttachment = 0x821A, kRenderbuffer = 0x8D41, kDepth24Stencil8 = 0x88F0, kRgba8 = 0x8058, kTextureWrapS = 0x2802, kTextureWrapT = 0x2803,
                 kClampToEdge = 0x812F, kRepeat = 0x2901, kMirroredRepeat = 0x8370, kNearestMipmapNearest = 0x2700,
                 kLinearMipmapNearest = 0x2701, kNearestMipmapLinear = 0x2702, kLinearMipmapLinear = 0x2703,
                 kTextureMinLod = 0x813A, kTextureMaxLod = 0x813B, kRgba = 0x1908, kUnsignedByte = 0x1401,
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
  void (*CompressedTexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, const void*);
  void (*TexParameteri)(GLenum, GLenum, GLint);
  void (*ActiveTexture)(GLenum);
  void (*PixelStorei)(GLenum, GLint);
  void (*GenVertexArrays)(GLsizei, GLuint*);
  void (*BindVertexArray)(GLuint);
  void (*DrawElements)(GLenum, GLsizei, GLenum, const void*);
  void (*DrawArrays)(GLenum, GLint, GLsizei);
  void* (*FenceSync)(GLenum, GLbitfield);
  GLenum (*ClientWaitSync)(void*, GLbitfield, unsigned long long);
  void (*DeleteSync)(void*);
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
  void (*GenSamplers)(GLsizei, GLuint*);
  void (*BindSampler)(GLuint, GLuint);
  void (*SamplerParameteri)(GLuint, GLenum, GLint);
  void (*SamplerParameterf)(GLuint, GLenum, GLfloat);
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
  // Opcionales: búferes persistentes (GL_EXT_buffer_storage), como los anillos del renderer GLES de Nebula.
  void (*BufferStorageEXT)(GLenum, GLsizeiptr, const void*, GLbitfield);
  void* (*MapBufferRange)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
  GLboolean (*UnmapBuffer)(GLenum);
  void (*DeleteBuffers)(GLsizei, const GLuint*);
  // Opcionales: caché de programas en disco (GLES 3.0, como el caché de shaders que galaxy-vita trae listo).
  void (*GetProgramBinary)(GLuint, GLsizei, GLsizei*, GLenum*, void*);
  void (*ProgramBinary)(GLuint, GLenum, const void*, GLsizei);
  void (*ProgramParameteri)(GLuint, GLenum, GLint);
  const unsigned char* (*GetString)(GLenum);
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
  CHOYA_GL(CompressedTexSubImage2D);
  CHOYA_GL(TexParameteri);
  CHOYA_GL(ActiveTexture);
  CHOYA_GL(PixelStorei);
  CHOYA_GL(GenVertexArrays);
  CHOYA_GL(BindVertexArray);
  CHOYA_GL(DrawElements);
  CHOYA_GL(DrawArrays);
  CHOYA_GL(FenceSync);
  CHOYA_GL(ClientWaitSync);
  CHOYA_GL(DeleteSync);
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
  CHOYA_GL(GenSamplers);
  CHOYA_GL(BindSampler);
  CHOYA_GL(SamplerParameteri);
  CHOYA_GL(SamplerParameterf);
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
  // Opcionales (sin ellas se suben los datos como antes).
  if (g_loader != nullptr) {
    gl.BufferStorageEXT = reinterpret_cast<decltype(gl.BufferStorageEXT)>(g_loader("glBufferStorageEXT"));
    gl.MapBufferRange = reinterpret_cast<decltype(gl.MapBufferRange)>(g_loader("glMapBufferRange"));
    gl.UnmapBuffer = reinterpret_cast<decltype(gl.UnmapBuffer)>(g_loader("glUnmapBuffer"));
    gl.DeleteBuffers = reinterpret_cast<decltype(gl.DeleteBuffers)>(g_loader("glDeleteBuffers"));
    gl.GetProgramBinary = reinterpret_cast<decltype(gl.GetProgramBinary)>(g_loader("glGetProgramBinary"));
    gl.ProgramBinary = reinterpret_cast<decltype(gl.ProgramBinary)>(g_loader("glProgramBinary"));
    gl.ProgramParameteri = reinterpret_cast<decltype(gl.ProgramParameteri)>(g_loader("glProgramParameteri"));
    gl.GetString = reinterpret_cast<decltype(gl.GetString)>(g_loader("glGetString"));
  }
  return ok;
}

// --- Cuadro capturado -----------------------------------------------------------------------------------------
enum class Op : uint8_t { Pass, Viewport, Scissor, Draw, Clear, Copy };
struct Item {
  Op op;
  uint8_t pad[7];
  union {
    struct {
      uint32_t width, height;
      uint8_t clearColor, clearDepth, hasResolve, pad;
      float rgba[4];
      float depth;
      Rect copy;  // zona del EFB que se copia a pantalla (ancho 0 = todo el pase; volcados viejos)
    } pass;
    Viewport viewport;
    Rect scissor;
    struct {
      uint64_t pipeline;
      uint32_t idxOffset, indexCount, instanceCount, uniformOffset, dstAlpha, pad;
      uint64_t bindGroup;
    } draw;
    struct {
      uint64_t pipeline;
      float rgba[4];
      float depth;
      uint32_t useScissor;
      Rect scissor;
    } clear;
    // Copia del EFB a una textura (GXCopyTex) al terminar el pase: zona rect -> textura tex (texW x texH, fmt).
    struct {
      uint64_t tex;
      Rect rect;
      uint32_t texW, texH, fmt;
      uint8_t opaque, pad[3];
    } copy;
  };
};
static_assert(sizeof(Item) == 56);
struct Frame {
  std::vector<uint8_t> verts, uniforms, indices, storage;
  std::vector<Item> items;
  bool ready = false;
  bool present = true;  // false: lote partido a mitad de cuadro (se dibuja en el EFB sin mostrarse)
};
Frame g_recording;
// Cuadros listos para dibujar, en orden, en grupos: los lotes partidos (staging lleno) de un cuadro y el cuadro que
// los termina. Si el motor va atrasado, un grupo se salta cuando uno más nuevo hace todas sus copias del EFB (casi
// siempre: el juego rehace las mismas copias cada cuadro); uno con copias que nadie repite (las caras de los Miis se
// copian una sola vez) se dibuja aunque no se muestre.
struct Group {
  std::vector<Frame> frames;
  std::vector<uint64_t> copies;  // texturas que copia (ordenadas)
};
std::vector<Frame> g_open;   // lotes partidos del cuadro en curso
std::deque<Group> g_queue;
std::vector<Frame> g_pool;  // cuadros ya dibujados, para reusar sus búferes
void recycle(Frame&& f) {
  if (g_pool.size() < 4) g_pool.push_back(std::move(f));
}
Frame g_last;  // el último cuadro dibujado (para volcarlo)
bool g_recordingActive = false;
std::mutex g_frameMutex;

std::mutex g_pipelineMutex;
std::unordered_map<uint64_t, PipelineDesc> g_pipelines;
std::unordered_map<uint64_t, ClearDesc> g_clears;

struct Stats {
  uint64_t frames = 0, draws = 0, clears = 0, skippedLine = 0, skippedConfig = 0, skippedAlign = 0,
           skippedProgram = 0, passes = 0;
  uint64_t bytesVerts = 0, bytesUniforms = 0, bytesIndices = 0, bytesStorage = 0;
  uint64_t texUploads = 0, missingTextures = 0, copies = 0;
  uint64_t interval = 0;  // cuadros desde el último informe
  double cpuMs = 0, gpuWaitMs = 0;
  // Como el backend de galaxy-vita: estado GL que no cambió (no se llama a GL), dibujos que no pintan nada, programas.
  uint64_t stateSkipped = 0, stateApplied = 0, texBindsSkipped = 0, texBinds = 0, rejectedNoWrite = 0,
           rejectedScissor = 0, progCompiled = 0, progCached = 0, packUploads = 0;
  double compileMs = 0;
} g_stats;

std::unordered_map<uint64_t, BindDesc> g_bindGroups;

struct TexUpload {
  uint64_t id;
  uint32_t width, height, mips;
  std::vector<uint8_t> data;
  // Del paquete de recursos convertidos (assets.wlpk): formato de salida del conversor (0 = RGBA8 de aurora),
  // banderas de swizzle y dónde están los datos en el archivo (se leen en el hilo del motor).
  uint8_t packFormat = 0, packFlags = 0;
  uint64_t packOffset = 0, packSize = 0;
};
struct GlTexture {
  GLuint id = 0;
  uint32_t width = 0, height = 0, mips = 0;
  int kind = -1;     // TexKind
  size_t bytes = 0;  // memoria en GL (todas las mips)
};
// --- Paquete de recursos convertidos (assets.wlpk) ----------------------------------------------------------------
// El conversor de Choya (python -m choya convert) lee el disco, reduce cada textura para la pantalla de la RG35XX H
// y la recodifica a ETC2/EAC (o RGB565, RGBA8 si la compresión la dañaba). Aquí se busca cada textura que el juego
// carga por la misma clave que usa el conversor (crc32 del nivel 0 GX + tamaño + formato, choya/pack.py); si está,
// el motor sube esa versión en vez de la que aurora decodifica a RGBA8. Formato: choya/pack.py y runtime/include/wl.
#pragma pack(push, 1)
struct WlpkHeader {
  char magic[4];
  uint32_t version, entryCount, flags;
  uint64_t tocOffset, namesOffset, namesSize, manifestOffset, manifestSize, reserved;
};
struct WlpkEntry {
  uint8_t kind, format, count, flags;
  uint32_t nameOffset;
  uint64_t key, dataOffset, dataSize;
  uint16_t width, height;
  uint32_t extra0, extra1, extra2;
};
#pragma pack(pop)
static_assert(sizeof(WlpkHeader) == 64 && sizeof(WlpkEntry) == 48);
enum PackFormat : uint8_t {
  kPkEtc2Rgb8 = 1, kPkEtc2Rgba8, kPkRgb565, kPkRgba4444, kPkRgba5551, kPkR11Eac, kPkRg11Eac, kPkL8, kPkLa8, kPkRgba8
};
constexpr uint8_t kPkSwizzleI = 1, kPkSwizzleIA = 2;

struct PackState {
  FILE* file = nullptr;
  std::vector<WlpkEntry> textures;  // solo las texturas, ordenadas por clave
  std::atomic<uint64_t> hits{0}, misses{0}, bytes{0}, readErrors{0};
  std::mutex readMutex;
};
PackState& pack() {
  static PackState* p = new PackState;  // sin destruir: el juego puede cargar texturas hasta el final
  return *p;
}

uint32_t crc32_ieee(const uint8_t* data, size_t size) {
  static const auto table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < size; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

size_t pack_level_size(uint8_t fmt, uint32_t w, uint32_t h) {
  const size_t blocks = static_cast<size_t>((w + 3) / 4) * ((h + 3) / 4);
  switch (fmt) {
  case kPkEtc2Rgb8: case kPkR11Eac: return blocks * 8;
  case kPkEtc2Rgba8: case kPkRg11Eac: return blocks * 16;
  case kPkRgb565: case kPkRgba4444: case kPkRgba5551: case kPkLa8: return static_cast<size_t>(w) * h * 2;
  case kPkL8: return static_cast<size_t>(w) * h;
  case kPkRgba8: return static_cast<size_t>(w) * h * 4;
  default: return 0;
  }
}

// Compartido entre el juego y el motor. En el heap y sin destruir: aurora libera texturas hasta el final.
struct TexShared {
  std::mutex mutex;
  std::vector<TexUpload> pending;                 // del juego al hilo del motor
  std::vector<uint64_t> released;
  std::unordered_map<uint64_t, TexUpload> kept;   // copias para volcados (CHOYA_GX_VOLCADO)
  bool keep = false;
};
TexShared& texs() {
  static TexShared* s = new TexShared;
  return *s;
}
std::unordered_map<uint64_t, GlTexture> g_textures;  // solo en el hilo del motor
// Texturas que son copias del EFB (GXCopyTex): las llena el motor en la GPU, con su framebuffer.
struct CopyTexture {
  GLuint tex = 0, fbo = 0;
  uint32_t width = 0, height = 0;
};
std::unordered_map<uint64_t, CopyTexture> g_copyTextures;  // solo en el hilo del motor
GLuint g_whiteTexture = 0;
std::atomic<uint64_t> g_bindGen{1};  // sube cuando register_bind_group cambia un grupo ya visto
uint64_t g_texGen = 1;  // sube cuando cambia un nombre GL de textura (subidas nuevas, copias, borrados)
std::unordered_map<uint64_t, GLuint> g_samplers;

bool find_bind_group(uint64_t ref, BindDesc& out) {
  std::lock_guard lock(g_pipelineMutex);
  auto it = g_bindGroups.find(ref);
  if (it == g_bindGroups.end()) return false;
  out = it->second;
  return true;
}

bool find_pipeline(uint64_t ref, PipelineDesc& out) {
  std::lock_guard lock(g_pipelineMutex);
  auto it = g_pipelines.find(ref);
  if (it == g_pipelines.end()) return false;
  out = it->second;
  return true;
}

bool find_clear(uint64_t ref, ClearDesc& out) {
  std::lock_guard lock(g_pipelineMutex);
  auto it = g_clears.find(ref);
  if (it == g_clears.end()) return false;
  out = it->second;
  return true;
}

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

std::string vertex_color(const PipelineDesc& cfg, u32 channel) {
  const int attr = vertex_color_attr(cfg, channel);
  return attr >= 0 ? color_attr(cfg, static_cast<GXAttr>(attr)) : "vec4(1.0)";
}

std::string num(u32 v) { return std::to_string(v) + "u"; }
std::string uvec(u32 word) { return "uv4(" + num(word) + ")"; }

// Componente de un canal según la tabla de intercambio (GX_CH_RED..GX_CH_ALPHA).
char swz(u8 ch) { return "rgba"[ch & 3]; }

// --- TEV en flotante (motor de Choya). El TEV exacto en enteros llega en el paso 2b-2. ---
std::string tev_kc(u8 sel) {
  if (sel <= GX_TEV_KCSEL_1_8) return "vec3(" + std::to_string(8 - sel) + ".0 / 8.0)";
  if (sel >= GX_TEV_KCSEL_K0 && sel <= GX_TEV_KCSEL_K3) return "k" + std::to_string(sel - GX_TEV_KCSEL_K0) + ".rgb";
  if (sel >= GX_TEV_KCSEL_K0_R && sel <= GX_TEV_KCSEL_K3_A) {
    const int k = (sel - GX_TEV_KCSEL_K0_R) & 3, c = (sel - GX_TEV_KCSEL_K0_R) >> 2;
    return std::string("vec3(k") + std::to_string(k) + "." + "rgba"[c] + ")";
  }
  return "vec3(1.0)";
}
std::string tev_ka(u8 sel) {
  if (sel <= GX_TEV_KASEL_1_8) return "(" + std::to_string(8 - sel) + ".0 / 8.0)";
  if (sel >= GX_TEV_KASEL_K0_R && sel <= GX_TEV_KASEL_K3_A) {
    const int k = (sel - GX_TEV_KASEL_K0_R) & 3, c = (sel - GX_TEV_KASEL_K0_R) >> 2;
    return std::string("k") + std::to_string(k) + "." + "rgba"[c];
  }
  return "1.0";
}

std::string tev_color_arg(const PipelineDesc& cfg, const TevStageDesc& s, u32 idx, u8 arg) {
  const bool hasTex = s.texMap != 0xFF;
  const u8* tsw = cfg.swap[s.swapTex & 3];
  const u8* rsw = cfg.swap[s.swapRas & 3];
  const std::string ras = s.channel == GX_COLOR1A1 || s.channel == GX_COLOR1 || s.channel == GX_ALPHA1 ? "rast1" : "rast0";
  const bool noRas = s.channel == GX_COLOR_ZERO || s.channel == GX_COLOR_NULL || s.channel == GX_ALPHA_BUMP ||
                     s.channel == GX_ALPHA_BUMPN;
  const std::string si = "s" + std::to_string(idx);
  switch (arg) {
  case GX_CC_CPREV: return "prev.rgb";
  case GX_CC_APREV: return "vec3(prev.a)";
  case GX_CC_C0: return "r0.rgb";
  case GX_CC_A0: return "vec3(r0.a)";
  case GX_CC_C1: return "r1.rgb";
  case GX_CC_A1: return "vec3(r1.a)";
  case GX_CC_C2: return "r2.rgb";
  case GX_CC_A2: return "vec3(r2.a)";
  case GX_CC_TEXC:
    if (!hasTex) return cfg.numTexGens == 0 ? "vec3(0.0)" : "vec3(1.0)";
    return si + "." + swz(tsw[0]) + swz(tsw[1]) + swz(tsw[2]);
  case GX_CC_TEXA:
    if (!hasTex) return cfg.numTexGens == 0 ? "vec3(0.0)" : "vec3(1.0)";
    return "vec3(" + si + "." + swz(tsw[3]) + ")";
  case GX_CC_RASC:
    if (noRas) return "vec3(0.0)";
    return ras + "." + swz(rsw[0]) + swz(rsw[1]) + swz(rsw[2]);
  case GX_CC_RASA:
    if (noRas) return "vec3(0.0)";
    return "vec3(" + ras + "." + swz(rsw[3]) + ")";
  case GX_CC_ONE: return "vec3(1.0)";
  case GX_CC_HALF: return "vec3(0.5)";
  case GX_CC_KONST: return tev_kc(s.kcSel);
  default: return "vec3(0.0)";
  }
}

std::string tev_alpha_arg(const PipelineDesc& cfg, const TevStageDesc& s, u32 idx, u8 arg) {
  const bool hasTex = s.texMap != 0xFF;
  const u8* tsw = cfg.swap[s.swapTex & 3];
  const u8* rsw = cfg.swap[s.swapRas & 3];
  const std::string ras = s.channel == GX_COLOR1A1 || s.channel == GX_COLOR1 || s.channel == GX_ALPHA1 ? "rast1" : "rast0";
  const bool noRas = s.channel == GX_COLOR_ZERO || s.channel == GX_COLOR_NULL || s.channel == GX_ALPHA_BUMP ||
                     s.channel == GX_ALPHA_BUMPN;
  switch (arg) {
  case GX_CA_APREV: return "prev.a";
  case GX_CA_A0: return "r0.a";
  case GX_CA_A1: return "r1.a";
  case GX_CA_A2: return "r2.a";
  case GX_CA_TEXA:
    if (!hasTex) return cfg.numTexGens == 0 ? "0.0" : "1.0";
    return "s" + std::to_string(idx) + "." + swz(tsw[3]);
  case GX_CA_RASA:
    if (noRas) return "0.0";
    return ras + "." + swz(rsw[3]);
  case GX_CA_KONST: return tev_ka(s.kaSel);
  default: return "0.0";
  }
}

const char* tev_reg(u8 r) {
  switch (r) {
  case GX_TEVREG0: return "r0";
  case GX_TEVREG1: return "r1";
  case GX_TEVREG2: return "r2";
  default: return "prev";
  }
}

std::string tev_bias(u8 bias) { return bias == GX_TB_ADDHALF ? " + 0.5" : bias == GX_TB_SUBHALF ? " - 0.5" : ""; }
std::string tev_scale(u8 scale) {
  switch (scale) {
  case GX_CS_SCALE_2: return " * 2.0";
  case GX_CS_SCALE_4: return " * 4.0";
  case GX_CS_DIVIDE_2: return " * 0.5";
  default: return "";
  }
}

// Una operación TEV (color con vec3, alfa con float).
std::string tev_op(bool alpha, u8 op, u8 bias, u8 scale, u8 clampOut, const std::string& a, const std::string& b,
                   const std::string& c, const std::string& d) {
  const std::string T = alpha ? "float" : "vec3";
  std::string v;
  if (op == GX_TEV_ADD || op == GX_TEV_SUB) {
    const std::string lerp = "mix(" + a + ", " + b + ", " + c + ")";
    v = "((" + d + (op == GX_TEV_SUB ? " - " : " + ") + lerp + ")" + tev_bias(bias) + ")" + tev_scale(scale);
  } else {
    // Comparaciones: d + (a ? b : 0) ? c : 0, con valores de 8 bits (sin escala ni sesgo).
    auto q = [](const std::string& x) { return "floor(" + x + " * 255.0 + 0.5)"; };
    std::string cond;
    switch (op) {
    case GX_TEV_COMP_R8_GT: cond = q(a + ".r") + " > " + q(b + ".r"); break;
    case GX_TEV_COMP_R8_EQ: cond = q(a + ".r") + " == " + q(b + ".r"); break;
    case GX_TEV_COMP_GR16_GT:
      cond = "dot(" + q(a + ".rg") + ", vec2(1.0, 256.0)) > dot(" + q(b + ".rg") + ", vec2(1.0, 256.0))"; break;
    case GX_TEV_COMP_GR16_EQ: cond = "all(equal(" + q(a + ".rg") + ", " + q(b + ".rg") + "))"; break;
    case GX_TEV_COMP_BGR24_GT:
      cond = "dot(" + q(a + ".rgb") + ", vec3(1.0, 256.0, 65536.0)) > dot(" + q(b + ".rgb") + ", vec3(1.0, 256.0, 65536.0))";
      break;
    case GX_TEV_COMP_BGR24_EQ: cond = "all(equal(" + q(a + ".rgb") + ", " + q(b + ".rgb") + "))"; break;
    default:  // RGB8 / A8 por componente
      if (alpha) {
        cond = q(a) + (op == GX_TEV_COMP_A8_GT ? " > " : " == ") + q(b);
      } else {
        const std::string cmp = op == GX_TEV_COMP_RGB8_GT ? "greaterThan(" : "equal(";
        return "(" + d + " + " + c + " * vec3(" + cmp + q(a) + ", " + q(b) + ")))";
      }
      break;
    }
    v = "(" + d + " + ((" + cond + ") ? " + c + " : " + T + "(0.0)))";
  }
  return clampOut ? "clamp(" + v + ", 0.0, 1.0)" : "clamp(" + v + ", -4.0, 4.0)";
}

std::string alpha_test(u8 comp, u8 ref) {
  const std::string r = std::to_string(ref) + ".0";
  const std::string a = "floor(prev.a * 255.0 + 0.5)";
  switch (comp) {
  case GX_NEVER: return "false";
  case GX_LESS: return a + " < " + r;
  case GX_EQUAL: return a + " == " + r;
  case GX_LEQUAL: return a + " <= " + r;
  case GX_GREATER: return a + " > " + r;
  case GX_NEQUAL: return a + " != " + r;
  case GX_GEQUAL: return a + " >= " + r;
  default: return "true";
  }
}

void build_sources(const PipelineDesc& cfg, std::string& vs, std::string& fs) {
  // ---------------- vertex shader ----------------
  vs = "#version 320 es\nprecision highp float;\nprecision highp int;\n"
       "uniform highp usampler2D vbuf;\nuniform highp usampler2D abuf;\n"
       "layout(std140) uniform U { uvec4 u[1024]; };\nout vec4 v_color0;\nout vec4 v_color1;\n";
  for (u32 i = 0; i < 8; ++i) {
    if ((cfg.sampledTexCoords >> i) & 1) vs += "out vec3 v_tex" + std::to_string(i) + ";\n";
  }
  vs += kHelpers;
  vs += "void main() {\n  uint vidx = uint(gl_VertexID);\n  uint vtx_start = uw(0u);\n";
  auto raw_u8 = [&](GXAttr attr) {
    char buf;
    std::string offs;
    bool le;
    attr_address(cfg, attr, buf, offs, le);
    return "lb_" + std::string(1, buf) + "(" + offs + ")";
  };
  if (cfg.attrs[GX_VA_PNMTXIDX].attrType != GX_NONE) {
    vs += "  uint pnmtx = " + raw_u8(GX_VA_PNMTXIDX) + " / 3u;\n";
  } else {
    vs += "  uint pnmtx = uw(1u);\n";
  }
  const auto& pos = cfg.attrs[GX_VA_POS];
  if (pos.cnt == 2) {
    vs += "  vec3 in_pos = vec3(" + vec_attr(cfg, GX_VA_POS, 2) + ", 0.0);\n";
  } else {
    vs += "  vec3 in_pos = " + vec_attr(cfg, GX_VA_POS, 3) + ";\n";
  }
  vs += "  uint m = " + num(cfg.offPostex) + " + pnmtx * 12u;\n"
        "  vec4 p = vec4(in_pos, 1.0);\n"
        "  vec4 mv = vec4(dot(p, uv4(m)), dot(p, uv4(m + 4u)), dot(p, uv4(m + 8u)), 1.0);\n"
        "  gl_Position = vec4(dot(mv, " + uvec(cfg.offProj) + "), dot(mv, " + uvec(cfg.offProj + 4u) + "), dot(mv, " +
        uvec(cfg.offProj + 8u) + "), dot(mv, " + uvec(cfg.offProj + 12u) + "));\n";

  // Canales de color (sin iluminación todavía): material o color del vértice.
  for (u32 ch = 0; ch < 2; ++ch) {
    const std::string out = "v_color" + std::to_string(ch);
    if (((cfg.sampledChannels >> ch) & 1) == 0) {
      vs += "  " + out + " = vec4(1.0);\n";
      continue;
    }
    const auto& cc = cfg.chans[ch];
    const auto& cca = cfg.chans[ch + GX_ALPHA0];
    std::string vc = "vc" + std::to_string(ch);
    if (cc.matSrc == GX_SRC_VTX || cca.matSrc == GX_SRC_VTX) vs += "  vec4 " + vc + " = " + vertex_color(cfg, ch) + ";\n";
    const std::string rgb = cc.matSrc == GX_SRC_VTX ? vc + ".rgb" : uvec(cfg.offChanMat[ch]) + ".rgb";
    const std::string a = cca.matSrc == GX_SRC_VTX ? vc + ".a" : uvec(cfg.offChanMatA[ch]) + ".a";
    vs += "  " + out + " = vec4(" + rgb + ", " + a + ");\n";
  }

  // Coordenadas de textura.
  for (u32 i = 0; i < 8; ++i) {
    if (((cfg.sampledTexCoords >> i) & 1) == 0) continue;
    const auto& tg = cfg.texgens[i];
    const std::string tc = "tc" + std::to_string(i);
    auto tex_src = [&](u32 n) {
      const GXAttr attr = static_cast<GXAttr>(GX_VA_TEX0 + n);
      const auto& a = cfg.attrs[attr];
      if (a.attrType == GX_NONE) return std::string("vec4(0.0, 0.0, 1.0, 1.0)");
      if (a.cnt == 1) return "vec4(" + vec_attr(cfg, attr, 1) + ".x, 0.0, 1.0, 1.0)";
      return "vec4(" + vec_attr(cfg, attr, 2) + ", 1.0, 1.0)";
    };
    if (tg.src >= GX_TG_TEX0 && tg.src <= GX_TG_TEX7) {
      vs += "  vec4 " + tc + " = " + tex_src(tg.src - GX_TG_TEX0) + ";\n";
    } else if (tg.src == GX_MAX_TEXGENSRC) {
      vs += "  vec4 " + tc + " = " + tex_src(i) + ";\n";
    } else if (tg.src == GX_TG_COLOR0 || tg.src == GX_TG_COLOR1) {
      vs += "  vec4 " + tc + " = vec4(" + vertex_color(cfg, tg.src - GX_TG_COLOR0) + ".rgb, 1.0);\n";
    } else {  // posición (y por ahora también normales y tangentes)
      vs += "  vec4 " + tc + " = vec4(in_pos, 1.0);\n";
    }
    std::string tmp;
    if (tg.type == GX_TG_MTX2x4 || tg.type == GX_TG_MTX3x4) {
      std::string base;
      if (tg.mtxSlot == 0xFE) {
        vs += "  uint tm" + std::to_string(i) + " = " + num(cfg.offPostex) + " + (" +
              raw_u8(static_cast<GXAttr>(GX_VA_TEX0MTXIDX + i)) + " / 3u) * 12u;\n";
        base = "tm" + std::to_string(i);
      } else if (tg.mtxSlot != 0xFF) {
        base = num(cfg.offPostex + 12u * tg.mtxSlot);
      }
      if (base.empty()) {
        tmp = tc + ".xyz";
      } else {
        tmp = "vec3(dot(" + tc + ", uv4(" + base + ")), dot(" + tc + ", uv4(" + base + " + 4u)), dot(" + tc + ", uv4(" +
              base + " + 8u)))";
      }
      if (tg.type == GX_TG_MTX2x4) tmp = "vec3((" + tmp + ").xy, 1.0)";
    } else {  // SRTG y otros
      tmp = "vec3(" + tc + ".xy, 1.0)";
    }
    const std::string out = "v_tex" + std::to_string(i);
    vs += "  " + out + " = " + tmp + ";\n";
    if (tg.type == GX_TG_MTX2x4 || tg.type == GX_TG_MTX3x4) {
      vs += "  if (" + out + ".z == 0.0) { " + out + ".xy = clamp(" + out + ".xy / 2.0, -1.0, 1.0); }\n";
    }
  }
  vs += "}\n";

  // ---------------- fragment shader ----------------
  fs = "#version 320 es\nprecision highp float;\nprecision highp int;\n"
       "layout(std140) uniform U { uvec4 u[1024]; };\nin vec4 v_color0;\nin vec4 v_color1;\n";
  for (u32 i = 0; i < 8; ++i) {
    if ((cfg.sampledTexCoords >> i) & 1) fs += "in vec3 v_tex" + std::to_string(i) + ";\n";
  }
  for (u32 i = 0; i < 8; ++i) fs += "uniform sampler2D tex" + std::to_string(i) + ";\n";
  fs += "uniform vec4 u_debug;\nlayout(location = 0) out vec4 o_color;\n"
        "vec4 uv4(uint w) { return uintBitsToFloat(u[w >> 2u]); }\n"
        "void main() {\n  vec4 rast0 = v_color0;\n  vec4 rast1 = v_color1;\n";
  for (u32 r = 0; r < 4; ++r) {
    const std::string name = r == 0 ? "prev" : "r" + std::to_string(r - 1);
    fs += "  vec4 " + name + " = " + (cfg.offTevReg[r] ? uvec(cfg.offTevReg[r]) : std::string("vec4(0.0)")) + ";\n";
  }
  for (u32 k = 0; k < 4; ++k) {
    fs += "  vec4 k" + std::to_string(k) + " = " + (cfg.offKColor[k] ? uvec(cfg.offKColor[k]) : std::string("vec4(1.0)")) +
          ";\n";
  }
  for (u32 i = 0; i < cfg.tevStageCount && i < 16; ++i) {
    const auto& s = cfg.stages[i];
    const std::string si = "s" + std::to_string(i);
    if (s.texMap != 0xFF && s.texCoord < 8) {
      const std::string tci = std::to_string(s.texCoord), tmi = std::to_string(s.texMap);
      const std::string scale = cfg.offTexScale[s.texCoord] ? uvec(cfg.offTexScale[s.texCoord]) + ".xy" : "vec2(1.0)";
      const std::string size = cfg.offTexSize[s.texMap] ? uvec(cfg.offTexSize[s.texMap]) : "vec4(1.0, 1.0, 0.0, 0.0)";
      const auto& tg = cfg.texgens[s.texCoord];
      const std::string uv = tg.type == GX_TG_MTX3x4
                                 ? "(v_tex" + tci + ".xy / (v_tex" + tci + ".z == 0.0 ? 1.0 : v_tex" + tci + ".z))"
                                 : "v_tex" + tci + ".xy";
      fs += "  vec4 sz" + std::to_string(i) + " = " + size + ";\n";
      fs += "  vec4 " + si + " = texture(tex" + tmi + ", " + uv + " * " + scale + " / sz" + std::to_string(i) +
            ".xy, sz" + std::to_string(i) + ".z);\n";
    } else {
      fs += "  vec4 " + si + " = vec4(1.0);\n";
    }
    const std::string ca = tev_color_arg(cfg, s, i, s.ca), cb = tev_color_arg(cfg, s, i, s.cb),
                      cc = tev_color_arg(cfg, s, i, s.cc), cd = tev_color_arg(cfg, s, i, s.cd);
    const std::string aa = tev_alpha_arg(cfg, s, i, s.aa), ab = tev_alpha_arg(cfg, s, i, s.ab),
                      ac = tev_alpha_arg(cfg, s, i, s.ac), ad = tev_alpha_arg(cfg, s, i, s.ad);
    // Las dos operaciones leen los registros de antes de la etapa.
    fs += "  {\n    vec3 c_out = " + tev_op(false, s.cop, s.cbias, s.cscale, s.cclamp, ca, cb, cc, cd) + ";\n";
    fs += "    float a_out = " + tev_op(true, s.aop, s.abias, s.ascale, s.aclamp, aa, ab, ac, ad) + ";\n";
    fs += std::string("    ") + tev_reg(s.cout) + ".rgb = c_out;\n    " + tev_reg(s.aout) + ".a = a_out;\n  }\n";
  }
  // Comparación de alfa.
  if (cfg.alphaComp0 != GX_ALWAYS || cfg.alphaComp1 != GX_ALWAYS) {
    const std::string t0 = "(" + alpha_test(cfg.alphaComp0, cfg.alphaRef0) + ")";
    const std::string t1 = "(" + alpha_test(cfg.alphaComp1, cfg.alphaRef1) + ")";
    std::string pass;
    switch (cfg.alphaOp) {
    case GX_AOP_OR: pass = t0 + " || " + t1; break;
    case GX_AOP_XOR: pass = t0 + " != " + t1; break;
    case GX_AOP_XNOR: pass = t0 + " == " + t1; break;
    default: pass = t0 + " && " + t1; break;
    }
    fs += "  if (!(" + pass + ")) discard;\n";
  }
  fs += debug_colors() ? "  o_color = u_debug;\n}\n" : "  o_color = clamp(prev, 0.0, 1.0);\n}\n";
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

// --- Caché de programas en disco ------------------------------------------------------------------------------
// galaxy-vita trae los shaders ya compilados y los prepara en la pantalla de carga para que no haya tirones a mitad
// del juego. Aquí no se puede traer nada hecho (cada driver compila a su modo), pero el resultado sí se guarda: el
// primer encuentro con un programa lo compila (decenas de ms en la Mali) y lo escribe en CHOYA_GX_CACHE; las
// siguientes veces se carga el binario (glProgramBinary) en una fracción de eso. La clave es el texto de los dos
// shaders más el driver, así que un cambio del generador o del driver nunca carga un binario viejo.
constexpr GLenum kProgramBinaryLength = 0x8741, kNumProgramBinaryFormats = 0x87FE,
                 kProgramBinaryRetrievableHint = 0x8257;
std::string g_cacheDir;
bool g_cacheTried = false, g_cacheOn = false;
uint64_t g_driverHash = 0;
int g_cacheWriteErrors = 0;

uint64_t fnv64(const void* data, size_t size, uint64_t h = 1469598103934665603ull) {
  const auto* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}

void cache_init() {
  if (g_cacheTried) return;
  g_cacheTried = true;
  const char* dir = std::getenv("CHOYA_GX_CACHE");
  if (dir == nullptr || dir[0] == '\0' || std::strcmp(dir, "0") == 0) return;
  if (gl.GetProgramBinary == nullptr || gl.ProgramBinary == nullptr || gl.GetString == nullptr) {
    log_line("motor: caché de programas apagado (el driver no tiene glProgramBinary)");
    return;
  }
  GLint formats = 0;
  gl.GetIntegerv(kNumProgramBinaryFormats, &formats);
  if (formats <= 0) {
    log_line("motor: caché de programas apagado (el driver no da formatos binarios)");
    return;
  }
  for (GLenum e : {GLenum{0x1F00}, GLenum{0x1F01}, GLenum{0x1F02}}) {  // fabricante, renderer, versión
    const unsigned char* str = gl.GetString(e);
    const char* text = str != nullptr ? reinterpret_cast<const char*>(str) : "";
    g_driverHash = fnv64(text, std::strlen(text) + 1, g_driverHash ? g_driverHash : 1469598103934665603ull);
  }
  g_cacheDir = dir;
  ::mkdir(dir, 0755);
  size_t files = 0;
  if (DIR* d = ::opendir(dir)) {
    while (const dirent* e = ::readdir(d)) {
      if (std::strstr(e->d_name, ".bin") != nullptr) ++files;
    }
    ::closedir(d);
    g_cacheOn = true;
  }
  log_line("motor: caché de programas en %s (%zu guardados)%s", dir, files, g_cacheOn ? "" : ": no pude abrir la carpeta");
}

std::string cache_path(uint64_t h) {
  char name[32];
  std::snprintf(name, sizeof(name), "/%016llx.bin", static_cast<unsigned long long>(h));
  return g_cacheDir + name;
}

struct CacheHeader {
  char magic[4];  // CHPB
  uint32_t format, length;
  uint32_t pad;
};

bool cache_load(GLuint prog, uint64_t h) {
  if (!g_cacheOn) return false;
  FILE* f = std::fopen(cache_path(h).c_str(), "rb");
  if (f == nullptr) return false;
  CacheHeader hd{};
  std::vector<uint8_t> data;
  bool ok = std::fread(&hd, sizeof(hd), 1, f) == 1 && std::memcmp(hd.magic, "CHPB", 4) == 0 && hd.length > 0 &&
            hd.length < (64u << 20);
  if (ok) {
    data.resize(hd.length);
    ok = std::fread(data.data(), 1, data.size(), f) == data.size();
  }
  std::fclose(f);
  if (!ok) return false;
  gl.ProgramBinary(prog, hd.format, data.data(), static_cast<GLsizei>(data.size()));
  GLint linked = 0;
  gl.GetProgramiv(prog, kLinkStatus, &linked);
  if (!linked) std::remove(cache_path(h).c_str());  // el driver ya no lo acepta: se compila y se reescribe
  return linked != 0;
}

void cache_store(GLuint prog, uint64_t h) {
  if (!g_cacheOn || g_cacheWriteErrors > 3) return;
  GLint length = 0;
  gl.GetProgramiv(prog, kProgramBinaryLength, &length);
  if (length <= 0) return;
  std::vector<uint8_t> data(static_cast<size_t>(length));
  GLsizei got = 0;
  GLenum format = 0;
  gl.GetProgramBinary(prog, length, &got, &format, data.data());
  if (got <= 0) return;
  const std::string path = cache_path(h), tmp = path + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  CacheHeader hd{{'C', 'H', 'P', 'B'}, format, static_cast<uint32_t>(got), 0};
  bool ok = f != nullptr && std::fwrite(&hd, sizeof(hd), 1, f) == 1 &&
            std::fwrite(data.data(), 1, static_cast<size_t>(got), f) == static_cast<size_t>(got);
  if (f != nullptr) ok = std::fclose(f) == 0 && ok;
  ok = ok && std::rename(tmp.c_str(), path.c_str()) == 0;  // completo o nada (si se apaga a mitad)
  if (!ok) {
    std::remove(tmp.c_str());
    if (++g_cacheWriteErrors == 1) log_line("motor: no pude guardar en el caché de programas (%s)", path.c_str());
  }
}

// Lo que GL olvida al cargar un binario o enlazar: las unidades de los samplers y el bloque de uniformes.
void setup_program(Program& prog) {
  prog.ok = true;
  gl.UseProgram(prog.id);
  gl.Uniform1i(gl.GetUniformLocation(prog.id, "vbuf"), 0);
  gl.Uniform1i(gl.GetUniformLocation(prog.id, "abuf"), 1);
  prog.debugLoc = gl.GetUniformLocation(prog.id, "u_debug");
  for (int i = 0; i < 8; ++i) {
    const std::string name = "tex" + std::to_string(i);
    gl.Uniform1i(gl.GetUniformLocation(prog.id, name.c_str()), 2 + i);  // unidades 2..9 (0 y 1: vbuf, abuf)
  }
  const GLuint block = gl.GetUniformBlockIndex(prog.id, "U");
  if (block != kInvalidIndex) gl.UniformBlockBinding(prog.id, block, 0);
}

const Program& program_for(const PipelineDesc& cfg) {
  const uint64_t key = shader_key(cfg);
  auto it = g_programs.find(key);
  if (it != g_programs.end()) return it->second;
  const auto start = std::chrono::steady_clock::now();
  cache_init();
  Program prog;
  std::string vs, fs, err;
  build_sources(cfg, vs, fs);
  uint64_t h = fnv64(vs.data(), vs.size() + 1, g_driverHash ? g_driverHash : 1469598103934665603ull);
  h = fnv64(fs.data(), fs.size() + 1, h);
  prog.id = gl.CreateProgram();
  if (cache_load(prog.id, h)) {
    setup_program(prog);
    if (g_stats.progCached++ == 0 && g_programs.empty()) log_line("motor: programas cargados del caché de disco");
    return g_programs.emplace(key, prog).first->second;
  }
  const GLuint v = compile(kVertexShader, vs, err);
  const GLuint f = v ? compile(kFragmentShader, fs, err) : 0;
  if (v && f) {
    gl.AttachShader(prog.id, v);
    gl.AttachShader(prog.id, f);
    if (g_cacheOn && gl.ProgramParameteri != nullptr) gl.ProgramParameteri(prog.id, kProgramBinaryRetrievableHint, 1);
    gl.LinkProgram(prog.id);
    GLint ok = 0;
    gl.GetProgramiv(prog.id, kLinkStatus, &ok);
    if (!ok) {
      GLint len = 0;
      gl.GetProgramiv(prog.id, kInfoLogLength, &len);
      err.assign(static_cast<size_t>(len > 1 ? len : 1), '\0');
      gl.GetProgramInfoLog(prog.id, len, nullptr, err.data());
    } else {
      setup_program(prog);
      cache_store(prog.id, h);
    }
  }
  if (v) gl.DeleteShader(v);
  if (f) gl.DeleteShader(f);
  ++g_stats.progCompiled;
  g_stats.compileMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  if (!prog.ok && g_programErrors++ < 3) {
    log_line("motor: programa %016llx no compiló: %s", static_cast<unsigned long long>(key), err.c_str());
    log_line("motor: vertex shader:\n%s", vs.c_str());
    log_line("motor: fragment shader:\n%s", fs.c_str());
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

// Estado de render de un pipeline, calculado una vez por pipeline (no en cada dibujo).
struct StateBlock {
  GLenum depthFunc, cs, cd, as, ad, ceq, aeq;
  uint8_t depthMask, blend, colorMask, alphaMask;
  uint32_t dstAlpha;  // UINT32_MAX: sin alfa constante
  bool operator==(const StateBlock&) const = default;
};

StateBlock make_state(const PipelineDesc& c, uint32_t dstAlpha) {
  StateBlock st{};
  // Profundidad: GL solo escribe profundidad con la prueba activa, así que "sin comparar" es ALWAYS.
  st.depthFunc = c.depthCompare ? compare(c.depthFunc) : kAlways;
  st.depthMask = c.depthUpdate ? 1 : 0;
  const bool hasAlpha = c.pixelFmt == GX_PF_RGBA6_Z24;
  st.colorMask = c.colorUpdate ? 1 : 0;
  st.alphaMask = c.alphaUpdate && hasAlpha ? 1 : 0;
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
  st.dstAlpha = dstAlpha;
  if (dstAlpha != UINT32_MAX) {
    as = kConstantAlpha;
    ad = kZero;
    aeq = kFuncAdd;
    blend = true;
  }
  st.blend = blend ? 1 : 0;
  if (!blend) cs = cd = as = ad = ceq = aeq = 0;  // sin mezcla los factores no importan (no se comparan)
  st.cs = cs;
  st.cd = cd;
  st.as = as;
  st.ad = ad;
  st.ceq = ceq;
  st.aeq = aeq;
  return st;
}

// --- Estado GL en sombra (como el GlState de galaxy-vita) ---------------------------------------------------------
// Nada de abajo llama a GL si el valor no cambia. Con ~19000 dibujos por cuadro en Golf eran cientos de miles de
// llamadas al driver de la Mali por cuadro solo para repetir el mismo estado. shadow_invalidate() después de
// cualquier cosa que toque GL por fuera (copias del EFB, la copia a pantalla, subidas de texturas).
struct GlShadow {
  GLuint program;
  GLuint tex[8], samp[8];
  int unit;
  int8_t depthTest, blendOn, cullOn, scissorOn, depthMask, colorMask, alphaMask;
  GLenum depthFunc;
  GLenum blend[6];
  float blendAlpha;
  GLint vp[4], sc[4];
  float dr[2];
  GLuint uboBuf;
  GLintptr uboOffset;
  StateBlock last;
  bool lastValid;
};
GlShadow S;

void shadow_invalidate() {
  std::memset(&S, 0xFF, sizeof(S));  // ningún valor válido de GL es todo unos
  S.lastValid = false;
  S.dr[0] = S.dr[1] = -1.f;
  S.blendAlpha = -1.f;
}

void set_enable(GLenum cap, int8_t& cur, bool on) {
  if (cur == (on ? 1 : 0)) return;
  cur = on ? 1 : 0;
  S.lastValid = false;
  if (on) gl.Enable(cap);
  else gl.Disable(cap);
}
void set_depth_mask(bool on) {
  if (S.depthMask == (on ? 1 : 0)) return;
  S.depthMask = on ? 1 : 0;
  S.lastValid = false;
  gl.DepthMask(on ? 1 : 0);
}
void set_color_mask(bool color, bool alpha) {
  if (S.colorMask == (color ? 1 : 0) && S.alphaMask == (alpha ? 1 : 0)) return;
  S.colorMask = color ? 1 : 0;
  S.alphaMask = alpha ? 1 : 0;
  S.lastValid = false;
  gl.ColorMask(color, color, color, alpha);
}
void set_viewport(const GLint r[4]) {
  if (std::memcmp(S.vp, r, sizeof(S.vp)) == 0) return;
  std::memcpy(S.vp, r, sizeof(S.vp));
  gl.Viewport(r[0], r[1], r[2], r[3]);
}
void set_scissor(const GLint r[4]) {
  const GLint c[4] = {r[0], r[1], std::max(0, r[2]), std::max(0, r[3])};
  if (std::memcmp(S.sc, c, sizeof(S.sc)) == 0) return;
  std::memcpy(S.sc, c, sizeof(S.sc));
  gl.Scissor(c[0], c[1], c[2], c[3]);
}
void set_depth_range(float n, float f) {
  if (S.dr[0] == n && S.dr[1] == f) return;
  S.dr[0] = n;
  S.dr[1] = f;
  gl.DepthRangef(n, f);
}
void set_program(GLuint id) {
  if (S.program == id) return;
  S.program = id;
  gl.UseProgram(id);
}
void set_ubo(GLuint buf, GLintptr offset) {
  if (S.uboBuf == buf && S.uboOffset == offset) return;
  S.uboBuf = buf;
  S.uboOffset = offset;
  gl.BindBufferRange(kUniformBuffer, 0, buf, offset, 16384);
}

void apply_state(const StateBlock& st) {
  if (S.lastValid && S.last == st) {
    ++g_stats.stateSkipped;
    return;
  }
  ++g_stats.stateApplied;
  set_enable(kDepthTest, S.depthTest, true);
  if (S.depthFunc != st.depthFunc) {
    S.depthFunc = st.depthFunc;
    gl.DepthFunc(st.depthFunc);
  }
  set_depth_mask(st.depthMask != 0);
  // Recorte de caras pendiente (paso 2b): en 2a se dibuja todo para ver las formas.
  set_enable(kCullFace, S.cullOn, false);
  set_color_mask(st.colorMask != 0, st.alphaMask != 0);
  if (st.dstAlpha != UINT32_MAX) {
    const float a = static_cast<float>(st.dstAlpha) / 255.f;
    if (S.blendAlpha != a) {
      S.blendAlpha = a;
      gl.BlendColor(0.f, 0.f, 0.f, a);
    }
  }
  set_enable(kBlend, S.blendOn, st.blend != 0);
  if (st.blend) {
    const GLenum b[6] = {st.cs, st.cd, st.as, st.ad, st.ceq, st.aeq};
    if (std::memcmp(S.blend, b, sizeof(b)) != 0) {
      if (std::memcmp(S.blend, b, 4 * sizeof(GLenum)) != 0) gl.BlendFuncSeparate(st.cs, st.cd, st.as, st.ad);
      if (S.blend[4] != st.ceq || S.blend[5] != st.aeq) gl.BlendEquationSeparate(st.ceq, st.aeq);
      std::memcpy(S.blend, b, sizeof(b));
    }
  }
  S.last = st;
  S.lastValid = true;
}
// --- Recursos -------------------------------------------------------------------------------------------------
constexpr int kTexWidth = 4096;
constexpr int kRing = 3;  // recursos por cuadro en anillo: la Mali no espera a que la GPU suelte el cuadro anterior
struct WordTexture {
  GLuint id = 0;
  int rows = 0;
};
// Anillo persistente: un búfer GL creado una vez (GL_EXT_buffer_storage) y mapeado para siempre; la CPU escribe
// en él con memcpy y la GPU lee de ahí. Igual que los anillos del renderer GLES de Nebula (Super Mario Galaxy en la
// RG35XX H): nada de memoria nueva en cada cuadro. Solo se rehace si un cuadro no cabe.
struct Ring {
  GLuint buf = 0;
  uint8_t* ptr = nullptr;
  size_t size = 0;
};
struct FrameResources {
  WordTexture vtex, atex;
  GLuint ubo = 0, ebo = 0;
  size_t uboSize = 0, eboSize = 0;
  Ring rv, ra, ru, ri;    // vértices, arreglos, uniformes, índices (con búferes persistentes)
  void* fence = nullptr;  // la GPU terminó el cuadro que usó estos recursos
};
bool g_persistent = false;  // GL_EXT_buffer_storage disponible y funcionando
constexpr GLenum kPixelUnpackBuffer = 0x88EC, kCopyWriteBuffer = 0x8F37;
constexpr GLbitfield kMapWrite = 0x0002, kMapPersistent = 0x0040, kMapCoherent = 0x0080;

bool ensure_ring(Ring& r, size_t need, size_t minimum) {
  if (r.buf != 0 && r.size >= need) return true;
  if (r.buf != 0) {
    gl.BindBuffer(kCopyWriteBuffer, r.buf);
    gl.UnmapBuffer(kCopyWriteBuffer);
    gl.DeleteBuffers(1, &r.buf);
    r = Ring{};
  }
  const size_t size = std::max(minimum, need + need / 2);
  gl.GenBuffers(1, &r.buf);
  gl.BindBuffer(kCopyWriteBuffer, r.buf);
  gl.BufferStorageEXT(kCopyWriteBuffer, static_cast<GLsizeiptr>(size), nullptr,
                      kMapWrite | kMapPersistent | kMapCoherent);
  r.ptr = static_cast<uint8_t*>(gl.MapBufferRange(kCopyWriteBuffer, 0, static_cast<GLsizeiptr>(size),
                                                  kMapWrite | kMapPersistent | kMapCoherent));
  gl.BindBuffer(kCopyWriteBuffer, 0);
  if (r.ptr == nullptr) {
    log_line("motor: no se pudo mapear un búfer persistente de %zu KB; se sube como antes", size / 1024);
    gl.DeleteBuffers(1, &r.buf);
    r = Ring{};
    return false;
  }
  r.size = size;
  return true;
}
FrameResources g_ring[kRing];
int g_ringIndex = 0;
GLuint g_vao = 0;
GLint g_uboAlign = 16;
bool g_glReady = false, g_glFailed = false;
std::vector<uint8_t> g_tail;

bool upload_words(WordTexture& t, const std::vector<uint8_t>& bytes, Ring* ring = nullptr) {
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
  if (ring != nullptr && ensure_ring(*ring, static_cast<size_t>(rows) * kTexWidth * 4, 4u << 20)) {
    // Desde el anillo persistente (PBO): la textura se llena en la GPU, sin copias del driver ni memoria nueva.
    std::memcpy(ring->ptr, bytes.data(), bytes.size());
    const size_t padded = words * 4;
    if (padded > bytes.size()) std::memset(ring->ptr + bytes.size(), 0, padded - bytes.size());
    gl.BindBuffer(kPixelUnpackBuffer, ring->buf);
    const size_t rowBytes = static_cast<size_t>(kTexWidth) * 4;
    const int fullRows = static_cast<int>(words / kTexWidth);
    if (fullRows > 0) {
      gl.TexSubImage2D(kTexture2D, 0, 0, 0, kTexWidth, fullRows, kRedInteger, kUnsignedInt, nullptr);
    }
    const int restWords = static_cast<int>(words - static_cast<size_t>(fullRows) * kTexWidth);
    if (restWords > 0) {
      gl.TexSubImage2D(kTexture2D, 0, 0, fullRows, restWords, 1, kRedInteger, kUnsignedInt,
                       reinterpret_cast<const void*>(static_cast<uintptr_t>(fullRows) * rowBytes));
    }
    gl.BindBuffer(kPixelUnpackBuffer, 0);
    return true;
  }
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

// Cómo se guarda una textura en GL. aurora entrega todo en RGBA8 (4 bytes por píxel aunque el original sea I4 o
// CMPR); en la consola eso llena la memoria (1 GB compartido con la GPU) y en Golf el sistema se queda sin RAM. Se
// mira el contenido y se usa el formato más chico que no pierde (o casi) nada.
enum TexKind { kTexGray = 0, kTexGrayAlpha, kTexRgb565, kTexRgb5a1, kTexRgba8 };
constexpr GLenum kRed = 0x1903, kRg = 0x8227, kR8 = 0x8229, kRg8 = 0x822B, kRgb = 0x1907, kRgb565 = 0x8D62,
                 kRgb5A1 = 0x8057, kUnsignedShort565 = 0x8363, kUnsignedShort5551 = 0x8034, kSwizzleR = 0x8E42,
                 kSwizzleG = 0x8E43, kSwizzleB = 0x8E44, kSwizzleA = 0x8E45;
int classify_texture(const std::vector<uint8_t>& rgba) {
  static const bool keepRgba8 = std::getenv("CHOYA_TEX_RGBA8") != nullptr;
  if (keepRgba8) return kTexRgba8;
  bool gray = true, opaque = true, binaryAlpha = true;
  for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
    const uint8_t r = rgba[i], g = rgba[i + 1], b = rgba[i + 2], a = rgba[i + 3];
    if (r != g || g != b) gray = false;
    if (a != 255) opaque = false;
    if (a != 0 && a != 255) binaryAlpha = false;
    if (!gray && !opaque && !binaryAlpha) return kTexRgba8;
  }
  if (gray) return opaque ? kTexGray : kTexGrayAlpha;
  if (opaque) return kTexRgb565;
  return binaryAlpha ? kTexRgb5a1 : kTexRgba8;
}
size_t texel_bytes(int kind) { return kind == kTexGray ? 1 : kind == kTexRgba8 ? 4 : 2; }

// Convierte una mip RGBA8 al formato de la textura (en g_texScratch) y la sube.
std::vector<uint8_t> g_texScratch;
void upload_mip(int kind, GLint mip, uint32_t w, uint32_t h, const uint8_t* src) {
  const size_t n = static_cast<size_t>(w) * h;
  GLenum format = kRgba, type = kUnsignedByte;
  const void* data = src;
  if (kind != kTexRgba8) {
    g_texScratch.resize(n * texel_bytes(kind));
    uint8_t* d8 = g_texScratch.data();
    auto* d16 = reinterpret_cast<uint16_t*>(g_texScratch.data());
    for (size_t i = 0; i < n; ++i) {
      const uint8_t* p = src + i * 4;
      switch (kind) {
      case kTexGray: d8[i] = p[0]; break;
      case kTexGrayAlpha: d8[i * 2] = p[0]; d8[i * 2 + 1] = p[3]; break;
      case kTexRgb565:
        d16[i] = static_cast<uint16_t>(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
        break;
      default:  // 5551
        d16[i] = static_cast<uint16_t>(((p[0] >> 3) << 11) | ((p[1] >> 3) << 6) | ((p[2] >> 3) << 1) | (p[3] >> 7));
        break;
      }
    }
    data = g_texScratch.data();
    format = kind == kTexGray ? kRed : kind == kTexGrayAlpha ? kRg : kind == kTexRgb565 ? kRgb : kRgba;
    type = kind == kTexRgb565 ? kUnsignedShort565 : kind == kTexRgb5a1 ? kUnsignedShort5551 : kUnsignedByte;
  }
  gl.TexSubImage2D(kTexture2D, mip, 0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h), format, type, data);
}

size_t g_texBytes = 0;  // memoria de las texturas en GL
std::vector<uint8_t> g_packRead;  // datos leídos del paquete (se reusa)

// Una textura del paquete de recursos convertidos: se crea con su formato (ETC2/EAC comprimido en la GPU, o los
// formatos chicos sin comprimir) y sus mipmaps tal cual los dejó el conversor.
bool upload_packed(GlTexture& tex, const TexUpload& up) {
  const uint8_t* data = up.data.data();
  size_t size = up.data.size();
  if (up.data.empty()) {
    auto& pk = pack();
    g_packRead.resize(up.packSize);
    std::lock_guard rl(pk.readMutex);
    if (pk.file == nullptr || std::fseek(pk.file, static_cast<long>(up.packOffset), SEEK_SET) != 0 ||
        std::fread(g_packRead.data(), 1, g_packRead.size(), pk.file) != g_packRead.size()) {
      if (pk.readErrors++ == 0) log_line("paquete: no pude leer una textura (offset %llu)",
                                         static_cast<unsigned long long>(up.packOffset));
      return false;
    }
    data = g_packRead.data();
    size = g_packRead.size();
  }
  GLenum internal = 0, format = 0, type = 0;
  bool compressed = false;
  switch (up.packFormat) {
  case kPkEtc2Rgb8: internal = 0x9274; compressed = true; break;   // GL_COMPRESSED_RGB8_ETC2
  case kPkEtc2Rgba8: internal = 0x9278; compressed = true; break;  // GL_COMPRESSED_RGBA8_ETC2_EAC
  case kPkR11Eac: internal = 0x9270; compressed = true; break;     // GL_COMPRESSED_R11_EAC
  case kPkRg11Eac: internal = 0x9272; compressed = true; break;    // GL_COMPRESSED_RG11_EAC
  case kPkRgb565: internal = kRgb565; format = kRgb; type = kUnsignedShort565; break;
  case kPkRgba4444: internal = 0x8056; format = kRgba; type = 0x8033; break;  // RGBA4, UNSIGNED_SHORT_4_4_4_4
  case kPkRgba5551: internal = kRgb5A1; format = kRgba; type = kUnsignedShort5551; break;
  case kPkL8: internal = kR8; format = kRed; type = kUnsignedByte; break;
  case kPkLa8: internal = kRg8; format = kRg; type = kUnsignedByte; break;
  case kPkRgba8: internal = kRgba8; format = kRgba; type = kUnsignedByte; break;
  default: return false;
  }
  // Solo los niveles que vienen completos en los datos.
  uint32_t levels = 0;
  size_t need = 0;
  for (uint32_t mip = 0; mip < up.mips; ++mip) {
    const size_t bytes = pack_level_size(up.packFormat, std::max(up.width >> mip, 1u), std::max(up.height >> mip, 1u));
    if (need + bytes > size) break;
    need += bytes;
    ++levels;
  }
  if (levels == 0) return false;
  if (tex.id != 0) {
    gl.DeleteTextures(1, &tex.id);
    g_texBytes -= tex.bytes;
  }
  gl.GenTextures(1, &tex.id);
  ++g_texGen;
  gl.BindTexture(kTexture2D, tex.id);
  gl.TexStorage2D(kTexture2D, static_cast<GLsizei>(levels), internal, static_cast<GLsizei>(up.width),
                  static_cast<GLsizei>(up.height));
  size_t offset = 0;
  for (uint32_t mip = 0; mip < levels; ++mip) {
    const uint32_t w = std::max(up.width >> mip, 1u), h = std::max(up.height >> mip, 1u);
    const size_t bytes = pack_level_size(up.packFormat, w, h);
    if (compressed) {
      gl.CompressedTexSubImage2D(kTexture2D, static_cast<GLint>(mip), 0, 0, static_cast<GLsizei>(w),
                                 static_cast<GLsizei>(h), internal, static_cast<GLsizei>(bytes), data + offset);
    } else {
      gl.TexSubImage2D(kTexture2D, static_cast<GLint>(mip), 0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h),
                       format, type, data + offset);
    }
    offset += bytes;
  }
  // Intensidad (I4/I8): R en los cuatro canales; intensidad + alfa (IA4/IA8): R en color, G en alfa. L8/LA8 del
  // perfil gles2 sin bandera: gris opaco o gris con alfa.
  const bool swI = (up.packFlags & kPkSwizzleI) != 0;
  const bool swIA = (up.packFlags & kPkSwizzleIA) != 0 || (up.packFormat == kPkLa8 && !swI);
  if (swI || swIA || up.packFormat == kPkL8) {
    gl.TexParameteri(kTexture2D, kSwizzleR, static_cast<GLint>(kRed));
    gl.TexParameteri(kTexture2D, kSwizzleG, static_cast<GLint>(kRed));
    gl.TexParameteri(kTexture2D, kSwizzleB, static_cast<GLint>(kRed));
    gl.TexParameteri(kTexture2D, kSwizzleA, swI ? static_cast<GLint>(kRed) : swIA ? 0x1904 /* GL_GREEN */ : 1 /* ONE */);
  }
  tex.width = up.width;
  tex.height = up.height;
  tex.mips = up.mips;
  tex.kind = 100 + up.packFormat;
  tex.bytes = offset;
  g_texBytes += tex.bytes;
  ++g_stats.texUploads;
  ++g_stats.packUploads;
  return true;
}

void process_textures() {
  std::vector<TexUpload> pending;
  std::vector<uint64_t> released;
  {
    auto& s = texs();
    std::lock_guard lock(s.mutex);
    pending.swap(s.pending);
    released.swap(s.released);
  }
  for (uint64_t id : released) {
    auto it = g_textures.find(id);
    if (it != g_textures.end()) {
      gl.DeleteTextures(1, &it->second.id);
      g_texBytes -= it->second.bytes;
      g_textures.erase(it);
      ++g_texGen;
    }
    auto c = g_copyTextures.find(id);
    if (c != g_copyTextures.end()) {
      gl.DeleteFramebuffers(1, &c->second.fbo);
      gl.DeleteTextures(1, &c->second.tex);
      g_copyTextures.erase(c);
      ++g_texGen;
    }
  }
  if (pending.empty()) return;
  gl.ActiveTexture(kTexture0 + 2);
  gl.PixelStorei(kUnpackAlignment, 1);  // filas de 1 o 2 bytes por píxel
  for (auto& up : pending) {
    GlTexture& tex = g_textures[up.id];
    if (up.packFormat != 0) {
      if (!upload_packed(tex, up) && tex.id == 0) g_textures.erase(up.id);
      continue;
    }
    const int kind = classify_texture(up.data);
    if (tex.id == 0 || tex.width != up.width || tex.height != up.height || tex.mips != up.mips || tex.kind != kind) {
      if (tex.id != 0) {
        gl.DeleteTextures(1, &tex.id);
        g_texBytes -= tex.bytes;
      }
      gl.GenTextures(1, &tex.id);
      ++g_texGen;
      gl.BindTexture(kTexture2D, tex.id);
      const GLenum internal = kind == kTexGray ? kR8 : kind == kTexGrayAlpha ? kRg8 : kind == kTexRgb565 ? kRgb565
                              : kind == kTexRgb5a1 ? kRgb5A1 : kRgba8;
      gl.TexStorage2D(kTexture2D, static_cast<GLsizei>(up.mips), internal, static_cast<GLsizei>(up.width),
                      static_cast<GLsizei>(up.height));
      if (kind == kTexGray || kind == kTexGrayAlpha) {  // gris: (r, r, r, 1) o (r, r, r, g)
        gl.TexParameteri(kTexture2D, kSwizzleR, static_cast<GLint>(kRed));
        gl.TexParameteri(kTexture2D, kSwizzleG, static_cast<GLint>(kRed));
        gl.TexParameteri(kTexture2D, kSwizzleB, static_cast<GLint>(kRed));
        gl.TexParameteri(kTexture2D, kSwizzleA, kind == kTexGray ? 1 /* GL_ONE */ : 0x1904 /* GL_GREEN */);
      }
      tex.width = up.width;
      tex.height = up.height;
      tex.mips = up.mips;
      tex.kind = kind;
      tex.bytes = 0;
      for (uint32_t mip = 0; mip < up.mips; ++mip) {
        tex.bytes += static_cast<size_t>(std::max(up.width >> mip, 1u)) * std::max(up.height >> mip, 1u) *
                     texel_bytes(kind);
      }
      g_texBytes += tex.bytes;
    } else {
      gl.BindTexture(kTexture2D, tex.id);
    }
    size_t offset = 0;
    for (uint32_t mip = 0; mip < up.mips; ++mip) {
      const uint32_t w = std::max(up.width >> mip, 1u), h = std::max(up.height >> mip, 1u);
      const size_t bytes = static_cast<size_t>(w) * h * 4;
      if (offset + bytes > up.data.size()) break;
      upload_mip(kind, static_cast<GLint>(mip), w, h, up.data.data() + offset);
      offset += bytes;
    }
    ++g_stats.texUploads;
  }
  gl.PixelStorei(kUnpackAlignment, 4);
  if (g_texScratch.capacity() > (8u << 20)) std::vector<uint8_t>().swap(g_texScratch);
}

GLuint sampler_for(const SamplerDesc& s) {
  uint64_t key = 0;
  std::memcpy(&key, &s, 8);
  uint32_t lods[2];
  std::memcpy(lods, &s.minLod, 8);
  key ^= (static_cast<uint64_t>(lods[0]) * 0x9E3779B97F4A7C15ull) ^ (static_cast<uint64_t>(lods[1]) << 1);
  auto it = g_samplers.find(key);
  if (it != g_samplers.end()) return it->second;
  GLuint id = 0;
  gl.GenSamplers(1, &id);
  auto wrap = [](uint8_t w) { return static_cast<GLint>(w == 1 ? kRepeat : w == 2 ? kMirroredRepeat : kClampToEdge); };
  gl.SamplerParameteri(id, kTextureWrapS, wrap(s.wrapS));
  gl.SamplerParameteri(id, kTextureWrapT, wrap(s.wrapT));
  gl.SamplerParameteri(id, kTextureMagFilter, static_cast<GLint>(s.magFilter ? kLinear : kNearest));
  GLenum minf;
  if (s.mipFilter == 0) {
    minf = s.minFilter ? kLinear : kNearest;
  } else if (s.mipFilter == 1) {
    minf = s.minFilter ? kLinearMipmapNearest : kNearestMipmapNearest;
  } else {
    minf = s.minFilter ? kLinearMipmapLinear : kNearestMipmapLinear;
  }
  gl.SamplerParameteri(id, kTextureMinFilter, static_cast<GLint>(minf));
  gl.SamplerParameterf(id, kTextureMinLod, s.minLod);
  gl.SamplerParameterf(id, kTextureMaxLod, s.mipFilter == 0 ? 0.f : s.maxLod);
  g_samplers.emplace(key, id);
  return id;
}

// Texturas de cada grupo ya resueltas a nombres GL (la "memo" de enlaces de galaxy-vita): se rehacen solo si aurora
// registró grupos nuevos o el motor creó o borró texturas desde la última vez.
struct ResolvedBind {
  uint64_t bindGen = 0, texGen = 0;
  GLuint tex[8], samp[8];
};
std::unordered_map<uint64_t, ResolvedBind> g_resolvedBinds;

const ResolvedBind& resolve_bind(uint64_t bindGroup) {
  ResolvedBind& r = g_resolvedBinds[bindGroup];
  const uint64_t bindGen = g_bindGen.load(std::memory_order_acquire);
  if (r.bindGen == bindGen && r.texGen == g_texGen) return r;
  r.bindGen = bindGen;
  r.texGen = g_texGen;
  BindDesc bd;
  const bool has = bindGroup != 0 && find_bind_group(bindGroup, bd);
  if (!has) r.texGen = 0;  // sin registrar todavía: volver a buscarlo
  for (int i = 0; i < 8; ++i) {
    GLuint tex = g_whiteTexture;
    GLuint samp = 0;
    if (has && bd.tex[i] != 0) {
      auto it = g_textures.find(bd.tex[i]);
      if (it != g_textures.end()) {
        tex = it->second.id;
      } else if (auto c = g_copyTextures.find(bd.tex[i]); c != g_copyTextures.end()) {
        tex = c->second.tex;  // copia del EFB
      } else {
        ++g_stats.missingTextures;  // paleta, o una copia que todavía no se hace
        r.texGen = 0;               // volver a buscarla la próxima vez (la copia puede llegar en este cuadro)
      }
      samp = sampler_for(bd.samp[i]);
    }
    r.tex[i] = tex;
    r.samp[i] = samp;
  }
  return r;
}

void bind_textures(uint64_t bindGroup) {
  const ResolvedBind& r = resolve_bind(bindGroup);
  for (int i = 0; i < 8; ++i) {
    if (S.tex[i] == r.tex[i] && S.samp[i] == r.samp[i]) {
      ++g_stats.texBindsSkipped;
      continue;
    }
    ++g_stats.texBinds;
    if (S.tex[i] != r.tex[i]) {
      if (S.unit != 2 + i) {
        S.unit = 2 + i;
        gl.ActiveTexture(kTexture0 + 2 + static_cast<GLenum>(i));
      }
      gl.BindTexture(kTexture2D, r.tex[i]);
      S.tex[i] = r.tex[i];
    }
    if (S.samp[i] != r.samp[i]) {
      gl.BindSampler(static_cast<GLuint>(2 + i), r.samp[i]);
      S.samp[i] = r.samp[i];
    }
  }
}

// Pipelines ya resueltos (la caché de resolución de galaxy-vita): programa y estado GL calculados una vez por
// pipeline de aurora, no en cada dibujo (antes: copiar el descriptor con candado, hash y búsqueda del programa).
struct ResolvedPipe {
  const Program* prog = nullptr;
  StateBlock state{};
  bool skipLine = false, noWrite = false;
};
std::unordered_map<uint64_t, ResolvedPipe> g_resolvedPipes;

const ResolvedPipe* resolve_pipe(uint64_t ref) {
  auto it = g_resolvedPipes.find(ref);
  if (it != g_resolvedPipes.end()) return &it->second;
  PipelineDesc pc;
  if (!find_pipeline(ref, pc)) return nullptr;  // todavía sin registrar: no se guarda
  ResolvedPipe r;
  r.skipLine = pc.lineMode != 0 || pc.cullMode == GX_CULL_ALL;
  r.state = make_state(pc, UINT32_MAX);
  // Un dibujo que no escribe color, alfa ni profundidad no cambia nada en el EFB (GX no tiene estencil).
  r.noWrite = !r.state.colorMask && !r.state.alphaMask && !r.state.depthMask;
  if (!r.skipLine && !r.noWrite) {
    r.prog = &program_for(pc);
    S.program = ~0u;  // compilar o cargar un programa lo deja en uso (glUseProgram en setup_program)
  }
  return &g_resolvedPipes.emplace(ref, r).first->second;
}

// El estado del pipeline con el alfa constante del dibujo (GXSetDstAlpha), igual que make_state(pc, dstAlpha).
StateBlock with_dst_alpha(StateBlock st, uint32_t dstAlpha) {
  if (dstAlpha == UINT32_MAX) return st;
  if (!st.blend) {
    st.cs = kOne;
    st.cd = kZero;
    st.ceq = kFuncAdd;
  }
  st.as = kConstantAlpha;
  st.ad = kZero;
  st.aeq = kFuncAdd;
  st.blend = 1;
  st.dstAlpha = dstAlpha;
  return st;
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
  {
    const uint32_t white = 0xFFFFFFFFu;
    gl.GenTextures(1, &g_whiteTexture);
    gl.ActiveTexture(kTexture0 + 2);
    gl.BindTexture(kTexture2D, g_whiteTexture);
    gl.TexStorage2D(kTexture2D, 1, kRgba8, 1, 1);
    gl.TexSubImage2D(kTexture2D, 0, 0, 0, 1, 1, kRgba, kUnsignedByte, &white);
  }
  {
    // Búferes persistentes: la extensión y sus funciones; CHOYA_GX_PERSISTENTE=0 los apaga.
    const char* env = std::getenv("CHOYA_GX_PERSISTENTE");
    const bool wanted = env == nullptr || std::strcmp(env, "0") != 0;
    g_persistent = wanted && gl.BufferStorageEXT != nullptr && gl.MapBufferRange != nullptr && gl.UnmapBuffer != nullptr &&
                   gl.DeleteBuffers != nullptr;
    if (g_persistent) {
      Ring probe;
      g_persistent = ensure_ring(probe, 4096, 4096);
      if (probe.buf != 0) {
        gl.BindBuffer(kCopyWriteBuffer, probe.buf);
        gl.UnmapBuffer(kCopyWriteBuffer);
        gl.BindBuffer(kCopyWriteBuffer, 0);
        gl.DeleteBuffers(1, &probe.buf);
      }
    }
  }
  log_line("motor: listo (alineación de UBO %d; búferes persistentes %s)", g_uboAlign,
           g_persistent ? "sí, como en Nebula" : "no, se suben los datos en cada cuadro");
  return true;
}


// Framebuffer del EFB (color RGBA8 + profundidad/estencil).
struct Efb {
  GLuint fbo = 0, color = 0, depth = 0;  // depth: textura (las copias del EFB de profundidad la leen)
  int width = 0, height = 0;
} g_efb;

bool ensure_efb(int width, int height) {
  if (g_efb.fbo != 0 && width <= g_efb.width && height <= g_efb.height) return true;
  if (g_efb.fbo != 0) {
    gl.DeleteFramebuffers(1, &g_efb.fbo);
    gl.DeleteTextures(1, &g_efb.color);
    gl.DeleteTextures(1, &g_efb.depth);
  }
  g_efb.width = std::max(width, g_efb.width);
  g_efb.height = std::max(height, g_efb.height);
  gl.GenTextures(1, &g_efb.color);
  gl.BindTexture(kTexture2D, g_efb.color);
  gl.TexStorage2D(kTexture2D, 1, kRgba8, g_efb.width, g_efb.height);
  gl.TexParameteri(kTexture2D, kTextureMinFilter, kLinear);
  gl.TexParameteri(kTexture2D, kTextureMagFilter, kLinear);
  gl.GenTextures(1, &g_efb.depth);
  gl.BindTexture(kTexture2D, g_efb.depth);
  gl.TexStorage2D(kTexture2D, 1, kDepth24Stencil8, g_efb.width, g_efb.height);
  gl.TexParameteri(kTexture2D, kTextureMinFilter, kNearest);
  gl.TexParameteri(kTexture2D, kTextureMagFilter, kNearest);
  gl.GenFramebuffers(1, &g_efb.fbo);
  gl.BindFramebuffer(kFramebuffer, g_efb.fbo);
  gl.FramebufferTexture2D(kFramebuffer, kColorAttachment0, kTexture2D, g_efb.color, 0);
  gl.FramebufferTexture2D(kFramebuffer, kDepthStencilAttachment, kTexture2D, g_efb.depth, 0);
  const GLenum status = gl.CheckFramebufferStatus(kFramebuffer);
  if (status != kFramebufferComplete) {
    log_line("motor: el framebuffer del EFB (%dx%d) no está completo (0x%04X)", g_efb.width, g_efb.height, status);
    return false;
  }
  log_line("motor: EFB de %dx%d", g_efb.width, g_efb.height);
  return true;
}

// Copia una zona del EFB (coordenadas del pase, origen arriba a la izquierda) a toda la pantalla, escalada.
void present_efb(const Rect& r, int screenWidth, int screenHeight) {
  gl.Disable(kScissorTest);
  gl.BindFramebuffer(kReadFramebuffer, g_efb.fbo);
  gl.BindFramebuffer(kDrawFramebuffer, 0);
  // El EFB se dibuja con y hacia arriba en GL (como la pantalla): la copia no voltea.
  const int y0 = g_efb.height - r.y - r.height;
  gl.BlitFramebuffer(r.x, y0, r.x + r.width, y0 + r.height, 0, 0, screenWidth, screenHeight, kColorBufferBit, kLinear);
  gl.BindFramebuffer(kFramebuffer, g_efb.fbo);
}

// --- Copias del EFB a texturas (GXCopyTex) ------------------------------------------------------------------
// Igual que tex_copy_conv de aurora: la zona del EFB se convierte al formato de la copia y se guarda en RGBA8 con el
// valor que el juego leerá al muestrearla (intensidad en los cuatro canales, alfa a 1 en RGB565, profundidad en
// bytes...). Fila 0 de la textura = arriba de la zona, como las texturas subidas.
const char* kCopyVs = R"(#version 300 es
void main() {
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";
const char* kCopyFs = R"(#version 300 es
precision highp float;
precision highp int;
uniform sampler2D u_color;
uniform highp sampler2D u_depth;
uniform vec4 u_rect;   // zona del pase: x, y (arriba), ancho, alto
uniform vec4 u_size;   // ancho y alto del EFB, ancho y alto de la textura
uniform vec4 u_mode;   // formato (GXTexFmt), alfa opaco, Z invertida
out vec4 o_color;
float inten(vec3 c) { return dot(c, vec3(0.257, 0.504, 0.098)) + 16.0 / 255.0; }
float q4(float v) { return floor(v * 16.0) / 15.0; }
void main() {
  vec2 t = gl_FragCoord.xy / u_size.zw;
  vec2 p = u_rect.xy + t * u_rect.zw;
  vec2 uv = vec2(p.x / u_size.x, 1.0 - p.y / u_size.y);
  int fmt = int(u_mode.x + 0.5);
  vec4 r;
  if ((fmt & 0x10) != 0) {
    float d = texture(u_depth, uv).r;
    float z = u_mode.z > 0.5 ? 1.0 - d : d;
    uint z24 = min(uint(clamp(z, 0.0, 1.0) * 16777215.0 + 0.5), 0xFFFFFFu);
    vec3 b = vec3(float((z24 >> 16) & 255u), float((z24 >> 8) & 255u), float(z24 & 255u)) / 255.0;
    if (fmt == 0x11) r = vec4(b.x);                        // Z8
    else if (fmt == 0x13) r = vec4(b.x, b.x, b.x, 1.0);    // Z16
    else if (fmt == 0x16) r = vec4(b, 1.0);                // Z24X8
    else if (fmt == 0x30) r = vec4(q4(b.x));               // Z4
    else if (fmt == 0x39) r = vec4(b.y);                   // Z8M
    else if (fmt == 0x3A) r = vec4(b.z);                   // Z8L
    else r = vec4(b.y, b.y, b.y, b.z);                     // Z16L
  } else {
    vec4 c = texture(u_color, uv);
    if (u_mode.y > 0.5) c.a = 1.0;
    float i = inten(c.rgb);
    if (fmt == 0x0) r = vec4(q4(i));
    else if (fmt == 0x1) r = vec4(i);
    else if (fmt == 0x2) r = vec4(q4(i), q4(i), q4(i), q4(c.a));
    else if (fmt == 0x3) r = vec4(i, i, i, c.a);
    else if (fmt == 0x4) r = vec4(c.rgb, 1.0);
    else if (fmt == 0x20) r = vec4(q4(c.r));
    else if (fmt == 0x22) r = vec4(q4(c.r), q4(c.r), q4(c.r), q4(c.a));
    else if (fmt == 0x23) r = vec4(c.r, c.r, c.r, c.a);
    else if (fmt == 0x27) r = vec4(c.a);
    else if (fmt == 0x28) r = vec4(c.r);
    else if (fmt == 0x29) r = vec4(c.g);
    else if (fmt == 0x2A) r = vec4(c.b);
    else if (fmt == 0x2B) r = vec4(c.r, c.r, c.r, c.g);
    else if (fmt == 0x2C) r = vec4(c.g, c.g, c.g, c.b);
    else r = c;                                            // RGB5A3, RGBA8, YUVA8
  }
  o_color = clamp(r, 0.0, 1.0);
}
)";
struct CopyProgram {
  GLuint id = 0;
  GLint rect = -1, size = -1, mode = -1;
  bool tried = false;
} g_copyProgram;

bool copy_program() {
  if (g_copyProgram.tried) return g_copyProgram.id != 0;
  g_copyProgram.tried = true;
  std::string err;
  const GLuint v = compile(kVertexShader, kCopyVs, err);
  const GLuint f = v ? compile(kFragmentShader, kCopyFs, err) : 0;
  if (v && f) {
    const GLuint prog = gl.CreateProgram();
    gl.AttachShader(prog, v);
    gl.AttachShader(prog, f);
    gl.LinkProgram(prog);
    GLint ok = 0;
    gl.GetProgramiv(prog, kLinkStatus, &ok);
    if (ok) {
      g_copyProgram.id = prog;
      gl.UseProgram(prog);
      gl.Uniform1i(gl.GetUniformLocation(prog, "u_color"), 10);
      gl.Uniform1i(gl.GetUniformLocation(prog, "u_depth"), 11);
      g_copyProgram.rect = gl.GetUniformLocation(prog, "u_rect");
      g_copyProgram.size = gl.GetUniformLocation(prog, "u_size");
      g_copyProgram.mode = gl.GetUniformLocation(prog, "u_mode");
    } else {
      GLint len = 0;
      gl.GetProgramiv(prog, kInfoLogLength, &len);
      err.assign(static_cast<size_t>(len > 1 ? len : 1), '\0');
      gl.GetProgramInfoLog(prog, len, nullptr, err.data());
    }
  }
  if (v) gl.DeleteShader(v);
  if (f) gl.DeleteShader(f);
  if (g_copyProgram.id == 0) log_line("motor: no compila el programa de copias del EFB: %s", err.c_str());
  return g_copyProgram.id != 0;
}

// --- Borrados dentro del pase como un dibujo -------------------------------------------------------------------
// En Golf, glClear sobre el EFB justo después de copiarlo (GXCopyTex con borrado) hacía que el driver de la Mali le
// diera memoria nueva al EFB (~1.5 MB, el tamaño del EFB) en cada cuadro y no la devolvía: 0.19.1 lo midió en
// "borrados" (+150 a +200 MB por informe). Un triángulo que cubre el recorte con el color y la profundidad del
// borrado deja lo mismo en el EFB sin que el driver lo trate como un borrado. CHOYA_GX_BORRAR_CON_GL=1: el glClear.
constexpr const char* kClearVs = R"(#version 300 es
void main() {
  vec2 p = vec2(float((gl_VertexID & 1) << 2) - 1.0, float((gl_VertexID & 2) << 1) - 1.0);
  gl_Position = vec4(p, 0.0, 1.0);
}
)";
constexpr const char* kClearFs = R"(#version 300 es
precision highp float;
uniform vec4 u_color;
uniform vec4 u_depth;
out vec4 frag;
void main() {
  frag = u_color;
  gl_FragDepth = u_depth.x;
}
)";
struct ClearProgram {
  GLuint id = 0;
  GLint color = -1, depth = -1;
  bool tried = false;
} g_clearProgram;

bool clear_by_draw() {
  static const bool on = [] {
    const char* e = std::getenv("CHOYA_GX_BORRAR_CON_GL");
    return e == nullptr || std::strcmp(e, "1") != 0;
  }();
  if (!on) return false;
  if (g_clearProgram.tried) return g_clearProgram.id != 0;
  g_clearProgram.tried = true;
  std::string err;
  const GLuint v = compile(kVertexShader, kClearVs, err);
  const GLuint f = v ? compile(kFragmentShader, kClearFs, err) : 0;
  if (v && f) {
    const GLuint prog = gl.CreateProgram();
    gl.AttachShader(prog, v);
    gl.AttachShader(prog, f);
    gl.LinkProgram(prog);
    GLint ok = 0;
    gl.GetProgramiv(prog, kLinkStatus, &ok);
    if (ok) {
      g_clearProgram.id = prog;
      g_clearProgram.color = gl.GetUniformLocation(prog, "u_color");
      g_clearProgram.depth = gl.GetUniformLocation(prog, "u_depth");
    }
  }
  if (v) gl.DeleteShader(v);
  if (f) gl.DeleteShader(f);
  if (g_clearProgram.id == 0) log_line("motor: no compila el programa de borrados; se usa glClear (%s)", err.c_str());
  return g_clearProgram.id != 0;
}

// CHOYA_GX_COPIAS=0 (solo para diagnosticar): no hace las copias del EFB a textura.
bool copies_enabled() {
  static const bool on = [] {
    const char* e = std::getenv("CHOYA_GX_COPIAS");
    return e == nullptr || std::strcmp(e, "0") != 0;
  }();
  return on;
}

// Hace la copia; deja enlazado el framebuffer del EFB (el que llama repone viewport y recorte).
void run_copy(const Item& it) {
  const auto& c = it.copy;
  if (c.texW == 0 || c.texH == 0 || c.texW > 4096 || c.texH > 4096 || c.rect.width <= 0 || c.rect.height <= 0) return;
  if (!copy_program()) return;
  CopyTexture& ct = g_copyTextures[c.tex];
  if (ct.tex == 0 || ct.width != c.texW || ct.height != c.texH) {
    if (ct.fbo != 0) gl.DeleteFramebuffers(1, &ct.fbo);
    if (ct.tex != 0) gl.DeleteTextures(1, &ct.tex);
    gl.ActiveTexture(kTexture0 + 10);
    gl.GenTextures(1, &ct.tex);
    ++g_texGen;
    gl.BindTexture(kTexture2D, ct.tex);
    gl.TexStorage2D(kTexture2D, 1, kRgba8, static_cast<GLsizei>(c.texW), static_cast<GLsizei>(c.texH));
    gl.GenFramebuffers(1, &ct.fbo);
    gl.BindFramebuffer(kFramebuffer, ct.fbo);
    gl.FramebufferTexture2D(kFramebuffer, kColorAttachment0, kTexture2D, ct.tex, 0);
    ct.width = c.texW;
    ct.height = c.texH;
  }
  gl.BindFramebuffer(kFramebuffer, ct.fbo);
  gl.Viewport(0, 0, static_cast<GLsizei>(c.texW), static_cast<GLsizei>(c.texH));
  gl.Disable(kScissorTest);
  gl.Disable(kDepthTest);
  gl.Disable(kBlend);
  gl.Disable(kCullFace);
  gl.ColorMask(1, 1, 1, 1);
  gl.UseProgram(g_copyProgram.id);
  gl.Uniform4f(g_copyProgram.rect, static_cast<float>(c.rect.x), static_cast<float>(c.rect.y),
               static_cast<float>(c.rect.width), static_cast<float>(c.rect.height));
  gl.Uniform4f(g_copyProgram.size, static_cast<float>(g_efb.width), static_cast<float>(g_efb.height),
               static_cast<float>(c.texW), static_cast<float>(c.texH));
  gl.Uniform4f(g_copyProgram.mode, static_cast<float>(c.fmt), c.opaque ? 1.f : 0.f, g_reversedZ ? 1.f : 0.f, 0.f);
  gl.ActiveTexture(kTexture0 + 10);
  gl.BindTexture(kTexture2D, g_efb.color);
  gl.BindSampler(10, 0);
  gl.ActiveTexture(kTexture0 + 11);
  gl.BindTexture(kTexture2D, g_efb.depth);
  gl.BindSampler(11, 0);
  gl.DrawArrays(kTriangles, 0, 3);
  // Sin el EFB enlazado para muestrear mientras se vuelve a dibujar en él.
  gl.BindTexture(kTexture2D, 0);
  gl.ActiveTexture(kTexture0 + 10);
  gl.BindTexture(kTexture2D, 0);
  gl.BindFramebuffer(kFramebuffer, g_efb.fbo);
  gl.Enable(kScissorTest);
  ++g_stats.copies;
}

bool draw_frame_locked(Frame& f, int screenWidth, int screenHeight, bool present);

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

void register_bind_group(uint64_t ref, const BindDesc& desc) noexcept {
  std::lock_guard lock(g_pipelineMutex);
  // aurora lo llama en cada dibujo: solo un grupo nuevo o cambiado invalida las texturas ya resueltas del motor.
  auto [it, added] = g_bindGroups.try_emplace(ref, desc);
  if (!added) {
    if (std::memcmp(&it->second, &desc, sizeof(desc)) == 0) return;
    it->second = desc;  // el mismo hash es el mismo contenido; se reemplaza por si cambió la textura
    g_bindGen.fetch_add(1, std::memory_order_release);
  }
}
bool has_bind_group(uint64_t ref) noexcept {
  std::lock_guard lock(g_pipelineMutex);
  return g_bindGroups.contains(ref);
}

void texture_data(uint64_t id, uint32_t width, uint32_t height, uint32_t mips, const uint8_t* rgba,
                  size_t size) noexcept {
  if (width == 0 || height == 0 || mips == 0 || rgba == nullptr) return;
  TexUpload up{id, width, height, mips, std::vector<uint8_t>(rgba, rgba + size)};
  auto& s = texs();
  std::lock_guard lock(s.mutex);
  if (s.keep) s.kept[id] = up;
  s.pending.push_back(std::move(up));
}

void texture_released(uint64_t id) noexcept {
  auto& s = texs();
  std::lock_guard lock(s.mutex);
  s.released.push_back(id);
  s.kept.erase(id);
}

size_t gx_level_size(uint32_t fmt, uint32_t w, uint32_t h) noexcept {
  uint32_t bw = 4, bh = 4, bpp = 16;
  switch (fmt) {
  case GX_TF_I4: case GX_TF_C4: case GX_TF_CMPR: bw = 8; bh = 8; bpp = 4; break;
  case GX_TF_I8: case GX_TF_IA4: case GX_TF_C8: bw = 8; bh = 4; bpp = 8; break;
  case GX_TF_IA8: case GX_TF_RGB565: case GX_TF_RGB5A3: case GX_TF_C14X2: break;
  case GX_TF_RGBA8: bpp = 32; break;
  default: return 0;
  }
  const size_t bx = (w + bw - 1) / bw, by = (h + bh - 1) / bh;
  return bx * by * bw * bh * bpp / 8;
}

uint64_t texture_key(const uint8_t* level0, size_t size, uint32_t w, uint32_t h, uint32_t fmt) noexcept {
  const uint64_t crc = crc32_ieee(level0, size);
  return (crc << 32) | (static_cast<uint64_t>((w - 1) & 0x3FF) << 22) | (static_cast<uint64_t>((h - 1) & 0x3FF) << 12) |
         (static_cast<uint64_t>(fmt & 0xF) << 8);
}

bool pack_open(const char* path) noexcept {
  auto& pk = pack();
  if (pk.file != nullptr || path == nullptr || path[0] == '\0') return pk.file != nullptr;
  FILE* f = std::fopen(path, "rb");
  if (f == nullptr) {
    log_line("paquete: no encontré %s; las texturas salen del disco (aurora)", path);
    return false;
  }
  WlpkHeader h{};
  bool ok = std::fread(&h, sizeof(h), 1, f) == 1 && std::memcmp(h.magic, "WLPK", 4) == 0 && h.version == 1 &&
            h.entryCount < (1u << 22);
  std::vector<WlpkEntry> toc;
  if (ok) {
    toc.resize(h.entryCount);
    ok = std::fseek(f, static_cast<long>(h.tocOffset), SEEK_SET) == 0 &&
         (toc.empty() || std::fread(toc.data(), sizeof(WlpkEntry), toc.size(), f) == toc.size());
  }
  if (!ok) {
    std::fclose(f);
    log_line("paquete: %s no es un assets.wlpk válido (versión 1)", path);
    return false;
  }
  size_t audio = 0, compressed = 0;
  uint64_t bytes = 0;
  for (const WlpkEntry& e : toc) {
    if (e.kind == 1 && e.format >= kPkEtc2Rgb8 && e.format <= kPkRgba8 && e.width > 0 && e.height > 0) {
      pk.textures.push_back(e);
      bytes += e.dataSize;
      if (e.format == kPkEtc2Rgb8 || e.format == kPkEtc2Rgba8 || e.format == kPkR11Eac || e.format == kPkRg11Eac) {
        ++compressed;
      }
    } else if (e.kind == 2) {
      ++audio;
    }
  }
  std::sort(pk.textures.begin(), pk.textures.end(), [](const WlpkEntry& a, const WlpkEntry& b) { return a.key < b.key; });
  pk.file = f;
  log_line("paquete: %s con %zu texturas convertidas (%zu en ETC2/EAC, %.1f MB) y %zu audios (el audio todavía sale "
           "del disco)", path, pk.textures.size(), compressed, static_cast<double>(bytes) / 1048576.0, audio);
  return true;
}

bool pack_is_open() noexcept { return pack().file != nullptr; }

int64_t pack_lookup(uint32_t fmt, uint32_t width, uint32_t height, const uint8_t* level0, size_t size) noexcept {
  auto& pk = pack();
  if (pk.file == nullptr || level0 == nullptr || width == 0 || height == 0 || width > 1024 || height > 1024) return -1;
  const size_t need = gx_level_size(fmt, width, height);
  if (need == 0 || size < need) return -1;
  const uint64_t key = texture_key(level0, need, width, height, fmt);
  auto it = std::lower_bound(pk.textures.begin(), pk.textures.end(), key,
                             [](const WlpkEntry& e, uint64_t k) { return e.key < k; });
  if (it == pk.textures.end() || it->key != key) {
    ++pk.misses;
    return -1;
  }
  return it - pk.textures.begin();
}

bool texture_from_pack(uint64_t id, int64_t index) noexcept {
  auto& pk = pack();
  if (pk.file == nullptr || index < 0 || static_cast<size_t>(index) >= pk.textures.size()) return false;
  const WlpkEntry& e = pk.textures[static_cast<size_t>(index)];
  ++pk.hits;
  pk.bytes += e.dataSize;
  TexUpload up{id, e.width, e.height, std::max<uint32_t>(e.count, 1), {}};
  up.packFormat = e.format;
  up.packFlags = e.flags;
  up.packOffset = e.dataOffset;
  up.packSize = e.dataSize;
  auto& s = texs();
  std::lock_guard lock(s.mutex);
  if (s.keep) {  // volcados: con los datos ya leídos, para escribirlos en el archivo
    TexUpload copy = up;
    copy.data.resize(e.dataSize);
    std::lock_guard rl(pk.readMutex);
    if (std::fseek(pk.file, static_cast<long>(e.dataOffset), SEEK_SET) == 0 &&
        std::fread(copy.data.data(), 1, copy.data.size(), pk.file) == copy.data.size()) {
      s.kept[id] = std::move(copy);
    }
  }
  s.pending.push_back(std::move(up));
  return true;
}

void texture_packed_data(uint64_t id, uint32_t width, uint32_t height, uint32_t mips, uint8_t format, uint8_t flags,
                         const uint8_t* data, size_t size) noexcept {
  if (width == 0 || height == 0 || mips == 0 || data == nullptr || format < kPkEtc2Rgb8 || format > kPkRgba8) return;
  TexUpload up{id, width, height, mips, std::vector<uint8_t>(data, data + size)};
  up.packFormat = format;
  up.packFlags = flags;
  auto& s = texs();
  std::lock_guard lock(s.mutex);
  if (s.keep) s.kept[id] = up;
  s.pending.push_back(std::move(up));
}

// Núcleos: el hilo del juego (el que procesa el GX) se queda solo en el último núcleo y los demás hilos del proceso
// (motor de Choya, hilos de la Mali, audio, aurora) en los otros tres, para que nada le quite tiempo ni lo mueva de
// núcleo (cachés frías). Se llama desde el hilo del juego; dos veces, para alcanzar a los hilos que nacen después.
// CHOYA_NUCLEOS=0 lo apaga.
void pin_game_thread() noexcept {
#if defined(__linux__)
  const char* env = std::getenv("CHOYA_NUCLEOS");
  if (env != nullptr && std::strcmp(env, "0") == 0) return;
  const long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
  if (ncpu < 3 || ncpu > CPU_SETSIZE) return;
  const int gameCpu = static_cast<int>(ncpu) - 1;
  cpu_set_t game, others;
  CPU_ZERO(&game);
  CPU_ZERO(&others);
  CPU_SET(gameCpu, &game);
  for (int i = 0; i < gameCpu; ++i) CPU_SET(i, &others);
  const pid_t self = static_cast<pid_t>(syscall(SYS_gettid));
  if (sched_setaffinity(self, sizeof(game), &game) != 0) {
    log_line("motor: no pude fijar el hilo del juego a un núcleo");
    return;
  }
  int moved = 0;
  if (DIR* d = ::opendir("/proc/self/task")) {
    while (const dirent* e = ::readdir(d)) {
      const pid_t tid = static_cast<pid_t>(std::atoi(e->d_name));
      if (tid <= 0 || tid == self) continue;
      if (sched_setaffinity(tid, sizeof(others), &others) == 0) ++moved;
    }
    ::closedir(d);
  }
  log_line("motor: hilo del juego %d solo en el núcleo %d; %d hilos más en los núcleos 0-%d", static_cast<int>(self),
           gameCpu, moved, gameCpu - 1);
#endif
}

void set_keep_textures(bool keep) noexcept {
  auto& s = texs();
  std::lock_guard lock(s.mutex);
  s.keep = keep;
}

static std::chrono::steady_clock::time_point g_captureStart;
static std::atomic<uint64_t> g_captureUs{0};

void capture_begin(const uint8_t* verts, size_t vertsSize, const uint8_t* uniforms, size_t uniformsSize,
                   const uint8_t* indices, size_t indicesSize, const uint8_t* storage, size_t storageSize) noexcept {
  g_captureStart = std::chrono::steady_clock::now();
  {
    std::lock_guard lock(g_frameMutex);
    if (!g_pool.empty() && g_recording.verts.capacity() == 0) {
      g_recording = std::move(g_pool.back());
      g_pool.pop_back();
    }
  }
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
                  float clearDepthValue, bool hasResolve, const Rect& copyRect) noexcept {
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
  it.pass.copy = copyRect;
  g_recording.items.push_back(it);
}

void capture_copy(uint64_t texId, const Rect& rect, uint32_t texW, uint32_t texH, uint32_t fmt, bool opaque) noexcept {
  if (!g_recordingActive) return;
  Item it{};
  it.op = Op::Copy;
  it.copy.tex = texId;
  it.copy.rect = rect;
  it.copy.texW = texW;
  it.copy.texH = texH;
  it.copy.fmt = fmt;
  it.copy.opaque = opaque ? 1 : 0;
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
                  uint32_t uniformOffset, uint32_t dstAlpha, uint64_t bindGroup) noexcept {
  if (!g_recordingActive) return;
  Item it{};
  it.op = Op::Draw;
  it.draw.pipeline = pipeline;
  it.draw.idxOffset = idxOffset;
  it.draw.indexCount = indexCount;
  it.draw.instanceCount = instanceCount;
  it.draw.uniformOffset = uniformOffset;
  it.draw.dstAlpha = dstAlpha;
  it.draw.bindGroup = bindGroup;
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

void capture_end(bool present) noexcept {
  if (!g_recordingActive) return;
  g_recordingActive = false;
  g_captureUs += static_cast<uint64_t>(
      std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - g_captureStart).count());
  g_recording.present = present;
  g_recording.ready = true;
  std::lock_guard lock(g_frameMutex);
  g_open.push_back(std::move(g_recording));
  g_recording = Frame{};
  if (!present) {
    if (g_open.size() > 16) {  // un cuadro que nunca termina: no acumular memoria sin fin
      recycle(std::move(g_open.front()));
      g_open.erase(g_open.begin());
    }
    return;
  }
  Group g;
  g.frames.swap(g_open);
  for (const Frame& f : g.frames) {
    for (const Item& it : f.items) {
      if (it.op == Op::Copy) g.copies.push_back(it.copy.tex);
    }
  }
  std::sort(g.copies.begin(), g.copies.end());
  g.copies.erase(std::unique(g.copies.begin(), g.copies.end()), g.copies.end());
  g_queue.push_back(std::move(g));
  // Los grupos que el motor no alcanzó a dibujar y cuyas copias rehacen los más nuevos, sobran (de atrás hacia
  // adelante, con todas las copias de los grupos posteriores).
  std::vector<uint64_t> later = g_queue.back().copies, merged;
  for (size_t i = g_queue.size() - 1; i-- > 0;) {
    Group& old = g_queue[i];
    if (std::includes(later.begin(), later.end(), old.copies.begin(), old.copies.end())) {
      for (Frame& f : old.frames) recycle(std::move(f));
      g_queue.erase(g_queue.begin() + static_cast<std::ptrdiff_t>(i));
    } else {
      merged.clear();
      std::set_union(later.begin(), later.end(), old.copies.begin(), old.copies.end(), std::back_inserter(merged));
      later.swap(merged);
    }
  }
  // El motor no avanza: no acumular trabajo ni memoria sin fin (aquí ya solo quedan grupos con copias únicas).
  while (g_queue.size() > 6) {
    for (Frame& f : g_queue.front().frames) recycle(std::move(f));
    g_queue.pop_front();
  }
}

void capture_discard() noexcept { g_recordingActive = false; }

// --- Volcado ------------------------------------------------------------------------------------------------
namespace {
constexpr char kDumpMagic[8] = {'C', 'H', 'O', 'Y', 'A', 'G', 'X', '4'};  // 4: texturas del paquete; 3 y 2 también se leen

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
  // Grupos de texturas y texturas que usa el cuadro.
  std::vector<std::pair<uint64_t, BindDesc>> groups;
  for (const Item& it : f.items) {
    if (it.op != Op::Draw || it.draw.bindGroup == 0) continue;
    auto g = g_bindGroups.find(it.draw.bindGroup);
    if (g != g_bindGroups.end()) groups.emplace_back(g->first, g->second);
  }
  const uint64_t ng = groups.size();
  std::fwrite(&ng, sizeof(ng), 1, out);
  std::vector<uint64_t> texIds;
  for (const auto& [ref, d] : groups) {
    std::fwrite(&ref, sizeof(ref), 1, out);
    std::fwrite(&d, sizeof(d), 1, out);
    for (uint64_t id : d.tex) {
      if (id != 0 && std::find(texIds.begin(), texIds.end(), id) == texIds.end()) texIds.push_back(id);
    }
  }
  auto& s = texs();
  std::lock_guard tlock(s.mutex);
  std::vector<const TexUpload*> texs_out;
  for (uint64_t id : texIds) {
    auto k = s.kept.find(id);
    if (k != s.kept.end()) texs_out.push_back(&k->second);
  }
  const uint64_t nt = texs_out.size();
  std::fwrite(&nt, sizeof(nt), 1, out);
  for (const TexUpload* up : texs_out) {
    const uint32_t hdr[4] = {up->width, up->height, up->mips,
                             static_cast<uint32_t>(up->packFormat) | (static_cast<uint32_t>(up->packFlags) << 8)};
    std::fwrite(&up->id, sizeof(up->id), 1, out);
    std::fwrite(hdr, sizeof(hdr), 1, out);
    write_blob(out, up->data);
  }
  std::fclose(out);
  return true;
}

bool load_frame(const char* path) noexcept {
  FILE* in = std::fopen(path, "rb");
  if (!in) return false;
  char magic[8];
  uint32_t header[4];
  bool ok = std::fread(magic, 1, 8, in) == 8 && std::memcmp(magic, kDumpMagic, 7) == 0 &&
            (magic[7] == '2' || magic[7] == '3' || magic[7] == '4') &&
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
  uint64_t ng = 0, nt = 0;
  ok = ok && std::fread(&ng, sizeof(ng), 1, in) == 1;
  for (uint64_t i = 0; ok && i < ng; ++i) {
    uint64_t ref;
    BindDesc d;
    ok = std::fread(&ref, sizeof(ref), 1, in) == 1 && std::fread(&d, sizeof(d), 1, in) == 1;
    if (ok) register_bind_group(ref, d);
  }
  ok = ok && std::fread(&nt, sizeof(nt), 1, in) == 1;
  for (uint64_t i = 0; ok && i < nt; ++i) {
    uint64_t id;
    uint32_t hdr[4] = {0, 0, 0, 0};
    std::vector<uint8_t> data;
    const size_t words = magic[7] == '4' ? 4 : 3;
    ok = std::fread(&id, sizeof(id), 1, in) == 1 && std::fread(hdr, sizeof(uint32_t), words, in) == words &&
         read_blob(in, data);
    if (ok && (hdr[3] & 0xFF) != 0) {
      texture_packed_data(id, hdr[0], hdr[1], hdr[2], static_cast<uint8_t>(hdr[3]), static_cast<uint8_t>(hdr[3] >> 8),
                          data.data(), data.size());
    } else if (ok) {
      texture_data(id, hdr[0], hdr[1], hdr[2], data.data(), data.size());
    }
  }
  std::fclose(in);
  if (!ok) return false;
  g_reversedZ = header[2] != 0;
  f.ready = true;
  std::lock_guard lock(g_frameMutex);
  Group g;
  g.frames.push_back(std::move(f));
  g_queue.push_back(std::move(g));
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
  // Todos los cuadros pendientes, en orden (el juego puede capturar el siguiente mientras tanto); solo el último se
  // muestra, los anteriores dejan sus copias del EFB y lo que dibujaron en el EFB.
  std::deque<Group> groups;
  {
    std::lock_guard lock(g_frameMutex);
    if (g_queue.empty()) return false;
    groups.swap(g_queue);
  }
  std::vector<Frame> batch;
  for (Group& g : groups) {
    for (Frame& f : g.frames) batch.push_back(std::move(f));
  }
  bool drawn = false;
  for (size_t i = 0; i < batch.size(); ++i) {
    drawn = draw_frame_locked(batch[i], screenWidth, screenHeight, i + 1 == batch.size());
  }
  {
    std::lock_guard lock(g_frameMutex);
    for (size_t i = 0; i < batch.size(); ++i) {
      if (i + 1 == batch.size()) std::swap(g_last, batch[i]);  // el g_last anterior se recicla
      recycle(std::move(batch[i]));
    }
  }
  return drawn;
}

namespace {
// Memoria de la consola acotada a propósito, como la memoria de video en galaxy-vita (que no deja crecer los búferes
// del EFB con poca memoria libre y suelta lo que no se usa): con menos de CHOYA_GX_MEMORIA_MIN MB disponibles (96 por
// omisión) el motor suelta los cuadros que guarda para reusar sus búferes y le devuelve al sistema la memoria libre
// del heap (malloc_trim). En la RG35XX H (1 GB, sin swap) eso es lo que separa un tirón de que el sistema mate al juego.
// --- Memoria de la Mali por partes del cuadro ------------------------------------------------------------------
// En Golf la memoria de la GPU (debugfs gpu_memory) pasa de 130 a 530 MB en medio minuto mientras las texturas del
// motor son 16 MB: es memoria que el driver pide por su cuenta. Para saber qué la pide, se lee el total antes y
// después de cada parte del cuadro y la diferencia se suma a esa parte. CHOYA_GX_MALI=0 lo apaga.
enum MaliPart { kMaliSubidas, kMaliDibujos, kMaliCopias, kMaliPantalla, kMaliFuera, kMaliPases, kMaliBorrados,
                kMaliParts };
// Pase por pase (por su número dentro del cuadro): cuánto crece mientras se dibuja en él, y cómo es el pase.
constexpr int kMaliMaxPasses = 32;
struct MaliPass {
  long pages = 0;
  uint32_t frames = 0, draws = 0;
  int w = 0, h = 0;
  uint8_t clears = 0;  // 1 color, 2 profundidad
  uint32_t afterDepthCopy = 0, afterColorCopy = 0;  // en cuántos cuadros vino justo después de copiar el EFB
};
struct MaliProf {
  int state = 0;  // 0 sin probar, 1 activo, -1 apagado
  int fd = -1;    // gpu_memory abierto una vez (pread en cada lectura: antes eran abrir/leer/cerrar 40 veces)
  const char* path = nullptr;
  long last = -1;               // páginas en la última lectura
  long part[kMaliParts] = {};   // páginas sumadas por parte (en el intervalo)
  long reportStart = -1;        // páginas al empezar el intervalo
  long history[3] = {-1, -1, -1};
  MaliPass pass[kMaliMaxPasses];
  int curPass = -1;             // pase abierto en este cuadro
} g_mali;

long mali_read() {
  if (g_mali.fd < 0) return -1;
  char buf[128];
  const ssize_t n = pread(g_mali.fd, buf, sizeof(buf) - 1, 0);
  if (n <= 0) return -1;
  buf[n] = 0;
  char name[64];
  long pages = -1;
  if (std::sscanf(buf, "%63s %ld", name, &pages) != 2) pages = -1;
  return pages;
}

bool mali_on() {
  if (g_mali.state == 0) {
    g_mali.state = -1;
    const char* env = std::getenv("CHOYA_GX_MALI");
    if (env != nullptr && std::strcmp(env, "0") == 0) return false;
    for (const char* p : {"/sys/kernel/debug/mali0/gpu_memory", "/sys/kernel/debug/mali/gpu_memory"}) {
      g_mali.fd = ::open(p, O_RDONLY);
      g_mali.path = p;
      if (g_mali.fd >= 0 && mali_read() >= 0) {
        g_mali.state = 1;
        break;
      }
      if (g_mali.fd >= 0) ::close(g_mali.fd);
      g_mali.fd = -1;
    }
    if (g_mali.state != 1) return false;
    g_mali.last = g_mali.reportStart = mali_read();
    log_line("motor: midiendo la memoria de la Mali por partes del cuadro y por pase (%s)", g_mali.path);
  }
  return g_mali.state == 1;
}

void mali_mark(MaliPart part) {
  if (!mali_on()) return;
  const long now = mali_read();
  if (now < 0) return;
  const long delta = now - g_mali.last;
  g_mali.part[part] += delta;
  if (part == kMaliDibujos && g_mali.curPass >= 0) g_mali.pass[g_mali.curPass].pages += delta;
  g_mali.last = now;
}

// Al empezar un pase: se cierra lo dibujado en el anterior y se anota cómo es el nuevo.
bool g_maliCopiedDepth = false, g_maliCopiedColor = false;  // copias del EFB desde el último pase
void mali_pass_begin(int index, int w, int h, bool clearColor, bool clearDepth) {
  if (!mali_on()) return;
  if (index < 0 || index >= kMaliMaxPasses) {
    g_mali.curPass = -1;
    return;
  }
  MaliPass& p = g_mali.pass[index];
  ++p.frames;
  p.w = w;
  p.h = h;
  p.clears = static_cast<uint8_t>((clearColor ? 1 : 0) | (clearDepth ? 2 : 0));
  if (g_maliCopiedDepth) ++p.afterDepthCopy;
  if (g_maliCopiedColor) ++p.afterColorCopy;
  g_maliCopiedDepth = g_maliCopiedColor = false;
  g_mali.curPass = index;
}

// Cuántos cuadros puede tener la GPU pendientes a la vez (1 a kRing). CHOYA_GX_EN_VUELO=1 obliga a terminar cada cuadro
// antes de empezar el siguiente. Ya no se baja solo: en Golf no frenó el crecimiento (0.17.1) y no sirvió de nada.
int g_inFlight = 3;
bool g_inFlightFixed = false;

void mali_report(double frames) {
  if (g_mali.state != 1) return;
  const long now = g_mali.last;
  const auto mb = [](long pages) { return static_cast<double>(pages) * 4.0 / 1024.0; };
  log_line("motor: Mali %.0f MB (%+.1f MB en %.0f cuadros): subidas %+.1f, empezar pases %+.1f, dibujos %+.1f, "
           "borrados %+.1f, copias del EFB %+.1f, pantalla %+.1f, entre cuadros %+.1f; cuadros en vuelo %d",
           mb(now), mb(now - g_mali.reportStart), frames, mb(g_mali.part[kMaliSubidas]), mb(g_mali.part[kMaliPases]),
           mb(g_mali.part[kMaliDibujos]), mb(g_mali.part[kMaliBorrados]), mb(g_mali.part[kMaliCopias]),
           mb(g_mali.part[kMaliPantalla]), mb(g_mali.part[kMaliFuera]), g_inFlight);
  // Los 4 pases en los que más crece al dibujar (solo si el intervalo creció o algún pase pidió más de 4 MB).
  int order[kMaliMaxPasses];
  for (int i = 0; i < kMaliMaxPasses; ++i) order[i] = i;
  std::sort(order, order + kMaliMaxPasses, [](int a, int b) { return g_mali.pass[a].pages > g_mali.pass[b].pages; });
  for (int k = 0; k < 4; ++k) {
    const MaliPass& p = g_mali.pass[order[k]];
    if (p.frames == 0 || mb(p.pages) < 4.0) break;
    log_line("motor: Mali pase %d (%dx%d, borra %s, %.0f dibujos por cuadro%s%s): %+.1f MB al dibujar (%+.2f MB por "
             "cuadro)", order[k], p.w, p.h,
             p.clears == 3 ? "color y profundidad" : p.clears == 1 ? "color" : p.clears == 2 ? "profundidad" : "nada",
             static_cast<double>(p.draws) / p.frames,
             p.afterDepthCopy * 2 > p.frames ? ", después de copiar la profundidad" : "",
             p.afterColorCopy * 2 > p.frames ? ", después de copiar el color" : "", mb(p.pages),
             mb(p.pages) / p.frames);
  }
  for (MaliPass& p : g_mali.pass) p = MaliPass{};
  for (long& p : g_mali.part) p = 0;
  g_mali.reportStart = now;
  g_mali.history[0] = g_mali.history[1];
  g_mali.history[1] = g_mali.history[2];
  g_mali.history[2] = now;
}

void low_memory_relief() {
  static const long minMb = [] {
    const char* e = std::getenv("CHOYA_GX_MEMORIA_MIN");
    return e != nullptr ? std::atol(e) : 96L;
  }();
  if (minMb <= 0) return;
  long availKb = -1;
  if (FILE* m = std::fopen("/proc/meminfo", "r")) {
    char line[128];
    while (std::fgets(line, sizeof(line), m)) {
      if (std::sscanf(line, "MemAvailable: %ld", &availKb) == 1) break;
    }
    std::fclose(m);
  }
  if (availKb < 0 || availKb / 1024 >= minMb) return;
  size_t pooled = 0;
  {
    std::lock_guard lock(g_frameMutex);
    pooled = g_pool.size();
    g_pool.clear();
    g_pool.shrink_to_fit();
  }
  g_resolvedBinds.clear();  // se rehacen solas
#if defined(__GLIBC__)
  malloc_trim(0);
#endif
  long afterKb = -1;
  if (FILE* m = std::fopen("/proc/meminfo", "r")) {
    char line[128];
    while (std::fgets(line, sizeof(line), m)) {
      if (std::sscanf(line, "MemAvailable: %ld", &afterKb) == 1) break;
    }
    std::fclose(m);
  }
  log_line("motor: memoria baja (%ld MB disponibles, mínimo %ld): solté %zu cuadros guardados y el heap libre; quedan "
           "%ld MB", availKb / 1024, minMb, pooled, afterKb / 1024);
}

bool draw_frame_locked(Frame& f, int screenWidth, int screenHeight, bool present) {
  present = present && f.present;
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
  process_textures();
  uint64_t lastBindGroup = ~0ull;
  uint64_t lastPipeRef = ~0ull;
  const ResolvedPipe* lastPipe = nullptr;
  int passIndex = -1;
  g_mali.curPass = -1;
  g_maliCopiedDepth = g_maliCopiedColor = false;
  g_stats.bytesVerts += f.verts.size();
  g_stats.bytesUniforms += f.uniforms.size();
  g_stats.bytesIndices += f.indices.size();
  g_stats.bytesStorage += f.storage.size();

  // Subir los datos del cuadro a los recursos que tocan en el anillo.
  mali_mark(kMaliFuera);
  static const bool inFlightInit = [] {
    if (const char* e = std::getenv("CHOYA_GX_EN_VUELO")) {
      g_inFlight = std::clamp(std::atoi(e), 1, kRing);
      g_inFlightFixed = true;
    }
    return true;
  }();
  (void)inFlightInit;
  if (g_inFlight < kRing) {
    // Esperar también al cuadro de hace g_inFlight (los de antes ya terminaron si ese terminó).
    FrameResources& prev = g_ring[(g_ringIndex + kRing - g_inFlight) % kRing];
    if (prev.fence != nullptr) {
      const auto waitStart = std::chrono::steady_clock::now();
      for (int tries = 0; tries < 20; ++tries) {
        if (gl.ClientWaitSync(prev.fence, 1, 100000000ull) != 0x911B /* GL_TIMEOUT_EXPIRED */) break;
      }
      g_stats.gpuWaitMs +=
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waitStart).count();
    }
  }
  FrameResources& res = g_ring[g_ringIndex];
  g_ringIndex = (g_ringIndex + 1) % kRing;
  // Antes de reusar los recursos, esperar a que la GPU termine el cuadro que los usó (hace kRing cuadros). Sin esto
  // la CPU le mandaba cuadros a la Mali más rápido de lo que los dibuja (el intercambio no espera) y la memoria de la
  // GPU crecía hasta acabarse la RAM (Golf: de 160 a 530 MB y el juego a 0.1 cuadros/s).
  if (res.fence != nullptr) {
    const auto waitStart = std::chrono::steady_clock::now();
    constexpr GLenum kTimeoutExpired = 0x911B;
    for (int tries = 0; tries < 20; ++tries) {  // hasta 2 s
      const GLenum r = gl.ClientWaitSync(res.fence, 1 /* GL_SYNC_FLUSH_COMMANDS_BIT */, 100000000ull);
      if (r != kTimeoutExpired) break;  // listo o falló (kWaitFailed): no esperar más
    }
    gl.DeleteSync(res.fence);
    res.fence = nullptr;
    g_stats.gpuWaitMs +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waitStart).count();
  }
  gl.ActiveTexture(kTexture0);
  if (!upload_words(res.vtex, f.verts, g_persistent ? &res.rv : nullptr)) {
    log_line("motor: cuadro demasiado grande para la textura de vértices");
    return false;
  }
  gl.ActiveTexture(kTexture0 + 1);
  if (!upload_words(res.atex, f.storage, g_persistent ? &res.ra : nullptr)) {
    log_line("motor: cuadro demasiado grande para la textura de arreglos");
    return false;
  }
  GLuint ubo = res.ubo;
  bool ringUniforms = false;
  if (g_persistent && ensure_ring(res.ru, f.uniforms.size(), 4u << 20) &&
      (f.indices.empty() || ensure_ring(res.ri, f.indices.size(), 2u << 20))) {
    // Uniformes e índices en los anillos persistentes: memcpy y listo.
    std::memcpy(res.ru.ptr, f.uniforms.data(), f.uniforms.size());
    ubo = res.ru.buf;
    gl.BindVertexArray(g_vao);
    if (!f.indices.empty()) {
      std::memcpy(res.ri.ptr, f.indices.data(), f.indices.size());
      gl.BindBuffer(kElementArrayBuffer, res.ri.buf);
    }
    ringUniforms = true;
  }
  // Sin búferes persistentes: la GPU ya soltó estos búferes (la espera de arriba), así que se reescriben sin pedir
  // memoria nueva; solo crecen si el cuadro no cabe.
  if (!ringUniforms) {
  gl.BindBuffer(kUniformBuffer, res.ubo);
  if (f.uniforms.size() > res.uboSize) {
    res.uboSize = f.uniforms.size() + f.uniforms.size() / 4;
    gl.BufferData(kUniformBuffer, static_cast<GLsizeiptr>(res.uboSize), nullptr, kStreamDraw);
  }
  gl.BufferSubData(kUniformBuffer, 0, static_cast<GLsizeiptr>(f.uniforms.size()), f.uniforms.data());
  gl.BindVertexArray(g_vao);
  gl.BindBuffer(kElementArrayBuffer, res.ebo);
  if (!f.indices.empty()) {
    if (f.indices.size() > res.eboSize) {
      res.eboSize = f.indices.size() + f.indices.size() / 4;
      gl.BufferData(kElementArrayBuffer, static_cast<GLsizeiptr>(res.eboSize), nullptr, kStreamDraw);
    }
    gl.BufferSubData(kElementArrayBuffer, 0, static_cast<GLsizeiptr>(f.indices.size()), f.indices.data());
  }
  }
  gl.ActiveTexture(kTexture0);
  gl.BindTexture(kTexture2D, res.vtex.id);
  gl.ActiveTexture(kTexture0 + 1);
  gl.BindTexture(kTexture2D, res.atex.id);
  uint32_t drawIndex = 0;
  mali_mark(kMaliSubidas);
  shadow_invalidate();  // las subidas y la textura de vértices cambiaron estado por fuera
  S.unit = 1;


  // Coordenadas del pase (origen arriba a la izquierda) -> GL (origen abajo) dentro del EFB.
  int passW = efbW, passH = efbH;
  Rect passCopy{0, 0, efbW, efbH};
  auto to_gl_rect = [&](float x, float y, float w, float h, GLint out[4]) {
    out[0] = static_cast<GLint>(x);
    out[1] = static_cast<GLint>(static_cast<float>(g_efb.height) - y - h);
    out[2] = static_cast<GLint>(w + 0.5f);
    out[3] = static_cast<GLint>(h + 0.5f);
  };
  GLint scissorRect[4] = {0, g_efb.height - efbH, efbW, efbH};
  GLint viewportRect[4] = {0, g_efb.height - efbH, efbW, efbH};
  bool presented = false;
  bool passOpen = false;
  bool passResolves = false;
  auto end_pass = [&]() {
    // La copia a pantalla (XFB) es el EFB al terminar el pase que se copia (GXCopyDisp u otra copia).
    if (passOpen && passResolves && present) {
      mali_mark(kMaliDibujos);
      present_efb(passCopy, screenWidth, screenHeight);
      mali_mark(kMaliPantalla);
      presented = true;
      shadow_invalidate();
    }
  };
  for (const Item& it : f.items) {
    switch (it.op) {
    case Op::Pass: {
      mali_mark(kMaliDibujos);  // lo dibujado en el pase anterior
      end_pass();
      ++g_stats.passes;
      passOpen = true;
      passResolves = it.pass.hasResolve != 0;
      passW = it.pass.width ? static_cast<int>(it.pass.width) : efbW;
      passH = it.pass.height ? static_cast<int>(it.pass.height) : efbH;
      passCopy = it.pass.copy.width > 0 && it.pass.copy.height > 0 ? it.pass.copy : Rect{0, 0, passW, passH};
      viewportRect[0] = 0;
      viewportRect[1] = g_efb.height - passH;
      viewportRect[2] = passW;
      viewportRect[3] = passH;
      set_viewport(viewportRect);
      set_enable(kScissorTest, S.scissorOn, false);
      GLbitfield bits = 0;
      if (it.pass.clearColor) {
        set_color_mask(true, true);
        gl.ClearColor(it.pass.rgba[0], it.pass.rgba[1], it.pass.rgba[2], it.pass.rgba[3]);
        bits |= kColorBufferBit;
      }
      if (it.pass.clearDepth) {
        set_depth_mask(true);
        gl.ClearDepthf(it.pass.depth);
        bits |= kDepthBufferBit;
      }
      if (bits) gl.Clear(bits);
      mali_mark(kMaliPases);
      mali_pass_begin(++passIndex, passW, passH, it.pass.clearColor != 0, it.pass.clearDepth != 0);
      set_enable(kScissorTest, S.scissorOn, true);
      scissorRect[0] = 0;
      scissorRect[1] = g_efb.height - passH;
      scissorRect[2] = passW;
      scissorRect[3] = passH;
      set_scissor(scissorRect);
    } break;
    case Op::Viewport: {
      GLint r[4];
      to_gl_rect(it.viewport.left, it.viewport.top, it.viewport.width, it.viewport.height, r);
      set_viewport(r);
      std::memcpy(viewportRect, r, sizeof(r));
      const float minDepth = g_reversedZ ? 1.0f - it.viewport.zfar : it.viewport.znear;
      const float maxDepth = g_reversedZ ? 1.0f - it.viewport.znear : it.viewport.zfar;
      set_depth_range(minDepth, maxDepth);
    } break;
    case Op::Scissor: {
      to_gl_rect(static_cast<float>(it.scissor.x), static_cast<float>(it.scissor.y),
                     static_cast<float>(it.scissor.width), static_cast<float>(it.scissor.height), scissorRect);
      set_scissor(scissorRect);
    } break;
    case Op::Copy: {
      mali_mark(kMaliDibujos);
      if (copies_enabled()) run_copy(it);
      mali_mark(kMaliCopias);
      ((it.copy.fmt & 0x10) != 0 ? g_maliCopiedDepth : g_maliCopiedColor) = true;
      shadow_invalidate();  // la copia usó su propio programa, estado y unidades 10/11
      set_viewport(viewportRect);
      set_enable(kScissorTest, S.scissorOn, true);
      set_scissor(scissorRect);
      gl.BindVertexArray(g_vao);
      lastBindGroup = ~0ull;  // las texturas de copia pudieron rehacerse
    } break;
    case Op::Clear: {
      ClearDesc cc;
      if (!find_clear(it.clear.pipeline, cc)) {
        ++g_stats.skippedConfig;
        break;
      }
      GLint r[4] = {scissorRect[0], scissorRect[1], scissorRect[2], scissorRect[3]};
      if (it.clear.useScissor) {
        to_gl_rect(static_cast<float>(it.clear.scissor.x), static_cast<float>(it.clear.scissor.y),
                       static_cast<float>(it.clear.scissor.width), static_cast<float>(it.clear.scissor.height), r);
      }
      set_scissor(r);
      set_color_mask(cc.color != 0, cc.alpha != 0);
      set_depth_mask(cc.depth != 0);
      mali_mark(kMaliDibujos);
      if ((cc.color || cc.alpha || cc.depth) && clear_by_draw()) {
        const GLint full[4] = {0, 0, g_efb.width, g_efb.height};
        set_viewport(full);  // gl_FragDepth no depende del rango de profundidad: no se toca
        set_program(g_clearProgram.id);
        gl.Uniform4f(g_clearProgram.color, it.clear.rgba[0], it.clear.rgba[1], it.clear.rgba[2], it.clear.rgba[3]);
        gl.Uniform4f(g_clearProgram.depth, std::clamp(it.clear.depth, 0.f, 1.f), 0.f, 0.f, 0.f);
        set_enable(kBlend, S.blendOn, false);
        set_enable(kCullFace, S.cullOn, false);
        set_enable(kDepthTest, S.depthTest, true);
        if (S.depthFunc != kAlways) {
          S.depthFunc = kAlways;
          gl.DepthFunc(kAlways);
        }
        S.lastValid = false;  // el siguiente dibujo vuelve a poner su estado
        gl.DrawArrays(kTriangles, 0, 3);
        set_viewport(viewportRect);
      } else {
        gl.ClearColor(it.clear.rgba[0], it.clear.rgba[1], it.clear.rgba[2], it.clear.rgba[3]);
        gl.ClearDepthf(it.clear.depth);
        GLbitfield bits = 0;
        if (cc.color || cc.alpha) bits |= kColorBufferBit;
        if (cc.depth) bits |= kDepthBufferBit;
        if (bits) gl.Clear(bits);
      }
      mali_mark(kMaliBorrados);
      set_scissor(scissorRect);
      ++g_stats.clears;
    } break;
    case Op::Draw: {
      if (it.draw.pipeline != lastPipeRef) {
        lastPipe = resolve_pipe(it.draw.pipeline);
        lastPipeRef = lastPipe != nullptr ? it.draw.pipeline : ~0ull;
      }
      const ResolvedPipe* rp = lastPipe;
      if (rp == nullptr) {
        ++g_stats.skippedConfig;
        break;
      }
      if (rp->skipLine || it.draw.instanceCount != 1) {
        ++g_stats.skippedLine;
        break;
      }
      if (rp->noWrite) {
        ++g_stats.rejectedNoWrite;
        break;
      }
      if (scissorRect[2] <= 0 || scissorRect[3] <= 0) {  // recorte vacío: no puede pintar ni un píxel
        ++g_stats.rejectedScissor;
        break;
      }
      if (it.draw.uniformOffset % static_cast<uint32_t>(g_uboAlign) != 0 ||
          it.draw.uniformOffset + 16384u > f.uniforms.size()) {
        ++g_stats.skippedAlign;
        break;
      }
      const Program& prog = *rp->prog;
      if (!prog.ok) {
        ++g_stats.skippedProgram;
        break;
      }
      set_program(prog.id);
      set_ubo(ubo, static_cast<GLintptr>(it.draw.uniformOffset));
      if (it.draw.bindGroup != lastBindGroup) {
        bind_textures(it.draw.bindGroup);
        lastBindGroup = it.draw.bindGroup;
      }
      apply_state(with_dst_alpha(rp->state, it.draw.dstAlpha));
      if (debug_colors()) {
        static const float kPalette[12][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 0}, {1, 0, 1}, {0, 1, 1},
                                              {1, .5f, 0}, {.5f, 0, 1}, {0, .6f, .3f}, {.6f, .3f, 0}, {.3f, .3f, .3f},
                                              {1, 1, 1}};
        const float* c = kPalette[drawIndex % 12];
        gl.Uniform4f(prog.debugLoc, c[0], c[1], c[2], 1.f);
        set_enable(kBlend, S.blendOn, false);
        set_color_mask(true, true);
      }
      ++drawIndex;
      if (g_mali.curPass >= 0) ++g_mali.pass[g_mali.curPass].draws;
      gl.DrawElements(kTriangles, static_cast<GLsizei>(it.draw.indexCount), kUnsignedShort,
                      reinterpret_cast<const void*>(static_cast<uintptr_t>(it.draw.idxOffset)));
      ++g_stats.draws;
    } break;
    }
  }
  end_pass();
  mali_mark(kMaliDibujos);
  if (!presented && present) {
    // Ningún pase se copió: se muestra cómo quedó el último.
    present_efb(Rect{0, 0, passW, passH}, screenWidth, screenHeight);
    presented = true;
    mali_mark(kMaliPantalla);
  }
  gl.BindFramebuffer(kFramebuffer, 0);
  gl.Disable(kScissorTest);
  gl.ColorMask(1, 1, 1, 1);
  gl.DepthMask(1);

  res.fence = gl.FenceSync(0x9117 /* GL_SYNC_GPU_COMMANDS_COMPLETE */, 0);
  g_stats.cpuMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cpuStart).count();
  // Informe cada 300 cuadros o cada 10 s (lo que llegue antes: con el juego lento también se ve qué pasa).
  ++g_stats.frames;
  ++g_stats.interval;
  static auto lastReport = std::chrono::steady_clock::now();
  const auto nowReport = std::chrono::steady_clock::now();
  if (g_stats.interval >= 300 || nowReport - lastReport >= std::chrono::seconds(10)) {
    lastReport = nowReport;
    const double n = static_cast<double>(g_stats.interval);
    log_line("motor: KB por cuadro: vértices %.0f, uniformes %.0f, índices %.0f, arreglos %.0f; CPU del motor %.2f ms "
             "por cuadro (copia %.2f ms, esperando a la GPU %.2f ms)",
             g_stats.bytesVerts / n / 1024, g_stats.bytesUniforms / n / 1024, g_stats.bytesIndices / n / 1024,
             g_stats.bytesStorage / n / 1024, g_stats.cpuMs / n, g_captureUs.exchange(0) / n / 1000.0,
             g_stats.gpuWaitMs / n);
    log_line("motor: texturas: %zu en GL, %llu subidas, %zu copias del EFB (%.1f por cuadro), %llu enlaces sin textura "
             "(paletas u otras)",
             g_textures.size(), static_cast<unsigned long long>(g_stats.texUploads), g_copyTextures.size(),
             g_stats.copies / n, static_cast<unsigned long long>(g_stats.missingTextures));
    log_line("motor: %llu cuadros, por cuadro: %.1f dibujos, %.1f borrados, %.1f pases; saltados: %llu sin config, "
         "%llu líneas/puntos, %llu alineación, %llu sin programa; programas %zu; error GL 0x%04X",
         static_cast<unsigned long long>(g_stats.frames), g_stats.draws / n, g_stats.clears / n,
         g_stats.passes / n, static_cast<unsigned long long>(g_stats.skippedConfig),
         static_cast<unsigned long long>(g_stats.skippedLine), static_cast<unsigned long long>(g_stats.skippedAlign),
         static_cast<unsigned long long>(g_stats.skippedProgram), g_programs.size(), gl.GetError());
    {
      const double st = static_cast<double>(g_stats.stateSkipped + g_stats.stateApplied);
      const double tb = static_cast<double>(g_stats.texBindsSkipped + g_stats.texBinds);
      log_line("motor: estado GL repetido sin llamar al driver %.0f %%, texturas ya enlazadas %.0f %%; dibujos que no "
               "pintan %.1f por cuadro (sin escritura %llu, recorte vacío %llu); programas nuevos %llu compilados "
               "(%.0f ms), %llu del caché de disco",
               st > 0 ? 100.0 * static_cast<double>(g_stats.stateSkipped) / st : 0.0,
               tb > 0 ? 100.0 * static_cast<double>(g_stats.texBindsSkipped) / tb : 0.0,
               static_cast<double>(g_stats.rejectedNoWrite + g_stats.rejectedScissor) / n,
               static_cast<unsigned long long>(g_stats.rejectedNoWrite),
               static_cast<unsigned long long>(g_stats.rejectedScissor),
               static_cast<unsigned long long>(g_stats.progCompiled), g_stats.compileMs,
               static_cast<unsigned long long>(g_stats.progCached));
    }
    if (pack().file != nullptr) {
      auto& pk = pack();
      log_line("motor: paquete convertido: %llu texturas del juego encontradas (%.1f MB), %llu que no están (se usan las "
               "de aurora), %llu subidas en este intervalo",
               static_cast<unsigned long long>(pk.hits.load()), static_cast<double>(pk.bytes.load()) / 1048576.0,
               static_cast<unsigned long long>(pk.misses.load()), static_cast<unsigned long long>(g_stats.packUploads));
    }
    // Memoria: el proceso y lo que guarda el motor (para ver si algo crece sin parar).
    long rssPages = 0;
    if (FILE* statm = std::fopen("/proc/self/statm", "r")) {
      long sizePages = 0;
      if (std::fscanf(statm, "%ld %ld", &sizePages, &rssPages) != 2) rssPages = 0;
      std::fclose(statm);
    }
    size_t groups = 0, bindGroups = 0, pipelines = 0, pending = 0, pendingBytes = 0, kept = 0, keptBytes = 0;
    {
      std::lock_guard lock(g_frameMutex);
      groups = g_queue.size();
    }
    {
      std::lock_guard lock(g_pipelineMutex);
      bindGroups = g_bindGroups.size();
      pipelines = g_pipelines.size();
    }
    {
      auto& ts = texs();
      std::lock_guard lock(ts.mutex);
      pending = ts.pending.size();
      for (const auto& up : ts.pending) pendingBytes += up.data.size();
      kept = ts.kept.size();
      for (const auto& [id, up] : ts.kept) keptBytes += up.data.size();
    }
    long rssAnon = 0, rssFile = 0, rssShmem = 0;  // KB
    if (FILE* st = std::fopen("/proc/self/status", "r")) {
      char line[128];
      while (std::fgets(line, sizeof(line), st)) {
        std::sscanf(line, "RssAnon: %ld", &rssAnon);
        std::sscanf(line, "RssFile: %ld", &rssFile);
        std::sscanf(line, "RssShmem: %ld", &rssShmem);
      }
      std::fclose(st);
    }
    log_line("motor: memoria del proceso %ld MB (anónima %ld, archivos/GPU %ld, compartida %ld); texturas en GL %zu MB; "
             "grupos en cola %zu, grupos de texturas %zu, pipelines %zu, texturas por subir %zu (%zu KB), guardadas para "
             "volcados %zu (%zu MB)",
             rssPages * 4 / 1024, rssAnon / 1024, rssFile / 1024, rssShmem / 1024, g_texBytes >> 20, groups,
             bindGroups, pipelines, pending, pendingBytes / 1024, kept, keptBytes >> 20);
    mali_report(n);
    low_memory_relief();
    g_stats = Stats{.frames = g_stats.frames};
  }
  return presented;
}
} // namespace
} // namespace choya::gx
