// Choya Wii Converter: pantalla GLES propia para la consola (paso 1 del motor de Choya dentro de WiiCompiled).
//
// Abre EGL directamente sobre el framebuffer de la Mali (fbdev, sin X11/Wayland/DRM), igual que el SDL2 "mali"
// de KNULLI, y presenta un cuadro por cada cuadro del juego. En este paso todavía no dibuja la geometría del
// juego: limpia la pantalla con un color que cambia y una barra que avanza, para comprobar que la pantalla, el
// contexto GLES y el ritmo de presentación funcionan dentro del proceso del juego, y anota en choya-gles.txt qué
// versión de OpenGL ES y qué extensiones ofrece el driver (de eso depende cómo se arma el motor).
//
// Desde el paso 2a, cada cuadro lo dibuja el motor de Choya dentro de aurora (lib/choya/choya_gx.cpp) con los
// datos del cuadro que armó aurora; la pantalla de prueba solo sale mientras todavía no hay cuadro.
//
// Solo se activa con CHOYA_GLES=1. Si algo falla lo anota y se apaga; el juego sigue corriendo.
// Las bibliotecas se cargan con dlopen: el ejecutable no depende de libEGL/libGLESv2 para arrancar.
#if defined(__linux__)
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <dirent.h>
#include <map>
#include <pthread.h>
#include <sys/syscall.h>
#include <vector>

#include <SDL3/SDL.h>

#include "choya_mando.h"  // el mando de la consola (como en Nebula)

extern "C" bool choya_gx_draw_frame(int screenWidth, int screenHeight);  // motor de Choya (aurora)
extern "C" void choya_gx_request_dump();  // volcado del siguiente cuadro (aurora)
extern "C" int choya_medir_empezar(int seconds);  // medición por tramos (choya_perfil.cpp)

