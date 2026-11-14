// Prueba de punta a punta sin Dawn: aurora (con el WebGPU nulo de Choya) lee el GX de un "juego" que dibuja un
// triángulo rojo sobre fondo azul, el motor de Choya captura el cuadro y lo dibuja con GLES (Mesa llvmpipe, EGL
// pbuffer). Se leen píxeles: centro rojo, esquina azul.
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <dolphin/gx.h>
#include <dolphin/mtx.h>
#include <dolphin/vi.h>

#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static void* g_egl;
static void* g_gles;
static void* (*g_eglGetProc)(const char*) = nullptr;
extern "C" void* choya_gl_proc(const char* name) {  // lo usa choya_gx.cpp (símbolo débil)
  void* p = dlsym(g_gles, name);
  if (p == nullptr && g_eglGetProc != nullptr) p = g_eglGetProc(name);
  return p;
}
extern "C" void choya_gles_log(const char* line) { std::printf("[choya] %s\n", line); }
extern "C" bool choya_gx_draw_frame(int screenWidth, int screenHeight);
extern "C" bool choya_gx_sin_gpu();

static bool init_egl(int w, int h) {
  g_egl = dlopen("libEGL.so.1", RTLD_NOW);
  g_gles = dlopen("libGLESv2.so.2", RTLD_NOW);
  if (g_egl == nullptr || g_gles == nullptr) return false;
  auto GetDisplay = (void* (*)(void*))dlsym(g_egl, "eglGetDisplay");
  g_eglGetProc = (void* (*)(const char*))dlsym(g_egl, "eglGetProcAddress");
  auto Initialize = (unsigned (*)(void*, int*, int*))dlsym(g_egl, "eglInitialize");
  auto BindAPI = (unsigned (*)(unsigned))dlsym(g_egl, "eglBindAPI");
  auto ChooseConfig = (unsigned (*)(void*, const int*, void**, int, int*))dlsym(g_egl, "eglChooseConfig");
  auto CreatePbuffer = (void* (*)(void*, void*, const int*))dlsym(g_egl, "eglCreatePbufferSurface");
  auto CreateContext = (void* (*)(void*, void*, void*, const int*))dlsym(g_egl, "eglCreateContext");
  auto MakeCurrent = (unsigned (*)(void*, void*, void*, void*))dlsym(g_egl, "eglMakeCurrent");
  void* d = GetDisplay(nullptr);
  int ma, mi;
  if (!Initialize(d, &ma, &mi)) {
    std::puts("EGL: eglInitialize");
    return false;
  }
  BindAPI(0x30A0);
  const int ca[] = {0x3033, 0x0001, 0x3040, 0x0040, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3025, 24, 0x3038};
  void* cfg;
  int n;
  if (!ChooseConfig(d, ca, &cfg, 1, &n) || n == 0) {
    std::puts("EGL: eglChooseConfig");
    return false;
  }
  const int pa[] = {0x3057, w, 0x3056, h, 0x3038};
  void* s = CreatePbuffer(d, cfg, pa);
  const int xa[] = {0x3098, 3, 0x30FB, 2, 0x3038};
  void* c = CreateContext(d, cfg, nullptr, xa);
  std::printf("EGL: superficie %p contexto %p\n", s, c);
  return s != nullptr && c != nullptr && MakeCurrent(d, s, s, c);
}

static unsigned pixel(int x, int y) {
  auto ReadPixels = (void (*)(int, int, int, int, unsigned, unsigned, void*))dlsym(g_gles, "glReadPixels");
  unsigned char p[4];
  ReadPixels(x, y, 1, 1, 0x1908, 0x1401, p);
  std::printf("píxel (%d,%d) = %d,%d,%d\n", x, y, p[0], p[1], p[2]);
  return (p[0] << 16) | (p[1] << 8) | p[2];
}

static void log_cb(AuroraLogLevel level, const char* module, const char* message, unsigned int) {
  std::printf("[%d %s] %s\n", static_cast<int>(level), module, message);
  if (level == LOG_FATAL) std::abort();
}

static u8 g_xfb[640 * 480 * 2] __attribute__((aligned(32)));
static u8 g_copia[100 * 100 * 4] __attribute__((aligned(32)));
// 4x4 RGBA8 de GX: un bloque AR (16 pares) y uno GB (16 pares). Verde: A=255 R=0, G=255 B=0.
static u8 g_verde[64] __attribute__((aligned(32))) = {
    255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0,
    255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0,
    255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0};

