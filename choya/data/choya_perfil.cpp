// Choya Wii Converter: medición por tramos del cuadro (y el perfil por muestreo de siempre).
//
// Qué mide, durante una ventana de unos segundos en una escena lenta:
//  - Llamadas: cuántas veces se llama cada función del juego (abi_bridge.h, parche de Choya, avisa en cada llamada
//    entre funciones traducidas y a las nativas como GXSetTevOp mientras se mide).
//  - Pared: 1000 veces por segundo, en el hilo del juego aunque esté dormido o esperando, el contador de programa, la
//    pila de llamadas del anfitrión (registros de marco) y la del juego (cadena de marcos de la pila de la Wii). En la
//    PC, `choya perfil perfil.txt` reparte el cuadro en tres tramos: actualizar el juego (dentro de
//    EGG::SceneManager::calc), preparar el dibujo (dentro de EGG::SceneManager::draw, con la parte de aurora aparte)
//    y esperar (dormido en un futex, un sleep, un ioctl de la Mali...), con lo que acumula y cuántas veces se llama
//    cada función en cada tramo, y qué llamada espera.
//  - CPU: SIGPROF de todos los hilos, como antes (qué hace el hilo del motor, el audio...).
//
// Cómo se empieza: CHOYA_PERFIL="espera,segundos" (modo diagnóstico, por tiempo) o MENU sola sostenida 2 s
// (choya_gles.cpp, en cualquier modo: 20 s desde ese momento). Escribe perfil.txt (o CHOYA_PERFIL_SALIDA).
// Sin eso no hace nada: el aviso de cada llamada es una lectura y un salto que casi nunca se toma.
#include <cstdint>

// Lo que lee el parche de abi_bridge.h. Fuera del #if: el enlace no debe depender de la plataforma.
extern "C" {
volatile bool g_choyaMidiendo = false;
// CHOYA_SIN_REVERB=1: la dirección de AxManager::AuxCallbackFunc (efectos de audio de nw4r: la reverberación). El
// parche de InvokeIndirectCpu (abi_bridge.h) no la llama: deja en silencio la vuelta de los efectos y se ahorra el
// trabajo de AXFXReverbHiExpCallback, que en los menús era ~9 % del hilo del juego. 0 = normal.
uint32_t g_choyaSinEfectos = 0;
}
extern "C" bool choya_sin_efectos(void* ctx);
// Memoria del juego (montones de MEM/EGG): abi_bridge.h avisa al entrar a MEMAllocFromExpHeapEx,
// MEMAllocFromFrmHeapEx, MEMFreeToExpHeap, MEMResizeForMBlockExpHeap y EGG::Heap::alloc (la comparación es con una
// constante: en las llamadas directas no cuesta nada fuera de esas funciones), y al cerrarse por un salto a 0.
extern "C" void choya_vigila_memoria(uint32_t target, void* ctx);
extern "C" void choya_informe_memoria(uint32_t target, const void* ctx);
extern "C" void choya_medir_llamada(uint32_t target, const void* ctx);
extern "C" int choya_medir_empezar(int seconds);  // 1: empezó; 0: ya estaba midiendo

#if defined(__linux__)
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <fcntl.h>
#include <map>
#include <pthread.h>
#include <string>
#include <sys/syscall.h>
#include <sys/time.h>
#include <thread>
#include <ucontext.h>
#include <unistd.h>
#include <utility>
#include <vector>

#if __has_include("guest_flat_memory.h")
#include "guest_flat_memory.h"
#define CHOYA_GUEST_BASE (static_cast<uintptr_t>(GuestFlat::kFixedFlatGuestBase))
#define CHOYA_GUEST_ACTIVE() (GuestFlat::IsActive())
#define CHOYA_CON_SIMBOLOS 1  // la tabla de nombres del juego del runtime (guest_symbol_table.cpp o su stub)
#elif defined(CHOYA_PRUEBA_MEMORIA)  // prueba de la memoria del juego en la PC: una memoria "de la Wii" falsa
extern "C" uint8_t* choya_prueba_base;
#define CHOYA_GUEST_BASE (reinterpret_cast<uintptr_t>(choya_prueba_base))
#define CHOYA_GUEST_ACTIVE() (choya_prueba_base != nullptr)
#else  // fuera del runtime (pruebas): sin pila del juego
#define CHOYA_GUEST_BASE (uintptr_t{0})
#define CHOYA_GUEST_ACTIVE() false
#endif

extern "C" char __ehdr_start;  // inicio del ejecutable (PIE) en memoria

namespace choya_medida {  // con nombre: en una compilación unity no choca con otros archivos

// Direcciones de Wii Sports que marcan los tramos; las escribe Choya al copiar este archivo (retarget.py).
constexpr uint32_t kCalc = 0x0u /*CHOYA_DIR_CALC*/;
constexpr uint32_t kDraw = 0x0u /*CHOYA_DIR_DRAW*/;
constexpr uint32_t kCalcSize = 0x0u /*CHOYA_TAM_CALC*/;
constexpr uint32_t kAuxCallback = 0x0u /*CHOYA_DIR_AUX*/;  // nw4r::snd::detail::AxManager::AuxCallbackFunc
constexpr uint32_t kDrawSize = 0x0u /*CHOYA_TAM_DRAW*/;
constexpr uint32_t kCodeLo = 0x80000000u, kCodeHi = 0x80800000u;  // texto del juego (main.dol y RELs bajos)

// --- Muestreo de CPU de todos los hilos (SIGPROF, como en las versiones anteriores) ---------------------------
constexpr size_t kMaxSamples = 1u << 20;
uintptr_t* g_pcs = nullptr;
uint32_t* g_tids = nullptr;
std::atomic<size_t> g_count{0};
std::atomic<bool> g_sampling{false};

void OnProf(int, siginfo_t*, void* raw) {
    if (!g_sampling.load(std::memory_order_relaxed) || g_pcs == nullptr) return;
    const size_t i = g_count.fetch_add(1, std::memory_order_relaxed);
    if (i >= kMaxSamples) return;
    const auto* uc = static_cast<const ucontext_t*>(raw);
#if defined(__aarch64__)
    g_pcs[i] = static_cast<uintptr_t>(uc->uc_mcontext.pc);
#elif defined(__x86_64__)
    g_pcs[i] = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]);
