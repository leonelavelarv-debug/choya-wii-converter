// Hilo GX de Choya (0.23.0). Ver choya_hilo.hpp.
#include "choya_hilo.hpp"

#include "choya_gx_core.hpp"
#include "../gfx/common.hpp"
#include "../gfx/staging_capacity.hpp"
#include "../gx/command_processor.hpp"
#include "../gx/fifo.hpp"
#include "../internal.hpp"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

extern "C" bool choya_gx_sin_gpu();
extern "C" void choya_gx_log(const char* line);

namespace choya::hilo {
namespace {
namespace detail = aurora::gx::fifo::detail;
using Reloj = std::chrono::steady_clock;

enum class Tipo : uint8_t { Fifo, Lista, Bp, Cierre };
struct Tramo {
  Tipo tipo;
  uint8_t a;     // Lista: big endian; Bp: registro
  uint32_t off;  // Fifo: en el FIFO del lote; Lista y Cierre: en datos; Bp: valor
  uint32_t len;
  Cierre fn;
  const uint8_t* ptr;  // Lista en la RAM del juego: se lee de ahí (como la GPU de la Wii)
};
struct Lote {
  uint8_t* fifo = nullptr;  // malloc, como el FIFO de la API (fifo.cpp)
  uint32_t fifoCap = 0;
  std::vector<uint8_t> datos;
  std::vector<Tramo> tramos;
};

constexpr uint32_t kFifoInicial = 64 * 1024;
constexpr size_t kUmbral = 48 * 1024;        // se entrega al juntar esto
constexpr size_t kUmbralOcioso = 4 * 1024;   // o esto, si el hilo GX no tiene nada
constexpr size_t kMaxCola = 48;              // lotes en espera: más y el juego espera (memoria acotada)

void log_line(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void log_line(const char* fmt, ...) {
  char line[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  choya_gx_log(line);
}

int gettid_actual() noexcept {
#if defined(__linux__)
  return static_cast<int>(syscall(SYS_gettid));
#else
  return 0;
#endif
}

// Compartido entre el hilo del juego y el hilo GX.
std::mutex g_m;
std::condition_variable g_cvTrabajo;  // el hilo GX espera lotes
std::condition_variable g_cvLibre;    // el juego espera a que el hilo GX termine (o que la cola baje)
std::deque<Lote*> g_cola;
std::vector<Lote*> g_libres;
bool g_ocupado = false;
bool g_parar = false;
bool g_arrancado = false;
std::thread g_hilo;
std::atomic<bool> g_ocioso{true};
std::atomic<uint64_t> g_ocupadoNs{0};
std::atomic<int> g_tidJuego{0};
std::atomic<int> g_tidGx{0};

// Solo el hilo del juego.
Lote* g_obra = nullptr;  // lote que se arma (tramos y datos); su FIFO es el de la API (detail::sBufferData)
uint32_t g_segIni = 0;   // inicio del tramo abierto del FIFO de la API
thread_local int t_exclusivo = 0;
thread_local bool t_enHilo = false;
// El hilo que escribe el FIFO de la API (el del juego). Si otro hilo llama a una función que toma el procesador, no
// toma nada: antes tampoco había candado entre hilos.
std::atomic<bool> g_hayProductor{false};
std::thread::id g_productor;

bool es_productor() noexcept {
  if (!g_hayProductor.load(std::memory_order_acquire)) {
    g_productor = std::this_thread::get_id();
    g_hayProductor.store(true, std::memory_order_release);
    return true;
  }
  return g_productor == std::this_thread::get_id();
}

struct Motivo {
  const char* quien;
  uint32_t entradas;
  uint32_t esperas;
  uint64_t ns;
};
// RAM del juego (MEM1 y MEM2 en el proceso), la dice el runtime (gx_guest_write_hooks.cpp).
std::atomic<uintptr_t> g_ramIni[2]{};
std::atomic<uintptr_t> g_ramFin[2]{};
uint64_t g_listaPtr = 0, g_listaCopia = 0;

bool en_ram_juego(const uint8_t* p, uint32_t n) noexcept {
  static const bool copiar = [] {
    const char* v = std::getenv("CHOYA_GX_HILO_COPIAR");  // 1: copiar también las listas de la RAM del juego
    return v != nullptr && std::strcmp(v, "1") == 0;
  }();
  if (copiar) return false;
  const auto a = reinterpret_cast<uintptr_t>(p);
  for (int i = 0; i < 2; ++i) {
    const uintptr_t ini = g_ramIni[i].load(std::memory_order_relaxed);
    const uintptr_t fin = g_ramFin[i].load(std::memory_order_relaxed);
    if (ini != 0 && a >= ini && a <= fin && n <= fin - a) return true;
  }
  return false;
}

Motivo g_motivos[64];
size_t g_numMotivos = 0;
uint64_t g_lotes = 0, g_bytes = 0, g_cuadros = 0;

// ---------------------------------------------------------------- hilo GX

void esperar_sellado() {
  const auto w = aurora::wait_for_frame_worker_sealed();  // sin el aviso al runtime (aurora.cpp)
  if (w.count() > 0) ::choya::gx::note_aurora(0, static_cast<uint64_t>(w.count()));
}

void drenar(const uint8_t* data, uint32_t len) {
  uint32_t consumed = 0;
  bool retried = false;
  while (consumed < len) {
    const auto count = aurora::gx::fifo::process(data + consumed, len - consumed, true);
    if (count == 0 && retried) throw aurora::gfx::StagingCapacityError("FIFO draw does not fit after capacity submission");
    consumed += count;
    if (consumed == len) break;
    aurora::gfx::split_staging_batch();
    retried = true;
  }
}

void procesar(Lote& l) {
  static int errores = 0;
  for (const Tramo& t : l.tramos) {
    try {
      switch (t.tipo) {
      case Tipo::Fifo:
        esperar_sellado();
        drenar(l.fifo + t.off, t.len);
        break;
      case Tipo::Lista:
        esperar_sellado();
        choya_gx_procesar_lista(t.ptr != nullptr ? t.ptr : l.datos.data() + t.off, t.len, t.a != 0);
        break;
      case Tipo::Bp:
        choya_gx_aplicar_bp(t.a, t.off);
        break;
      case Tipo::Cierre:
        t.fn(l.datos.data() + t.off);
        break;
      }
    } catch (const std::exception& e) {
      if (errores++ < 8) log_line("aurora: hilo GX: error al procesar (%s): %s", t.tipo == Tipo::Lista ? "lista" : "FIFO", e.what());
    }
  }
}

void bucle() {
  t_enHilo = true;
  g_tidGx.store(gettid_actual());
#if defined(__linux__)
  pthread_setname_np(pthread_self(), "choya-gx");
#endif
  std::unique_lock lk(g_m);
  for (;;) {
    g_cvTrabajo.wait(lk, [] { return g_parar || !g_cola.empty(); });
    if (g_cola.empty()) break;  // g_parar
    Lote* l = g_cola.front();
    g_cola.pop_front();
    g_ocupado = true;
    g_ocioso.store(false, std::memory_order_relaxed);
    lk.unlock();
    const auto t0 = Reloj::now();
    procesar(*l);
    g_ocupadoNs.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(Reloj::now() - t0).count(),
                          std::memory_order_relaxed);
    l->tramos.clear();
    l->datos.clear();
    lk.lock();
    g_libres.push_back(l);
    if (g_cola.empty()) {
      g_ocupado = false;
      g_ocioso.store(true, std::memory_order_relaxed);
    }
    g_cvLibre.notify_all();
  }
  g_ocupado = false;
  g_cvLibre.notify_all();
}

// ---------------------------------------------------------------- hilo del juego

Lote& obra() {
  if (g_obra == nullptr) g_obra = new Lote();
  return *g_obra;
}

void cerrar_fifo() {
  const uint32_t size = detail::sBufferSize;
  if (g_segIni > size) g_segIni = size;  // el FIFO se vació por su cuenta (solo con el procesador tomado)
  if (size > g_segIni) {
    obra().tramos.push_back(Tramo{Tipo::Fifo, 0, g_segIni, size - g_segIni, nullptr, nullptr});
    g_segIni = size;
  }
}

// Entrega lo cerrado al hilo GX. El tramo abierto del FIFO (desde g_segIni) se queda en el FIFO nuevo de la API.
void entregar() {
  Lote& o = obra();
  if (o.tramos.empty()) return;
  if (g_tidJuego.load(std::memory_order_relaxed) == 0) g_tidJuego.store(gettid_actual());
  Lote* nuevo = nullptr;
  {
    std::lock_guard lk(g_m);
    if (!g_libres.empty()) {
      nuevo = g_libres.back();
      g_libres.pop_back();
    }
  }
  if (nuevo == nullptr) nuevo = new Lote();
  const uint32_t size = std::max(detail::sBufferSize, g_segIni);
  const uint32_t cola = size - g_segIni;
  uint8_t* buf = nuevo->fifo;
  uint32_t cap = nuevo->fifoCap;
  nuevo->fifo = nullptr;
  nuevo->fifoCap = 0;
  if (buf == nullptr || cap < cola || cap < kFifoInicial) {
    std::free(buf);
    cap = std::max<uint32_t>(cola, kFifoInicial);
    buf = static_cast<uint8_t*>(std::malloc(cap));
  }
  if (cola != 0) std::memcpy(buf, detail::sBufferData + g_segIni, cola);
  o.fifo = detail::sBufferData;
  o.fifoCap = detail::sBufferCapacity;
  detail::sBufferData = buf;
  detail::sBufferCapacity = cap;
  detail::sBufferSize = cola;
  g_segIni = 0;
  g_obra = nuevo;
  ++g_lotes;
  g_bytes += size + o.datos.size();
  std::unique_lock lk(g_m);
  if (g_cola.size() >= kMaxCola) {
    g_cvLibre.wait(lk, [] { return g_cola.size() < kMaxCola / 2; });
  }
  g_cola.push_back(&o);
  if (!g_arrancado) {
    g_arrancado = true;
    g_hilo = std::thread(bucle);
  }
  g_cvTrabajo.notify_one();
}

size_t pendiente() { return detail::sBufferSize + obra().datos.size(); }

void tal_vez_entregar() {
  const size_t p = pendiente();
  if (p >= kUmbral || (p >= kUmbralOcioso && g_ocioso.load(std::memory_order_relaxed))) entregar();
}

Motivo& motivo(const char* quien) {
  for (size_t i = 0; i < g_numMotivos; ++i) {
    if (g_motivos[i].quien == quien) return g_motivos[i];
  }
  if (g_numMotivos < std::size(g_motivos)) {
    g_motivos[g_numMotivos] = Motivo{quien, 0, 0, 0};
    return g_motivos[g_numMotivos++];
  }
  static Motivo otros{"otros", 0, 0, 0};
  return otros;
}

} // namespace

bool activo() noexcept {
  static const bool on = [] {
    const char* v = std::getenv("CHOYA_GX_HILO");
    if (v == nullptr || std::strcmp(v, "1") != 0) return false;
    if (!choya_gx_sin_gpu()) {
      log_line("aurora: hilo GX apagado: necesita el motor de Choya sin Dawn");
      return false;
    }
    log_line("aurora: hilo GX de Choya: el GX se decodifica en otro núcleo (CHOYA_GX_HILO=1; el archivo sin-hilo-gx "
             "lo apaga)");
    return true;
  }();
  return on;
}

bool en_hilo() noexcept { return t_enHilo; }
bool exclusivo() noexcept { return t_exclusivo > 0; }

void fifo_listo() noexcept {
  es_productor();
  cerrar_fifo();
  tal_vez_entregar();
}

void lista(const void* data, uint32_t nbytes, bool bigEndian) noexcept {
  cerrar_fifo();
  const auto* p = static_cast<const uint8_t*>(data);
  if (en_ram_juego(p, nbytes)) {
    // Lista en la RAM del juego: como en la Wii, el GX la lee después (el juego no la cambia sin esperar a la GPU).
    obra().tramos.push_back(Tramo{Tipo::Lista, static_cast<uint8_t>(bigEndian ? 1 : 0), 0, nbytes, nullptr, p});
    g_listaPtr += nbytes;
  } else {
    // Prestada (copia del runtime, un paquete suelto): se copia.
    auto& d = obra().datos;
    const auto off = static_cast<uint32_t>(d.size());
    d.insert(d.end(), p, p + nbytes);
    obra().tramos.push_back(Tramo{Tipo::Lista, static_cast<uint8_t>(bigEndian ? 1 : 0), off, nbytes, nullptr, nullptr});
    g_listaCopia += nbytes;
  }
  tal_vez_entregar();
}

void bp(uint8_t reg, uint32_t value) noexcept {
  obra().tramos.push_back(Tramo{Tipo::Bp, reg, value, 0, nullptr, nullptr});
}

void cierre(Cierre fn, const void* datos, size_t size) noexcept {
  auto& d = obra().datos;
  d.resize((d.size() + 15) & ~size_t{15});
  const auto off = static_cast<uint32_t>(d.size());
  const auto* p = static_cast<const uint8_t*>(datos);
  d.insert(d.end(), p, p + size);
  obra().tramos.push_back(Tramo{Tipo::Cierre, 0, off, static_cast<uint32_t>(size), fn, nullptr});
}

void cierre_tras_fifo(Cierre fn, const void* datos, size_t size) noexcept {
  cerrar_fifo();
  cierre(fn, datos, size);
  tal_vez_entregar();
}

Exclusivo::Exclusivo(const char* quien) noexcept {
  if (!activo() || t_enHilo || !es_productor()) return;
  tomado_ = true;
  if (t_exclusivo++ > 0) return;
  entregar();
  Motivo& m = motivo(quien);
  ++m.entradas;
  std::unique_lock lk(g_m);
  if (g_cola.empty() && !g_ocupado) return;
  const auto t0 = Reloj::now();
  g_cvLibre.wait(lk, [] { return g_cola.empty() && !g_ocupado; });
  ++m.esperas;
  m.ns += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Reloj::now() - t0).count());
}

