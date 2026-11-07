#include "wl/tev_gen.h"

#include <cstdio>
#include <cstring>

namespace wl {

namespace {

std::string f(float v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.6f", v);
    return b;
}

std::string vec4c(const int16_t r[4]) {
    return "vec4(" + f(r[0] / 255.0f) + ", " + f(r[1] / 255.0f) + ", " + f(r[2] / 255.0f) + ", " +
           f(r[3] / 255.0f) + ")";
}

const char* kFrac[8] = {"1.0", "0.875", "0.75", "0.625", "0.5", "0.375", "0.25", "0.125"};

// Selección de color konstante (KCSEL) como expresión vec3.
std::string konst_color(const TevMaterialRec& m, unsigned sel) {
    if (sel < 8) return std::string("vec3(") + kFrac[sel] + ")";
    auto k = [&](int i, int c) { return f(m.kregs[i][c] / 255.0f); };
    if (sel >= 0x0C && sel <= 0x0F) {
        const int i = int(sel - 0x0C);
        return "vec3(" + k(i, 0) + ", " + k(i, 1) + ", " + k(i, 2) + ")";
    }
    if (sel >= 0x10 && sel <= 0x1F) {
        const int i = int(sel & 3), c = int((sel - 0x10) >> 2);  // 0 R, 1 G, 2 B, 3 A
        return "vec3(" + k(i, c) + ")";
    }
    return "vec3(1.0)";
}

std::string konst_alpha(const TevMaterialRec& m, unsigned sel) {
    if (sel < 8) return kFrac[sel];
    if (sel >= 0x10 && sel <= 0x1F) return f(m.kregs[sel & 3][(sel - 0x10) >> 2] / 255.0f);
    return "1.0";
}

const char* kColorIn[16] = {"prev.rgb", "prev.aaa", "c0.rgb", "c0.aaa", "c1.rgb", "c1.aaa", "c2.rgb", "c2.aaa",
                            "tex.rgb", "tex.aaa", "ras.rgb", "ras.aaa", "vec3(1.0)", "vec3(0.5)", "kc",
                            "vec3(0.0)"};
const char* kAlphaIn[8] = {"prev.a", "c0.a", "c1.a", "c2.a", "tex.a", "ras.a", "ka", "0.0"};
const char* kDest[4] = {"prev", "c0", "c1", "c2"};
const char* kBias[3] = {"0.0", "0.5", "-0.5"};
const char* kScale[4] = {"1.0", "2.0", "4.0", "0.5"};
const char* kCompare[8] = {"false", "(A < R)", "(A == R)", "(A <= R)", "(A > R)", "(A != R)", "(A >= R)", "true"};

// Tabla de intercambio de canales (swap table) como sufijo GLSL, p. ej. ".rgba" o ".rrra".
std::string swap_suffix(const TevMaterialRec& m, unsigned table) {
    static const char comp[4] = {'r', 'g', 'b', 'a'};
    const uint32_t lo = m.ksel[table * 2], hi = m.ksel[table * 2 + 1];
    std::string s = ".";
    s += comp[lo & 3];
    s += comp[(lo >> 2) & 3];
    s += comp[hi & 3];
    s += comp[(hi >> 2) & 3];
    return s;
}

// Coordenada de textura de un texgen GX.
std::string texgen_expr(uint32_t info) {
    const unsigned type = (info >> 4) & 7, src = (info >> 7) & 31;
    if (type == 0 || type == 1) {
        if (src == 1) return "env";  // a partir de la normal: mapa de entorno / reflejo
        if (src == 0) return "(v_pos.xy * 0.05 + 0.5)";
        return "v_uv0";              // TEX0..7: aquí solo hay UV0
    }
    return "v_col.rg";               // COLOR_STRGBC0/1
}

}  // namespace

const char* tev_vertex_source() {
    return R"(#version 300 es
layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 nrm;
layout(location = 2) in vec4 col;
layout(location = 3) in vec2 uv;
uniform mat4 mvp;
uniform mat4 mv;
out vec3 v_nrm;
out vec3 v_pos;
out vec4 v_col;
out vec2 v_uv0;
void main() {
  v_nrm = mat3(mv) * nrm;
  v_pos = (mv * vec4(pos, 1.0)).xyz;
  v_col = col;
  v_uv0 = uv;
  gl_Position = mvp * vec4(pos, 1.0);
}
)";
}