namespace {

// --- EGL/GLES mínimos (sin cabeceras: los tipos y constantes necesarios) ---
using EGLDisplay = void*;
using EGLConfig = void*;
using EGLSurface = void*;
using EGLContext = void*;
using EGLint = int32_t;
using EGLBoolean = unsigned int;
using GLenum = unsigned int;
using GLbitfield = unsigned int;
using GLint = int;
using GLsizei = int;
using GLfloat = float;

constexpr EGLint EGL_NONE = 0x3038, EGL_RED_SIZE = 0x3024, EGL_GREEN_SIZE = 0x3023, EGL_BLUE_SIZE = 0x3022,
                 EGL_ALPHA_SIZE = 0x3021, EGL_DEPTH_SIZE = 0x3025, EGL_STENCIL_SIZE = 0x3026,
                 EGL_SURFACE_TYPE = 0x3033, EGL_WINDOW_BIT = 0x0004, EGL_RENDERABLE_TYPE = 0x3040,
                 EGL_OPENGL_ES3_BIT = 0x0040, EGL_CONTEXT_CLIENT_VERSION = 0x3098,
                 EGL_CONTEXT_MINOR_VERSION = 0x30FB, EGL_VENDOR = 0x3053, EGL_VERSION = 0x3054,
                 EGL_EXTENSIONS = 0x3055, EGL_OPENGL_ES_API = 0x30A0;
constexpr GLenum GL_VENDOR = 0x1F00, GL_RENDERER = 0x1F01, GL_VERSION = 0x1F02, GL_EXTENSIONS = 0x1F03,
                 GL_SHADING_LANGUAGE_VERSION = 0x8B8C, GL_COLOR_BUFFER_BIT = 0x4000,
                 GL_DEPTH_BUFFER_BIT = 0x0100, GL_SCISSOR_TEST = 0x0C11, GL_MAX_TEXTURE_SIZE = 0x0D33,
                 GL_NUM_EXTENSIONS = 0x821D, GL_MAX_VERTEX_ATTRIBS = 0x8869,
                 GL_MAX_UNIFORM_BLOCK_SIZE = 0x8A30, GL_MAX_FRAGMENT_UNIFORM_VECTORS = 0x8DFD,
                 GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS = 0x8B4D,
                 GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS = 0x90DD, GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS = 0x90D6;

struct Egl {
    EGLDisplay (*GetDisplay)(void*);
    EGLBoolean (*Initialize)(EGLDisplay, EGLint*, EGLint*);
    EGLBoolean (*BindAPI)(unsigned int);
    EGLBoolean (*ChooseConfig)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*);
    EGLSurface (*CreateWindowSurface)(EGLDisplay, EGLConfig, void*, const EGLint*);
    EGLContext (*CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*);
    EGLBoolean (*MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
    EGLBoolean (*SwapBuffers)(EGLDisplay, EGLSurface);
    EGLBoolean (*SwapInterval)(EGLDisplay, EGLint);
    const char* (*QueryString)(EGLDisplay, EGLint);
    EGLint (*GetError)();
    void* (*GetProcAddress)(const char*);
};
struct Gl {
    const unsigned char* (*GetString)(GLenum);
    const unsigned char* (*GetStringi)(GLenum, unsigned int);
    void (*GetIntegerv)(GLenum, GLint*);
    void (*ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
    void (*Clear)(GLbitfield);
    void (*Viewport)(GLint, GLint, GLsizei, GLsizei);
    void (*Enable)(GLenum);
    void (*Disable)(GLenum);
    void (*Scissor)(GLint, GLint, GLsizei, GLsizei);
    GLenum (*GetError)();
};

struct FbdevWindow {  // ventana nativa del EGL de Mali para fbdev
    unsigned short width;
    unsigned short height;
};

struct State {
    void* glesLib = nullptr;
    uint64_t motorFrames = 0, patternFrames = 0;
    bool tried = false;
    bool ok = false;
    Egl egl{};
    Gl gl{};
    EGLDisplay display = nullptr;
    EGLSurface surface = nullptr;
    EGLContext context = nullptr;
    FbdevWindow window{640, 480};
    uint64_t frames = 0;
    double swapMsTotal = 0, swapMsWorst = 0;
    uint64_t swaps = 0;  // intercambios en la ventana del informe (0.23.3: antes se contaba con g.frames)
    std::chrono::steady_clock::time_point windowStart{};
    FILE* log = nullptr;
};
State g;
void DrawPattern();
void Present();

void Log(const char* fmt, const char* a = "", const char* b = "") {
    std::fprintf(stderr, "[choya-gles] ");
    std::fprintf(stderr, fmt, a, b);
    std::fprintf(stderr, "\n");
    if (g.log) {
        std::fprintf(g.log, fmt, a, b);
        std::fprintf(g.log, "\n");
        std::fflush(g.log);
    }
}

void* OpenFirst(const char* const* names) {
    for (const char* const* n = names; *n; ++n) {
        if (void* h = dlopen(*n, RTLD_NOW | RTLD_GLOBAL)) {
            Log("biblioteca: %s", *n);
            return h;
        }
    }
    return nullptr;
}

template <typename T>
bool Load(void* lib, void* (*gpa)(const char*), T& fn, const char* name) {
    void* p = lib ? dlsym(lib, name) : nullptr;
    if (!p && gpa) p = gpa(name);
    fn = reinterpret_cast<T>(p);
    if (!p) Log("falta la función %s", name);
    return p != nullptr;
}

bool Init() {
    const char* logPath = std::getenv("CHOYA_GLES_LOG");
    g.log = std::fopen(logPath && *logPath ? logPath : "choya-gles.txt", "w");

    static const char* const kEgl[] = {"libEGL.so.1", "libEGL.so", "libmali.so.0", "libmali.so", nullptr};
    static const char* const kGles[] = {"libGLESv2.so.2", "libGLESv2.so", "libmali.so.0", "libmali.so", nullptr};
    void* egl = OpenFirst(kEgl);
    void* gles = OpenFirst(kGles);
    g.glesLib = gles;
    if (!egl) {
        Log("no pude abrir libEGL (%s)", dlerror());
        return false;
    }
    bool ok = Load(egl, nullptr, g.egl.GetProcAddress, "eglGetProcAddress");
    auto* gpa = g.egl.GetProcAddress;
    ok &= Load(egl, gpa, g.egl.GetDisplay, "eglGetDisplay");
    ok &= Load(egl, gpa, g.egl.Initialize, "eglInitialize");
    ok &= Load(egl, gpa, g.egl.BindAPI, "eglBindAPI");
    ok &= Load(egl, gpa, g.egl.ChooseConfig, "eglChooseConfig");
    ok &= Load(egl, gpa, g.egl.CreateWindowSurface, "eglCreateWindowSurface");
    ok &= Load(egl, gpa, g.egl.CreateContext, "eglCreateContext");
    ok &= Load(egl, gpa, g.egl.MakeCurrent, "eglMakeCurrent");
    ok &= Load(egl, gpa, g.egl.SwapBuffers, "eglSwapBuffers");
    ok &= Load(egl, gpa, g.egl.SwapInterval, "eglSwapInterval");
    ok &= Load(egl, gpa, g.egl.QueryString, "eglQueryString");
    ok &= Load(egl, gpa, g.egl.GetError, "eglGetError");
    ok &= Load(gles, gpa, g.gl.GetString, "glGetString");
    Load(gles, gpa, g.gl.GetStringi, "glGetStringi");
    ok &= Load(gles, gpa, g.gl.GetIntegerv, "glGetIntegerv");
    ok &= Load(gles, gpa, g.gl.ClearColor, "glClearColor");
    ok &= Load(gles, gpa, g.gl.Clear, "glClear");
    ok &= Load(gles, gpa, g.gl.Viewport, "glViewport");
    ok &= Load(gles, gpa, g.gl.Enable, "glEnable");
    ok &= Load(gles, gpa, g.gl.Disable, "glDisable");
    ok &= Load(gles, gpa, g.gl.Scissor, "glScissor");
    ok &= Load(gles, gpa, g.gl.GetError, "glGetError");
    if (!ok) return false;

    // Tamaño real de la pantalla.
    if (int fd = open("/dev/fb0", O_RDONLY); fd >= 0) {
        fb_var_screeninfo vinfo{};
        if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) == 0 && vinfo.xres && vinfo.yres) {
            g.window.width = static_cast<unsigned short>(vinfo.xres);
            g.window.height = static_cast<unsigned short>(vinfo.yres);
        }
        close(fd);
    }
    char size[32];
    std::snprintf(size, sizeof(size), "%ux%u", g.window.width, g.window.height);
    Log("pantalla: %s", size);