#else
    g_pcs[i] = 0;
#endif
    g_tids[i] = static_cast<uint32_t>(syscall(SYS_gettid));
}

// --- Llamadas ---------------------------------------------------------------------------------------------------
uint32_t* g_calls = nullptr;          // por dirección del juego (kCodeLo..kCodeHi, de 4 en 4)
std::atomic<uint64_t> g_otherCalls{0};
std::atomic<uint32_t> g_frames{0};    // llamadas a kCalc mientras se mide = cuadros
std::atomic<int> g_gameTid{0};
const void* volatile g_gameCtx = nullptr;  // CpuContext del hilo del juego (gpr[0..31] al inicio)
thread_local bool t_isGame = false;
uintptr_t g_stackLo = 0, g_stackHi = 0;  // pila del hilo del juego (para no leer fuera al recorrerla)
bool g_guestOk = false;                   // memoria plana del juego activa: se puede leer su pila
uintptr_t g_textLo = 0, g_textHi = 0;     // código del ejecutable (de /proc/self/maps)

void find_text() {
    char exe[512] = {};
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return;
    FILE* maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr) return;
    char line[1024];
    while (std::fgets(line, sizeof(line), maps)) {
        unsigned long lo = 0, hi = 0;
        char perms[8] = {};
        if (std::sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) != 3 || perms[2] != 'x') continue;
        if (std::strstr(line, exe) == nullptr) continue;
        if (g_textLo == 0 || lo < g_textLo) g_textLo = lo;
        if (hi > g_textHi) g_textHi = hi;
    }
    std::fclose(maps);
}

// ¿Una dirección de retorno al ejecutable? (en AArch64, la instrucción anterior es un BL o un BLR)
bool is_return_into_exe(uintptr_t v) {
    if (v < g_textLo + 4 || v >= g_textHi) return false;
#if defined(__aarch64__)
    if ((v & 3) != 0) return false;
    uint32_t insn;
    std::memcpy(&insn, reinterpret_cast<const void*>(v - 4), 4);
    return (insn & 0xFC000000u) == 0x94000000u || (insn & 0xFFFFFC1Fu) == 0xD63F0000u;
#else
    return true;
#endif
}

// --- Muestras de pared del hilo del juego -------------------------------------------------------------------
constexpr int kHostDepth = 14, kGuestDepth = 20;
struct Wall {
    uint64_t pc, lr;
    uint64_t host[kHostDepth];
    uint32_t guest[kGuestDepth];
    uint8_t nHost, nGuest;
    int16_t wait;  // -2: corriendo (muestra por señal); si no, llamada al sistema en la que duerme (-1: fallo de página)
};
constexpr size_t kMaxWall = 64 * 1024;
Wall* g_wall = nullptr;
std::atomic<size_t> g_wallCount{0};
std::atomic<bool> g_wallOn{false};
int g_wallSignal = 0;

inline bool guest_ram(uint32_t a) {
    return (a >= 0x80000000u && a < 0x81800000u) || (a >= 0x90000000u && a < 0x94000000u);
}
inline uint32_t guest_read32(uint32_t a) {
    uint32_t v;
    std::memcpy(&v, reinterpret_cast<const void*>(CHOYA_GUEST_BASE + a), 4);
    return __builtin_bswap32(v);
}

// Pila del juego: cadena de marcos de la Wii desde r1 ([sp] = marco anterior, [anterior + 4] = LR guardado).
void guest_stack(Wall& w) {
    w.nGuest = 0;
    const auto* ctx = static_cast<const uint32_t*>(g_gameCtx);
    if (ctx == nullptr || !g_guestOk) return;
    uint32_t gsp = ctx[1];
    // Tope de vueltas: si r1 apunta a algo que no es la pila, la cadena puede subir de 8 en 8 bytes muchísimo rato
    // (en 0.19.0 la señal llegó a costar ~15 % del hilo del juego).
    for (int steps = 0; steps < 48 && w.nGuest < kGuestDepth && guest_ram(gsp) && (gsp & 7) == 0; ++steps) {
        const uint32_t prev = guest_read32(gsp);
        if (!guest_ram(prev) || prev <= gsp || prev - gsp > 0x20000u) break;
        const uint32_t lr = guest_read32(prev + 4);
        if (lr >= kCodeLo && lr < kCodeHi) w.guest[w.nGuest++] = lr;
        gsp = prev;
    }
}

volatile int g_wallTarget = -1;  // índice de la muestra que llena la señal en curso
std::atomic<uint64_t> g_wallNs{0};  // tiempo total dentro de OnWall (lo que la medición le quita al hilo del juego)
std::atomic<uint64_t> g_wallSignals{0};

inline uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

void OnWall(int, siginfo_t*, void* raw) {
    const int i = g_wallTarget;
    if (!g_wallOn.load(std::memory_order_relaxed) || g_wall == nullptr || i < 0) return;
    g_wallTarget = -1;
    const uint64_t t0 = now_ns();
    Wall& w = g_wall[i];
    const auto* uc = static_cast<const ucontext_t*>(raw);
    uintptr_t fp = 0, sp = 0;
#if defined(__aarch64__)
    w.pc = uc->uc_mcontext.pc;
    w.lr = uc->uc_mcontext.regs[30];
    fp = uc->uc_mcontext.regs[29];
    sp = uc->uc_mcontext.sp;
#elif defined(__x86_64__)
    w.pc = static_cast<uint64_t>(uc->uc_mcontext.gregs[REG_RIP]);
    w.lr = 0;
    fp = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RBP]);
    sp = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RSP]);
#else
    w.pc = w.lr = 0;
