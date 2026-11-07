// Choya Wii Converter: perfilador por muestreo para medir dónde se va el tiempo en la consola.
// Solo se activa con CHOYA_PERFIL="espera,segundos" (p. ej. "20,35"): espera a que el juego llegue a la
// pantalla de título, toma muestras del contador de programa con SIGPROF durante "segundos" y escribe
// perfil.txt (o CHOYA_PERFIL_SALIDA). En la PC, `choya perfil perfil.txt` pone nombre a cada dirección.
// Sin la variable no hace nada.
#if defined(__linux__)
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <string>
#include <sys/syscall.h>
#include <sys/time.h>
#include <thread>
#include <ucontext.h>
#include <unistd.h>
#include <utility>
#include <vector>

extern "C" char __ehdr_start;  // inicio del ejecutable (PIE) en memoria

namespace {

constexpr size_t kMaxSamples = 1u << 20;
uintptr_t g_pcs[kMaxSamples];
uint32_t g_tids[kMaxSamples];
std::atomic<size_t> g_count{0};
std::atomic<bool> g_sampling{false};

void OnProf(int, siginfo_t*, void* raw) {
    if (!g_sampling.load(std::memory_order_relaxed)) return;
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
            std::string where = info.dli_fname ? info.dli_fname : "?";
            const size_t slash = where.rfind('/');
            if (slash != std::string::npos) where = where.substr(slash + 1);
            where += ":";
            where += info.dli_sname ? info.dli_sname : "?";
            ++outside[{g_tids[i], where}];
        }
    }
    FILE* f = std::fopen(path, "w");
    if (!f) return;
    std::fprintf(f, "# choya perfil v1\nmuestras %zu\nsegundos %d\n", n, seconds);
    for (const auto& [tid, count] : perThread) std::fprintf(f, "hilo %u %zu %s\n", tid, count, ThreadName(tid).c_str());
    std::vector<std::pair<size_t, std::pair<uint32_t, uintptr_t>>> exeSorted;
    for (const auto& [key, count] : inExe) exeSorted.push_back({count, key});
    std::sort(exeSorted.begin(), exeSorted.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& [count, key] : exeSorted) std::fprintf(f, "pc %u %zu 0x%lx\n", key.first, count, static_cast<unsigned long>(key.second));
    for (const auto& [key, count] : outside) std::fprintf(f, "lib %u %zu %s\n", key.first, count, key.second.c_str());
    std::fclose(f);
}

struct Starter {
    Starter() {
        const char* spec = std::getenv("CHOYA_PERFIL");
        if (spec == nullptr || *spec == '\0') return;
        int wait = 20, seconds = 35;
        std::sscanf(spec, "%d,%d", &wait, &seconds);
        const char* out = std::getenv("CHOYA_PERFIL_SALIDA");
        std::string path = out && *out ? out : "perfil.txt";
        struct sigaction sa {};
        sa.sa_sigaction = OnProf;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGPROF, &sa, nullptr);
        std::thread([wait, seconds, path] {
            std::this_thread::sleep_for(std::chrono::seconds(wait));
            itimerval on{};
            on.it_interval.tv_usec = 1000;  // 1 ms (el núcleo lo redondea a su tick)
            on.it_value.tv_usec = 1000;
            g_sampling = true;
            setitimer(ITIMER_PROF, &on, nullptr);
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
            itimerval off{};
            setitimer(ITIMER_PROF, &off, nullptr);
            g_sampling = false;
            Dump(path.c_str(), seconds);
            std::fprintf(stderr, "[choya] perfil escrito en %s (%zu muestras)\n", path.c_str(), g_count.load());
        }).detach();
    }
};
Starter g_starter;

} // namespace
#endif