    g.display = g.egl.GetDisplay(nullptr);  // EGL_DEFAULT_DISPLAY
    EGLint major = 0, minor = 0;
    if (!g.display || !g.egl.Initialize(g.display, &major, &minor)) {
        Log("eglInitialize falló");
        return false;
    }
    Log("EGL: %s %s", g.egl.QueryString(g.display, EGL_VENDOR), g.egl.QueryString(g.display, EGL_VERSION));
    Log("EGL extensiones: %s", g.egl.QueryString(g.display, EGL_EXTENSIONS));
    g.egl.BindAPI(EGL_OPENGL_ES_API);

    const EGLint configAttrs[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                                  EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                  EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE};
    EGLConfig config = nullptr;
    EGLint count = 0;
    if (!g.egl.ChooseConfig(g.display, configAttrs, &config, 1, &count) || count == 0) {
        Log("no hay configuración EGL para GLES 3 con RGBA8 y profundidad 24");
        return false;
    }
    g.surface = g.egl.CreateWindowSurface(g.display, config, &g.window, nullptr);
    if (!g.surface) {
        char err[16];
        std::snprintf(err, sizeof(err), "0x%04X", g.egl.GetError());
        Log("eglCreateWindowSurface falló (%s)", err);
        return false;
    }
    // Se pide la versión más alta que dé el driver: 3.2, 3.1 y 3.0.
    for (EGLint minorVersion : {2, 1, 0}) {
        const EGLint ctxAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, minorVersion, EGL_NONE};
        g.context = g.egl.CreateContext(g.display, config, nullptr, ctxAttrs);
        if (g.context) break;
    }
    if (!g.context || !g.egl.MakeCurrent(g.display, g.surface, g.surface, g.context)) {
        Log("no pude crear el contexto GLES 3");
        return false;
    }
    // El runtime ya marca el ritmo de 60 Hz: el intercambio no debe esperar al vsync además.
    g.egl.SwapInterval(g.display, 0);

    auto str = [](const unsigned char* s) { return s ? reinterpret_cast<const char*>(s) : "?"; };
    Log("GL_VENDOR: %s", str(g.gl.GetString(GL_VENDOR)));
    Log("GL_RENDERER: %s", str(g.gl.GetString(GL_RENDERER)));
    Log("GL_VERSION: %s", str(g.gl.GetString(GL_VERSION)));
    Log("GLSL: %s", str(g.gl.GetString(GL_SHADING_LANGUAGE_VERSION)));
    struct {
        GLenum id;
        const char* name;
    } limits[] = {{GL_MAX_TEXTURE_SIZE, "max_texture_size"},
                  {GL_MAX_VERTEX_ATTRIBS, "max_vertex_attribs"},
                  {GL_MAX_UNIFORM_BLOCK_SIZE, "max_uniform_block_size"},
                  {GL_MAX_FRAGMENT_UNIFORM_VECTORS, "max_fragment_uniform_vectors"},
                  {GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, "max_combined_texture_units"},
                  {GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, "max_ssbo_bindings (GLES 3.1)"},
                  {GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, "max_vertex_ssbo (GLES 3.1)"}};
    for (const auto& l : limits) {
        GLint v = -1;
        g.gl.GetIntegerv(l.id, &v);
        g.gl.GetError();  // los de 3.1 dan error en un contexto 3.0
        char num[24];
        std::snprintf(num, sizeof(num), "%d", v);
        Log("%s: %s", l.name, num);
    }
    GLint numExt = 0;
    g.gl.GetIntegerv(GL_NUM_EXTENSIONS, &numExt);
    std::string exts;
    for (GLint i = 0; g.gl.GetStringi && i < numExt; ++i) {
        exts += str(g.gl.GetStringi(GL_EXTENSIONS, static_cast<unsigned int>(i)));
        exts += ' ';
    }
    Log("GL extensiones: %s", exts.c_str());

    g.gl.Viewport(0, 0, g.window.width, g.window.height);
    g.windowStart = std::chrono::steady_clock::now();
    Log("listo: la pantalla de Choya está activa");
    return true;
}

} // namespace

