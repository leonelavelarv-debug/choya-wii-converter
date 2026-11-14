// Banco de pruebas del GX de aurora (sin Dawn): cuánto le cuesta al hilo del juego convertir un cuadro parecido a
// los de Wii Sports. Por cuadro: 1350 materiales (textura + lista de material con BP y XF) y 1350 formas (arreglos +
// lista de forma con CP, matrices indexadas y 4 tiras de triángulos con índices de 16 bits), más 600 cuadros de
// texto/menú con GXBegin. Mide ms por cuadro del hilo del juego. Con valgrind --tool=callgrind
// --toggle-collect=banco_cuadro* se ve en qué funciones de aurora se va.
#include <aurora/aurora.h>
#include <dolphin/gx.h>
#include <dolphin/mtx.h>

#include <chrono>
#include <ctime>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

struct Dl {
  std::vector<u8> b;
  void u8_(u8 v) { b.push_back(v); }
  void u16_(u16 v) {
    b.push_back(v >> 8);
    b.push_back(v & 0xFF);
  }
  void u32_(u32 v) {
    u16_(v >> 16);
    u16_(v & 0xFFFF);
  }
  void bp(u8 reg, u32 value) {
    u8_(0x61);
    u32_(static_cast<u32>(reg) << 24 | (value & 0xFFFFFF));
  }
  void cp(u8 reg, u32 value) {
    u8_(0x08);
    u8_(reg);
    u32_(value);
  }
  void xf(u16 addr, std::initializer_list<u32> values) {
    u8_(0x10);
    u32_(static_cast<u32>(values.size() - 1) << 16 | addr);
    for (u32 v : values) u32_(v);
  }
  void indx(u8 op, u16 idx, u16 len, u16 dst) {
    u8_(op);
    u32_(static_cast<u32>(idx) << 16 | static_cast<u32>(len - 1) << 12 | dst);
  }
  void pad32() {
    while (b.size() % 32 != 0) b.push_back(0);
  }
};

u32 fbits(float f) {
  u32 u;
  std::memcpy(&u, &f, 4);
  return u;
}

// Material de dos etapas TEV (textura × color de vértice, luego un color constante), como uno de G3D.
Dl material(int i) {
  Dl d;
  d.bp(0xFE, 0xFFFFFF);
  d.bp(0x00, 1 | 1 << 4 | 1 << 10 | 2 << 14);  // genmode: 1 texgen, 1 canal, 2 etapas, cull back
  d.bp(0xF3, 0x3F0000 | (i & 0xFF));            // alpha compare
  d.bp(0x40, 0x17);                             // zmode
  d.bp(0x41, (i & 1) ? 0x0034A0 : 0x000000);    // blend
  d.bp(0x42, 0);
  d.bp(0x28, 0 | 0 << 3 | 1 << 6 | 0 << 7 | 7 << 12 | 0 << 15 | 0 << 18 | 7 << 19);  // tev order 0/1
  d.bp(0xC0, 0x08FAF0 + (i & 3));  // color env 0
  d.bp(0xC1, 0x08F2E0);            // alpha env 0
  d.bp(0xC2, 0x08A0F0);            // color env 1
  d.bp(0xC3, 0x08F0C0);            // alpha env 1
  for (int k = 0; k < 8; ++k) d.bp(0xF6 + k, 0x00000F | (k << 4));
  for (int k = 0; k < 4; ++k) {
    d.bp(0xE0 + 2 * k, (i * 7 + k) & 0x7FF);
    d.bp(0xE1 + 2 * k, (i * 3 + k) & 0x7FF);
  }
  d.xf(0x1009, {1});
  d.xf(0x100C, {0xFFFFFFFFu - static_cast<u32>(i)});
  d.xf(0x100E, {0x0002 | 1 << 1});
  d.xf(0x1010, {0x0002});
  d.xf(0x103F, {1});
  d.xf(0x1040, {0x00000280});
  d.xf(0x1050, {0x0000003D});
  d.pad32();
  return d;
}

// Forma: VCD/VAT, 3 matrices de posición y de normal indexadas, 4 tiras de 18 vértices.
Dl forma(int i) {
  Dl d;
  d.cp(0x50, 1 | 3 << 9 | 3 << 11);  // PNMTXIDX directo, POS y NRM índice de 16 bits
  d.cp(0x60, 3);                      // TEX0 índice de 16 bits
  d.cp(0x70, 1 | 4 << 1 | 0 << 4 | 1 << 9 | 4 << 10 | 1 << 21 | 4 << 22 | 0 << 25 | 1u << 30);  // VAT A: f32
  d.cp(0x80, 0);
  d.cp(0x90, 0);
  for (int m = 0; m < 3; ++m) {
    d.indx(0x20, static_cast<u16>((i + m) % 64), 12, static_cast<u16>(m * 12));          // posición
    d.indx(0x28, static_cast<u16>((i + m) % 64), 9, static_cast<u16>(0x400 + m * 9));    // normal
  }
  for (int s = 0; s < 4; ++s) {
    d.u8_(0x98);  // tira de triángulos, formato 0
    d.u16_(18);
    for (int v = 0; v < 18; ++v) {
      d.u8_(static_cast<u8>((v % 3) * 3));
      const u16 idx = static_cast<u16>((i * 37 + s * 18 + v) % 1024);
      d.u16_(idx);
      d.u16_(idx);
      d.u16_(idx);
    }
  }
  d.pad32();
  return d;
}