#endif
    // Pila del anfitrión por los registros de marco (Clang los deja en AArch64): [fp] = marco anterior,
    // [fp + 8] = retorno. Solo marcos crecientes dentro de 8 MB de la pila del hilo.
    w.nHost = 0;
    const uintptr_t lo = std::max(sp, g_stackLo), hi = g_stackHi;
    while (w.nHost < kHostDepth && fp >= lo && fp + 16 <= hi && (fp & 7) == 0) {
        const uintptr_t* frame = reinterpret_cast<const uintptr_t*>(fp);
        const uintptr_t next = frame[0], ret = frame[1];
        if (ret == 0) break;
        w.host[w.nHost++] = ret;
        if (next <= fp) break;
        fp = next;
    }
    w.wait = -2;
    guest_stack(w);
    g_wallNs.fetch_add(now_ns() - t0, std::memory_order_relaxed);
    g_wallSignals.fetch_add(1, std::memory_order_relaxed);
}

std::string ThreadName(uint32_t tid) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/self/task/%u/comm", tid);
    std::string name = "?";
    if (FILE* f = std::fopen(path, "r")) {
        char buf[64] = {};
        if (std::fgets(buf, sizeof(buf), f)) {
            name = buf;
            while (!name.empty() && (name.back() == '\n' || name.back() == ' ')) name.pop_back();
        }
        std::fclose(f);
    }
    return name;
}

// Una dirección del anfitrión: desplazamiento en el ejecutable ("x1a2b") o biblioteca:símbolo.
std::string HostWhere(uintptr_t a) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(&__ehdr_start);
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(a), &info) != 0 && info.dli_fbase == &__ehdr_start) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "x%lx", static_cast<unsigned long>(a - base));
        return buf;
    }
    std::string where = info.dli_fname ? info.dli_fname : "?";
    const size_t slash = where.rfind('/');
    if (slash != std::string::npos) where = where.substr(slash + 1);
    where += ":";
    where += info.dli_sname ? info.dli_sname : "?";
    return where;
}