// Para el motor de Choya: funciones de GLES del contexto activo y el registro choya-gles.txt.
extern "C" void* choya_gl_proc(const char* name) {
    void* p = g.glesLib ? dlsym(g.glesLib, name) : nullptr;
    if (!p && g.egl.GetProcAddress) p = g.egl.GetProcAddress(name);
    return p;
}

extern "C" void choya_gles_log(const char* line) { Log("%s", line); }

// Se llama una vez por cada cuadro presentado del juego (desde el VI del runtime, en el hilo del juego).
// Hilo de la pantalla: tiene el contexto EGL, dibuja con el motor y presenta. Así el hilo del juego solo copia el
// cuadro (unos 0.1 ms) y no espera al dibujo ni a la GPU.
namespace {
std::mutex g_threadMutex;
std::condition_variable g_threadCv;
uint64_t g_requested = 0;  // cuadros que el juego ya terminó
bool g_threadStarted = false;

std::atomic<int> g_tidPantalla{0};
std::atomic<int> g_tidJuego{0};
std::atomic<uint64_t> g_cuadrosJuego{0};  // choya_gles_frame: un cuadro del juego (para los ms por cuadro de cada hilo)

void RenderThread() {
    pthread_setname_np(pthread_self(), "choya-pantalla");
    g_tidPantalla.store(static_cast<int>(syscall(SYS_gettid)));
    g.ok = Init();
    if (!g.ok) {
        Log("la pantalla de Choya quedó apagada; el juego sigue");
        return;
    }
    uint64_t done = 0;
    for (;;) {
        {
            std::unique_lock lock(g_threadMutex);
            g_threadCv.wait(lock, [&] { return g_requested != done; });
            done = g_requested;  // si el juego se adelantó, se dibuja solo el más reciente
        }
        ++g.frames;
        if (choya_gx_draw_frame(g.window.width, g.window.height)) {
            ++g.motorFrames;
        } else if (g.motorFrames == 0) {
            ++g.patternFrames;  // todavía no hay cuadros del juego: pantalla de prueba
            DrawPattern();
        } else {
            ++g.patternFrames;  // aviso sin cuadro nuevo: se queda el último en pantalla (antes parpadeaba la prueba)
            continue;
        }
        Present();
    }
}
} // namespace

