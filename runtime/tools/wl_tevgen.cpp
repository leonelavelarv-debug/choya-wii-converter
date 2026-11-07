// wl_tevgen: escribe los shaders GLSL generados para los materiales de los modelos de un paquete.
//   wl_tevgen assets.wlpk carpeta_salida [texto_en_el_nombre]
// Crea <modelo>__<material>.frag y common.vert; sirve para revisar y probar el traductor TEV.
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include "wl/model.h"
#include "wl/pack.h"
#include "wl/tev_gen.h"

static std::string safe(const char* s) {
    std::string o;
    for (; *s; ++s) o += (std::isalnum(static_cast<unsigned char>(*s)) || *s == '.' || *s == '-') ? *s : '_';
    return o.size() > 100 ? o.substr(o.size() - 100) : o;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "uso: wl_tevgen assets.wlpk carpeta [texto]\n");
        return 2;
    }
    wl::Pack pack;
    if (!pack.open(argv[1])) {
        std::fprintf(stderr, "error: %s\n", pack.error().c_str());
        return 1;
    }
    const std::string dir = argv[2];
    std::ofstream(dir + "/common.vert") << wl::tev_vertex_source();
    int written = 0;
    for (uint32_t i = 0; i < pack.size(); ++i) {
        const wl::PackEntry& e = pack.entry(i);
        if (e.kind != wl::KIND_MODEL) continue;
        if (argc > 3 && !std::strstr(pack.name(e), argv[3])) continue;
        wl::ModelView mv;
        if (!wl::model_view(pack, e, mv)) {
            std::fprintf(stderr, "modelo dañado: %s\n", pack.name(e));
            continue;
        }
        wl::TevGenOptions opt;
        opt.has_normals = mv.flags & wl::MODEL_HAS_NORMALS;
        opt.has_vertex_color = mv.flags & wl::MODEL_HAS_COLORS;
        for (uint32_t m = 0; m < mv.material_count; ++m) {
            const std::string path = dir + "/" + safe(pack.name(e)) + "__" + std::to_string(m) + ".frag";
            std::ofstream(path) << wl::tev_fragment_source(mv.materials[m], opt);
            ++written;
        }
    }
    std::printf("%d shaders escritos en %s\n", written, dir.c_str());
    return 0;
}
