// Prueba del motor de Choya con Mesa llvmpipe.\n// Sin argumentos: dibuja cuadros sintéticos y lee píxeles.\n// Con argumentos (cuadro.bin salida.ppm): vuelve a dibujar un cuadro volcado en la consola (CHOYA_GX_VOLCADO).
#include "choya_gx_core.hpp"
#include <dolphin/gx/GXEnum.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
using namespace choya;
static void* egl; static void* gles;
static void* gl_proc(const char* n) { return dlsym(gles, n); }
static void log_line(const char* l) { std::printf("[log] %s\n", l); }
typedef void* (*GetDisplayFn)(void*);
static void put_be_f32(std::vector<uint8_t>& v, float f){ uint32_t u; memcpy(&u,&f,4); v.push_back(u>>24); v.push_back(u>>16); v.push_back(u>>8); v.push_back(u); }
static void put_le_f32(std::vector<uint8_t>& v, float f){ uint32_t u; memcpy(&u,&f,4); v.push_back(u); v.push_back(u>>8); v.push_back(u>>16); v.push_back(u>>24); }
static void put_u32(std::vector<uint8_t>& v, size_t w, uint32_t x){ memcpy(v.data()+w*4,&x,4); }
static void put_f(std::vector<uint8_t>& v, size_t w, float x){ memcpy(v.data()+w*4,&x,4); }

static bool init_egl(int w, int h) {
  egl = dlopen("libEGL.so.1", RTLD_NOW); gles = dlopen("libGLESv2.so.2", RTLD_NOW);
  if (!egl || !gles) return false;
  auto GetDisplay = (GetDisplayFn)dlsym(egl, "eglGetDisplay");
  auto Initialize = (unsigned(*)(void*,int*,int*))dlsym(egl, "eglInitialize");
  auto BindAPI = (unsigned(*)(unsigned))dlsym(egl, "eglBindAPI");
  auto ChooseConfig = (unsigned(*)(void*,const int*,void**,int,int*))dlsym(egl, "eglChooseConfig");
  auto CreatePbuffer = (void*(*)(void*,void*,const int*))dlsym(egl, "eglCreatePbufferSurface");
  auto CreateContext = (void*(*)(void*,void*,void*,const int*))dlsym(egl, "eglCreateContext");
  auto MakeCurrent = (unsigned(*)(void*,void*,void*,void*))dlsym(egl, "eglMakeCurrent");
  void* d = GetDisplay(nullptr); int ma, mi; if (!Initialize(d,&ma,&mi)) return false;
  BindAPI(0x30A0);
  const int ca[] = {0x3033, 0x0001, 0x3040, 0x0040, 0x3024,8,0x3023,8,0x3022,8,0x3021,8,0x3025,24, 0x3038};
  void* cfg; int n; if (!ChooseConfig(d, ca, &cfg, 1, &n) || !n) return false;
  const int pa[] = {0x3057, w, 0x3056, h, 0x3038};
  void* s = CreatePbuffer(d, cfg, pa);
  const int xa[] = {0x3098, 3, 0x30FB, 2, 0x3038};
  void* c = CreateContext(d, cfg, nullptr, xa);
  if (!s || !c || !MakeCurrent(d, s, s, c)) return false;
  gx::set_gl_loader(gl_proc);
  gx::set_logger(log_line);
  return true;
}

static int pixel(int x, int y) {
  static auto ReadPixels = (void(*)(int,int,int,int,unsigned,unsigned,void*))dlsym(gles, "glReadPixels");
  unsigned char p[4]; ReadPixels(x, y, 1, 1, 0x1908, 0x1401, p);
  std::printf("pixel (%d,%d) = %d,%d,%d\n", x, y, p[0], p[1], p[2]);
  return (p[0] << 16) | (p[1] << 8) | p[2];
}