// Los controles del juego son los de Nebula (virtual_remote.cpp, parche de Choya; MENU + Start sale). Aquí solo se
// anota en choya-gles.txt qué mandos ve SDL, y MENU + R1 pide un volcado del cuadro en modo diagnóstico.
namespace {
void CheckGamepads() {
    static uint64_t frames = 0;
    static int lastCount = -1;
    static int dumpHeld = 0;
    ++frames;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    bool dump = false;
    ChoyaMando console{};
    const bool haveConsole = choya_mando_leer(&console);
    if (haveConsole && console.guide && console.rightshoulder) dump = true;
    // MENU sola (sin ningún otro botón): medición por tramos (no la toca el juego: MENU no es ningún botón de la Wii).
    const bool menuAlone = haveConsole && console.guide && !console.south && !console.east && !console.west &&
                           !console.north && !console.back && !console.start && !console.leftshoulder &&
                           !console.rightshoulder && !console.dpup && !console.dpdown && !console.dpleft &&
                           !console.dpright && console.lefttrigger < 0.5f && console.righttrigger < 0.5f;
    for (int i = 0; ids && i < count; ++i) {
        SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i]);
        if (pad && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_GUIDE) &&
            SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) {
            dump = true;
        }
    }
    if (frames == 120 || frames == 1200 || (frames > 120 && count != lastCount)) {
        lastCount = count;
        int njoy = 0;
        SDL_JoystickID* joys = SDL_GetJoysticks(&njoy);
        char line[256];
        std::snprintf(line, sizeof(line), "mandos (cuadro %llu): %d joystick(s), %d como gamepad",
                      static_cast<unsigned long long>(frames), njoy, count);
        Log("%s", line);
        for (int i = 0; joys && i < njoy; ++i) {
            const char* name = SDL_GetJoystickNameForID(joys[i]);
            Log(SDL_IsGamepad(joys[i]) ? "  gamepad: %s" : "  joystick sin mapeo de gamepad: %s", name ? name : "?");
        }
        SDL_free(joys);
    }
    SDL_free(ids);
    // MENU + R1 sostenidos 1 s (de reloj: también con el juego lento): vuelca el cuadro que se ve
    // (cuadro-manual-<n>.bin; solo en modo diagnóstico).
    static std::chrono::steady_clock::time_point dumpSince{};
    const auto now = std::chrono::steady_clock::now();
    if (!dump) {
        dumpHeld = 0;
    } else if (dumpHeld == 0) {
        dumpHeld = 1;
        dumpSince = now;
    } else if (dumpHeld == 1 && now - dumpSince >= std::chrono::seconds(1)) {
        dumpHeld = 2;
        Log("MENU + R1: volcado del cuadro pedido");
        choya_gx_request_dump();
    }
    // MENU sola sostenida 2 s: mide 20 s la escena que se ve (perfil.txt; en la PC: choya perfil perfil.txt).
    static int measureHeld = 0;
    static std::chrono::steady_clock::time_point measureSince{};
    if (!menuAlone) {
        measureHeld = 0;
    } else if (measureHeld == 0) {
        measureHeld = 1;
        measureSince = now;
    } else if (measureHeld == 1 && now - measureSince >= std::chrono::seconds(2)) {
        measureHeld = 2;
        if (choya_medir_empezar(20)) Log("MENU sola: midiendo los tramos del cuadro 20 s (perfil.txt)");
        else Log("MENU sola: ya se está midiendo (en el modo diagnóstico se mide solo de los 20 a los 55 s)");
    }
}
} // namespace

