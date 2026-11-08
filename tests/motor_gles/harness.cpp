// Prueba del motor de Choya (paso 2a) con Mesa llvmpipe: dibuja un cuadro sintético y lee píxeles.
#include "../lib/choya/choya_gx.hpp"
#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace aurora;
namespace aurora::gx {
ShaderInfo build_shader_info(const ShaderConfig& c) noexcept {
  ShaderInfo i; i.sampledColorChannels.set(0); i.matrixLayout.postexCount = 1; i.matrixLayout.nrmCount = 0; return i; }
}
static void* egl; static void* gles;
extern "C" void* choya_gl_proc(const char* n) { void* p = dlsym(gles, n); return p; }
extern "C" void choya_gles_log(const char* l) { std::printf("[log] %s\n", l); }
typedef void* (*GetDisplayFn)(void*);
static void put_be_f32(std::vector<uint8_t>& v, float f){ uint32_t u; memcpy(&u,&f,4); v.push_back(u>>24); v.push_back(u>>16); v.push_back(u>>8); v.push_back(u); }
static void put_le_f32(std::vector<uint8_t>& v, float f){ uint32_t u; memcpy(&u,&f,4); v.push_back(u); v.push_back(u>>8); v.push_back(u>>16); v.push_back(u>>24); }
static void put_u32(std::vector<uint8_t>& v, size_t w, uint32_t x){ memcpy(v.data()+w*4,&x,4); }
static void put_f(std::vector<uint8_t>& v, size_t w, float x){ memcpy(v.data()+w*4,&x,4); }
int main() {
  egl = dlopen("libEGL.so.1", RTLD_NOW); gles = dlopen("libGLESv2.so.2", RTLD_NOW);
  auto GetDisplay = (GetDisplayFn)dlsym(egl, "eglGetDisplay");
  auto Initialize = (unsigned(*)(void*,int*,int*))dlsym(egl, "eglInitialize");
  auto BindAPI = (unsigned(*)(unsigned))dlsym(egl, "eglBindAPI");
  auto ChooseConfig = (unsigned(*)(void*,const int*,void**,int,int*))dlsym(egl, "eglChooseConfig");
  auto CreatePbuffer = (void*(*)(void*,void*,const int*))dlsym(egl, "eglCreatePbufferSurface");
  auto CreateContext = (void*(*)(void*,void*,void*,const int*))dlsym(egl, "eglCreateContext");
  auto MakeCurrent = (unsigned(*)(void*,void*,void*,void*))dlsym(egl, "eglMakeCurrent");
  void* d = GetDisplay(nullptr); int ma, mi; if (!Initialize(d,&ma,&mi)) { std::puts("sin EGL"); return 2; }
  BindAPI(0x30A0);
  const int ca[] = {0x3033, 0x0001, 0x3040, 0x0040, 0x3024,8,0x3023,8,0x3022,8,0x3021,8,0x3025,24, 0x3038};
  void* cfg; int n; if (!ChooseConfig(d, ca, &cfg, 1, &n) || !n) { std::puts("sin config"); return 2; }
  const int pa[] = {0x3057, 640, 0x3056, 480, 0x3038};
  void* s = CreatePbuffer(d, cfg, pa);
  const int xa[] = {0x3098, 3, 0x30FB, 2, 0x3038};
  void* c = CreateContext(d, cfg, nullptr, xa);
  if (!s || !c || !MakeCurrent(d, s, s, c)) { std::puts("sin contexto"); return 2; }
  auto GetString = (const char*(*)(unsigned))dlsym(gles, "glGetString");
  std::printf("GL: %s / %s\n", GetString(0x1F02), GetString(0x1F01));

  // Dibujo 1: posición f32 directa (big endian) + color RGBA8 del vértice (rojo).
  gx::PipelineConfig p1{};
  auto& s1 = p1.shaderConfig;
  s1.vtxStride = 16;
  s1.attrs[GX_VA_POS] = {GX_DIRECT, 3, GX_F32, 0, 0, 0, false, 0};
  s1.attrs[GX_VA_CLR0] = {GX_DIRECT, 4, GX_RGBA8, 12, 0, 0, false, 0};
  s1.colorChannels[0].matSrc = GX_SRC_VTX; s1.colorChannels[GX_ALPHA0].matSrc = GX_SRC_VTX;
  p1.depthFunc = GX_ALWAYS; p1.cullMode = GX_CULL_NONE; p1.blendMode = GX_BM_NONE; p1.pixelFmt = GX_PF_RGB8_Z24;
  p1.dstAlpha = UINT32_MAX; p1.colorUpdate = true; p1.alphaUpdate = true; p1.depthUpdate = true;
  // Dibujo 2: posición indexada (u8) desde los arreglos, flotantes little endian, color de material (verde).
  gx::PipelineConfig p2 = p1;
  auto& s2 = p2.shaderConfig;
  s2.vtxStride = 1;
  s2.attrs[GX_VA_POS] = {GX_INDEX8, 3, GX_F32, 0, 12, 0, true, 0};
  s2.attrs[GX_VA_CLR0] = {};
  s2.colorChannels[0].matSrc = GX_SRC_REG; s2.colorChannels[GX_ALPHA0].matSrc = GX_SRC_REG;
  choya::register_gx_pipeline(1, p1);
  choya::register_gx_pipeline(2, p2);
  std::vector<uint8_t> verts;
  float tri1[3][3] = {{-0.9f,-0.5f,0.5f},{-0.1f,-0.5f,0.5f},{-0.5f,0.5f,0.5f}};
  for (auto& v : tri1) { put_be_f32(verts, v[0]); put_be_f32(verts, v[1]); put_be_f32(verts, v[2]);
    verts.push_back(255); verts.push_back(0); verts.push_back(0); verts.push_back(255); }
  const uint32_t vtx2 = static_cast<uint32_t>(verts.size());
  verts.push_back(2); verts.push_back(0); verts.push_back(1);  // índices u8 al arreglo
  std::vector<uint8_t> storage;
  float tri2[3][3] = {{0.5f,0.5f,0.5f},{0.1f,-0.5f,0.5f},{0.9f,-0.5f,0.5f}};
  for (auto& v : tri2) { put_le_f32(storage, v[0]); put_le_f32(storage, v[1]); put_le_f32(storage, v[2]); }
  std::vector<uint8_t> uni(512, 0);
  for (int k = 0; k < 2; ++k) {
    size_t b = k * 64;  // palabras (256 bytes)
    put_u32(uni, b + 0, k == 0 ? 0 : vtx2);
    for (int r = 0; r < 4; ++r) put_f(uni, b + 20 + r * 4 + r, 1.f);       // proj identidad
    for (int r = 0; r < 3; ++r) put_f(uni, b + 36 + r * 4 + r, 1.f);       // postex identidad
    if (k == 1) { put_f(uni, b + 48 + 1, 1.f); put_f(uni, b + 48 + 3, 1.f); }  // material verde
  }
  std::vector<uint16_t> idx = {0, 1, 2, 0, 1, 2};
  choya::capture_begin(verts.data(), verts.size(), uni.data(), uni.size(), (const uint8_t*)idx.data(), idx.size()*2,
                       storage.data(), storage.size());
  const float blue[4] = {0, 0, 1, 1};
  choya::capture_pass(640, 480, true, true, blue, 0.f);
  choya::capture_viewport(gfx::Viewport{0, 0, 640, 480, 0, 1});
  choya::capture_scissor(gfx::ClipRect{0, 0, 640, 480});
  gx::DrawData d1{}; d1.pipeline = 1; d1.idxRange = {0, 6}; d1.indexCount = 3; d1.instanceCount = 1; d1.uniformRange = {0, 256}; d1.dstAlpha = UINT32_MAX;
  gx::DrawData d2 = d1; d2.pipeline = 2; d2.idxRange = {6, 6}; d2.uniformRange = {256, 256};
  choya::capture_gx_draw(d1);
  choya::capture_gx_draw(d2);
  choya::capture_end();
  if (!choya_gx_draw_frame(640, 480)) { std::puts("no dibujó"); return 1; }
  auto ReadPixels = (void(*)(int,int,int,int,unsigned,unsigned,void*))dlsym(gles, "glReadPixels");
  auto px = [&](int x, int y) { unsigned char p[4]; ReadPixels(x, y, 1, 1, 0x1908, 0x1401, p);
    std::printf("pixel (%d,%d) = %d,%d,%d\n", x, y, p[0], p[1], p[2]); return (p[0] << 16) | (p[1] << 8) | p[2]; };
  int a = px(160, 240), b = px(480, 240), z = px(320, 470);
  bool ok = a == 0xFF0000 && b == 0x00FF00 && z == 0x0000FF;
  // Cuadro 2: posición s16 con frac 8 (big endian), color RGB565 amarillo, pase de 640x528 con viewport escalado.
  gx::PipelineConfig p3 = p1;
  auto& s3 = p3.shaderConfig;
  s3.vtxStride = 8;
  s3.attrs[GX_VA_POS] = {GX_DIRECT, 3, GX_S16, 0, 0, 8, false, 0};
  s3.attrs[GX_VA_CLR0] = {GX_DIRECT, 4, GX_RGB565, 6, 0, 0, false, 0};
  choya::register_gx_pipeline(3, p3);
  std::vector<uint8_t> v3;
  auto s16 = [&](float f) { int16_t q = (int16_t)(f * 256); v3.push_back((uint16_t)q >> 8); v3.push_back((uint16_t)q & 0xFF); };
  float t3[3][2] = {{-1.f,-1.f},{1.f,-1.f},{0.f,1.f}};
  for (auto& v : t3) { s16(v[0]); s16(v[1]); s16(0.5f); v3.push_back(0xFF); v3.push_back(0xE0); }
  std::vector<uint8_t> u3(256, 0);
  for (int r = 0; r < 4; ++r) put_f(u3, 20 + r * 4 + r, 1.f);
  for (int r = 0; r < 3; ++r) put_f(u3, 36 + r * 4 + r, 1.f);
  std::vector<uint16_t> i3 = {0, 1, 2};
  choya::capture_begin(v3.data(), v3.size(), u3.data(), u3.size(), (const uint8_t*)i3.data(), 6, nullptr, 0);
  const float black[4] = {0, 0, 0, 1};
  choya::capture_pass(640, 528, true, true, black, 0.f);
  // viewport en la mitad superior del EFB (528 de alto): en pantalla debe quedar arriba.
  choya::capture_viewport(gfx::Viewport{0, 0, 640, 264, 0, 1});
  choya::capture_scissor(gfx::ClipRect{0, 0, 640, 528});
  gx::DrawData d3 = d1; d3.pipeline = 3; d3.idxRange = {0, 6}; d3.uniformRange = {0, 256};
  choya::capture_gx_draw(d3);
  choya::capture_end();
  choya_gx_draw_frame(640, 480);
  int top = px(320, 400), bottom = px(320, 100);
  ok = ok && top == 0xFFFF00 && bottom == 0x000000;
  std::puts(ok ? "PRUEBA OK" : "PRUEBA FALLÓ");
  return ok ? 0 : 1;
}