static gx::PipelineDesc base_desc() {
  gx::PipelineDesc d{};
  d.sampledChannels = 1; d.postexCount = 1;
  d.offProj = 20; d.offPostex = 36; d.offChanMat[0] = 48; d.offChanMatA[0] = 48;
  // Una etapa TEV que deja pasar el color del rasterizador (canal 0).
  d.tevStageCount = 1;
  auto& s = d.stages[0];
  s.ca = s.cb = s.cc = GX_CC_ZERO; s.cd = GX_CC_RASC; s.cop = GX_TEV_ADD; s.cclamp = 1; s.cout = GX_TEVPREV;
  s.aa = s.ab = s.ac = GX_CA_ZERO; s.ad = GX_CA_RASA; s.aop = GX_TEV_ADD; s.aclamp = 1; s.aout = GX_TEVPREV;
  s.channel = GX_COLOR0A0; s.texCoord = 0xFF; s.texMap = 0xFF; s.kcSel = GX_TEV_KCSEL_1; s.kaSel = GX_TEV_KASEL_1;
  for (int i = 0; i < 4; ++i) { d.swap[i][0] = 0; d.swap[i][1] = 1; d.swap[i][2] = 2; d.swap[i][3] = 3; }
  d.alphaComp0 = d.alphaComp1 = GX_ALWAYS;
  for (auto& tg : d.texgens) { tg.type = GX_TG_MTX2x4; tg.src = GX_MAX_TEXGENSRC; tg.mtxSlot = 0xFF; }
  d.depthFunc = GX_ALWAYS; d.cullMode = GX_CULL_NONE; d.blendMode = GX_BM_NONE; d.pixelFmt = GX_PF_RGB8_Z24;
  d.colorUpdate = 1; d.alphaUpdate = 1; d.depthUpdate = 1;
  for (auto& a : d.attrs) a.attrType = GX_NONE;
  return d;
}

// Vuelve a dibujar un cuadro volcado y lo guarda como PPM (P6) de 640x480.
static int replay(const char* in, const char* out) {
  if (!gx::load_frame(in)) { std::printf("no pude leer %s\n", in); return 1; }
  if (!gx::draw_frame(640, 480)) { std::puts("no dibujó"); return 1; }
  static auto ReadPixels = (void(*)(int,int,int,int,unsigned,unsigned,void*))dlsym(gles, "glReadPixels");
  std::vector<unsigned char> px(640 * 480 * 4);
  ReadPixels(0, 0, 640, 480, 0x1908, 0x1401, px.data());
  FILE* f = std::fopen(out, "wb");
  std::fprintf(f, "P6\n640 480\n255\n");
  for (int y = 479; y >= 0; --y)
    for (int x = 0; x < 640; ++x) std::fwrite(&px[(y * 640 + x) * 4], 1, 3, f);
  std::fclose(f);
  std::printf("listo: %s\n", out);
  return 0;
}

