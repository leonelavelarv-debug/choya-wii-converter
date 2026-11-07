// choya_model: visor de modelos 3D del paquete en la RG35XX H (SDL2 + OpenGL ES 3).
// Puerta 1 del plan: comprobar que la geometría, las UV y las texturas de Wii Sports
// se ven bien y medir los FPS. El sombreado es simplificado (sin TEV todavía).
//
//   Cruceta / stick izquierdo: girar      A / B: acercar / alejar
//   L1 / R1: modelo anterior / siguiente  X: cull on/off   Y: texturas on/off
//   Select: iluminación on/off            Start (o Escape): salir
#include <SDL2/SDL.h>
#include <GLES3/gl3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "wl/model.h"
#include "wl/pack.h"
#include "wl/texcache.h"

namespace {

const char* VS = R"(#version 300 es
layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 nrm;
layout(location = 2) in vec4 col;
layout(location = 3) in vec2 uv;
uniform mat4 mvp;
out vec3 v_nrm;
out vec4 v_col;
out vec2 v_uv;
void main() { v_nrm = nrm; v_col = col; v_uv = uv; gl_Position = mvp * vec4(pos, 1.0); }
)";

const char* FS = R"(#version 300 es
precision mediump float;
in vec3 v_nrm;
in vec4 v_col;
in vec2 v_uv;
uniform sampler2D tex;
uniform bool use_tex;
uniform bool use_vcol;
uniform bool lit;
uniform bool alpha_test;
uniform vec3 light;
out vec4 color;
void main() {
  vec4 c = use_tex ? texture(tex, v_uv) : vec4(0.8, 0.8, 0.8, 1.0);
  if (use_vcol) c *= v_col;
  if (lit) {
    float l = length(v_nrm) > 0.0 ? max(dot(normalize(v_nrm), light), 0.0) : 1.0;
    c.rgb *= 0.55 + 0.45 * l;
  }
  if (alpha_test && c.a < 0.5) discard;
  color = c;
}
)";

struct Mat4 {
    float m[16];
};

Mat4 mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int rr = 0; rr < 4; ++rr) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[k * 4 + rr] * b.m[c * 4 + k];
            r.m[c * 4 + rr] = s;
        }
    return r;
}

Mat4 perspective(float fovy, float aspect, float zn, float zf) {
    const float f = 1.0f / std::tan(fovy / 2);
    Mat4 r{};
    r.m[0] = f / aspect;
    r.m[5] = f;
    r.m[10] = (zf + zn) / (zn - zf);
    r.m[11] = -1;
    r.m[14] = 2 * zf * zn / (zn - zf);
    return r;
}

Mat4 look_at(const float eye[3], const float at[3]) {
    float f[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
    float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (float& v : f) v /= fl;
    const float up[3] = {0, 1, 0};
    float s[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0]};
    float sl = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    if (sl < 1e-6f) sl = 1;
    for (float& v : s) v /= sl;
    float u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};
    Mat4 r{};
    r.m[0] = s[0]; r.m[4] = s[1]; r.m[8] = s[2];
    r.m[1] = u[0]; r.m[5] = u[1]; r.m[9] = u[2];
    r.m[2] = -f[0]; r.m[6] = -f[1]; r.m[10] = -f[2];
    r.m[12] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
    r.m[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
    r.m[14] = f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2];
    r.m[15] = 1;
    return r;
}

GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        std::fprintf(stderr, "shader: %s\n", log);
    }
    return s;
}

GLint wrap_mode(uint8_t w) { return w == 0 ? GL_CLAMP_TO_EDGE : (w == 2 ? GL_MIRRORED_REPEAT : GL_REPEAT); }

