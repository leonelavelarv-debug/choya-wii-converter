// choya_model: visor de modelos 3D del paquete en la RG35XX H (SDL2 + OpenGL ES 3).
// Puerta 1 del plan: geometría, texturas y materiales TEV de Wii Sports, con FPS.
//
//   Cruceta / stick izquierdo: girar      A / B: acercar / alejar
//   L1 / R1: modelo anterior / siguiente  X: cull on/off   Y: TEV / sombreado simple
//   Select: texturas on/off (modo simple) Start (o Escape): salir
#include <SDL2/SDL.h>
#include <GLES3/gl3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "wl/camera.h"
#include "wl/model_render.h"
#include "wl/pack.h"

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
    const char* start_at = argc > 2 ? argv[2] : std::getenv("CHOYA_MODEL");
    if (start_at)
        for (size_t i = 0; i < models.size(); ++i)
            if (std::strstr(pack.name(pack.entry(models[i])), start_at)) {
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

    {
        wl::ModelRenderer renderer(pack);
        if (!renderer.init()) return 1;
        wl::Orbit orbit;
        wl::RenderOptions opt;
        auto load = [&](size_t i) {
            const wl::LoadInfo li = renderer.load(models[i]);
            if (!li.ok) {
                std::printf("[%zu/%zu] %s  DAÑADO\n", i + 1, models.size(), li.name.c_str());
            } else {
                std::printf("[%zu/%zu] %s  %u vértices, %u triángulos, %u submallas, %u materiales, "
                            "%d shaders nuevos, %.0f ms%s\n",
                            i + 1, models.size(), li.name.c_str(), li.vertices, li.triangles, li.submeshes,
                            li.materials, li.new_programs, li.ms,
                            li.missing_textures ? "  (texturas faltantes)" : "");
            }
            std::fflush(stdout);
            orbit = wl::Orbit{};
        };
        load(cur);

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
                        case SDLK_c: opt.cull = !opt.cull; break;
                        case SDLK_t: opt.textures = !opt.textures; break;
                        case SDLK_l: opt.lighting = !opt.lighting; break;
                        case SDLK_y: opt.tev = !opt.tev; break;
                        default: break;
                    }
                }
                if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
                    switch (ev.cbutton.button) {
                        case SDL_CONTROLLER_BUTTON_START: running = false; break;
                        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: step = 1; break;
                        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: step = -1; break;
                        case SDL_CONTROLLER_BUTTON_X:
                            opt.cull = !opt.cull;
                            std::printf("cull %s\n", opt.cull ? "on" : "off");
                            break;
                        case SDL_CONTROLLER_BUTTON_Y:
                            opt.tev = !opt.tev;
                            std::printf("modo %s\n", opt.tev ? "TEV" : "simple");
                            break;
                        case SDL_CONTROLLER_BUTTON_BACK: opt.textures = !opt.textures; break;
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
            orbit.yaw += rx * 1.8f * dt;
            orbit.pitch = std::clamp(orbit.pitch + ry * 1.2f * dt, -1.45f, 1.45f);
            orbit.zoom = std::clamp(orbit.zoom * std::pow(0.4f, zz * dt), 0.05f, 20.0f);

            glViewport(0, 0, sw, sh);
            glClearColor(0.18f, 0.22f, 0.28f, 1);
            glDepthMask(GL_TRUE);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            float view[16], proj[16];
            wl::orbit_matrices(renderer.view(), orbit, float(sw) / sh, view, proj);
            renderer.draw(view, proj, opt);
            SDL_GL_SwapWindow(win);
            ++frames;
            if (now - fps_t0 >= 2000) {
                std::printf("[fps] %.1f  %s  (%s)\n", frames * 1000.0 / (now - fps_t0),
                            pack.name(pack.entry(models[cur])), opt.tev ? "TEV" : "simple");
                std::fflush(stdout);
                frames = 0;
                fps_t0 = now;
            }
        }
        if (renderer.tev_failures())
            std::printf("[tev] %d materiales usaron el sombreado simple por error de shader\n", renderer.tev_failures());
    }
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