std::vector<Dl> g_mats, g_shapes;
std::vector<u8> g_pos, g_nrm, g_tex, g_mtx, g_nmtx;
std::vector<std::vector<u8>> g_texData;
std::vector<GXTexObj> g_texObjs;

void preparar() {
  for (int i = 0; i < 60; ++i) g_mats.push_back(material(i));
  for (int i = 0; i < 80; ++i) g_shapes.push_back(forma(i));
  auto be = [](std::vector<u8>& out, float f) {
    const u32 u = fbits(f);
    out.push_back(u >> 24);
    out.push_back(u >> 16);
    out.push_back(u >> 8);
    out.push_back(u);
  };
  for (int i = 0; i < 1024; ++i) {
    be(g_pos, std::sin(i * 0.1f) * 50);
    be(g_pos, std::cos(i * 0.13f) * 50);
    be(g_pos, -100 - (i % 50));
    be(g_nrm, 0);
    be(g_nrm, 0);
    be(g_nrm, 1);
    be(g_tex, (i % 17) / 16.f);
    be(g_tex, (i % 13) / 12.f);
  }
  for (int m = 0; m < 64; ++m) {
    const float r[12] = {1, 0, 0, m * 0.5f, 0, 1, 0, 0, 0, 0, 1, 0};
    for (float f : r) be(g_mtx, f);
    for (int k = 0; k < 9; ++k) be(g_nmtx, k % 4 == 0 ? 1.f : 0.f);
  }
  for (int t = 0; t < 40; ++t) {
    std::vector<u8> d(32 * 32 * 4);
    for (size_t k = 0; k < d.size(); ++k) d[k] = static_cast<u8>(k * (t + 3));
    g_texData.push_back(std::move(d));
  }
  g_texObjs.resize(40);
  for (int t = 0; t < 40; ++t) {
    GXInitTexObj(&g_texObjs[t], g_texData[t].data(), 32, 32, GX_TF_RGBA8, GX_REPEAT, GX_REPEAT, GX_FALSE);
  }
}

u8 g_xfb[640 * 480 * 2] __attribute__((aligned(32)));

void estado_inicial() {
  GXSetCopyClear(GXColor{40, 60, 90, 255}, GX_MAX_Z24);
  GXSetViewport(0, 0, 640, 480, 0, 1);
  GXSetScissor(0, 0, 640, 480);
  Mtx44 proj;
  C_MTXPerspective(proj, 60, 4.f / 3.f, 1, 1000);
  GXSetProjection(proj, GX_PERSPECTIVE);
}

bool g_reemplazos = false;  // BANCO_REEMPLAZOS=1: el reemplazo de texturas de aurora encendido (el runtime lo enciende)

