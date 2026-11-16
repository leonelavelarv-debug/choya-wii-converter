// choya_model: visor de modelos 3D del paquete en la RG35XX H (SDL2 + OpenGL ES 3).
// Puerta 1 del plan: geometría, texturas y materiales TEV de Wii Sports, con FPS.
//
//   Cruceta / stick izquierdo: girar      A / B: acercar / alejar
//   L1 / R1: modelo anterior / siguiente  X: cull on/off   Y: TEV / sombreado simple
//   Select: texturas on/off (modo simple) Start (o Escape): salir
//   R2 / L2 (teclado N): grupo de animaciones siguiente / anterior (CHR0 + SRT0/CLR0/PAT0/VIS0 del mismo nombre)
//   L3 (teclado E): escena SCN0 siguiente (luces y cámaras de referencia); después de la última, la luz del visor
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
        // Grupos de animaciones del modelo (CHR0 horneada + SRT0/CLR0/PAT0/VIS0 del mismo nombre): R2 el siguiente, L2
        // el anterior; el último vuelve a la pose y los materiales de reposo. En el teclado, N. Todo se arma al cargar
        // el modelo; por cuadro solo se copian valores.
        std::vector<wl::AnimGroup> groups;
        int anim_cur = -1;
        wl::AnimView anim;
        bool has_chr = false;
        std::vector<wl::MatAnimView> mats;
        float anim_t = 0;
        bool r2_down = false, l2_down = false;
        auto pick_anim = [&](int step) {
            if (groups.empty()) return;
            const int n = int(groups.size()) + 1;  // + reposo
            anim_cur = ((anim_cur + 1 + step) % n + n) % n - 1;
            anim_t = 0;
            mats.clear();
            has_chr = false;
            renderer.reset_material_state();
            if (anim_cur < 0) {
                renderer.set_pose(nullptr, 0);
                std::printf("  pose de reposo\n");
            } else {
                const wl::AnimGroup& g = groups[size_t(anim_cur)];
                has_chr = g.chr >= 0 && wl::anim_view(pack, pack.entry(uint32_t(g.chr)), anim);
                if (!has_chr) renderer.set_pose(nullptr, 0);
                int textures = 0;
                for (uint32_t mi : g.mats) {
                    wl::MatAnimView m;
                    if (wl::mat_anim_view(pack, pack.entry(mi), m)) {
                        textures += renderer.prepare_textures(m);
                        mats.push_back(m);
                    }
                }
                std::printf("  grupo %d/%zu: %s (%u cuadros%s, %zu de material, %d texturas de PAT0)\n", anim_cur + 1,
                            groups.size(), g.name.c_str(), g.frames, has_chr ? ", CHR0" : "", mats.size(), textures);
            }
            std::fflush(stdout);
        };
        // Escenas SCN0 (la más cercana al modelo primero); -1 = luz y cámara del visor.
        std::vector<uint32_t> scenes;
        int scene_cur = -1;
        wl::SceneView scene;
        float scene_t = 0;
        auto pick_scene = [&]() {
            if (scenes.empty()) {
                std::printf("  sin escenas SCN0 en el paquete: luz y cámara del visor\n");
                return;
            }
            scene_cur = scene_cur + 1 >= int(scenes.size()) ? -1 : scene_cur + 1;
            scene_t = 0;
            if (scene_cur >= 0 && wl::scene_view(pack, pack.entry(scenes[size_t(scene_cur)]), scene)) {
                renderer.set_scene(&scene, 0);
                std::printf("  escena %d/%zu: %s (%u conjuntos, %u luces, %u cámaras, %u nieblas sin aplicar)\n",
                            scene_cur + 1, scenes.size(), pack.name(pack.entry(scenes[size_t(scene_cur)])),
                            scene.set_count, scene.light_count, scene.camera_count, scene.fog_count);
            } else {
                scene_cur = -1;
                renderer.set_scene(nullptr, 0);
                std::printf("  sin escena: luz y cámara del visor (no son las del juego)\n");
            }
            std::fflush(stdout);
        };
        bool l3_down = false;
        auto load = [&](size_t i) {
            const wl::LoadInfo li = renderer.load(models[i]);
            groups = wl::anim_groups_for(pack, pack.entry(models[i]).key);
            anim_cur = -1;
            mats.clear();
            has_chr = false;
            if (!groups.empty()) std::printf("  %zu grupos de animaciones (R2/L2 o N)\n", groups.size());
            scenes = wl::scenes_for(pack, pack.name(pack.entry(models[i])));
            if (scene_cur >= int(scenes.size())) scene_cur = -1;
            if (scene_cur >= 0 && wl::scene_view(pack, pack.entry(scenes[size_t(scene_cur)]), scene))
                renderer.set_scene(&scene, 0);
            else
                scene_cur = -1, renderer.set_scene(nullptr, 0);
            if (li.light_set_materials || li.ref_matrices || li.hidden_submeshes)
                std::printf("  %d materiales con conjunto de luces, %d matrices con ref_camera/ref_light, %d submallas "
                            "ocultas por su nodo%s\n",
                            li.light_set_materials, li.ref_matrices, li.hidden_submeshes,
                            scenes.empty() ? "; sin escena SCN0: luz y cámara del visor" : " (L3 o E: escena)");
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
                        case SDLK_n: pick_anim(1); break;
                        case SDLK_e: pick_scene(); break;
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
            for (SDL_GameController* c : pads) {
                const bool r2 = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000;
                const bool l2 = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000;
                if (r2 && !r2_down) pick_anim(1);
                if (l2 && !l2_down) pick_anim(-1);
                r2_down = r2;
                l2_down = l2;
            }
            for (SDL_GameController* c : pads) {
                const bool l3 = SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSTICK);
                if (l3 && !l3_down) pick_scene();
                l3_down = l3;
            }
            if (anim_cur >= 0) {
                // Un solo reloj para la CHR0 y las de material del grupo (60 cuadros por segundo, como NW4R).
                const wl::AnimGroup& g = groups[size_t(anim_cur)];
                anim_t += dt * 60.0f;
                uint32_t f = uint32_t(anim_t);
                if (g.frames && f >= g.frames) {
                    if (g.loop) {
                        anim_t = std::fmod(anim_t, float(g.frames));
                        f = uint32_t(anim_t) % g.frames;
                    } else {
                        f = g.frames - 1;
                        if (anim_t > g.frames + 30) anim_t = 0;  // al terminar, pausa y otra vez
                    }
                }
                if (has_chr) renderer.set_pose(anim.frame(anim.loop ? f % anim.frames : std::min(f, anim.frames - 1)),
                                               anim.bones);
                renderer.reset_material_state();  // estado base: una animación no deja restos en la otra
                for (const wl::MatAnimView& m : mats) renderer.apply_mat_anim(m, f);
            }
            if (scene_cur >= 0) {
                scene_t += dt * 60.0f;
                renderer.set_scene(&scene, uint32_t(scene_t));
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
                std::printf("[fps] %.1f  %s  (%s)\n%s\n", frames * 1000.0 / (now - fps_t0),
                            pack.name(pack.entry(models[cur])), opt.tev ? "TEV" : "simple",
                            renderer.scene_diag().text().c_str());
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