// --- Perfil de hilos y núcleos (0.23.4) ---------------------------------------------------------------------------
// Cada 10 s (CHOYA_HILOS_CADA=segundos; CHOYA_HILOS=0 lo apaga): qué tan ocupado estuvo cada núcleo (/proc/stat) y
// cada hilo del proceso (/proc/self/task/*/stat y schedstat): CPU usada, núcleo donde corrió y cuánto esperó turno
// (listo para correr pero sin núcleo libre). El juego, el hilo GX y la pantalla van con su nombre; los demás (Mali,
// audio, aurora) con el que les puso su programa.
extern "C" __attribute__((weak)) int choya_gx_hilo_tid_gx();  // hilo GX de Choya (aurora)
namespace {
struct HiloAntes {
    uint64_t ticks = 0, runNs = 0, waitNs = 0;
};

bool LeerArchivo(const char* path, char* buf, size_t size) {
    const int fd = ::open(path, O_RDONLY);
    if (fd < 0) return false;
    const ssize_t n = ::read(fd, buf, size - 1);
    ::close(fd);
    if (n <= 0) return false;
    buf[n] = 0;
    return true;
}

// /proc/stat: tiempo ocupado y total de cada núcleo (en ticks).
std::vector<std::pair<uint64_t, uint64_t>> LeerNucleos() {
    std::vector<std::pair<uint64_t, uint64_t>> out;
    FILE* f = std::fopen("/proc/stat", "r");
    if (!f) return out;
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "cpu", 3) != 0 || line[3] < '0' || line[3] > '9') continue;
        unsigned long long v[10] = {};
        int idx = 0;
        if (std::sscanf(line, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu", &idx, &v[0], &v[1], &v[2], &v[3], &v[4],
                        &v[5], &v[6], &v[7]) < 5) {
            continue;
        }
        const uint64_t idle = v[3] + v[4];
        uint64_t total = 0;
        for (int i = 0; i < 8; ++i) total += v[i];
        if (idx >= static_cast<int>(out.size())) out.resize(idx + 1);
        out[idx] = {total - idle, total};
    }
    std::fclose(f);
    return out;
}