std::string tev_fragment_source(const TevMaterialRec& m, const TevGenOptions& opt) {
    const unsigned stages = m.num_stages ? (m.num_stages > 16 ? 16 : m.num_stages) : 1;
    std::string s;
    s += "#version 300 es\nprecision highp float;\n";
    s += "in vec3 v_nrm;\nin vec3 v_pos;\nin vec4 v_col;\nin vec2 v_uv0;\nout vec4 color;\n";
    s += "uniform vec3 light;\n";
    bool used[8] = {};
    for (unsigned i = 0; i < stages; ++i)
        if (m.stages[i].tex_on) used[m.stages[i].texmap & 7] = true;
    for (int i = 0; i < 8; ++i)
        if (used[i]) s += "uniform sampler2D tex" + std::to_string(i) + ";\n";

    s += "void main() {\n";
    s += "  vec3 n = length(v_nrm) > 0.0 ? normalize(v_nrm) : vec3(0.0, 0.0, 1.0);\n";
    s += "  vec2 env = vec2(n.x, -n.y) * 0.5 + 0.5;\n";
    // Canal de color COLOR0: color de vértice (o blanco) con iluminación aproximada.
    s += std::string("  vec4 ras0 = ") + (opt.has_vertex_color ? "v_col" : "vec4(1.0)") + ";\n";
    if (opt.has_normals) s += "  ras0.rgb *= 0.45 + 0.55 * max(dot(n, light), 0.0);\n";
    const unsigned ntg = m.num_texgens > 8 ? 8 : m.num_texgens;
    for (unsigned i = 0; i < 8; ++i)
        s += "  vec2 tc" + std::to_string(i) + " = " + (i < ntg ? texgen_expr(m.texgen[i]) : std::string("v_uv0")) +
             ";\n";
    s += "  vec4 prev = " + vec4c(m.cregs[0]) + ";\n";
    s += "  vec4 c0 = " + vec4c(m.cregs[1]) + ";\n";
    s += "  vec4 c1 = " + vec4c(m.cregs[2]) + ";\n";
    s += "  vec4 c2 = " + vec4c(m.cregs[3]) + ";\n";
    s += "  vec4 tex; vec4 ras; vec3 kc; float ka;\n";

    for (unsigned i = 0; i < stages; ++i) {
        const TevStageRec& st = m.stages[i];
        const uint32_t ce = st.color_env, ae = st.alpha_env;
        const uint32_t ks = m.ksel[i / 2];
        const unsigned kcsel = (i & 1) ? (ks >> 14) & 31 : (ks >> 4) & 31;
        const unsigned kasel = (i & 1) ? (ks >> 19) & 31 : (ks >> 9) & 31;
        s += "  // etapa " + std::to_string(i) + "\n";
        if (st.tex_on)
            s += "  tex = texture(tex" + std::to_string(st.texmap & 7) + ", tc" + std::to_string(st.texcoord & 7) +
                 ")" + swap_suffix(m, (ae >> 2) & 3) + ";\n";
        else
            s += "  tex = vec4(1.0);\n";
        if (st.ras == 0 || st.ras == 1)
            s += "  ras = ras0" + swap_suffix(m, ae & 3) + ";\n";
        else
            s += "  ras = vec4(0.0);\n";
        s += "  kc = " + konst_color(m, kcsel) + ";\n";
        s += "  ka = " + konst_alpha(m, kasel) + ";\n";

        // Color
        {
            const char* a = kColorIn[(ce >> 12) & 15];
            const char* b = kColorIn[(ce >> 8) & 15];
            const char* c = kColorIn[(ce >> 4) & 15];
            const char* d = kColorIn[ce & 15];
            const unsigned bias = (ce >> 16) & 3, sub = (ce >> 18) & 1, clampb = (ce >> 19) & 1;
            const unsigned scale = (ce >> 20) & 3, dest = (ce >> 22) & 3;
            std::string expr;
            if (bias == 3) {  // modo de comparación: d + (a OP b ? c : 0)
                const unsigned op = (scale << 1) | sub;
                const bool eq = op & 1;
                if (op >= 6)  // por componente
                    expr = std::string("(") + d + " + " + c + " * vec3(" + (eq ? "equal(" : "greaterThan(") + a +
                           ", " + b + ")))";
                else
                    expr = std::string("(") + d + " + " + c + " * (" + a + ".r " + (eq ? "==" : ">") + " " + b +
                           ".r ? 1.0 : 0.0))";
            } else {
                expr = std::string("((") + d + (sub ? " - " : " + ") + "mix(" + a + ", " + b + ", " + c + ") + vec3(" +
                       kBias[bias] + ")) * " + kScale[scale] + ")";
            }
            s += std::string("  ") + kDest[dest] + ".rgb = " +
                 (clampb ? "clamp(" + expr + ", 0.0, 1.0)" : "clamp(" + expr + ", -4.0, 4.0)") + ";\n";
        }
        // Alfa
        {
            const char* a = kAlphaIn[(ae >> 13) & 7];
            const char* b = kAlphaIn[(ae >> 10) & 7];
            const char* c = kAlphaIn[(ae >> 7) & 7];
            const char* d = kAlphaIn[(ae >> 4) & 7];
            const unsigned bias = (ae >> 16) & 3, sub = (ae >> 18) & 1, clampb = (ae >> 19) & 1;
            const unsigned scale = (ae >> 20) & 3, dest = (ae >> 22) & 3;
            std::string expr;
            if (bias == 3) {
                const bool eq = ((scale << 1) | sub) & 1;
                expr = std::string("(") + d + " + " + c + " * (" + a + (eq ? " == " : " > ") + b + " ? 1.0 : 0.0))";
            } else {
                expr = std::string("((") + d + (sub ? " - " : " + ") + "mix(" + a + ", " + b + ", " + c + ") + " +
                       kBias[bias] + ") * " + kScale[scale] + ")";
            }
            s += std::string("  ") + kDest[dest] + ".a = " +
                 (clampb ? "clamp(" + expr + ", 0.0, 1.0)" : "clamp(" + expr + ", -4.0, 4.0)") + ";\n";
        }
    }
    s += "  vec4 outc = clamp(prev, 0.0, 1.0);\n";
    // Prueba de alfa (dos comparaciones y una operación lógica).
    const uint32_t ac = m.alpha_compare;
    const unsigned c0 = (ac >> 16) & 7, c1 = (ac >> 19) & 7, logic = (ac >> 22) & 3;
    if (!(c0 == 7 && c1 == 7 && logic != 2)) {  // ALWAYS/ALWAYS (salvo XOR) = sin prueba
        auto cmp = [&](unsigned c, unsigned ref) {
            std::string e = kCompare[c];
            const std::string A = "a8", R = f(float(ref));
            std::string out;
            for (char ch : e) {
                if (ch == 'A') out += A;
                else if (ch == 'R') out += R;
                else out += ch;
            }
            return out;
        };
        const char* ops[4] = {" && ", " || ", " != ", " == "};  // AND, OR, XOR, XNOR
        s += "  float a8 = floor(outc.a * 255.0 + 0.5);\n";
        s += "  if (!(" + cmp(c0, ac & 0xFF) + ops[logic] + cmp(c1, (ac >> 8) & 0xFF) + ")) discard;\n";
    }
    s += "  color = outc;\n}\n";
    return s;
}