void Dump(const char* path, int seconds) {
    const size_t n = std::min(g_count.load(), kMaxSamples);
    const uintptr_t base = reinterpret_cast<uintptr_t>(&__ehdr_start);
    std::map<uint32_t, size_t> perThread;
    std::map<std::pair<uint32_t, uintptr_t>, size_t> inExe;           // (hilo, desplazamiento) -> muestras
    std::map<std::pair<uint32_t, std::string>, size_t> outside;        // (hilo, biblioteca:símbolo) -> muestras
    for (size_t i = 0; i < n; ++i) {
        ++perThread[g_tids[i]];
        Dl_info info{};
        if (dladdr(reinterpret_cast<void*>(g_pcs[i]), &info) != 0 && info.dli_fbase == &__ehdr_start) {
            ++inExe[{g_tids[i], g_pcs[i] - base}];
        } else {
            ++outside[{g_tids[i], HostWhere(g_pcs[i])}];
        }
    }
    FILE* f = std::fopen(path, "w");
    if (!f) return;
    std::fprintf(f, "# choya perfil v2\nmuestras %zu\nsegundos %d\nfecha %lld\n", n, seconds,
                 static_cast<long long>(std::time(nullptr)));
    for (const auto& [tid, count] : perThread) std::fprintf(f, "hilo %u %zu %s\n", tid, count, ThreadName(tid).c_str());
    std::vector<std::pair<size_t, std::pair<uint32_t, uintptr_t>>> exeSorted;
    for (const auto& [key, count] : inExe) exeSorted.push_back({count, key});
    std::sort(exeSorted.begin(), exeSorted.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& [count, key] : exeSorted) std::fprintf(f, "pc %u %zu 0x%lx\n", key.first, count, static_cast<unsigned long>(key.second));
    for (const auto& [key, count] : outside) std::fprintf(f, "lib %u %zu %s\n", key.first, count, key.second.c_str());

    // Tramos: cuadros, hilo del juego, muestras de pared agrupadas por pila idéntica, llamadas.
    const size_t nw = std::min(g_wallCount.load(), kMaxWall);
#if defined(__aarch64__)
    std::fprintf(f, "arq aarch64\n");
#elif defined(__x86_64__)
    std::fprintf(f, "arq x86_64\n");
#endif
    std::fprintf(f, "costo_senal %llu %llu\n", static_cast<unsigned long long>(g_wallNs.load()),
                 static_cast<unsigned long long>(g_wallSignals.load()));
    std::fprintf(f, "cuadros %u\nhilo_juego %d %s\npared %zu\ndirs %08x %08x\n", g_frames.load(), g_gameTid.load(),
                 ThreadName(static_cast<uint32_t>(g_gameTid.load())).c_str(), nw, kCalc, kDraw);
    std::map<std::string, size_t> stacks;
    std::map<uintptr_t, std::string> where;  // memo de dladdr
    auto host = [&](uintptr_t a) -> const std::string& {
        auto it = where.find(a);
        if (it == where.end()) it = where.emplace(a, HostWhere(a)).first;
        return it->second;
    };
    for (size_t i = 0; i < nw; ++i) {
        const Wall& w = g_wall[i];
        if (w.pc == 0) continue;  // la señal no llegó a tiempo
        std::string line = std::to_string(w.wait) + " " + host(w.pc) + " " + (w.lr ? host(w.lr) : std::string("-")) + " h";
        for (int k = 0; k < w.nHost; ++k) line += " " + host(w.host[k]);
        line += " g";
        char buf[16];
        for (int k = 0; k < w.nGuest; ++k) {
            std::snprintf(buf, sizeof(buf), " %08x", w.guest[k]);
            line += buf;
        }
        ++stacks[line];
    }
    for (const auto& [line, count] : stacks) std::fprintf(f, "q %zu %s\n", count, line.c_str());
    // Resumen en la consola (sin nombres: solo la pila del juego y si dormía), para resumen.txt.
    size_t upd = 0, drw = 0, wt = 0, oth = 0, used = 0;
    std::map<int, size_t> waitBy;
    for (size_t i = 0; i < nw; ++i) {
        const Wall& w = g_wall[i];
        if (w.pc == 0) continue;
        ++used;
        if (w.wait != -2) {
            ++wt;
            ++waitBy[w.wait];
            continue;
        }
        int phase = 0;
        for (int k = 0; k < w.nGuest && phase == 0; ++k) {
            const uint32_t lr = w.guest[k];
            if (kCalc != 0 && lr >= kCalc && lr < kCalc + kCalcSize) phase = 1;
            else if (kDraw != 0 && lr >= kDraw && lr < kDraw + kDrawSize) phase = 2;
        }
        (phase == 1 ? upd : phase == 2 ? drw : oth) += 1;
    }
    if (used > 0) {
        const unsigned frames = std::max(1u, g_frames.load());
        const double frameMs = seconds * 1000.0 / frames;
        auto ms = [&](size_t c) { return frameMs * static_cast<double>(c) / static_cast<double>(used); };
        char line[512];
        std::snprintf(line, sizeof(line),
                      "[choya] tramos: %.1f cuadros/s, %.1f ms por cuadro = actualizar %.1f + dibujo %.1f + esperar %.1f "
                      "+ otros %.1f ms",
                      frames / static_cast<double>(seconds), frameMs, ms(upd), ms(drw), ms(wt), ms(oth));
        std::fprintf(f, "# %s\n", line + 8);
        std::fprintf(stderr, "%s\n", line);
        std::fprintf(stderr, "[choya] la medición le quitó al hilo del juego %.2f ms por cuadro en las señales (%llu)\n",
                     static_cast<double>(g_wallNs.load()) / 1e6 / frames,
                     static_cast<unsigned long long>(g_wallSignals.load()));
        std::string waits = "[choya] esperas (ms por cuadro):";
        for (const auto& [nr, c] : waitBy) {
            const char* name = nr == -1 ? "fallo de página" : nullptr;
#if defined(__aarch64__)
            switch (nr) {
            case 98: name = "futex"; break;
            case 101: name = "nanosleep"; break;
            case 115: name = "clock_nanosleep"; break;
            case 73: name = "ppoll"; break;
            case 22: name = "epoll"; break;
            case 29: name = "ioctl"; break;
            case 63: name = "read"; break;
            case 124: name = "sched_yield"; break;
            default: break;
            }
#endif
            char part[64];
            if (name) std::snprintf(part, sizeof(part), " %s %.1f;", name, ms(c));
            else std::snprintf(part, sizeof(part), " %d %.1f;", nr, ms(c));
            waits += part;
        }
        std::fprintf(stderr, "%s\n", waits.c_str());
    }
    if (g_calls != nullptr) {
        std::vector<std::pair<uint32_t, uint32_t>> calls;
        for (uint32_t i = 0; i < (kCodeHi - kCodeLo) / 4; ++i) {
            if (g_calls[i] != 0) calls.push_back({g_calls[i], kCodeLo + i * 4});
        }
        std::sort(calls.begin(), calls.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (const auto& [count, addr] : calls) std::fprintf(f, "llamadas %08x %u\n", addr, count);
        std::fprintf(f, "llamadas_otras %llu\n", static_cast<unsigned long long>(g_otherCalls.load()));
    }
    std::fclose(f);
}

std::atomic<bool> g_busy{false};
std::string g_outPath = "perfil.txt";

void Measure(int seconds) {
    // Memoria solo cuando se mide (unos 23 MB en total).
    if (g_pcs == nullptr) {
        g_pcs = new uintptr_t[kMaxSamples];
        g_tids = new uint32_t[kMaxSamples];
        g_wall = new Wall[kMaxWall];
        g_calls = new uint32_t[(kCodeHi - kCodeLo) / 4]();
    }
    g_count = 0;
    g_wallCount = 0;
    g_wallNs = 0;
    g_wallSignals = 0;
    g_frames = 0;
    g_guestOk = CHOYA_GUEST_BASE != 0 && CHOYA_GUEST_ACTIVE();
    if (g_textHi == 0) find_text();
    std::atomic_thread_fence(std::memory_order_seq_cst);
    g_choyaMidiendo = true;  // desde aquí cada llamada del juego se cuenta y se ve qué hilo corre el juego
    // El hilo del juego: el que llama a EGG::SceneManager::calc (hasta 3 s).
    for (int i = 0; i < 300 && g_gameTid.load() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const int gameTid = g_gameTid.load();
    // CPU de todos los hilos.
    struct sigaction prof {};
    prof.sa_sigaction = OnProf;
    prof.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&prof.sa_mask);
    sigaddset(&prof.sa_mask, SIGRTMIN + 6);  // la de pared espera a que termine esta (y al revés, abajo)
    sigaction(SIGPROF, &prof, nullptr);
    itimerval on{};
    on.it_interval.tv_usec = 1000;  // 1 ms (el núcleo lo redondea a su tick)
    on.it_value.tv_usec = 1000;
    g_sampling = true;
    setitimer(ITIMER_PROF, &on, nullptr);
    // Pared del hilo del juego, cada 2 ms (CHOYA_PERFIL_MS) desde este hilo. Si el juego está corriendo se le pide la muestra con una
    // señal (contador de programa y pilas exactas); si está dormido o bloqueado no se le despierta: se lee de
    // /proc en qué llamada al sistema está y dónde (así un sleep o un poll no se acortan por medir).
    std::fprintf(stderr, "[choya] midiendo %d s (hilo del juego %d)\n", seconds, gameTid);
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    if (gameTid != 0 && kCalc != 0) {
        g_wallSignal = SIGRTMIN + 6;
        struct sigaction wall {};
        wall.sa_sigaction = OnWall;
        wall.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&wall.sa_mask);
        sigaddset(&wall.sa_mask, SIGPROF);
        sigaction(g_wallSignal, &wall, nullptr);
        char path[96];
        std::snprintf(path, sizeof(path), "/proc/self/task/%d/stat", gameTid);
        const int statFd = open(path, O_RDONLY);
        std::snprintf(path, sizeof(path), "/proc/self/task/%d/syscall", gameTid);
        const int sysFd = open(path, O_RDONLY);
        const pid_t pid = getpid();
        g_wallOn = true;
        auto next = std::chrono::steady_clock::now();
        char buf[512];
        static const int periodMs = [] {
            const char* e = std::getenv("CHOYA_PERFIL_MS");
            const int v = e ? std::atoi(e) : 2;
            return v >= 1 && v <= 50 ? v : 2;
        }();
        while (std::chrono::steady_clock::now() < end) {
            next += std::chrono::milliseconds(periodMs);
            std::this_thread::sleep_until(next);
            const size_t i = g_wallCount.load();
            if (i >= kMaxWall || statFd < 0) continue;
            Wall& w = g_wall[i];
            w.pc = 0;
            const ssize_t n = pread(statFd, buf, sizeof(buf) - 1, 0);
            if (n <= 0) continue;
            buf[n] = 0;
            const char* paren = std::strrchr(buf, ')');
            const char state = paren && paren[1] ? paren[2] : '?';
            g_wallCount.store(i + 1);
            if (state == 'R') {
                g_wallTarget = static_cast<int>(i);
                syscall(SYS_tgkill, pid, gameTid, g_wallSignal);
                continue;
            }
            // Dormido (S) o bloqueado (D): "nr args... sp pc", "-1 sp pc" (fallo de página) o "running".
            const ssize_t m = sysFd >= 0 ? pread(sysFd, buf, sizeof(buf) - 1, 0) : -1;
            if (m <= 0) continue;
            buf[m] = 0;
            if (std::strncmp(buf, "running", 7) == 0) continue;
            char* p = buf;
            const long nr = std::strtol(p, &p, 10);
            unsigned long long words[8] = {};
            int got = 0;
            while (got < 8 && *p) {
                char* q = nullptr;
                const unsigned long long v = std::strtoull(p, &q, 16);
                if (q == p) break;
                words[got++] = v;
                p = q;
            }
            if (got < 2) continue;
            const uintptr_t usp = static_cast<uintptr_t>(words[got - 2]), upc = static_cast<uintptr_t>(words[got - 1]);
            w.wait = static_cast<int16_t>(nr);
            w.lr = 0;
            w.nHost = 0;
            // Quién la pidió: la primera dirección de retorno al ejecutable en la pila sobre sp (las bibliotecas del
            // sistema suelen ir sin registros de marco) y, si al lado hay un registro de marco, su cadena.
            if (usp >= g_stackLo && usp < g_stackHi) {
                for (uintptr_t a = usp & ~uintptr_t{7}; a + 8 <= g_stackHi && a < usp + 16384; a += 8) {
                    const uintptr_t v = *reinterpret_cast<const uintptr_t*>(a);
                    if (!is_return_into_exe(v)) continue;
                    w.host[w.nHost++] = v;
                    uintptr_t fp = a >= 8 ? *reinterpret_cast<const uintptr_t*>(a - 8) : 0;
                    while (w.nHost < kHostDepth && fp > a && fp + 16 <= g_stackHi && (fp & 7) == 0) {
                        const uintptr_t* rec = reinterpret_cast<const uintptr_t*>(fp);
                        if (rec[1] == 0) break;
                        w.host[w.nHost++] = rec[1];
                        if (rec[0] <= fp) break;
                        fp = rec[0];
                    }
                    break;
                }
            }
            guest_stack(w);
            w.pc = upc;
        }
        g_wallOn = false;
        if (statFd >= 0) close(statFd);
        if (sysFd >= 0) close(sysFd);
    } else {
        std::this_thread::sleep_until(end);
    }
    itimerval off{};
    setitimer(ITIMER_PROF, &off, nullptr);
    g_sampling = false;
    g_wallOn = false;
    g_choyaMidiendo = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // que terminen los avisos en vuelo
    Dump(g_outPath.c_str(), seconds);
    std::fprintf(stderr, "[choya] perfil escrito en %s (%zu muestras de CPU, %zu de pared, %u cuadros)\n",
                 g_outPath.c_str(), g_count.load(), g_wallCount.load(), g_frames.load());
    g_busy = false;
}

