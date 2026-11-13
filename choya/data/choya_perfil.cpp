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
extern "C" void choya_medir_llamada(uint32_t target, const void* ctx);
extern "C" int choya_medir_empezar(int seconds);  // 1: empezó; 0: ya estaba midiendo

#if defined(__linux__)
#include <algorithm>
#include <atomic>
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

void OnWall(int, siginfo_t*, void* raw) {
    const int i = g_wallTarget;
    if (!g_wallOn.load(std::memory_order_relaxed) || g_wall == nullptr || i < 0) return;
    g_wallTarget = -1;
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
#endif