void PerfilHilos() {
    pthread_setname_np(pthread_self(), "choya-hilos");
    const char* off = std::getenv("CHOYA_HILOS");
    if (off && std::strcmp(off, "0") == 0) return;
    const char* cada = std::getenv("CHOYA_HILOS_CADA");
    const int segundos = cada ? std::max(1, std::atoi(cada)) : 10;
    const double hz = static_cast<double>(sysconf(_SC_CLK_TCK));
    const int yo = static_cast<int>(syscall(SYS_gettid));
    std::map<int, HiloAntes> antes;
    auto nucleosAntes = LeerNucleos();
    auto t0 = std::chrono::steady_clock::now();
    uint64_t cuadrosAntes = g_cuadrosJuego.load();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(segundos));
        const auto t1 = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(t1 - t0).count();
        t0 = t1;
        const uint64_t cuadros = g_cuadrosJuego.load();
        const double cuadrosPorSeg = static_cast<double>(cuadros - cuadrosAntes) / dt;
        cuadrosAntes = cuadros;
        // Núcleos.
        const auto nucleos = LeerNucleos();
        char cab[96];
        std::snprintf(cab, sizeof(cab), "hilos (%d s, juego a %.1f cuadros/s): núcleos ocupados", segundos, cuadrosPorSeg);
        std::string linea = cab;
        for (size_t i = 0; i < nucleos.size(); ++i) {
            if (i >= nucleosAntes.size()) break;
            const double ocup = static_cast<double>(nucleos[i].first - nucleosAntes[i].first);
            const double tot = static_cast<double>(nucleos[i].second - nucleosAntes[i].second);
            char b[32];
            std::snprintf(b, sizeof(b), "%s %zu: %.0f %%", i ? "," : "", i, tot > 0 ? 100.0 * ocup / tot : 0.0);
            linea += b;
        }
        nucleosAntes = nucleos;
        Log("%s", linea.c_str());
        // Hilos.
        struct Hilo {
            int tid;
            std::string nombre;
            double cpu, espera;
            int nucleo;
        };
        std::vector<Hilo> hilos;
        std::map<int, HiloAntes> ahora;
        if (DIR* d = ::opendir("/proc/self/task")) {
            while (const dirent* e = ::readdir(d)) {
                const int tid = std::atoi(e->d_name);
                if (tid <= 0) continue;
                char path[64], buf[1024];
                std::snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
                if (!LeerArchivo(path, buf, sizeof(buf))) continue;
                const char* abre = std::strchr(buf, '(');
                const char* cierra = std::strrchr(buf, ')');
                if (!abre || !cierra || cierra < abre) continue;
                std::string nombre(abre + 1, cierra);
                // Campos después de ")": 3 estado ... 14 utime, 15 stime ... 39 núcleo.
                std::vector<unsigned long long> campos;
                const char* c = cierra + 2;
                while (*c) {
                    while (*c == ' ') ++c;
                    if (!*c) break;
                    campos.push_back(std::strtoull(c, nullptr, 10));
                    while (*c && *c != ' ') ++c;
                }
                if (campos.size() < 37) continue;
                HiloAntes h;
                h.ticks = campos[11] + campos[12];
                const int nucleo = static_cast<int>(campos[36]);
                std::snprintf(path, sizeof(path), "/proc/self/task/%d/schedstat", tid);
                if (LeerArchivo(path, buf, sizeof(buf))) {
                    unsigned long long run = 0, wait = 0;
                    if (std::sscanf(buf, "%llu %llu", &run, &wait) == 2) {
                        h.runNs = run;
                        h.waitNs = wait;
                    }
                }
                ahora[tid] = h;
                const auto a = antes.find(tid);
                if (a == antes.end()) continue;
                const double cpu = 100.0 * static_cast<double>(h.ticks - a->second.ticks) / hz / dt;
                const double espera = 100.0 * static_cast<double>(h.waitNs - a->second.waitNs) / 1e9 / dt;
                hilos.push_back({tid, nombre, cpu, espera, nucleo});
            }
            ::closedir(d);
        }
        antes.swap(ahora);
        if (hilos.empty()) continue;
        const int tidJuego = g_tidJuego.load();
        const int tidGx = choya_gx_hilo_tid_gx ? choya_gx_hilo_tid_gx() : 0;
        const int tidPantalla = g_tidPantalla.load();
        // CPU en ms por cuadro del juego (como un perfil por cuadro): % / 100 * 1000 / cuadros por segundo.
        const double msPorCuadro = cuadrosPorSeg > 0.1 ? 10.0 / cuadrosPorSeg : 0.0;
        auto describir = [msPorCuadro](const Hilo& h, const char* etiqueta) {
            char b[192];
            if (msPorCuadro > 0) {
                std::snprintf(b, sizeof(b), "%s %.0f %% CPU (%.1f ms por cuadro) en el núcleo %d, esperó turno %.0f %%",
                              etiqueta, h.cpu, h.cpu * msPorCuadro, h.nucleo, h.espera);
            } else {
                std::snprintf(b, sizeof(b), "%s %.0f %% CPU en el núcleo %d, esperó turno %.0f %%", etiqueta, h.cpu,
                              h.nucleo, h.espera);
            }
            return std::string(b);
        };
        std::string principales = "hilos: ";
        bool primero = true;
        for (const auto& [tid, etiqueta] : {std::pair<int, const char*>{tidJuego, "juego"},
                                            {tidGx, "hilo GX"},
                                            {tidPantalla, "pantalla"}}) {
            if (tid <= 0) continue;
            for (const Hilo& h : hilos) {
                if (h.tid != tid) continue;
                principales += (primero ? "" : "; ") + describir(h, etiqueta);
                primero = false;
            }
        }
        if (!primero) Log("%s", principales.c_str());
        std::sort(hilos.begin(), hilos.end(), [](const Hilo& a, const Hilo& b) { return a.cpu > b.cpu; });
        std::string otros = "hilos: otros:";
        int n = 0;
        double resto = 0;
        for (const Hilo& h : hilos) {
            if (h.tid == tidJuego || h.tid == tidGx || h.tid == tidPantalla || h.tid == yo) continue;
            if (n < 6 && h.cpu >= 0.5) {
                char b[128];
                std::snprintf(b, sizeof(b), "%s %s %.0f %% (núcleo %d, esperó %.0f %%)", n ? "," : "", h.nombre.c_str(),
                              h.cpu, h.nucleo, h.espera);
                otros += b;
                ++n;
            } else {
                resto += h.cpu;
            }
        }
        char b[64];
        std::snprintf(b, sizeof(b), "%s el resto %.0f %%", n ? ";" : "", resto);
        otros += b;
        Log("%s", otros.c_str());
    }
}
} // namespace