struct Starter {
    Starter() {
        if (const char* rv = std::getenv("CHOYA_SIN_REVERB"); rv && std::strcmp(rv, "1") == 0 && kAuxCallback != 0) {
            g_choyaSinEfectos = kAuxCallback;
            std::fprintf(stderr, "[choya] sin efectos de audio (reverberación): AuxCallbackFunc 0x%08X no se llama\n",
                         kAuxCallback);
        }
        if (const char* out = std::getenv("CHOYA_PERFIL_SALIDA"); out && *out) g_outPath = out;
        const char* spec = std::getenv("CHOYA_PERFIL");
        if (spec == nullptr || *spec == '\0') return;
        int wait = 20, seconds = 35;
        std::sscanf(spec, "%d,%d", &wait, &seconds);
        g_busy = true;
        std::thread([wait, seconds] {
            std::this_thread::sleep_for(std::chrono::seconds(wait));
            Measure(seconds);
        }).detach();
    }
};
Starter g_starter;


extern "C" void choya_medir_llamada(uint32_t target, const void* ctx) {
    if (target >= kCodeLo && target < kCodeHi) {
        ++g_calls[(target - kCodeLo) >> 2];  // sin atómicas: solo el hilo del juego llama casi siempre
    } else {
        g_otherCalls.fetch_add(1, std::memory_order_relaxed);
    }
    if (target == kCalc && kCalc != 0) {
        if (!t_isGame) {
            t_isGame = true;
            pthread_attr_t attr;
            if (pthread_getattr_np(pthread_self(), &attr) == 0) {
                void* addr = nullptr;
                size_t size = 0;
                if (pthread_attr_getstack(&attr, &addr, &size) == 0) {
                    g_stackLo = reinterpret_cast<uintptr_t>(addr);
                    g_stackHi = g_stackLo + size;
                }
                pthread_attr_destroy(&attr);
            }
            int expected = 0;
            g_gameTid.compare_exchange_strong(expected, static_cast<int>(syscall(SYS_gettid)));
        }
        g_frames.fetch_add(1, std::memory_order_relaxed);
    }
    if (t_isGame) g_gameCtx = ctx;
}