struct GpuModel {
    GLuint vao = 0, vbo = 0, ibo = 0;
    wl::ModelView view;
    std::string name;
};

}  // namespace

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : std::getenv("WL_PACK");
    if (!path) {
        std::fprintf(stderr, "uso: choya_model assets.wlpk [parte_del_nombre]\n");
        return 2;
    }
    wl::Pack pack;
    if (!pack.open(path)) {
        std::fprintf(stderr, "error: %s\n", pack.error().c_str());
        return 1;
    }
    std::vector<uint32_t> models;
    for (uint32_t i = 0; i < pack.size(); ++i)
        if (pack.entry(i).kind == wl::KIND_MODEL) models.push_back(i);
    std::sort(models.begin(), models.end(), [&](uint32_t a, uint32_t b) {
        return std::strcmp(pack.name(pack.entry(a)), pack.name(pack.entry(b))) < 0;
    });
    if (models.empty()) {
        std::fprintf(stderr, "el paquete no tiene modelos: conviértelo con Choya Wii Converter 0.2 o más nuevo\n");
        return 1;
    }
    size_t cur = 0;
    if (argc > 2)  // empezar en el primer modelo cuyo nombre contenga el texto
        for (size_t i = 0; i < models.size(); ++i)
            if (std::strstr(pack.name(pack.entry(models[i])), argv[2])) {
                cur = i;
                break;
            }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_Window* win = SDL_CreateWindow("choya_model", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 640, 480,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP);
    SDL_GLContext ctx = win ? SDL_GL_CreateContext(win) : nullptr;
    if (!ctx) {
        std::fprintf(stderr, "GL: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetSwapInterval(1);
    std::vector<SDL_GameController*> pads;
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
        if (SDL_IsGameController(i))
            if (SDL_GameController* c = SDL_GameControllerOpen(i)) pads.push_back(c);
    int sw = 640, sh = 480;
    SDL_GL_GetDrawableSize(win, &sw, &sh);
    std::printf("GL_RENDERER: %s\nGL_VERSION: %s\n%zu modelos en el paquete\n", glGetString(GL_RENDERER),
                glGetString(GL_VERSION), models.size());

    GLuint prog = glCreateProgram();
    glAttachShader(prog, compile(GL_VERTEX_SHADER, VS));
    glAttachShader(prog, compile(GL_FRAGMENT_SHADER, FS));
    glLinkProgram(prog);
    glUseProgram(prog);
    const GLint u_mvp = glGetUniformLocation(prog, "mvp");
    const GLint u_tex = glGetUniformLocation(prog, "tex");
    const GLint u_use_tex = glGetUniformLocation(prog, "use_tex");
    const GLint u_vcol = glGetUniformLocation(prog, "use_vcol");
    const GLint u_lit = glGetUniformLocation(prog, "lit");
    const GLint u_atest = glGetUniformLocation(prog, "alpha_test");
    const GLint u_light = glGetUniformLocation(prog, "light");
    glUniform1i(u_tex, 0);
    glUniform3f(u_light, 0.40f, 0.80f, 0.45f);

    std::unordered_map<uint64_t, GLuint> textures;
    auto texture = [&](uint64_t key) -> GLuint {
        if (!key) return 0;
        auto it = textures.find(key);
        if (it != textures.end()) return it->second;
        const wl::PackEntry* e = pack.find(key);
        GLuint id = (e && e->kind == uint8_t(wl::Kind::Texture)) ? wl::gl_upload(*e, pack.data(*e), true) : 0;
        textures[key] = id;
        return id;
    };

    GpuModel gm;
    float yaw = 0.6f, pitch = 0.35f, zoom = 1.0f;
    bool cull = true, use_textures = true, lighting = true;
    auto load = [&](size_t i) {
        if (gm.vao) {
            glDeleteVertexArrays(1, &gm.vao);
            glDeleteBuffers(1, &gm.vbo);
            glDeleteBuffers(1, &gm.ibo);
            gm = GpuModel{};
        }
        const wl::PackEntry& e = pack.entry(models[i]);
        gm.name = pack.name(e);
        if (!wl::model_view(pack, e, gm.view)) {
            std::printf("[%zu/%zu] %s  DAÑADO\n", i + 1, models.size(), gm.name.c_str());
            return;
        }
        glGenVertexArrays(1, &gm.vao);
        glBindVertexArray(gm.vao);
        glGenBuffers(1, &gm.vbo);
        glBindBuffer(GL_ARRAY_BUFFER, gm.vbo);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(gm.view.vertex_count * sizeof(wl::ModelVertex)), gm.view.vertices,
                     GL_STATIC_DRAW);
        glGenBuffers(1, &gm.ibo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gm.ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(gm.view.index_count * 4), gm.view.indices, GL_STATIC_DRAW);
        const GLsizei st = sizeof(wl::ModelVertex);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(0));
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(12));
        glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, st, reinterpret_cast<void*>(24));
        glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(28));
        for (GLuint a = 0; a < 4; ++a) glEnableVertexAttribArray(a);
        int missing = 0;
        for (uint32_t s = 0; s < gm.view.submesh_count; ++s)
            if (gm.view.submeshes[s].texture_key && !texture(gm.view.submeshes[s].texture_key)) ++missing;
        std::printf("[%zu/%zu] %s  %u vértices, %u triángulos, %u submallas%s\n", i + 1, models.size(),
                    gm.name.c_str(), gm.view.vertex_count, gm.view.index_count / 3, gm.view.submesh_count,
                    missing ? "  (texturas faltantes)" : "");
        std::fflush(stdout);
        yaw = 0.6f;
        pitch = 0.35f;
        zoom = 1.0f;
    };
    load(cur);

    glEnable(GL_DEPTH_TEST);
    glFrontFace(GL_CW);  // GX usa sentido horario como cara frontal
    uint32_t last = SDL_GetTicks(), fps_t0 = last, frames = 0;
    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            int step = 0;
            if (ev.type == SDL_QUIT) running = false;
            if (ev.type == SDL_KEYDOWN && (pads.empty() || ev.key.keysym.sym == SDLK_ESCAPE)) {
                switch (ev.key.keysym.sym) {
                    case SDLK_ESCAPE: running = false; break;
                    case SDLK_PAGEDOWN: step = 1; break;
                    case SDLK_PAGEUP: step = -1; break;
                    case SDLK_c: cull = !cull; break;
                    case SDLK_t: use_textures = !use_textures; break;
                    case SDLK_l: lighting = !lighting; break;
                    default: break;
                }
            }
            if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
                switch (ev.cbutton.button) {
                    case SDL_CONTROLLER_BUTTON_START: running = false; break;
                    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: step = 1; break;
                    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: step = -1; break;
                    case SDL_CONTROLLER_BUTTON_X: cull = !cull; std::printf("cull %s\n", cull ? "on" : "off"); break;
                    case SDL_CONTROLLER_BUTTON_Y: use_textures = !use_textures; break;
                    case SDL_CONTROLLER_BUTTON_BACK: lighting = !lighting; break;
                    default: break;
                }
            }
            if (step) {
                const long n = long(models.size());
                cur = size_t(((long(cur) + step) % n + n) % n);
                load(cur);
            }
        }
        const uint32_t now = SDL_GetTicks();
        const float dt = std::min(0.1f, (now - last) / 1000.0f);
        last = now;

        // Entrada continua: cruceta, stick izquierdo, A/B (y flechas/+/- sin mando)
        float rx = 0, ry = 0, zz = 0;
        const Uint8* keys = SDL_GetKeyboardState(nullptr);
        if (pads.empty()) {
            rx += keys[SDL_SCANCODE_RIGHT] - keys[SDL_SCANCODE_LEFT];
            ry += keys[SDL_SCANCODE_UP] - keys[SDL_SCANCODE_DOWN];
            zz += keys[SDL_SCANCODE_EQUALS] - keys[SDL_SCANCODE_MINUS];
        }
        for (SDL_GameController* c : pads) {
            rx += SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) -
                  SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
            ry += SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP) -
                  SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
            zz += SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A) -
                  SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_B);
            const float ax = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0f;
            const float ay = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0f;
            if (std::fabs(ax) > 0.25f) rx += ax;
            if (std::fabs(ay) > 0.25f) ry -= ay;
        }
        yaw += rx * 1.8f * dt;
        pitch = std::clamp(pitch + ry * 1.2f * dt, -1.45f, 1.45f);
        zoom = std::clamp(zoom * std::pow(0.4f, zz * dt), 0.05f, 20.0f);

        glViewport(0, 0, sw, sh);
        glClearColor(0.18f, 0.22f, 0.28f, 1);
        glDepthMask(GL_TRUE);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (gm.vao) {
            const wl::ModelView& v = gm.view;
            float c[3], ext = 0;
            for (int k = 0; k < 3; ++k) {
                c[k] = (v.bbox_min[k] + v.bbox_max[k]) / 2;
                ext = std::max(ext, v.bbox_max[k] - v.bbox_min[k]);
            }
            const float radius = std::max(ext * 0.5f, 0.01f);
            const float dist = radius * 2.6f * zoom;
            const float eye[3] = {c[0] + dist * std::cos(pitch) * std::sin(yaw), c[1] + dist * std::sin(pitch),
                                  c[2] + dist * std::cos(pitch) * std::cos(yaw)};
            const Mat4 mvp = mul(perspective(0.9f, float(sw) / sh, dist * 0.01f, dist * 10 + radius * 4),
                                 look_at(eye, c));
            glUniformMatrix4fv(u_mvp, 1, GL_FALSE, mvp.m);
            glUniform1i(u_vcol, (v.flags & wl::MODEL_HAS_COLORS) ? 1 : 0);
            glUniform1i(u_lit, (lighting && (v.flags & wl::MODEL_HAS_NORMALS)) ? 1 : 0);
            glBindVertexArray(gm.vao);
            for (int pass = 0; pass < 2; ++pass) {  // 0: opacos, 1: translúcidos
                if (pass == 1) {
                    glEnable(GL_BLEND);
                    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                    glDepthMask(GL_FALSE);
                } else {
                    glDisable(GL_BLEND);
                    glDepthMask(GL_TRUE);
                }
                for (uint32_t s = 0; s < v.submesh_count; ++s) {
                    const wl::ModelSubmesh& sm = v.submeshes[s];
                    if ((sm.xlu != 0) != (pass == 1)) continue;
                    if (cull && sm.cull) {
                        glEnable(GL_CULL_FACE);
                        glCullFace(sm.cull == 1 ? GL_FRONT : sm.cull == 2 ? GL_BACK : GL_FRONT_AND_BACK);
                    } else {
                        glDisable(GL_CULL_FACE);
                    }
                    const GLuint t = use_textures ? texture(sm.texture_key) : 0;
                    glUniform1i(u_use_tex, t ? 1 : 0);
                    glUniform1i(u_atest, pass == 0 ? 1 : 0);
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, t);
                    if (t) {
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap_mode(sm.wrap_s));
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap_mode(sm.wrap_t));
                    }
                    glDrawElements(GL_TRIANGLES, GLsizei(sm.count), GL_UNSIGNED_INT,
                                   reinterpret_cast<void*>(uintptr_t(sm.first) * 4));
                }
            }
        }
        SDL_GL_SwapWindow(win);
        ++frames;
        if (now - fps_t0 >= 2000) {
            std::printf("[fps] %.1f  %s\n", frames * 1000.0 / (now - fps_t0), gm.name.c_str());
            std::fflush(stdout);
            frames = 0;
            fps_t0 = now;
        }
    }
    for (auto& kv : textures)
        if (kv.second) wl::gl_delete(kv.second);
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