GlBlendState tev_blend_state(const TevMaterialRec& m) {
    // Constantes de OpenGL ES (se escriben aquí para no depender de las cabeceras GL en este archivo).
    constexpr uint32_t ZERO = 0, ONE = 1, SRC_COLOR = 0x0300, ONE_MINUS_SRC_COLOR = 0x0301, SRC_ALPHA = 0x0302,
                       ONE_MINUS_SRC_ALPHA = 0x0303, DST_ALPHA = 0x0304, ONE_MINUS_DST_ALPHA = 0x0305,
                       DST_COLOR = 0x0306, ONE_MINUS_DST_COLOR = 0x0307;
    static const uint32_t src_tab[8] = {ZERO, ONE, DST_COLOR, ONE_MINUS_DST_COLOR, SRC_ALPHA, ONE_MINUS_SRC_ALPHA,
                                        DST_ALPHA, ONE_MINUS_DST_ALPHA};
    static const uint32_t dst_tab[8] = {ZERO, ONE, SRC_COLOR, ONE_MINUS_SRC_COLOR, SRC_ALPHA, ONE_MINUS_SRC_ALPHA,
                                        DST_ALPHA, ONE_MINUS_DST_ALPHA};
    static const uint32_t func_tab[8] = {0x0200, 0x0201, 0x0202, 0x0203, 0x0204, 0x0205, 0x0206, 0x0207};
    GlBlendState b;
    const uint32_t v = m.blend;
    b.blend = (v & 1) != 0;
    b.subtract = (v & 0x800) != 0;
    b.src = src_tab[(v >> 8) & 7];
    b.dst = dst_tab[(v >> 5) & 7];
    b.color_write = (v & 8) != 0;
    b.alpha_write = (v & 16) != 0;
    if (b.subtract) {
        b.blend = true;
        b.src = ONE;
        b.dst = ONE;
    }
    b.depth_test = (m.zmode & 1) != 0;
    b.depth_func = func_tab[(m.zmode >> 1) & 7];
    b.depth_write = (m.zmode & 16) != 0;
    return b;
}

uint64_t tev_program_hash(const TevMaterialRec& m, const TevGenOptions& opt) {
    TevMaterialRec c = m;
    std::memset(c.maps, 0, sizeof c.maps);
    c.xlu = 0;
    c.cull = 0;
    c.blend = 0;
    c.zmode = 0;
    uint64_t h = 0xCBF29CE484222325ull;
    const auto* p = reinterpret_cast<const uint8_t*>(&c);
    for (size_t i = 0; i < sizeof c; ++i) {
        h ^= p[i];
        h *= 0x100000001B3ull;
    }
    h ^= (opt.has_normals ? 1u : 0u) | (opt.has_vertex_color ? 2u : 0u);
    return h * 0x100000001B3ull;
}

}  // namespace wl