// En vez de AuxCallbackFunc(data, context): data (r3) apunta a los punteros de los búferes del bus auxiliar
// (izquierdo, derecho, envolvente) de 96 muestras de 32 bits; el DSP los vuelve a leer como la vuelta del efecto.
// En cero = sin reverberación; el sonido directo no pasa por aquí y no cambia.
extern "C" bool choya_sin_efectos(void* ctx) {
    if (ctx == nullptr || CHOYA_GUEST_BASE == 0 || !CHOYA_GUEST_ACTIVE()) return false;
    const uint32_t data = static_cast<const uint32_t*>(ctx)[3];
    if (!guest_ram(data) || !guest_ram(data + 12)) return false;
    for (int i = 0; i < 3; ++i) {
        const uint32_t buf = guest_read32(data + 4u * static_cast<uint32_t>(i));
        if (guest_ram(buf) && guest_ram(buf + 96u * 4u - 1u)) {
            std::memset(reinterpret_cast<void*>(CHOYA_GUEST_BASE + buf), 0, 96u * 4u);
        }
    }
    return true;
}

// --- Memoria del juego -----------------------------------------------------------------------------------------
// Wii Sports arma cada Mii con nw4r::g3d::ScnRfl::Construct, que pide un bloque grande (la cara, las expresiones)
// al montón de modelos y, si no cabe, devuelve NULL; RPGrpModelRfl::Construct no lo revisa y el juego salta a 0 al
// llamar a G3dProc (0.20.9: "Missing translated function ... 0x0" con LR en RPGrpModelRfl::Construct). Aquí:
//  - Cuando una petición de 16 KB o más no cabe en un montón expandible, se anota quién la pidió, cuánto había libre
//    y el árbol de montones (una vez), y se le presta el bloque de otro montón expandible con espacio de sobra (el
//    que tenga el bloque libre más grande y le queden al menos 2 MB): así el Mii se arma en vez de cerrarse el
//    juego. Los bloques prestados se devuelven al montón correcto: MEMFreeToExpHeap y MEMResizeForMBlockExpHeap
//    reciben el montón que los contiene (EGG ya lo busca así para delete). Hasta 24 MB prestados en total.
//    CHOYA_PRESTAMOS=0 solo anota.
//  - EGG::Heap::alloc devuelve NULL sin mirar el espacio si el juego dejó asignar solo de un montón
//    (disableAllocationAllBut) y la petición va a otro: también se anota.
//  - Al cerrarse por un salto a 0 se escribe el árbol de montones con lo libre de cada uno.
constexpr uint32_t kMemAllocExp = 0x0u /*CHOYA_SYM:MEMAllocFromExpHeapEx*/;
constexpr uint32_t kMemAllocFrm = 0x0u /*CHOYA_SYM:MEMAllocFromFrmHeapEx*/;
constexpr uint32_t kMemFreeExp = 0x0u /*CHOYA_SYM:MEMFreeToExpHeap*/;
constexpr uint32_t kMemResizeExp = 0x0u /*CHOYA_SYM:MEMResizeForMBlockExpHeap*/;
constexpr uint32_t kEggHeapAlloc = 0x0u /*CHOYA_SYM:alloc__Q23EGG4HeapFUliPQ23EGG4Heap*/;
constexpr uint32_t kMemRootList = 0x0u /*CHOYA_SYM:sRootList*/;
constexpr uint32_t kEggAllocatable = 0x0u /*CHOYA_SYM:sAllocatableHeap__Q23EGG4Heap*/;
constexpr uint32_t kEggCurrent = 0x0u /*CHOYA_SYM:sCurrentHeap__Q23EGG4Heap*/;
constexpr uint32_t kExpMagic = 0x45585048u, kFrmMagic = 0x46524D48u, kUnitMagic = 0x554E5448u;  // EXPH FRMH UNTH
constexpr uint32_t kWatchMin = 16 * 1024;            // peticiones más chicas no se revisan (las grandes son las raras)
constexpr uint32_t kLendKeep = 2u << 20;             // lo que le queda al montón que presta, como mínimo
constexpr uint32_t kLendBudget = 24u << 20;          // total prestado
constexpr int kMaxNotes = 24;

struct HeapStat {
    bool ok = false;
    uint32_t magic = 0, start = 0, end = 0, free = 0, largest = 0, blocks = 0;
};
struct Loan { uint32_t block, size, owner, lender; };
std::vector<Loan> g_loans;
uint32_t g_lent = 0;
int g_notes = 0;
bool g_treeShown = false;
int g_lendMode = -1;  // -1: sin leer CHOYA_PRESTAMOS

inline bool guest_range(uint32_t a, uint32_t n) { return guest_ram(a) && n > 0 && guest_ram(a + n - 1); }
inline uint32_t round_up(uint32_t v, uint32_t a) { return a <= 1 ? v : (v + a - 1) & ~(a - 1); }
inline uint32_t round_down(uint32_t v, uint32_t a) { return a <= 1 ? v : v & ~(a - 1); }

bool heap_ok(uint32_t h) {
    if (!guest_range(h, 0x48)) return false;
    const uint32_t m = guest_read32(h);
    return m == kExpMagic || m == kFrmMagic || m == kUnitMagic;
}

// ¿Cabe size (alineado a align; negativo = desde el final) en el montón? Y lo libre que tiene.
HeapStat heap_stat(uint32_t h, uint32_t size = 0, int32_t align = 4, bool* fits = nullptr) {
    HeapStat st;
    if (fits) *fits = false;
    if (!heap_ok(h)) return st;
    st.magic = guest_read32(h);
    st.start = guest_read32(h + 0x18);
    st.end = guest_read32(h + 0x1C);
    const uint32_t al = static_cast<uint32_t>(align < 0 ? -align : align);
    if (st.magic == kExpMagic) {
        uint32_t b = guest_read32(h + 0x3C);  // freeMBlocks.head
        for (int n = 0; b != 0 && n < 4096; ++n) {
            if (!guest_range(b, 16)) return st;
            const uint32_t bsz = guest_read32(b + 4), mem = b + 16, bend = mem + bsz;
            st.free += bsz;
            st.largest = std::max(st.largest, bsz);
            ++st.blocks;
            if (fits && size != 0 && bend >= mem) {
                // Como MEMAllocFromExpHeapEx: el bloque usado lleva su cabecera de 16 bytes delante.
                if (align >= 0) {
                    const uint32_t p = round_up(mem, al);
                    if (p >= mem && p + size <= bend && p + size >= p) *fits = true;
                } else if (bend >= size + mem) {
                    if (round_down(bend - size, al) >= mem) *fits = true;
                }
            }
            b = guest_read32(b + 0xC);
        }
    } else if (st.magic == kFrmMagic) {
        const uint32_t head = guest_read32(h + 0x3C), tail = guest_read32(h + 0x40);
        st.free = tail > head ? tail - head : 0;
        st.largest = st.free;
        st.blocks = 1;
        if (fits && size != 0 && tail > head) {
            if (align >= 0) *fits = round_up(head, al) + size <= tail;
            else *fits = tail >= size && round_down(tail - size, al) >= head;
        }
    } else {
        st.blocks = 0;  // montón por unidades: no se usa para modelos
    }
    st.ok = true;
    return st;
}