// Un cuadro del "juego". noinline para que callgrind --toggle-collect lo encuentre.
__attribute__((noinline)) void banco_cuadro(int frame) {
  estado_inicial();
  GXSetArray(GX_VA_POS, g_pos.data(), static_cast<u32>(g_pos.size()), 12);
  GXSetArray(GX_VA_NRM, g_nrm.data(), static_cast<u32>(g_nrm.size()), 12);
  GXSetArray(GX_VA_TEX0, g_tex.data(), static_cast<u32>(g_tex.size()), 8);
  GXSetArray(GX_POS_MTX_ARRAY, g_mtx.data(), static_cast<u32>(g_mtx.size()), 48);
  GXSetArray(GX_NRM_MTX_ARRAY, g_nmtx.data(), static_cast<u32>(g_nmtx.size()), 36);
  // BANCO_JUEGO_US: trabajo del "juego" entre material y material (microsegundos de CPU), para ver cuánto del GX
  // se esconde detrás del juego con el hilo GX de Choya (CHOYA_GX_HILO=1).
  static const double juegoUs = std::getenv("BANCO_JUEGO_US") ? std::atof(std::getenv("BANCO_JUEGO_US")) : 0.0;
  for (int m = 0; m < 1350; ++m) {
    if (juegoUs > 0) {
      const auto fin = std::chrono::steady_clock::now() + std::chrono::nanoseconds(static_cast<int64_t>(juegoUs * 1000));
      while (std::chrono::steady_clock::now() < fin) {
      }
    }
    GXLoadTexObj(&g_texObjs[(m + frame) % 40], GX_TEXMAP0);
    GXApplyBPReg(0x0F, 0);  // como el runtime con un registro BP suelto (máscara indirecta: sin efecto aquí)
    if (g_reemplazos && m % 50 == 0) {  // paletas con el reemplazo de texturas encendido (como el runtime)
      static u16 paleta[16] __attribute__((aligned(32))) = {};
      paleta[m % 16] = static_cast<u16>(frame + m);
      GXTlutObj tlut;
      GXInitTlutObj(&tlut, paleta, GX_TL_RGB565, 16);
      GXLoadTlut(&tlut, GX_TLUT0);
    }
    const Dl& mat = g_mats[m % 60];
    GXCallDisplayList(mat.b.data(), static_cast<u32>(mat.b.size()));
    const Dl& shp = g_shapes[m % 80];
    GXCallDisplayList(shp.b.data(), static_cast<u32>(shp.b.size()));
  }
  // Texto y menús: cuadros con GXBegin, posición f32 y coordenada de textura.
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
  Mtx44 ortho;
  C_MTXOrtho(ortho, 0, 480, 0, 640, 0, 1);
  GXSetProjection(ortho, GX_ORTHOGRAPHIC);
  for (int q = 0; q < 600; ++q) {
    if (q % 12 == 0) GXLoadTexObj(&g_texObjs[q % 40], GX_TEXMAP0);
    const float x = static_cast<float>(q % 40) * 16, y = static_cast<float>(q / 40) * 30;
    GXBegin(GX_QUADS, GX_VTXFMT1, 4);
    GXPosition3f32(x, y, -0.5f);
    GXTexCoord2f32(0, 0);
    GXPosition3f32(x + 14, y, -0.5f);
    GXTexCoord2f32(1, 0);
    GXPosition3f32(x + 14, y + 14, -0.5f);
    GXTexCoord2f32(1, 1);
    GXPosition3f32(x, y + 14, -0.5f);
    GXTexCoord2f32(0, 1);
    GXEnd();
  }
  GXSetDispCopySrc(0, 0, 640, 480);
  GXSetDispCopyDst(640, 480);
  GXCopyDisp(g_xfb, GX_TRUE);
  GXDrawDone();
}

void log_cb(AuroraLogLevel level, const char* module, const char* message, unsigned int) {
  if (level >= LOG_WARNING) std::printf("[%d %s] %s\n", static_cast<int>(level), module, message);
  if (level == LOG_FATAL) std::abort();
}

}  // namespace

extern "C" void AuroraTakeGxStats(uint64_t* processNanos, uint64_t* lockNanos, uint64_t* draws);

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  const int frames = argc > 1 ? std::atoi(argv[1]) : 60;
  setenv("SDL_VIDEO_DRIVER", "offscreen", 1);
  AuroraConfig config{};
  config.appName = "choya-banco-gx";
  config.desiredBackend = BACKEND_NULL;
  config.windowWidth = 640;
  config.windowHeight = 480;
  config.logCallback = log_cb;
  config.logLevel = LOG_WARNING;
  g_reemplazos = std::getenv("BANCO_REEMPLAZOS") != nullptr;
  config.allowTextureReplacements = g_reemplazos;
  aurora_initialize(argc, argv, &config);
  static u8 fifo[1 << 20] __attribute__((aligned(32)));
  GXInit(fifo, sizeof(fifo));
  preparar();
  double total = 0, best = 1e9, cpu = 0;
  int counted = 0;
  auto cpu_ms = [] {  // CPU del hilo del juego (lo que espera bloqueado no cuenta)
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
  };
  for (int f = 0; f < frames; ++f) {
    aurora_update();
    if (!aurora_begin_frame()) continue;
    const auto t0 = std::chrono::steady_clock::now();
    const double c0 = cpu_ms();
    banco_cuadro(f);
    const double c1 = cpu_ms();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    aurora_end_frame();
    if (f >= 5) {
      total += ms;
      cpu += c1 - c0;
      best = ms < best ? ms : best;
      ++counted;
    }
  }
  uint64_t pn = 0, ln = 0, draws = 0;
  AuroraTakeGxStats(&pn, &ln, &draws);
  aurora_shutdown();
  std::printf("dibujos por cuadro: %.0f\n", frames > 0 ? static_cast<double>(draws) / frames : 0.0);
  if (counted > 0) {
    std::printf("banco gx: %.2f ms por cuadro (mejor %.2f) en %d cuadros\n", total / counted, best, counted);
    std::printf("banco gx: CPU del hilo del juego %.2f ms por cuadro\n", cpu / counted);
  }
  return 0;
}