// El motor pide dibujar sin un cuadro mostrado (pantalla de carga, cola llena): mismo aviso que un cuadro del juego.
// Si el lote no se muestra, choya_gx_draw_frame devuelve false y no se presenta nada (no parpadea).
extern "C" void choya_gles_wake() {
    if (!g_threadStarted) return;
    {
        std::lock_guard lock(g_threadMutex);
        ++g_requested;
    }
    g_threadCv.notify_one();
}

extern "C" void choya_gles_frame() {
    g_cuadrosJuego.fetch_add(1, std::memory_order_relaxed);
    CheckGamepads();
    if (!g.tried) {
        g.tried = true;
        g_tidJuego.store(static_cast<int>(syscall(SYS_gettid)));  // lo llama el hilo del juego (VI)
        std::thread(PerfilHilos).detach();
        const char* on = std::getenv("CHOYA_GLES");
        if (!on || std::strcmp(on, "1") != 0) return;
        g_threadStarted = true;
        std::thread(RenderThread).detach();
    }
    if (!g_threadStarted) return;
    {
        std::lock_guard lock(g_threadMutex);
        ++g_requested;
    }
    g_threadCv.notify_one();
}

namespace {
void DrawPattern() {
    // Prueba visible: el color de fondo cambia poco a poco y una barra blanca recorre la pantalla.
    const float t = static_cast<float>(g.frames % 360) / 360.0f;
    const float r = t < 0.5f ? t * 2 : (1 - t) * 2;
    g.gl.Disable(GL_SCISSOR_TEST);
    g.gl.ClearColor(0.10f + 0.5f * r, 0.15f, 0.35f + 0.4f * (1 - r), 1.0f);
    g.gl.Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    const GLint barX = static_cast<GLint>((g.frames * 4) % g.window.width);
    g.gl.Enable(GL_SCISSOR_TEST);
    g.gl.Scissor(barX, 0, 24, g.window.height);
    g.gl.ClearColor(1, 1, 1, 1);
    g.gl.Clear(GL_COLOR_BUFFER_BIT);
    g.gl.Disable(GL_SCISSOR_TEST);
}

void Present() {
    const auto swapStart = std::chrono::steady_clock::now();
    g.egl.SwapBuffers(g.display, g.surface);
    const double swapMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - swapStart).count();
    g.swapMsTotal += swapMs;
    if (swapMs > g.swapMsWorst) g.swapMsWorst = swapMs;
    ++g.swaps;

    // Cada 10 s: cuadros que de verdad se mostraron. Antes la ventana iba por g.frames, que cuenta también las vueltas
    // sin cuadro nuevo, y el informe salía con la mitad o menos de lo que se veía.
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - g.windowStart).count();
    if (secs >= 10.0) {
        char line[192];
        std::snprintf(line, sizeof(line),
                      "%.1f cuadros/s en pantalla, intercambio promedio %.2f ms, peor %.2f ms; del motor %llu, sin cuadro "
                      "nuevo %llu",
                      static_cast<double>(g.swaps) / secs, g.swapMsTotal / static_cast<double>(g.swaps), g.swapMsWorst,
                      static_cast<unsigned long long>(g.motorFrames), static_cast<unsigned long long>(g.patternFrames));
        Log("presentación: %s", line);
        g.swapMsTotal = g.swapMsWorst = 0;
        g.swaps = 0;
        g.windowStart = std::chrono::steady_clock::now();
    }
}
} // namespace
#endif