// Recorre el árbol de montones de MEM en orden (cada montón y luego sus hijos): sRootList y la lista de hijos de cada
// uno (MEMList en +0xC; el enlace del objeto, MEMLink, en el desplazamiento que dice la lista).
template <typename F>
void walk_heaps(uint32_t list, int depth, int& seen, F& f) {
    if (depth > 8 || !guest_range(list, 12)) return;
    const uint32_t offset = guest_read32(list + 8) & 0xFFFFu;
    uint32_t h = guest_read32(list);
    for (int n = 0; h != 0 && n < 128 && seen < 256 && heap_ok(h); ++n, ++seen) {
        f(h, depth);
        walk_heaps(h + 0xC, depth + 1, seen, f);
        h = guest_read32(h + offset + 4);  // MEMLink.next
    }
}
template <typename F>
void for_each_heap(F&& f) {
    if (kMemRootList == 0) return;
    int seen = 0;
    walk_heaps(kMemRootList, 0, seen, f);
}

uint32_t heap_containing(uint32_t addr) {
    uint32_t best = 0, bestSize = ~0u;
    for_each_heap([&](uint32_t h, int) {
        const uint32_t s = guest_read32(h + 0x18), e = guest_read32(h + 0x1C);
        if (addr >= s && addr < e && e - s < bestSize) { best = h; bestSize = e - s; }
    });
    return best;
}

