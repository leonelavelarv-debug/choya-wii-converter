// choya_render: dibuja modelos del paquete a PNG sin pantalla (EGL + OpenGL ES 3, p. ej. Mesa en WSL2).
//   choya_render assets.wlpk carpeta [texto] [--max N] [--size PIXELES] [--simple]
// Usa el mismo render que choya_model, así que lo que salga aquí es lo que verás en la consola.
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <zlib.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "wl/camera.h"
#include "wl/model_render.h"
#include "wl/pack.h"

namespace {

void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int s = 24; s >= 0; s -= 8) v.push_back(uint8_t(x >> s));
}

void chunk(FILE* f, const char* type, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> buf;
    put32(buf, uint32_t(data.size()));
    buf.insert(buf.end(), type, type + 4);
    buf.insert(buf.end(), data.begin(), data.end());
    put32(buf, uint32_t(crc32(0L, buf.data() + 4, uInt(buf.size() - 4))));
    std::fwrite(buf.data(), 1, buf.size(), f);
}

bool write_png(const std::string& path, int w, int h, const std::vector<uint8_t>& rgba) {
    std::vector<uint8_t> raw;
    raw.reserve(size_t(h) * (w * 4 + 1));
    for (int y = h - 1; y >= 0; --y) {  // GL lee de abajo hacia arriba
        raw.push_back(0);
        raw.insert(raw.end(), rgba.begin() + size_t(y) * w * 4, rgba.begin() + size_t(y + 1) * w * 4);
    }
    uLongf zlen = compressBound(uLong(raw.size()));
    std::vector<uint8_t> z(zlen);
    if (compress2(z.data(), &zlen, raw.data(), uLong(raw.size()), 6) != Z_OK) return false;
    z.resize(zlen);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ihdr;
    put32(ihdr, uint32_t(w));
    put32(ihdr, uint32_t(h));
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    chunk(f, "IHDR", ihdr);
    chunk(f, "IDAT", z);
    chunk(f, "IEND", {});
    std::fclose(f);
    return true;
}

std::string safe(const char* s) {
    std::string o;
    for (; *s; ++s) o += (std::isalnum(static_cast<unsigned char>(*s)) || *s == '.' || *s == '-') ? *s : '_';
    return o.size() > 100 ? o.substr(o.size() - 100) : o;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "uso: choya_render assets.wlpk carpeta [texto] [--max N] [--size P] [--simple]\n");
        return 2;
    }
    const char* filter = nullptr;
    int max_models = 20, size = 512;
    bool simple = false;
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--max") && i + 1 < argc) max_models = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--size") && i + 1 < argc) size = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--simple")) simple = true;
        else filter = argv[i];
    }
    wl::Pack pack;
    if (!pack.open(argv[1])) {
        std::fprintf(stderr, "error: %s\n", pack.error().c_str());
        return 1;
    }

    // Contexto GLES 3 sin pantalla
    EGLDisplay dpy = EGL_NO_DISPLAY;
    auto get_platform = reinterpret_cast<EGLDisplay (*)(EGLenum, void*, const EGLint*)>(
        eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (get_platform) dpy = get_platform(0x31DD /*EGL_PLATFORM_SURFACELESS_MESA*/, nullptr, nullptr);
    if (dpy == EGL_NO_DISPLAY) dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, nullptr, nullptr)) {
        std::fprintf(stderr, "EGL no disponible\n");
        return 1;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint cfg_attr[] = {EGL_RENDERABLE_TYPE, 0x0040 /*EGL_OPENGL_ES3_BIT*/, EGL_NONE};
    EGLConfig cfg = nullptr;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n == 0) cfg = nullptr;  // sin config (EGL_KHR_no_config_context)
    const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        std::fprintf(stderr, "no se pudo crear el contexto GLES 3\n");
        return 1;
    }
    std::printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));
    GLuint fb, rb[2];
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glGenRenderbuffers(2, rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb[0]);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, size, size);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb[0]);
    glBindRenderbuffer(GL_RENDERBUFFER, rb[1]);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, size, size);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb[1]);

    int done = 0;
    {
        wl::ModelRenderer renderer(pack);
        if (!renderer.init()) return 1;
        wl::RenderOptions opt;
        opt.tev = !simple;
        std::vector<uint8_t> px(size_t(size) * size * 4);
        for (uint32_t i = 0; i < pack.size() && done < max_models; ++i) {
            const wl::PackEntry& e = pack.entry(i);
            if (e.kind != wl::KIND_MODEL || (filter && !std::strstr(pack.name(e), filter))) continue;
            const wl::LoadInfo li = renderer.load(i);
            if (!li.ok) {
                std::printf("DAÑADO %s\n", li.name.c_str());
                continue;
            }
            glViewport(0, 0, size, size);
            glClearColor(0.18f, 0.22f, 0.28f, 1);
            glDepthMask(GL_TRUE);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            float view[16], proj[16];
            wl::orbit_matrices(renderer.view(), wl::Orbit{}, 1.0f, view, proj);
            renderer.draw(view, proj, opt);
            glReadPixels(0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            const std::string out = std::string(argv[2]) + "/" + safe(li.name.c_str()) + ".png";
            write_png(out, size, size, px);
            std::printf("%s  %u tri, %u materiales%s\n", out.c_str(), li.triangles, li.materials,
                        li.missing_textures ? "  (texturas faltantes)" : "");
            ++done;
        }
        if (renderer.tev_failures()) std::printf("[tev] %d shaders con error\n", renderer.tev_failures());
    }
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx);
    eglTerminate(dpy);
    std::printf("%d imágenes\n", done);
    return done ? 0 : 1;
}