Exclusivo::~Exclusivo() noexcept {
  if (tomado_) --t_exclusivo;
}

void cuadro() noexcept {
  if (!activo() || t_enHilo) return;
  static const uint64_t kCada = [] {
    const char* v = std::getenv("CHOYA_GX_HILO_INFORME");  // cada cuántos cuadros (pruebas)
    const long n = v != nullptr ? std::atol(v) : 0;
    return n > 0 ? static_cast<uint64_t>(n) : uint64_t{300};
  }();
  if (++g_cuadros < kCada) return;
  const double n = static_cast<double>(g_cuadros);
  uint32_t esperas = 0;
  uint64_t ns = 0;
  for (size_t i = 0; i < g_numMotivos; ++i) {
    esperas += g_motivos[i].esperas;
    ns += g_motivos[i].ns;
  }
  // Los motivos con más tiempo de espera primero.
  std::sort(g_motivos, g_motivos + g_numMotivos, [](const Motivo& a, const Motivo& b) { return a.ns > b.ns; });
  char detalle[256] = "";
  size_t pos = 0;
  for (size_t i = 0; i < g_numMotivos && i < 6 && pos < sizeof(detalle); ++i) {
    const Motivo& m = g_motivos[i];
    if (m.entradas == 0) continue;
    pos += static_cast<size_t>(std::snprintf(detalle + pos, sizeof(detalle) - pos, "%s%s %.1f/%.1f ms", pos ? ", " : "",
                                             m.quien, m.entradas / n, m.ns / n / 1e6));
  }
  log_line("aurora: hilo GX: %llu cuadros; por cuadro: %.1f lotes (%.0f KB; listas %.0f KB leídas de la RAM del juego, "
           "%.0f KB copiadas), hilo GX ocupado %.2f ms, el juego esperó %.2f ms en %.1f esperas; tomas por cuadro/ms: %s",
           static_cast<unsigned long long>(g_cuadros), g_lotes / n, g_bytes / n / 1024.0, g_listaPtr / n / 1024.0,
           g_listaCopia / n / 1024.0,
           g_ocupadoNs.exchange(0, std::memory_order_relaxed) / n / 1e6, ns / n / 1e6, esperas / n, detalle);
  g_cuadros = g_lotes = g_bytes = g_listaPtr = g_listaCopia = 0;
  for (size_t i = 0; i < g_numMotivos; ++i) g_motivos[i].entradas = g_motivos[i].esperas = 0, g_motivos[i].ns = 0;
}