const char* heap_kind(uint32_t magic) {
    return magic == kExpMagic ? "expandible" : magic == kFrmMagic ? "de cuadro" : "por unidades";
}

}  // namespace choya_medida
#if defined(CHOYA_CON_SIMBOLOS)
extern "C" {
extern const uint32_t kGuestMapSymbolCount;
extern const uint32_t kGuestMapSymbolAddresses[];
extern const char* const kGuestMapSymbolNames[];
}
#endif
namespace choya_medida {

// Nombre de una dirección del juego con la tabla de símbolos del runtime (la misma de los informes de cierre).
std::string guest_name(uint32_t a) {
    char buf[160];
#if defined(CHOYA_CON_SIMBOLOS)
    if (kGuestMapSymbolCount != 0) {
        uint32_t lo = 0, hi = kGuestMapSymbolCount;
        while (lo < hi) {
            const uint32_t mid = lo + (hi - lo) / 2;
            if (kGuestMapSymbolAddresses[mid] <= a) lo = mid + 1; else hi = mid;
        }
        if (lo > 0 && a - kGuestMapSymbolAddresses[lo - 1] < 0x10000u) {
            std::snprintf(buf, sizeof(buf), "%08X <%s+0x%X>", a, kGuestMapSymbolNames[lo - 1],
                          a - kGuestMapSymbolAddresses[lo - 1]);
            return buf;
        }
    }
#endif
    std::snprintf(buf, sizeof(buf), "%08X", a);
    return buf;
}

// Quién pidió: LR y la cadena de marcos desde r1 (hasta 8).
std::string guest_callers(const uint32_t* c) {
    std::string out = guest_name(c[33]);
    uint32_t sp = c[1];
    for (int n = 0; n < 8 && guest_range(sp, 8) && (sp & 7) == 0; ++n) {
        const uint32_t prev = guest_read32(sp);
        if (!guest_range(prev, 8) || prev <= sp || prev - sp > 0x20000u) break;
        const uint32_t lr = guest_read32(prev + 4);
        if (lr < 0x80000000u || lr >= 0x81800000u) break;
        out += " ← " + guest_name(lr);
        sp = prev;
    }
    return out;
}

void show_tree(const char* why) {
    std::fprintf(stderr, "[choya] montones del juego (%s):\n", why);
    for_each_heap([](uint32_t h, int depth) {
        const HeapStat st = heap_stat(h);
        uint32_t lent = 0;
        for (const Loan& l : g_loans) if (l.lender == h) lent += l.size;
        std::fprintf(stderr, "[choya]   %*s0x%08X %s %u KB: libre %u KB en %u bloques, el mayor %u KB%s",
                     depth * 2, "", h, heap_kind(st.magic), (st.end - st.start) >> 10, st.free >> 10, st.blocks,
                     st.largest >> 10, lent ? "" : "\n");
        if (lent) std::fprintf(stderr, "; prestó %u KB\n", lent >> 10);
    });
    if (kEggAllocatable != 0 && guest_range(kEggAllocatable, 4) && guest_read32(kEggAllocatable) != 0) {
        std::fprintf(stderr, "[choya]   EGG solo deja asignar del montón EGG 0x%08X\n", guest_read32(kEggAllocatable));
    }
    std::fflush(stderr);
}

// Otro montón expandible que tenga el bloque (con 2 MB de sobra después), el de bloque libre más grande.
uint32_t pick_lender(uint32_t owner, uint32_t size, int32_t align) {
    uint32_t best = 0, bestLargest = 0;
    for_each_heap([&](uint32_t h, int) {
        if (h == owner || guest_read32(h) != kExpMagic) return;
        // Ni un montón dentro del que se quedó sin espacio: el bloque caería en su zona y, al devolverlo con
        // MEMFreeToExpHeap(owner, ...), no se notaría que es prestado.
        const uint32_t os = guest_read32(owner + 0x18), oe = guest_read32(owner + 0x1C);
        if (guest_read32(h + 0x18) >= os && guest_read32(h + 0x1C) <= oe) return;
        bool fits = false;
        const HeapStat st = heap_stat(h, size, align, &fits);
        if (!fits || st.largest < size + kLendKeep) return;
        if (st.largest > bestLargest) { best = h; bestLargest = st.largest; }
    });
    return best;
}

void note(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void note(const char* fmt, ...) {
    if (g_notes >= kMaxNotes) return;
    if (++g_notes == kMaxNotes) {
        std::fprintf(stderr, "[choya] memoria del juego: ya no anoto más avisos\n");
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    std::fputs("[choya] memoria del juego: ", stderr);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    va_end(ap);
    std::fflush(stderr);
}

void watch_alloc(uint32_t* c, bool frame) {
    const uint32_t heap = c[3], size = c[4];
    const int32_t align = static_cast<int32_t>(c[5]);
    if (size < kWatchMin) return;
    bool fits = false;
    const HeapStat st = heap_stat(heap, size, align, &fits);
    if (!st.ok || fits) return;
    if (g_lendMode < 0) {
        const char* v = std::getenv("CHOYA_PRESTAMOS");
        g_lendMode = (v != nullptr && std::strcmp(v, "0") == 0) ? 0 : 1;
    }
    const std::string who = guest_callers(c);
    uint32_t lender = 0;
    if (!frame && g_lendMode == 1 && g_lent + size <= kLendBudget) lender = pick_lender(heap, size, align);
    note("no cabe un bloque de %u KB en el montón %s 0x%08X (%u KB, libre %u KB en %u bloques, el mayor %u KB)%s; "
         "lo pidió %s",
         (size + 1023) >> 10, heap_kind(st.magic), heap, (st.end - st.start) >> 10, st.free >> 10, st.blocks,
         st.largest >> 10,
         lender ? "; se lo presta otro montón" : (frame ? "" : g_lendMode == 0 ? " (préstamos apagados)"
                                                            : " y ningún otro montón tiene el espacio"),
         who.c_str());
    if (!g_treeShown) {
        g_treeShown = true;
        show_tree("la primera vez que no cupo un bloque");
    }
    if (lender != 0) {
        c[3] = lender;  // MEMAllocFromExpHeapEx(lender, size, align): un bloque normal de ese montón
        g_lent += size;
        g_loans.push_back({0, size, heap, lender});
    }
}

// El bloque prestado vuelve a su montón: MEMFreeToExpHeap / MEMResizeForMBlockExpHeap(heap, block, ...).
void redirect_free(uint32_t* c) {
    if (g_loans.empty()) return;
    const uint32_t heap = c[3], block = c[4];
    if (!heap_ok(heap)) return;
    if (block >= guest_read32(heap + 0x18) && block < guest_read32(heap + 0x1C)) return;  // es de ese montón
    const uint32_t real = heap_containing(block);
    if (real == 0 || guest_read32(real) != kExpMagic) return;
    c[3] = real;
    for (auto it = g_loans.begin(); it != g_loans.end(); ++it) {
        if (it->lender == real && it->owner == heap) {
            g_lent -= std::min(g_lent, it->size);
            g_loans.erase(it);
            break;
        }
    }
}

void watch_egg_alloc(const uint32_t* c) {
    if (kEggAllocatable == 0 || !guest_range(kEggAllocatable, 4)) return;
    const uint32_t only = guest_read32(kEggAllocatable);
    if (only == 0) return;
    uint32_t heap = c[5];
    if (heap == 0 && kEggCurrent != 0 && guest_range(kEggCurrent, 4)) heap = guest_read32(kEggCurrent);
    if (heap == only) return;
    const std::string who = guest_callers(c);
    note("EGG no deja asignar %u bytes del montón EGG 0x%08X (solo del 0x%08X: disableAllocationAllBut); lo pidió %s",
         c[3], heap, only, who.c_str());
}

}  // namespace choya_medida

extern "C" void choya_vigila_memoria(uint32_t target, void* ctx) {
    using namespace choya_medida;
    if (ctx == nullptr || CHOYA_GUEST_BASE == 0 || !CHOYA_GUEST_ACTIVE()) return;
    auto* c = static_cast<uint32_t*>(ctx);
    if (target == kMemAllocExp) watch_alloc(c, false);
    else if (target == kMemAllocFrm) watch_alloc(c, true);
    else if (target == kMemFreeExp || target == kMemResizeExp) redirect_free(c);
    else if (target == kEggHeapAlloc) watch_egg_alloc(c);
}

extern "C" void choya_informe_memoria(uint32_t target, const void* ctx) {
    using namespace choya_medida;
    if (CHOYA_GUEST_BASE == 0 || !CHOYA_GUEST_ACTIVE()) return;
    if (target == 0 && ctx != nullptr) {
        const auto* c = static_cast<const uint32_t*>(ctx);
        std::fprintf(stderr, "[choya] el juego saltó a 0 desde %s (r3=0x%08X: un objeto nulo, casi siempre un bloque que "
                     "no cupo)\n", guest_callers(c).c_str(), c[3]);
    }
    std::fprintf(stderr, "[choya] memoria del juego: %zu bloques prestados (%u KB)\n", g_loans.size(), g_lent >> 10);
    show_tree("al cerrarse");
}

namespace choya_medida {

extern "C" int choya_medir_empezar(int seconds) {
    bool expected = false;
    if (!g_busy.compare_exchange_strong(expected, true)) return 0;  // ya está midiendo
    std::thread([seconds] { Measure(seconds); }).detach();
    return 1;
}
} // namespace choya_medida (las dos de arriba son extern "C": su nombre no lleva el namespace)
#else
extern "C" void choya_medir_llamada(uint32_t, const void*) {}
extern "C" int choya_medir_empezar(int) { return 0; }
extern "C" bool choya_sin_efectos(void*) { return false; }
extern "C" void choya_vigila_memoria(uint32_t, void*) {}
extern "C" void choya_informe_memoria(uint32_t, const void*) {}
#endif
