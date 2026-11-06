// wl_view: visor de texturas del paquete en la RG35XX H (SDL2 + OpenGL ES).
// Sirve para la puerta 2 del plan: comprobar que el paquete carga y se ve bien en la consola.
//   Izquierda/Derecha (cruceta): textura anterior/siguiente   L1/R1: saltar 10
//   A: ajustar a pantalla / tamaño real    Start o Escape: salir
#include <SDL2/SDL.h>
#include <GLES3/gl3.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "wl/audio.h"
#include "wl/pack.h"
#include "wl/texcache.h"

namespace {

const char* VS3 = R"(#version 300 es
layout(location = 0) in vec2 pos;
layout(location = 1) in vec2 uv;
out vec2 v_uv;
void main() { v_uv = uv; gl_Position = vec4(pos, 0.0, 1.0); }
)";
const char* FS3 = R"(#version 300 es
precision mediump float;
in vec2 v_uv;
uniform sampler2D tex;
out vec4 color;
void main() {
  vec2 c = floor(gl_FragCoord.xy / 16.0);
  vec3 bg = mix(vec3(0.25), vec3(0.4), mod(c.x + c.y, 2.0));
  vec4 t = texture(tex, v_uv);
  color = vec4(mix(bg, t.rgb, t.a), 1.0);
}
)";
const char* VS2 = R"(attribute vec2 pos; attribute vec2 uv; varying vec2 v_uv;
void main() { v_uv = uv; gl_Position = vec4(pos, 0.0, 1.0); }
)";
const char* FS2 = R"(precision mediump float; varying vec2 v_uv; uniform sampler2D tex;
void main() {
  vec2 c = floor(gl_FragCoord.xy / 16.0);
  vec3 bg = mix(vec3(0.25), vec3(0.4), mod(c.x + c.y, 2.0));
  vec4 t = texture2D(tex, v_uv);
  gl_FragColor = vec4(mix(bg, t.rgb, t.a), 1.0);
}
)";

GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        std::fprintf(stderr, "shader: %s\n", log);
    }
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : std::getenv("WL_PACK");
    if (!path) {
        std::fprintf(stderr, "uso: wl_view assets.wlpk\n");
        return 2;
    }
    wl::Pack pack;
    if (!pack.open(path)) {
        std::fprintf(stderr, "error: %s\n", pack.error().c_str());
        return 1;
    }
    std::vector<uint32_t> textures;
    for (uint32_t i = 0; i < pack.size(); ++i)
        if (pack.entry(i).kind == uint8_t(wl::Kind::Texture)) textures.push_back(i);
    if (textures.empty()) {
        std::fprintf(stderr, "el paquete no tiene texturas\n");
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    bool gles3 = (pack.flags() & wl::PACK_FLAG_GLES3) != 0;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, gles3 ? 3 : 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_Window* win = SDL_CreateWindow("wl_view", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 640, 480,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP);
    SDL_GLContext ctx = win ? SDL_GL_CreateContext(win) : nullptr;
    if (!ctx && gles3) {
        std::fprintf(stderr, "sin GLES 3: el paquete usa ETC2, conviértelo con el perfil gles2\n");
        return 1;
    }
    if (!ctx) {
        std::fprintf(stderr, "GL: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetSwapInterval(1);
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
        if (SDL_IsGameController(i)) SDL_GameControllerOpen(i);

    int sw = 640, sh = 480;
    SDL_GL_GetDrawableSize(win, &sw, &sh);
    std::printf("GL_RENDERER: %s\nGL_VERSION: %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

    GLuint prog = glCreateProgram();
    glAttachShader(prog, compile(GL_VERTEX_SHADER, gles3 ? VS3 : VS2));
    glAttachShader(prog, compile(GL_FRAGMENT_SHADER, gles3 ? FS3 : FS2));
    glBindAttribLocation(prog, 0, "pos");
    glBindAttribLocation(prog, 1, "uv");
    glLinkProgram(prog);
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "tex"), 0);

    // Cargar todas las texturas: así se mide el tiempo de subida y la memoria real.
    std::vector<GLuint> ids(textures.size(), 0);
    uint64_t bytes = 0;
    int failed = 0;
    const uint32_t t0 = SDL_GetTicks();
    for (size_t i = 0; i < textures.size(); ++i) {
        const wl::PackEntry& e = pack.entry(textures[i]);
        ids[i] = wl::gl_upload(e, pack.data(e), gles3);
        if (!ids[i]) ++failed;
        bytes += e.data_size;
    }
    std::printf("%zu texturas subidas en %u ms (%.2f MiB), %d con error\n", textures.size(), SDL_GetTicks() - t0,
                bytes / 1048576.0, failed);

    size_t cur = 0;
    bool fit = true, running = true, dirty = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            int step = 0;
            if (ev.type == SDL_QUIT) running = false;
            if (ev.type == SDL_KEYDOWN) {
                switch (ev.key.keysym.sym) {
                    case SDLK_ESCAPE: running = false; break;
                    case SDLK_RIGHT: step = 1; break;
                    case SDLK_LEFT: step = -1; break;
                    case SDLK_PAGEDOWN: step = 10; break;
                    case SDLK_PAGEUP: step = -10; break;
                    case SDLK_SPACE: fit = !fit; dirty = true; break;
                    default: break;
                }
            }
            if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
                switch (ev.cbutton.button) {
                    case SDL_CONTROLLER_BUTTON_START: running = false; break;
                    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: step = 1; break;
                    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: step = -1; break;
                    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: step = 10; break;
                    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: step = -10; break;
                    case SDL_CONTROLLER_BUTTON_A: fit = !fit; dirty = true; break;
                    default: break;
                }
            }
            if (step) {
                const long n = long(textures.size());
                cur = size_t(((long(cur) + step) % n + n) % n);
                dirty = true;
            }
        }
        if (!dirty) {
            SDL_Delay(16);
            continue;
        }
        dirty = false;
        const wl::PackEntry& e = pack.entry(textures[cur]);
        float w = e.width, h = e.height;
        float scale = fit ? std::min(sw / w, sh / h) : 1.0f;
        float qx = w * scale / sw, qy = h * scale / sh;
        const float quad[] = {-qx, -qy, 0, 1, qx, -qy, 1, 1, -qx, qy, 0, 0, qx, qy, 1, 0};
        glViewport(0, 0, sw, sh);
        glClearColor(0.1f, 0.1f, 0.12f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, ids[cur]);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, quad);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, quad + 2);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        SDL_GL_SwapWindow(win);
        std::printf("[%zu/%zu] %s  %s %ux%u m%u\n", cur + 1, textures.size(), pack.name(e),
                    wl::out_format_name(wl::OutFormat(e.format)), e.width, e.height, e.count);
        std::fflush(stdout);
    }
    for (GLuint id : ids)
        if (id) wl::gl_delete(id);
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
