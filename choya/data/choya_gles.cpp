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

extern "C" bool choya_gx_draw_frame(int screenWidth, int screenHeight);  // motor de Choya (aurora)

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
extern "C" void choya_gles_frame() {
    if (!g.tried) {
        g.tried = true;
        const char* on = std::getenv("CHOYA_GLES");
        if (!on || std::strcmp(on, "1") != 0) return;
        g.ok = Init();
        if (!g.ok) Log("la pantalla de Choya quedó apagada; el juego sigue");
    }
    if (!g.ok) return;

    ++g.frames;
    if (choya_gx_draw_frame(g.window.width, g.window.height)) {
        ++g.motorFrames;
    } else {
        ++g.patternFrames;
        DrawPattern();
    }
    Present();
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

    if (g.frames % 300 == 0) {
        const double secs =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - g.windowStart).count();
        char line[160];
        std::snprintf(line, sizeof(line),
                      "%.1f cuadros/s, intercambio promedio %.2f ms, peor %.2f ms; del motor %llu, de prueba %llu",
                      300.0 / secs, g.swapMsTotal / 300.0, g.swapMsWorst,
                      static_cast<unsigned long long>(g.motorFrames), static_cast<unsigned long long>(g.patternFrames));
        Log("presentación: %s", line);
        g.swapMsTotal = g.swapMsWorst = 0;
        g.windowStart = std::chrono::steady_clock::now();
    }
}
} // namespace
#endif