void detener() noexcept {
  {
    std::lock_guard lk(g_m);
    if (!g_arrancado) return;
    g_parar = true;
  }
  g_cvTrabajo.notify_all();
  if (g_hilo.joinable()) g_hilo.join();
  std::lock_guard lk(g_m);
  g_arrancado = false;
  g_parar = false;
}

int tid_juego() noexcept { return g_tidJuego.load(); }
int tid_gx() noexcept { return g_tidGx.load(); }

} // namespace choya::hilo

extern "C" bool choya_gx_hilo_en_hilo() { return choya::hilo::en_hilo(); }
extern "C" bool choya_gx_hilo_activo() { return choya::hilo::activo(); }
extern "C" int choya_gx_hilo_tid_gx() { return choya::hilo::tid_gx(); }
extern "C" void choya_gx_hilo_memoria_juego(int banco, const void* base, uint64_t size) {
  if (banco < 0 || banco > 1 || base == nullptr || size == 0) return;
  const auto ini = reinterpret_cast<uintptr_t>(base);
  choya::hilo::g_ramFin[banco].store(ini + size - 1, std::memory_order_relaxed);
  choya::hilo::g_ramIni[banco].store(ini, std::memory_order_relaxed);
}
extern "C" void choya_gx_hilo_esperar() { CHOYA_HILO_EXCLUSIVO(); }
