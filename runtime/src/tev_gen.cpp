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
std::string konst_color(const TevMaterialRec& m, unsigned sel, bool dyn) {
    if (sel < 8) return std::string("vec3(") + kFrac[sel] + ")";
    if (dyn) {
        static const char comp[4] = {'r', 'g', 'b', 'a'};
        if (sel >= 0x0C && sel <= 0x0F) return "u_kreg[" + std::to_string(sel - 0x0C) + "].rgb";
        if (sel >= 0x10 && sel <= 0x1F)
            return std::string("vec3(u_kreg[") + std::to_string(sel & 3) + "]." + comp[(sel - 0x10) >> 2] + ")";
        return "vec3(1.0)";
    }
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

std::string konst_alpha(const TevMaterialRec& m, unsigned sel, bool dyn) {
    if (sel < 8) return kFrac[sel];
    if (dyn) {
        static const char comp[4] = {'r', 'g', 'b', 'a'};
        if (sel >= 0x10 && sel <= 0x1F)
            return std::string("u_kreg[") + std::to_string(sel & 3) + "]." + comp[(sel - 0x10) >> 2];
        return "1.0";
    }
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

// Coordenada de textura de un texgen GX (antes de la matriz de textura del material).
std::string texgen_expr(uint32_t info, unsigned map_mode) {
    const unsigned type = (info >> 4) & 7, src = (info >> 7) & 31;
    if (map_mode == 2) return "proj";    // proyección desde la cámara (NW4R ProjectionMapping)
    if (map_mode == 1 || map_mode == 3 || map_mode == 4) return "env";
    if (type == 0 || type == 1) {
        if (src == 1) return "env";      // a partir de la normal: mapa de entorno / reflejo
        if (src == 0) return "(v_pos.xy * 0.05 + 0.5)";
        return "v_uv0";                  // TEX0..7: aquí solo hay UV0
    }
    return "v_col.rg";                   // COLOR_STRGBC0/1
}

std::string vec4f(const float v[4]) {
    return "vec4(" + f(v[0]) + ", " + f(v[1]) + ", " + f(v[2]) + ", " + f(v[3]) + ")";
}
std::string rgba8(const uint8_t c[4]) {
    return "vec4(" + f(c[0] / 255.0f) + ", " + f(c[1] / 255.0f) + ", " + f(c[2] / 255.0f) + ", " + f(c[3] / 255.0f) +
           ")";
}

// Un canal de luz GX (como el XF): material y ambiente del registro o del vértice, luz difusa con la función del
// canal y una luz direccional blanca (la del visor) si el canal la tiene encendida y alguna luz en la máscara.
std::string chan_component(const std::string& vcol, const std::string& matc, const std::string& ambc, uint32_t ctrl,
                           const char* sw) {
    const std::string mat = (ctrl & 1) ? vcol + sw : matc + sw;
    if (!(ctrl & 2)) return mat;
    const std::string amb = (ctrl & 64) ? vcol + sw : ambc + sw;
    const unsigned mask = ((ctrl >> 2) & 0xF) | (((ctrl >> 11) & 0xF) << 4);
    if (!mask) return "clamp(" + amb + ", 0.0, 1.0) * " + mat;
    const unsigned fn = (ctrl >> 7) & 3;
    const std::string d = fn == 0 ? "1.0" : fn == 1 ? "ndl" : "max(ndl, 0.0)";
    return "clamp(" + amb + " + " + d + ", 0.0, 1.0) * " + mat;
}

// Igual que chan_component pero con las luces de GX en uniformes (modo dinámico). mask_sel: componente de u_lmask.
std::string chan_component_dyn(const std::string& vcol, const std::string& matc, const std::string& ambc,
                               uint32_t ctrl, const char* sw, const char* mask_sel, bool normals) {
    const std::string mat = (ctrl & 1) ? vcol + sw : matc + sw;
    if (!(ctrl & 2)) return mat;
    const std::string amb = (ctrl & 64) ? vcol + sw : ambc + sw;
    const unsigned dfn = normals ? (ctrl >> 7) & 3 : 0;
    const unsigned afn = ((ctrl >> 9) & 1) | (((ctrl >> 10) & 1) << 1);
    return "clamp(" + amb + " + gx_lights(u_lmask." + mask_sel + ", " + std::to_string(dfn) + ", " +
           std::to_string(afn) + ", n, v_pos)" + sw + ", 0.0, 1.0) * " + mat;
}

// Luces de GX (XF): por cada luz de la máscara, dirección hacia la luz, atenuación (ninguna, especular, foco) y la
// función difusa. u_lpos.w = 0: direccional (xyz ya es la dirección hacia la luz, en vista).
const char* kLightFn = R"(vec4 gx_lights(int mask, int dfn, int afn, vec3 n, vec3 p) {
  vec4 acc = vec4(0.0);
  for (int i = 0; i < 4; ++i) {
    if ((mask & (1 << i)) == 0) continue;
    bool dirl = u_lpos[i].w == 0.0;
    vec3 lv = dirl ? u_lpos[i].xyz : u_lpos[i].xyz - p;
    float d = dirl ? 1.0 : length(lv);
    vec3 ldir = d > 0.0 ? lv / (dirl ? length(lv) : d) : n;
    float attn = 1.0;
    if (afn == 1) {
      float a = dot(n, ldir) >= 0.0 ? max(0.0, dot(n, u_ldir[i].xyz)) : 0.0;
      vec3 k = dfn == 0 ? u_ldist[i].xyz : normalize(u_ldist[i].xyz);
      attn = max(0.0, dot(u_lcos[i].xyz, vec3(1.0, a, a * a))) / max(dot(k, vec3(1.0, a, a * a)), 1e-6);
    } else if (afn == 3) {
      float c = max(0.0, dot(ldir, u_ldir[i].xyz));
      attn = max(0.0, u_lcos[i].x + u_lcos[i].y * c + u_lcos[i].z * c * c) /
             max(dot(u_ldist[i].xyz, vec3(1.0, d, d * d)), 1e-6);
    }
    float diff = dfn == 0 ? 1.0 : (dfn == 1 ? dot(ldir, n) : max(dot(ldir, n), 0.0));
    acc += attn * diff * u_lcol[i];
  }
  return acc;
}
)";

}  // namespace