static void draw_triangle() {
  GXSetCopyClear(GXColor{0, 0, 255, 255}, GX_MAX_Z24);
  GXSetViewport(0, 0, 640, 480, 0, 1);
  GXSetScissor(0, 0, 640, 480);
  Mtx44 proj;
  C_MTXOrtho(proj, 0, 480, 0, 640, 0, 1);
  GXSetProjection(proj, GX_ORTHOGRAPHIC);
  Mtx id;
  PSMTXIdentity(id);
  GXLoadPosMtxImm(id, GX_PNMTX0);
  GXSetCurrentMtx(GX_PNMTX0);
  GXClearVtxDesc();
  GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
  GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
  GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
  GXSetNumChans(1);
  GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, 0, GX_DF_NONE, GX_AF_NONE);
  GXSetNumTexGens(0);
  GXSetNumTevStages(1);
  GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
  if (std::getenv("PRUEBA_TEV_CONST") != nullptr) {
    GXSetTevColor(GX_TEVREG0, GXColor{255, 0, 0, 255});
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C0);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_A0);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
  } else {
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
  }
  GXSetCullMode(GX_CULL_NONE);
  GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
  GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
  GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
  GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
  GXPosition3f32(100, 80, -0.5f);
  GXColor4u8(255, 0, 0, 255);
  GXPosition3f32(540, 80, -0.5f);
  GXColor4u8(255, 0, 0, 255);
  GXPosition3f32(320, 420, -0.5f);
  GXColor4u8(255, 0, 0, 255);
  GXEnd();
  if (std::getenv("PRUEBA_SOLO_TRIANGULO") == nullptr) {
    // Copia del EFB (como el icono de un Mii): 100x100 del centro del triángulo, y se borra el EFB.
    GXSetTexCopySrc(270, 190, 100, 100);
    GXSetTexCopyDst(100, 100, GX_TF_RGBA8, GX_FALSE);
    GXCopyTex(g_copia, GX_TRUE);
    GXPixModeSync();
    // Dos cuadros con textura: la copia (abajo a la izquierda) y una textura verde del "disco" (abajo a la derecha).
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GXSetNumChans(0);
    GXSetNumTexGens(1);
    GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
    GXSetTevOp(GX_TEVSTAGE0, GX_REPLACE);
    const struct {
      void* image;
      u16 size;
      float x0, y0;
    } quads[] = {{g_copia, 100, 20, 360}, {g_verde, 4, 520, 360}};
    for (const auto& q : quads) {
      GXTexObj obj;
      GXInitTexObj(&obj, q.image, q.size, q.size, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
      GXInitTexObjLOD(&obj, GX_NEAR, GX_NEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
      GXLoadTexObj(&obj, GX_TEXMAP0);
      GXBegin(GX_QUADS, GX_VTXFMT1, 4);
      GXPosition3f32(q.x0, q.y0, -0.5f);
      GXTexCoord2f32(0, 0);
      GXPosition3f32(q.x0 + 100, q.y0, -0.5f);
      GXTexCoord2f32(1, 0);
      GXPosition3f32(q.x0 + 100, q.y0 + 100, -0.5f);
      GXTexCoord2f32(1, 1);
      GXPosition3f32(q.x0, q.y0 + 100, -0.5f);
      GXTexCoord2f32(0, 1);
      GXEnd();
    }
  }
  if (std::getenv("PRUEBA_SOLO_TRIANGULO") == nullptr) {
    // Un cuadro verde más por lista de dibujo (GXBeginDisplayList / GXCallDisplayList), arriba a la derecha: con el
    // hilo GX de Choya la lista se copia y la procesa el otro hilo.
    static u8 lista[512] __attribute__((aligned(32)));
    static u32 tam = 0;
    if (tam == 0) {
      GXBeginDisplayList(lista, sizeof(lista));
      GXBegin(GX_QUADS, GX_VTXFMT1, 4);
      GXPosition3f32(520, 20, -0.5f);
      GXTexCoord2f32(0, 0);
      GXPosition3f32(620, 20, -0.5f);
      GXTexCoord2f32(1, 0);
      GXPosition3f32(620, 120, -0.5f);
      GXTexCoord2f32(1, 1);
      GXPosition3f32(520, 120, -0.5f);
      GXTexCoord2f32(0, 1);
      GXEnd();
      tam = GXEndDisplayList();
    }
    GXSetSourceVtxDesc(GX_VA_POS, GX_DIRECT);  // cambio directo del estado (diferido con el hilo GX)
    GXCallDisplayList(lista, tam);
  }
  GXSetDispCopySrc(0, 0, 640, 480);
  GXSetDispCopyDst(640, 480);
  GXCopyDisp(g_xfb, GX_TRUE);
  GXDrawDone();
}

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  setenv("CHOYA_GLES", "1", 1);
  setenv("SDL_VIDEO_DRIVER", "offscreen", 1);
  // EGL antes que SDL: con el video "offscreen" ya iniciado, eglInitialize de la pantalla por defecto falla.
  if (!init_egl(640, 480)) {
    std::puts("SIN EGL");
    return 2;
  }
  AuroraConfig config{};
  config.appName = "choya-sin-dawn";
  config.desiredBackend = BACKEND_NULL;
  config.windowWidth = 640;
  config.windowHeight = 480;
  config.logCallback = log_cb;
  config.logLevel = LOG_INFO;
  aurora_initialize(argc, argv, &config);
  static u8 fifo[256 * 1024] __attribute__((aligned(32)));
  GXInit(fifo, sizeof(fifo));
  if (!choya_gx_sin_gpu()) {
    std::puts("FALLO: el motor de Choya no está en modo sin GPU");
    return 1;
  }
  int frames = 0;
  for (int i = 0; i < 4; ++i) {
    aurora_update();
    if (!aurora_begin_frame()) continue;
    draw_triangle();
    aurora_end_frame();
    ++frames;
  }
  aurora_begin_frame();  // espera a que el hilo de aurora selle el último cuadro
  if (!choya_gx_draw_frame(640, 480)) {
    std::puts("FALLO: el motor no dibujó");
    return 1;
  }
  // glReadPixels cuenta y desde abajo: y_gl = 479 - y.
  const bool soloTriangulo = std::getenv("PRUEBA_SOLO_TRIANGULO") != nullptr;
  const unsigned centro = pixel(320, 479 - 280);
  const unsigned esquina = pixel(10, 10);
  const unsigned copia = pixel(70, 479 - 410);
  const unsigned verde = pixel(570, 479 - 410);
  const unsigned lista = pixel(570, 479 - 70);
  std::printf("cuadros %d\n", frames);
  aurora_shutdown();
  const bool ok = soloTriangulo ? centro == 0xFF0000 && esquina == 0x0000FF
                                : centro == 0x0000FF && esquina == 0x0000FF && copia == 0xFF0000 && verde == 0x00FF00 &&
                                      lista == 0x00FF00;
  if (!ok) {
    std::puts("FALLO: colores");
    return 1;
  }
  std::puts("sin Dawn: bien");
  return 0;
}