int main(int argc, char** argv) {
  if (!init_egl(640, 480)) { std::puts("sin contexto GLES"); return 2; }
  if (argc >= 3) return replay(argv[1], argv[2]);
  auto GetString = (const char*(*)(unsigned))dlsym(gles, "glGetString");
  std::printf("GL: %s / %s\n", GetString(0x1F02), GetString(0x1F01));

  // Dibujo 1: posición f32 directa (big endian) + color RGBA8 del vértice (rojo).
  gx::PipelineDesc p1 = base_desc();
  p1.vtxStride = 16;
  p1.attrs[GX_VA_POS] = {GX_DIRECT, 3, GX_F32, 0, 0, 0, 0, 0};
  p1.attrs[GX_VA_CLR0] = {GX_DIRECT, 4, GX_RGBA8, 12, 0, 0, 0, 0};
  p1.chans[0].matSrc = GX_SRC_VTX; p1.chans[GX_ALPHA0].matSrc = GX_SRC_VTX;
  // Dibujo 2: posición indexada (u8) desde los arreglos, flotantes little endian, color de material (verde).
  gx::PipelineDesc p2 = p1;
  p2.vtxStride = 1;
  p2.attrs[GX_VA_POS] = {GX_INDEX8, 3, GX_F32, 0, 12, 0, 1, 0};
  p2.attrs[GX_VA_CLR0] = {GX_NONE, 0, 0, 0, 0, 0, 0, 0};
  p2.chans[0].matSrc = GX_SRC_REG; p2.chans[GX_ALPHA0].matSrc = GX_SRC_REG;
  gx::register_pipeline(1, p1);
  gx::register_pipeline(2, p2);
  std::vector<uint8_t> verts;
  float tri1[3][3] = {{-0.9f,-0.5f,0.5f},{-0.1f,-0.5f,0.5f},{-0.5f,0.5f,0.5f}};
  for (auto& v : tri1) { put_be_f32(verts, v[0]); put_be_f32(verts, v[1]); put_be_f32(verts, v[2]);
    verts.push_back(255); verts.push_back(0); verts.push_back(0); verts.push_back(255); }
  const uint32_t vtx2 = static_cast<uint32_t>(verts.size());
  verts.push_back(2); verts.push_back(0); verts.push_back(1);  // índices u8 al arreglo
  verts.resize(20000, 0);  // más de una fila de la textura (16 KiB): prueba la subida por filas completas
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
  gx::capture_begin(verts.data(), verts.size(), uni.data(), uni.size(), (const uint8_t*)idx.data(), idx.size()*2,
                    storage.data(), storage.size());
  const float blue[4] = {0, 0, 1, 1};
  gx::capture_pass(640, 480, true, true, blue, 0.f, true, gx::Rect{0, 0, 640, 480});
  gx::capture_viewport(gx::Viewport{0, 0, 640, 480, 0, 1});
  gx::capture_scissor(gx::Rect{0, 0, 640, 480});
  gx::capture_draw(1, 0, 3, 1, 0, UINT32_MAX, 0);
  gx::capture_draw(2, 6, 3, 1, 256, UINT32_MAX, 0);
  // Un segundo pase (el EFB del cuadro siguiente) borra en negro: no debe verse, la pantalla es la copia.
  const float black[4] = {0, 0, 0, 1};
  gx::capture_pass(640, 480, true, true, black, 0.f, false, gx::Rect{0, 0, 0, 0});
  gx::capture_end();
  if (!gx::draw_frame(640, 480)) { std::puts("no dibujó"); return 1; }
  int a = pixel(160, 240), b = pixel(480, 240), z = pixel(320, 470);
  bool ok = a == 0xFF0000 && b == 0x00FF00 && z == 0x0000FF;
  if (std::getenv("CHOYA_PRUEBA_VOLCADO")) {
    ok = ok && gx::dump_last_frame(std::getenv("CHOYA_PRUEBA_VOLCADO"));
  }

  // Cuadro 2: posición s16 con frac 8 (big endian), color RGB565 amarillo, pase de 640x528 con viewport escalado.
  gx::PipelineDesc p3 = p1;
  p3.vtxStride = 8;
  p3.attrs[GX_VA_POS] = {GX_DIRECT, 3, GX_S16, 0, 0, 8, 0, 0};
  p3.attrs[GX_VA_CLR0] = {GX_DIRECT, 4, GX_RGB565, 6, 0, 0, 0, 0};
  gx::register_pipeline(3, p3);
  std::vector<uint8_t> v3;
  auto s16 = [&](float f) { int16_t q = (int16_t)(f * 256); v3.push_back((uint16_t)q >> 8); v3.push_back((uint16_t)q & 0xFF); };
  float t3[3][2] = {{-1.f,-1.f},{1.f,-1.f},{0.f,1.f}};
  for (auto& v : t3) { s16(v[0]); s16(v[1]); s16(0.5f); v3.push_back(0xFF); v3.push_back(0xE0); }
  std::vector<uint8_t> u3(256, 0);
  for (int r = 0; r < 4; ++r) put_f(u3, 20 + r * 4 + r, 1.f);
  for (int r = 0; r < 3; ++r) put_f(u3, 36 + r * 4 + r, 1.f);
  std::vector<uint16_t> i3 = {0, 1, 2};
  gx::capture_begin(v3.data(), v3.size(), u3.data(), u3.size(), (const uint8_t*)i3.data(), 6, nullptr, 0);
  gx::capture_pass(640, 528, true, true, black, 0.f, false, gx::Rect{0, 0, 0, 0});
  // viewport en la mitad superior del EFB (528 de alto): en pantalla debe quedar arriba.
  gx::capture_viewport(gx::Viewport{0, 0, 640, 264, 0, 1});
  gx::capture_scissor(gx::Rect{0, 0, 640, 528});
  gx::capture_draw(3, 0, 3, 1, 0, UINT32_MAX, 0);
  gx::capture_end();
  gx::draw_frame(640, 480);
  int top = pixel(320, 400), bottom = pixel(320, 100);
  ok = ok && top == 0xFFFF00 && bottom == 0x000000;

  // Cuadro 3: textura 2x2 (rojo, verde / azul, blanco) en un cuadro a pantalla completa, TEV = TEXC.
  gx::PipelineDesc p4 = base_desc();
  p4.vtxStride = 20;
  p4.attrs[GX_VA_POS] = {GX_DIRECT, 3, GX_F32, 0, 0, 0, 0, 0};
  p4.attrs[GX_VA_TEX0] = {GX_DIRECT, 2, GX_F32, 12, 0, 0, 0, 0};
  p4.sampledChannels = 0; p4.offChanMat[0] = 0; p4.offChanMatA[0] = 0;
  p4.numTexGens = 1; p4.sampledTexCoords = 1;
  p4.texgens[0] = {GX_TG_MTX2x4, GX_TG_TEX0, 0xFF, 0};
  p4.stages[0].cd = GX_CC_TEXC; p4.stages[0].ad = GX_CA_TEXA; p4.stages[0].texCoord = 0; p4.stages[0].texMap = 0;
  p4.offTexScale[0] = 48; p4.offTexSize[0] = 52;
  gx::register_pipeline(4, p4);
  const uint8_t texels[16] = {255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,255,255};
  gx::texture_data(77, 2, 2, 1, texels, sizeof(texels));
  gx::BindDesc bd{};
  bd.tex[0] = 77;
  bd.samp[0] = {0, 0, 0, 0, 0, {0, 0, 0}, 0.f, 0.f};
  gx::register_bind_group(99, bd);
  std::vector<uint8_t> v4;
  float q[4][5] = {{-1,1,.5f,0,0},{1,1,.5f,1,0},{-1,-1,.5f,0,1},{1,-1,.5f,1,1}};
  for (auto& v : q) for (float f : v) put_be_f32(v4, f);
  std::vector<uint8_t> u4(256, 0);
  for (int r = 0; r < 4; ++r) put_f(u4, 20 + r * 4 + r, 1.f);
  for (int r = 0; r < 3; ++r) put_f(u4, 36 + r * 4 + r, 1.f);
  put_f(u4, 48, 1.f); put_f(u4, 49, 1.f);   // escala de coordenadas
  put_f(u4, 52, 1.f); put_f(u4, 53, 1.f);   // tamaño de textura (normalizado)
  std::vector<uint16_t> i4 = {0, 1, 2, 2, 1, 3};
  gx::capture_begin(v4.data(), v4.size(), u4.data(), u4.size(), (const uint8_t*)i4.data(), 12, nullptr, 0);
  gx::capture_pass(640, 480, true, true, black, 0.f, false, gx::Rect{0, 0, 0, 0});
  gx::capture_viewport(gx::Viewport{0, 0, 640, 480, 0, 1});
  gx::capture_scissor(gx::Rect{0, 0, 640, 480});
  gx::capture_draw(4, 0, 6, 1, 0, UINT32_MAX, 99);
  gx::capture_end();
  gx::draw_frame(640, 480);
  // Arriba a la izquierda (v=0) rojo, arriba a la derecha verde, abajo azul y blanco (GL lee desde abajo).
  int tl = pixel(100, 400), tr = pixel(540, 400), bl = pixel(100, 80), br = pixel(540, 80);
  ok = ok && tl == 0xFF0000 && tr == 0x00FF00 && bl == 0x0000FF && br == 0xFFFFFF;
  if (!ok) std::puts("falló antes de las copias del EFB");

  // Cuadro 4: copias del EFB (GXCopyTex). El pase 1 dibuja la textura 2x2 y se copia a la textura 500 (RGBA8,
  // a 64x48) y a la 501 (I8: intensidad); el pase 2 dibuja la 500 a pantalla completa y la 501 en un cuadro chico.
  gx::BindDesc bc{};
  bc.tex[0] = 500;
  bc.samp[0] = bd.samp[0];
  gx::register_bind_group(100, bc);
  gx::BindDesc bi{};
  bi.tex[0] = 501;
  bi.samp[0] = bd.samp[0];
  gx::register_bind_group(101, bi);
  std::vector<uint8_t> v5 = v4;
  float q5[4][5] = {{-1,1,.5f,0,0},{-0.5f,1,.5f,1,0},{-1,0.5f,.5f,0,1},{-0.5f,0.5f,.5f,1,1}};  // arriba a la izquierda
  for (auto& v : q5) for (float f : v) put_be_f32(v5, f);
  std::vector<uint16_t> i5 = {0, 1, 2, 2, 1, 3, 4, 5, 6, 6, 5, 7};
  gx::capture_begin(v5.data(), v5.size(), u4.data(), u4.size(), (const uint8_t*)i5.data(), i5.size() * 2, nullptr, 0);
  gx::capture_pass(640, 480, true, true, black, 0.f, false, gx::Rect{0, 0, 0, 0});
  gx::capture_viewport(gx::Viewport{0, 0, 640, 480, 0, 1});
  gx::capture_scissor(gx::Rect{0, 0, 640, 480});
  gx::capture_draw(4, 0, 6, 1, 0, UINT32_MAX, 99);
  gx::capture_copy(500, gx::Rect{0, 0, 640, 480}, 64, 48, 0x6, false);
  gx::capture_copy(501, gx::Rect{0, 0, 640, 480}, 64, 48, 0x1, false);
  gx::capture_pass(640, 480, true, true, black, 0.f, true, gx::Rect{0, 0, 640, 480});
  gx::capture_viewport(gx::Viewport{0, 0, 640, 480, 0, 1});
  gx::capture_scissor(gx::Rect{0, 0, 640, 480});
  gx::capture_draw(4, 0, 6, 1, 0, UINT32_MAX, 100);
  gx::capture_draw(4, 12, 6, 1, 0, UINT32_MAX, 101);  // desde el índice 6 (en bytes)
  gx::capture_end();
  gx::draw_frame(640, 480);
  // La copia RGBA8 se ve igual que la textura (abajo a la derecha, fuera del cuadro chico).
  const int ctr = pixel(540, 400), cbl = pixel(100, 80), cbr = pixel(540, 80);
  // El cuadro chico (arriba a la izquierda) muestra la intensidad del rojo de la copia I8: 0.257 + 16/255 = 82.
  const int gray = pixel(40, 460);
  const int gr = (gray >> 16) & 0xFF, gg = (gray >> 8) & 0xFF;
  const bool copies = ctr == 0x00FF00 && cbl == 0x0000FF && cbr == 0xFFFFFF && gr >= 80 && gr <= 84 && gg == gr;
  if (!copies) std::printf("copias del EFB: %06X %06X %06X gris %06X\n", ctr, cbl, cbr, gray);
  ok = ok && copies;

  // Cuadro 5: el motor va atrasado. Llegan tres cuadros sin dibujarse: uno con una copia del EFB (textura 2x2 a la
  // textura 600), uno sin copias (se puede saltar) y un lote partido seguido del cuadro final que dibuja la 600. La
  // copia no se debe perder, y el lote partido deja en el EFB el fondo verde que el final no borra.
  gx::BindDesc b6{};
  b6.tex[0] = 600;
  b6.samp[0] = bd.samp[0];
  gx::register_bind_group(102, b6);
  gx::capture_begin(v4.data(), v4.size(), u4.data(), u4.size(), (const uint8_t*)i4.data(), 12, nullptr, 0);
  gx::capture_pass(640, 480, true, true, black, 0.f, false, gx::Rect{0, 0, 0, 0});
  gx::capture_viewport(gx::Viewport{0, 0, 640, 480, 0, 1});
  gx::capture_scissor(gx::Rect{0, 0, 640, 480});
  gx::capture_draw(4, 0, 6, 1, 0, UINT32_MAX, 99);
  gx::capture_copy(600, gx::Rect{0, 0, 640, 480}, 32, 32, 0x6, false);
  gx::capture_end();
  gx::capture_begin(v4.data(), v4.size(), u4.data(), u4.size(), (const uint8_t*)i4.data(), 12, nullptr, 0);
  gx::capture_pass(640, 480, true, true, black, 0.f, true, gx::Rect{0, 0, 640, 480});
  gx::capture_end();
  const float green[4] = {0, 1, 0, 1};
  gx::capture_begin(v5.data(), v5.size(), u4.data(), u4.size(), (const uint8_t*)i5.data(), i5.size() * 2, nullptr, 0);
  gx::capture_pass(640, 480, true, true, green, 0.f, false, gx::Rect{0, 0, 0, 0});
  gx::capture_end(false);
  gx::capture_begin(v5.data(), v5.size(), u4.data(), u4.size(), (const uint8_t*)i5.data(), i5.size() * 2, nullptr, 0);
  gx::capture_pass(640, 480, false, false, black, 0.f, true, gx::Rect{0, 0, 640, 480});
  gx::capture_viewport(gx::Viewport{0, 0, 640, 480, 0, 1});
  gx::capture_scissor(gx::Rect{0, 0, 640, 480});
  gx::capture_draw(4, 12, 6, 1, 0, UINT32_MAX, 102);  // cuadro chico arriba a la izquierda con la copia
  gx::capture_end();
  gx::draw_frame(640, 480);
  const int small = pixel(20, 470), bg = pixel(540, 80);
  const bool queued = small == 0xFF0000 && bg == 0x00FF00;
  if (!queued) std::printf("cola de cuadros: copia %06X fondo %06X\n", small, bg);
  ok = ok && queued;
  std::puts(ok ? "PRUEBA OK" : "PRUEBA FALLÓ");
  return ok ? 0 : 1;
}