const char* tev_vertex_source() {
    return R"(#version 300 es
layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 nrm;
layout(location = 2) in vec4 col;
layout(location = 3) in vec2 uv;
layout(location = 4) in uvec4 bone;
layout(location = 5) in vec4 weight;
uniform mat4 mvp;
uniform mat4 mv;
layout(std140) uniform Skin { vec4 skin[765]; };  // 255 huesos x 3 filas (matriz de piel 3x4)
uniform bool skinned;  // el modelo tiene huesos (un atributo apagado vale (0, 0, 0, 1): no basta con el peso)
out vec3 v_nrm;
out vec3 v_pos;
out vec4 v_col;
out vec2 v_uv0;
out vec4 v_clip;
vec3 skin_point(uint b, vec4 p) { return vec3(dot(skin[3u*b], p), dot(skin[3u*b+1u], p), dot(skin[3u*b+2u], p)); }
void main() {
  vec3 p = pos, n = nrm;
  float ws = weight.x + weight.y + weight.z + weight.w;
  if (skinned && ws > 0.0) {
    p = vec3(0.0); n = vec3(0.0);
    for (int k = 0; k < 4; ++k) {
      if (weight[k] <= 0.0) continue;
      p += weight[k] * skin_point(bone[k], vec4(pos, 1.0));
      n += weight[k] * skin_point(bone[k], vec4(nrm, 0.0));
    }
    p /= ws;
  }
  v_nrm = mat3(mv) * n;
  v_pos = (mv * vec4(p, 1.0)).xyz;
  v_col = col;
  v_uv0 = uv;
  gl_Position = mvp * vec4(p, 1.0);
  v_clip = gl_Position;
}
)";
}

std::string tev_fragment_source(const TevMaterialRec& m, const TevGenOptions& opt) {
    const unsigned stages = m.num_stages ? (m.num_stages > 16 ? 16 : m.num_stages) : 1;
    std::string s;
    s += "#version 300 es\nprecision highp float;\n";
    s += "in vec3 v_nrm;\nin vec3 v_pos;\nin vec4 v_col;\nin vec2 v_uv0;\nin vec4 v_clip;\nout vec4 color;\n";
    s += "uniform vec3 light;\n";
    if (opt.dynamic) {
        s += "uniform vec4 u_creg[4];\nuniform vec4 u_kreg[4];\nuniform vec4 u_chmat[2];\nuniform vec4 u_chamb[2];\n"
             "uniform vec4 u_tm[16];\nuniform mat4 u_tgm[8];\n";
        s += "uniform vec4 u_lpos[4];\nuniform vec4 u_ldir[4];\nuniform vec4 u_lcol[4];\nuniform vec4 u_lcos[4];\n"
             "uniform vec4 u_ldist[4];\nuniform ivec4 u_lmask;\n";
        s += kLightFn;
    }
    bool used[8] = {};
    for (unsigned i = 0; i < stages; ++i)
        if (m.stages[i].tex_on) used[m.stages[i].texmap & 7] = true;
    for (int i = 0; i < 8; ++i)
        if (used[i]) s += "uniform sampler2D tex" + std::to_string(i) + ";\n";

    s += "void main() {\n";
    s += "  vec3 n = length(v_nrm) > 0.0 ? normalize(v_nrm) : vec3(0.0, 0.0, 1.0);\n";
    s += "  vec2 env = vec2(n.x, -n.y) * 0.5 + 0.5;\n";
    s += "  vec2 proj = v_clip.xy / v_clip.w * 0.5 + 0.5;\n";
    s += std::string("  float ndl = ") + (opt.has_normals ? "dot(n, light)" : "1.0") + ";\n";
    const std::string vcol = opt.has_vertex_color ? "v_col" : "vec4(1.0)";
    // Canales de luz COLOR0A0 y COLOR1A1. Con el control del material (versión 3), como el XF de GX; si no, el
    // color de vértice (o blanco) con una luz aproximada, como antes.
    for (int c = 0; c < 2; ++c) {
        const std::string r = "ras" + std::to_string(c);
        const ChanRec& ch = m.chans[c];
        if (c >= m.num_chans && (m.chans[0].flags & 0x30)) {
            s += "  vec4 " + r + " = vec4(0.0);\n";
            continue;
        }
        if (!(ch.flags & 0x30)) {
            s += "  vec4 " + r + " = " + vcol + ";\n";  // como antes: los dos canales con el color de vértice
            if (opt.has_normals) s += "  " + r + ".rgb *= 0.45 + 0.55 * max(ndl, 0.0);\n";
            continue;
        }
        const std::string vc = c == 0 ? vcol : "vec4(1.0)";
        if (opt.dynamic) {  // colores en uniformes (el render pone blanco/gris si el material no los trae)
            const std::string matc = "u_chmat[" + std::to_string(c) + "]", ambc = "u_chamb[" + std::to_string(c) + "]";
            s += "  vec4 " + r + " = vec4(" +
                 chan_component_dyn(vc, matc, ambc, ch.flags & 16 ? ch.ctrl_color : 0, ".rgb", c ? "z" : "x",
                                    opt.has_normals) +
                 ", " +
                 chan_component_dyn(vc, matc, ambc, ch.flags & 32 ? ch.ctrl_alpha : 0, ".a", c ? "w" : "y",
                                    opt.has_normals) +
                 ");\n";
            continue;
        }
        const uint8_t white[4] = {255, 255, 255, 255}, gray[4] = {102, 102, 102, 255};
        const std::string matc = rgba8(ch.flags & 3 ? ch.mat : white);
        const std::string ambc = rgba8(ch.flags & 12 ? ch.amb : gray);
        s += "  vec4 " + r + " = vec4(" + chan_component(vc, matc, ambc, ch.flags & 16 ? ch.ctrl_color : 0, ".rgb") +
             ", " + chan_component(vc, matc, ambc, ch.flags & 32 ? ch.ctrl_alpha : 0, ".a") + ");\n";
    }
    const unsigned ntg = m.num_texgens > 8 ? 8 : m.num_texgens;
    for (unsigned i = 0; i < 8; ++i) {
        const unsigned mode = m.tex_mtx_mode[i];
        const std::string tc = "tc" + std::to_string(i);
        const std::string is = std::to_string(i);
        if (opt.dynamic && i < ntg) {
            // Modos de entorno (1, 3, 4) y proyección (2): la matriz de la escena (con ref_camera / ref_light) la pone el
            // render; después, la matriz de textura del material o de su animación SRT0 (identidad si no hay).
            const unsigned mm = mode ? mode - 1 : 0;
            std::string base;
            if (mm == 2)
                s += "  vec4 " + tc + "q = u_tgm[" + is + "] * vec4(v_pos, 1.0);\n",
                    base = tc + "q.xy / (abs(" + tc + "q.z) > 1e-6 ? " + tc + "q.z : 1e-6)";
            else if (mm == 1 || mm == 3 || mm == 4)
                base = "(u_tgm[" + is + "] * vec4(n, 1.0)).xy";
            else
                base = texgen_expr(m.texgen[i], 0);
            s += "  vec4 " + tc + "in = vec4(" + base + ", 1.0, 1.0);\n";
            s += "  vec2 " + tc + " = vec2(dot(u_tm[" + std::to_string(2 * i) + "], " + tc + "in), dot(u_tm[" +
                 std::to_string(2 * i + 1) + "], " + tc + "in));\n";
            continue;
        }
        const std::string base = i < ntg ? texgen_expr(m.texgen[i], mode ? mode - 1 : 0) : std::string("v_uv0");
        if (mode && i < ntg) {  // matriz de textura del material (la matriz posterior que carga NW4R)
            s += "  vec4 " + tc + "in = vec4(" + base + ", 1.0, 1.0);\n";
            s += "  vec2 " + tc + " = vec2(dot(" + vec4f(m.tex_mtx[i][0]) + ", " + tc + "in), dot(" +
                 vec4f(m.tex_mtx[i][1]) + ", " + tc + "in));\n";
        } else {
            s += "  vec2 " + tc + " = " + base + ";\n";
        }
    }
    if (opt.dynamic) {
        s += "  vec4 prev = u_creg[0];\n  vec4 c0 = u_creg[1];\n  vec4 c1 = u_creg[2];\n  vec4 c2 = u_creg[3];\n";
    } else {
        s += "  vec4 prev = " + vec4c(m.cregs[0]) + ";\n";
        s += "  vec4 c0 = " + vec4c(m.cregs[1]) + ";\n";
        s += "  vec4 c1 = " + vec4c(m.cregs[2]) + ";\n";
        s += "  vec4 c2 = " + vec4c(m.cregs[3]) + ";\n";
    }
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
            s += "  ras = ras" + std::to_string(st.ras) + swap_suffix(m, ae & 3) + ";\n";
        else
            s += "  ras = vec4(0.0);\n";
        s += "  kc = " + konst_color(m, kcsel, opt.dynamic) + ";\n";
        s += "  ka = " + konst_alpha(m, kasel, opt.dynamic) + ";\n";

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
    if (opt.dynamic) {  // lo que va en uniformes no separa programas
        std::memset(c.cregs, 0, sizeof c.cregs);
        std::memset(c.kregs, 0, sizeof c.kregs);
        std::memset(c.tex_mtx, 0, sizeof c.tex_mtx);
        for (ChanRec& ch : c.chans) {
            std::memset(ch.mat, 0, 4);
            std::memset(ch.amb, 0, 4);
            ch.ctrl_color &= ~((0xFu << 2) | (0xFu << 11));  // máscara de luces: la da el conjunto de luces
            ch.ctrl_alpha &= ~((0xFu << 2) | (0xFu << 11));
        }
        std::memset(c.cam_ref, 0, sizeof c.cam_ref);
        std::memset(c.light_ref, 0, sizeof c.light_ref);
        c.light_set = 0;
        c.fog = 0;
        c.ref_valid = 0;
        // el modo de mapeo (entorno / proyección) sí es estructural; "sin matriz" y "coordenadas" son lo mismo aquí
        for (uint8_t& md : c.tex_mtx_mode)
            if (md == 1) md = 0;
    }
    uint64_t h = 0xCBF29CE484222325ull;
    const auto* p = reinterpret_cast<const uint8_t*>(&c);
    for (size_t i = 0; i < sizeof c; ++i) {
        h ^= p[i];
        h *= 0x100000001B3ull;
    }
    h ^= (opt.has_normals ? 1u : 0u) | (opt.has_vertex_color ? 2u : 0u) | (opt.dynamic ? 4u : 0u);
    return h * 0x100000001B3ull;
}

}  // namespace wl
