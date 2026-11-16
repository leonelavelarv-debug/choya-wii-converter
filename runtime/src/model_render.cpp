#include "wl/model_render.h"

#include <GLES3/gl3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "wl/texcache.h"

namespace wl {

namespace {

const char* kSimpleVS = R"(#version 300 es
layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 nrm;
layout(location = 2) in vec4 col;
layout(location = 3) in vec2 uv;
layout(location = 4) in uvec4 bone;
layout(location = 5) in vec4 weight;
uniform mat4 mvp;
layout(std140) uniform Skin { vec4 skin[765]; };
uniform bool skinned;
out vec3 v_nrm;
out vec4 v_col;
out vec2 v_uv;
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
  v_nrm = n; v_col = col; v_uv = uv; gl_Position = mvp * vec4(p, 1.0);
}
)";

constexpr GLuint kSkinBinding = 0;
constexpr uint32_t kMaxBones = 255;

void bind_skin_block(GLuint prog) {
    const GLuint idx = glGetUniformBlockIndex(prog, "Skin");
    if (idx != GL_INVALID_INDEX) glUniformBlockBinding(prog, idx, kSkinBinding);
}

const char* kSimpleFS = R"(#version 300 es
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

GLuint compile(GLenum type, const char* src, std::string* log) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[2048];
        glGetShaderInfoLog(s, sizeof buf, nullptr, buf);
        if (log) *log = buf;
        glDeleteShader(s);
        return 0;
    }
    return s;
}

GLuint link(GLuint v, GLuint f) {
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char buf[2048];
        glGetProgramInfoLog(p, sizeof buf, nullptr, buf);
        std::fprintf(stderr, "programa sin enlazar: %s\n", buf);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

GLint wrap_mode(uint8_t w) { return w == 0 ? GL_CLAMP_TO_EDGE : (w == 2 ? GL_MIRRORED_REPEAT : GL_REPEAT); }

void mul4(const float* a, const float* b, float* r) {
    for (int c = 0; c < 4; ++c)
        for (int rr = 0; rr < 4; ++rr) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a[k * 4 + rr] * b[c * 4 + k];
            r[c * 4 + rr] = s;
        }
}

// --- matrices 3x4 (filas) como las de NW4R ----------------------------------------------------------------------
struct M34 {
    float m[3][4];
};
M34 m34_identity() {
    M34 r{};
    r.m[0][0] = r.m[1][1] = r.m[2][2] = 1.0f;
    return r;
}
// Vista de GL (columna-mayor 4x4) como 3x4 por filas.
M34 m34_from_gl(const float* v) {
    M34 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) r.m[i][j] = v[j * 4 + i];
    return r;
}
M34 m34_mul(const M34& a, const M34& b) {  // a·b (b afín)
    M34 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
            if (j == 3) s += a.m[i][3];
            r.m[i][j] = s;
        }
    return r;
}
M34 m34_rigid_inverse(const M34& a) {
    M34 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i][j] = a.m[j][i];
    for (int i = 0; i < 3; ++i) r.m[i][3] = -(r.m[i][0] * a.m[0][3] + r.m[i][1] * a.m[1][3] + r.m[i][2] * a.m[2][3]);
    return r;
}
void m34_rot(const M34& a, const float* v, float* out) {  // VEC3TransformNormal
    float t[3];
    for (int i = 0; i < 3; ++i) t[i] = a.m[i][0] * v[0] + a.m[i][1] * v[1] + a.m[i][2] * v[2];
    out[0] = t[0], out[1] = t[1], out[2] = t[2];
}
void m34_point(const M34& a, const float* v, float* out) {
    float t[3];
    for (int i = 0; i < 3; ++i) t[i] = a.m[i][0] * v[0] + a.m[i][1] * v[1] + a.m[i][2] * v[2] + a.m[i][3];
    out[0] = t[0], out[1] = t[1], out[2] = t[2];
}
void v3_norm(float* v) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0) v[0] /= l, v[1] /= l, v[2] /= l;
}
void v3_cross(const float* a, const float* b, float* r) {
    const float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    r[0] = x, r[1] = y, r[2] = z;
}
// C_VECHalfAngle (revolution/MTX/vec.c)
void half_angle(const float* a, const float* b, float* h) {
    float na[3] = {-a[0], -a[1], -a[2]}, nb[3] = {-b[0], -b[1], -b[2]};
    v3_norm(na);
    v3_norm(nb);
    h[0] = na[0] + nb[0], h[1] = na[1] + nb[1], h[2] = na[2] + nb[2];
    if (h[0] * h[0] + h[1] * h[1] + h[2] * h[2] > 0.0f) v3_norm(h);
}
// Camera::GetEnvironmentTexMtx con los valores por defecto de NW4R (lightScale 0.5, lightTrans 0.5).
M34 env_tex_mtx() {
    M34 r{};
    r.m[0][0] = 0.5f, r.m[0][3] = 0.5f, r.m[1][1] = -0.5f, r.m[1][3] = 0.5f, r.m[2][3] = 1.0f;
    return r;
}
// C_MTXLightPerspective(fovy, aspect, 0.5, -0.5, 0.5, 0.5) a partir de cot = 1/tan(fovy/2).
M34 light_perspective(float cot, float aspect) {
    M34 r{};
    r.m[0][0] = cot / aspect * 0.5f, r.m[0][2] = -0.5f;
    r.m[1][1] = cot * -0.5f, r.m[1][2] = -0.5f;
    r.m[2][2] = -1.0f;
    return r;
}
// C_MTXLightOrtho(t, b, l, r, 0.5, -0.5, 0.5, 0.5) para una cámara ortográfica de alto h.
M34 light_ortho(float h, float aspect) {
    const float t = h * 0.5f, b = -t, rr = t * aspect, l = -rr;
    M34 r{};
    r.m[0][0] = 2.0f / (rr - l) * 0.5f, r.m[0][3] = -(rr + l) / (rr - l) * 0.5f + 0.5f;
    r.m[1][1] = 2.0f / (t - b) * -0.5f, r.m[1][3] = -(t + b) / (t - b) * -0.5f + 0.5f;
    r.m[2][3] = 1.0f;
    return r;
}
void m34_to_gl(const M34& a, float* out) {  // mat4 columna-mayor con la fila 3 = (0, 0, 0, 1)
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 3; ++r) out[c * 4 + r] = a.m[r][c];
        out[c * 4 + 3] = c == 3 ? 1.0f : 0.0f;
    }
}
M34 zero_translation(M34 a) {
    a.m[0][3] = a.m[1][3] = a.m[2][3] = 0.0f;
    return a;
}
// Base de la matriz de entorno desde una dirección (EnvironmentMapping / EnvironmentSpecularMapping de g3d_state.cpp).
M34 look_basis(const float* look_in, bool specular) {
    float look[3] = {look_in[0], look_in[1], look_in[2]}, up_hint[3], right[3], up[3];
    v3_norm(look);
    if (std::fabs(look[0]) < 1e-6f && std::fabs(look[2]) < 1e-6f)
        up_hint[0] = up_hint[1] = 0.0f, up_hint[2] = look[1] <= 0.0f ? -1.0f : 1.0f;
    else
        up_hint[0] = up_hint[2] = 0.0f, up_hint[1] = 1.0f;
    M34 r{};
    if (!specular) {
        v3_cross(look, up_hint, right);
        v3_norm(right);
        v3_cross(right, look, up);
        for (int k = 0; k < 3; ++k) r.m[0][k] = right[k], r.m[1][k] = up[k], r.m[2][k] = -look[k];
    } else {
        v3_cross(up_hint, look, right);
        v3_norm(right);
        v3_cross(look, right, up);
        for (int k = 0; k < 3; ++k) r.m[0][k] = right[k], r.m[1][k] = up[k], r.m[2][k] = look[k];
    }
    return r;
}

// Luz del visor (sin escena): direccional blanca desde arriba a la derecha, en vista.
const float kViewerLight[3] = {0.30f, 0.55f, 0.78f};

}  // namespace

ModelRenderer::~ModelRenderer() {
    unload();
    for (auto& kv : textures_)
        if (kv.second) gl_delete(kv.second);
    for (auto& kv : programs_)
        if (kv.second.id) glDeleteProgram(kv.second.id);
    if (simple_) glDeleteProgram(simple_);
    if (white_) glDeleteTextures(1, &white_);
    if (skin_ubo_) glDeleteBuffers(1, &skin_ubo_);
}

void ModelRenderer::set_pose(const float* skin, uint32_t bones) {
    // 3 filas vec4 por hueso (std140); sin animación, la identidad (la pose de reposo).
    std::vector<float> data(size_t(kMaxBones) * 12, 0.0f);
    for (uint32_t b = 0; b < kMaxBones; ++b) {
        float* r = &data[size_t(b) * 12];
        if (skin && b < bones) {
            std::memcpy(r, skin + size_t(b) * 12, 12 * sizeof(float));
        } else {
            r[0] = r[5] = r[10] = 1.0f;
        }
    }
    glBindBuffer(GL_UNIFORM_BUFFER, skin_ubo_);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, GLsizeiptr(data.size() * sizeof(float)), data.data());
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
}

bool ModelRenderer::init() {
    std::string log;
    GLuint v = compile(GL_VERTEX_SHADER, kSimpleVS, &log);
    GLuint f = v ? compile(GL_FRAGMENT_SHADER, kSimpleFS, &log) : 0;
    if (!v || !f) {
        std::fprintf(stderr, "shader simple: %s\n", log.c_str());
        return false;
    }
    simple_ = link(v, f);
    bind_skin_block(simple_);
    glGenBuffers(1, &skin_ubo_);
    glBindBuffer(GL_UNIFORM_BUFFER, skin_ubo_);
    glBufferData(GL_UNIFORM_BUFFER, GLsizeiptr(kMaxBones * 12 * sizeof(float)), nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    set_pose(nullptr, 0);
    u_mvp_ = glGetUniformLocation(simple_, "mvp");
    u_tex_ = glGetUniformLocation(simple_, "tex");
    u_use_tex_ = glGetUniformLocation(simple_, "use_tex");
    u_vcol_ = glGetUniformLocation(simple_, "use_vcol");
    u_lit_ = glGetUniformLocation(simple_, "lit");
    u_atest_ = glGetUniformLocation(simple_, "alpha_test");
    u_light_ = glGetUniformLocation(simple_, "light");
    u_skinned_ = glGetUniformLocation(simple_, "skinned");
    const uint8_t px[4] = {255, 255, 255, 255};
    glGenTextures(1, &white_);
    glBindTexture(GL_TEXTURE_2D, white_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    return true;
}

uint32_t ModelRenderer::texture(uint64_t key) {
    if (!key) return 0;
    auto it = textures_.find(key);
    if (it != textures_.end()) return it->second;
    const PackEntry* e = pack_.find(key);
    const uint32_t id = (e && e->kind == uint8_t(Kind::Texture)) ? gl_upload(*e, pack_.data(*e), true) : 0;
    textures_[key] = id;
    return id;
}

ModelRenderer::Program ModelRenderer::build(const TevMaterialRec& m, const TevGenOptions& opt) {
    Program p;
    const std::string fs = tev_fragment_source(m, opt);
    std::string log;
    GLuint v = compile(GL_VERTEX_SHADER, tev_vertex_source(), &log);
    GLuint f = v ? compile(GL_FRAGMENT_SHADER, fs.c_str(), &log) : 0;
    if (!v || !f) {
        if (tev_failures_++ < 3) std::printf("[tev] shader con error, se usa el simple:\n%s\n%s\n", log.c_str(), fs.c_str());
        if (v) glDeleteShader(v);
        return p;
    }
    p.id = link(v, f);
    bind_skin_block(p.id);
    p.mvp = glGetUniformLocation(p.id, "mvp");
    p.skinned = glGetUniformLocation(p.id, "skinned");
    p.mv = glGetUniformLocation(p.id, "mv");
    p.light = glGetUniformLocation(p.id, "light");
    for (int i = 0; i < 8; ++i) p.tex[i] = glGetUniformLocation(p.id, ("tex" + std::to_string(i)).c_str());
    p.creg = glGetUniformLocation(p.id, "u_creg");
    p.kreg = glGetUniformLocation(p.id, "u_kreg");
    p.chmat = glGetUniformLocation(p.id, "u_chmat");
    p.chamb = glGetUniformLocation(p.id, "u_chamb");
    p.tm = glGetUniformLocation(p.id, "u_tm");
    p.tgm = glGetUniformLocation(p.id, "u_tgm");
    p.lpos = glGetUniformLocation(p.id, "u_lpos");
    p.ldir = glGetUniformLocation(p.id, "u_ldir");
    p.lcol = glGetUniformLocation(p.id, "u_lcol");
    p.lcos = glGetUniformLocation(p.id, "u_lcos");
    p.ldist = glGetUniformLocation(p.id, "u_ldist");
    p.lmask = glGetUniformLocation(p.id, "u_lmask");
    return p;
}

void ModelRenderer::unload() {
    if (vao_) {
        glDeleteVertexArrays(1, &vao_);
        glDeleteBuffers(1, &vbo_);
        glDeleteBuffers(1, &ibo_);
    }
    vao_ = vbo_ = ibo_ = 0;
    view_ = ModelView{};
}

LoadInfo ModelRenderer::load(uint32_t index) {
    const auto t0 = std::chrono::steady_clock::now();
    unload();
    LoadInfo info;
    const PackEntry& e = pack_.entry(index);
    info.name = pack_.name(e);
    if (!model_view(pack_, e, view_)) return info;
    set_pose(nullptr, 0);
    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);
    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(view_.vertex_count * sizeof(ModelVertex)), view_.vertices, GL_STATIC_DRAW);
    glGenBuffers(1, &ibo_);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(view_.index_count * 4), view_.indices, GL_STATIC_DRAW);
    const GLsizei st = sizeof(ModelVertex);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(0));
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(12));
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, st, reinterpret_cast<void*>(24));
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, st, reinterpret_cast<void*>(28));
    glVertexAttribIPointer(4, 4, GL_UNSIGNED_BYTE, st, reinterpret_cast<void*>(36));
    glVertexAttribPointer(5, 4, GL_UNSIGNED_BYTE, GL_TRUE, st, reinterpret_cast<void*>(40));
    for (GLuint a = 0; a < 6; ++a) glEnableVertexAttribArray(a);
    glBindVertexArray(0);

    gen_.has_normals = view_.flags & MODEL_HAS_NORMALS;
    gen_.has_vertex_color = view_.flags & MODEL_HAS_COLORS;
    // Valores en uniformes: animaciones y luces sin recompilar. WL_TEV_CONSTANTES=1 vuelve al modo de constantes de la
    // 0.41 (sin animaciones de material ni escena), para comparar.
    {
        const char* e = std::getenv("WL_TEV_CONSTANTES");
        gen_.dynamic = !(e && *e && std::strcmp(e, "0") != 0);
    }
    base_.assign(view_.material_count, MatState{});
    for (uint32_t m = 0; m < view_.material_count; ++m) {
        const TevMaterialRec& mat = view_.materials[m];
        MatState& st = base_[m];
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) st.creg[r][c] = mat.cregs[r][c] / 255.0f, st.kreg[r][c] = mat.kregs[r][c] / 255.0f;
        for (int ch = 0; ch < 2; ++ch) {
            const ChanRec& cr = mat.chans[ch];
            for (int c = 0; c < 4; ++c) {
                st.chmat[ch][c] = (cr.flags & 3) ? cr.mat[c] : 255.0f;   // 0..255; se normaliza al subir
                st.chamb[ch][c] = (cr.flags & 12) ? cr.amb[c] : (c == 3 ? 255.0f : 102.0f);
            }
        }
        for (int t = 0; t < 8; ++t) {
            static const float kId[8] = {1, 0, 0, 0, 0, 1, 0, 0};
            if (mat.tex_mtx_mode[t]) std::memcpy(st.tm[t], mat.tex_mtx[t], sizeof st.tm[t]);
            else std::memcpy(st.tm[t], kId, sizeof kId);
            st.map_key[t] = mat.maps[t].present ? mat.maps[t].texture_key : 0;
            if ((mat.ref_valid & 1) && mat.tex_mtx_mode[t] > 1 && (mat.cam_ref[t] >= 0 || mat.light_ref[t] >= 0))
                ++info.ref_matrices;
        }
        if ((mat.ref_valid & 1) && mat.light_set >= 0) ++info.light_set_materials;
    }
    state_ = base_;
    vis_base_.assign(view_.submesh_count, 1);
    for (uint32_t s2 = 0; s2 < view_.submesh_count; ++s2)
        if (view_.version >= 4 && (view_.submeshes[s2].vis_flags & 1)) vis_base_[s2] = 0, ++info.hidden_submeshes;
    vis_ = vis_base_;
    for (uint32_t m = 0; m < view_.material_count; ++m) {
        const TevMaterialRec& mat = view_.materials[m];
        const uint64_t h = tev_program_hash(mat, gen_);
        if (!programs_.count(h)) {
            programs_[h] = build(mat, gen_);
            ++info.new_programs;
        }
        for (int k = 0; k < 8; ++k)
            if (mat.maps[k].present && (!mat.maps[k].texture_key || !texture(mat.maps[k].texture_key)))
                ++info.missing_textures;
    }
    for (uint32_t s = 0; s < view_.submesh_count; ++s) texture(view_.submeshes[s].texture_key);
    info.vertices = view_.vertex_count;
    info.triangles = view_.index_count / 3;
    info.submeshes = view_.submesh_count;
    info.materials = view_.material_count;
    info.ok = true;
    info.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return info;
}

void ModelRenderer::draw(const float view[16], const float proj[16], const RenderOptions& opt) {
    if (!vao_) return;
    float pv[16];
    mul4(proj, view, pv);
    diag_ = SceneDiag{};
    diag_.scene = scene_ != nullptr;
    set_cache_.assign(scene_ ? scene_->set_count : 0, LightUniforms{});
    std::vector<uint8_t> counted(view_.material_count, 0);
    glBindBufferBase(GL_UNIFORM_BUFFER, kSkinBinding, skin_ubo_);
    glBindVertexArray(vao_);
    glFrontFace(GL_CW);  // GX usa sentido horario como cara frontal
    for (int pass = 0; pass < 2; ++pass) {  // 0: opacos, 1: translúcidos
        for (uint32_t s = 0; s < view_.submesh_count; ++s) {
            const ModelSubmesh& sm = view_.submeshes[s];
            if (s < vis_.size() && !vis_[s]) continue;  // VIS0 o nodo sin FLAG_VISIBLE
            const TevMaterialRec* mat =
                (view_.materials && sm.material < view_.material_count) ? &view_.materials[sm.material] : nullptr;
            const bool xlu = sm.xlu || (mat && mat->xlu);
            if (xlu != (pass == 1)) continue;
            const uint8_t cullmode = mat ? mat->cull : sm.cull;
            if (opt.cull && cullmode) {
                glEnable(GL_CULL_FACE);
                glCullFace(cullmode == 1 ? GL_FRONT : cullmode == 2 ? GL_BACK : GL_FRONT_AND_BACK);
            } else {
                glDisable(GL_CULL_FACE);
            }
            const Program* tp = nullptr;
            if (opt.tev && mat) {
                auto it = programs_.find(tev_program_hash(*mat, gen_));
                if (it != programs_.end() && it->second.id) tp = &it->second;
            }
            if (tp) {
                const GlBlendState bs = tev_blend_state(*mat);
                if (bs.blend || pass == 1) {
                    glEnable(GL_BLEND);
                    glBlendEquation(bs.subtract ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD);
                    if (bs.blend) glBlendFunc(bs.src, bs.dst);
                    else glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                } else {
                    glDisable(GL_BLEND);
                }
                if (bs.depth_test) {
                    glEnable(GL_DEPTH_TEST);
                    glDepthFunc(bs.depth_func);
                } else {
                    glDisable(GL_DEPTH_TEST);
                }
                glDepthMask(bs.depth_write ? GL_TRUE : GL_FALSE);
                glUseProgram(tp->id);
                glUniformMatrix4fv(tp->mvp, 1, GL_FALSE, pv);
                glUniform1i(tp->skinned, (view_.flags & MODEL_SKINNED) ? 1 : 0);
                glUniformMatrix4fv(tp->mv, 1, GL_FALSE, view);
                glUniform3f(tp->light, kViewerLight[0], kViewerLight[1], kViewerLight[2]);
                const MatState& st = state_[sm.material];
                if (gen_.dynamic) {
                    float c[2][4];
                    glUniform4fv(tp->creg, 4, &st.creg[0][0]);
                    glUniform4fv(tp->kreg, 4, &st.kreg[0][0]);
                    glUniform4fv(tp->tm, 16, &st.tm[0][0]);
                    // Luces: las del conjunto del material en la escena, o la del visor.
                    LightUniforms vl;
                    const LightUniforms* lu = nullptr;
                    const bool use_scene = scene_ && (mat->ref_valid & 1);
                    if (use_scene && mat->light_set >= 0) {
                        const int si = scene_->find_set(mat->light_set);
                        if (si >= 0) {
                            if (!set_cache_[size_t(si)].valid) light_uniforms(si, view, set_cache_[size_t(si)]);
                            lu = &set_cache_[size_t(si)];
                        } else if (!counted[sm.material]) {
                            ++diag_.missing_set;
                        }
                    }
                    if (!lu && use_scene) {  // NW4R: sin conjunto, sin luces y ambiente blanco
                        vl = LightUniforms{};
                        for (int k = 0; k < 4; ++k) vl.amb[k] = 255.0f;
                        vl.valid = true;
                        lu = &vl;
                    }
                    if (!lu) {
                        viewer_lights(*mat, vl);
                        lu = &vl;
                    }
                    if (!counted[sm.material]) {
                        counted[sm.material] = 1;
                        if (scene_ && use_scene && mat->light_set >= 0 && lu != &vl) ++diag_.lit_materials;
                        else if (!use_scene) ++diag_.viewer_light;
                    }
                    float am[2][4];
                    for (int ch = 0; ch < 2; ++ch) {
                        const bool amb_on = mat->chans[ch].flags & 12;
                        for (int k = 0; k < 4; ++k) {
                            c[ch][k] = st.chmat[ch][k] / 255.0f;
                            // LoadResMatChan: en COLOR0, ambiente = 0.5 + luz ambiente del conjunto * ambiente / 255
                            const float a = (use_scene && amb_on && ch == 0)
                                                ? std::floor(0.5f + lu->amb[k] * st.chamb[ch][k] / 255.0f)
                                                : st.chamb[ch][k];
                            am[ch][k] = a / 255.0f;
                        }
                    }
                    glUniform4fv(tp->chmat, 2, &c[0][0]);
                    glUniform4fv(tp->chamb, 2, &am[0][0]);
                    glUniform4fv(tp->lpos, 4, &lu->pos[0][0]);
                    glUniform4fv(tp->ldir, 4, &lu->dir[0][0]);
                    glUniform4fv(tp->lcol, 4, &lu->col[0][0]);
                    glUniform4fv(tp->lcos, 4, &lu->cos[0][0]);
                    glUniform4fv(tp->ldist, 4, &lu->dist[0][0]);
                    glUniform4i(tp->lmask, lu->mask[0], lu->mask[1], lu->mask[2], lu->mask[3]);
                    if (tp->tgm >= 0) {
                        float tg[8][16];
                        for (int k = 0; k < 8; ++k) scene_matrix(*mat, k, view, proj, tg[k]);
                        glUniformMatrix4fv(tp->tgm, 8, GL_FALSE, &tg[0][0]);
                    }
                }
                for (int k = 0; k < 8; ++k) {
                    if (tp->tex[k] < 0) continue;
                    const TevMapRec& mp = mat->maps[k];
                    const uint64_t key = st.map_key[k];  // la del material o la que puso una PAT0
                    const GLuint t = (mp.present && key) ? texture(key) : 0;
                    glActiveTexture(GL_TEXTURE0 + k);
                    glBindTexture(GL_TEXTURE_2D, t ? t : white_);
                    if (t) {
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap_mode(mp.wrap_s));
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap_mode(mp.wrap_t));
                    }
                    glUniform1i(tp->tex[k], k);
                }
            } else {
                glEnable(GL_DEPTH_TEST);
                glDepthFunc(GL_LEQUAL);
                if (pass == 1) {
                    glEnable(GL_BLEND);
                    glBlendEquation(GL_FUNC_ADD);
                    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                    glDepthMask(GL_FALSE);
                } else {
                    glDisable(GL_BLEND);
                    glDepthMask(GL_TRUE);
                }
                glUseProgram(simple_);
                glUniformMatrix4fv(u_mvp_, 1, GL_FALSE, pv);
                glUniform1i(u_skinned_, (view_.flags & MODEL_SKINNED) ? 1 : 0);
                glUniform3f(u_light_, 0.40f, 0.80f, 0.45f);
                glUniform1i(u_vcol_, gen_.has_vertex_color ? 1 : 0);
                glUniform1i(u_lit_, (opt.lighting && gen_.has_normals) ? 1 : 0);
                const GLuint t = opt.textures ? texture(sm.texture_key) : 0;
                glUniform1i(u_use_tex_, t ? 1 : 0);
                glUniform1i(u_atest_, pass == 0 ? 1 : 0);
                glUniform1i(u_tex_, 0);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, t);
                if (t) {
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap_mode(sm.wrap_s));
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap_mode(sm.wrap_t));
                }
            }
            glDrawElements(GL_TRIANGLES, GLsizei(sm.count), GL_UNSIGNED_INT,
                           reinterpret_cast<void*>(uintptr_t(sm.first) * 4));
        }
    }
    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
}

void ModelRenderer::reset_material_state() {
    state_ = base_;
    vis_ = vis_base_;
}

uint64_t ModelRenderer::map_key(uint32_t material, int unit) const {
    return material < state_.size() && unit >= 0 && unit < 8 ? state_[material].map_key[unit] : 0;
}

int ModelRenderer::prepare_textures(const MatAnimView& a) {
    int n = 0;
    if (a.type != MATANIM_PAT) return 0;
    for (uint32_t i = 0; i < a.key_count; ++i)
        if (texture(a.keys[i])) ++n;
    return n;
}

void ModelRenderer::apply_mat_anim(const MatAnimView& a, uint32_t frame) {
    if (!a.frames) return;
    const uint32_t f = a.loop ? frame % a.frames : std::min(frame, a.frames - 1);
    for (uint32_t i = 0; i < a.track_count; ++i) {
        const MatAnimTrack& t = a.tracks[i];
        const void* v = a.value(t, f);
        if (a.type == MATANIM_VIS) {
            if (t.target < vis_.size()) vis_[t.target] = *static_cast<const uint8_t*>(v) ? 1 : 0;
            continue;
        }
        if (t.target >= state_.size()) continue;
        MatState& st = state_[t.target];
        switch (a.type) {
            case MATANIM_SRT:
                if (t.slot < 8) std::memcpy(st.tm[t.slot], v, sizeof st.tm[t.slot]);
                break;
            case MATANIM_CLR: {
                const uint8_t* c = static_cast<const uint8_t*>(v);
                float* dst = nullptr;
                bool unit = false;  // registros TEV y konstantes en 0..1; canales en 0..255
                if (t.slot <= 1) dst = st.chmat[t.slot];
                else if (t.slot <= 3) dst = st.chamb[t.slot - 2];
                else if (t.slot <= 6) dst = st.creg[t.slot - 3], unit = true;  // TEV0..2 = C0..C2
                else if (t.slot <= 10) dst = st.kreg[t.slot - 7], unit = true;
                if (dst)
                    for (int k = 0; k < 4; ++k) dst[k] = unit ? c[k] / 255.0f : float(c[k]);
                break;
            }
            case MATANIM_PAT: {
                const uint16_t idx = *static_cast<const uint16_t*>(v);
                if (t.slot < 8 && idx != 0xFFFF && idx < a.key_count) st.map_key[t.slot] = a.keys[idx];
                break;
            }
            default: break;
        }
    }
}

void ModelRenderer::set_scene(const SceneView* scene, uint32_t frame) {
    scene_ = scene;
    scene_frame_ = scene && scene->frames ? (scene->loop ? frame % scene->frames : std::min(frame, scene->frames - 1))
                                          : 0;
}

void ModelRenderer::viewer_lights(const TevMaterialRec& m, LightUniforms& out) {
    out = LightUniforms{};
    out.pos[0][0] = kViewerLight[0], out.pos[0][1] = kViewerLight[1], out.pos[0][2] = kViewerLight[2];
    out.pos[0][3] = 0.0f;  // direccional
    for (int k = 0; k < 3; ++k) out.dir[0][k] = kViewerLight[k];
    for (int k = 0; k < 4; ++k) out.col[0][k] = 1.0f, out.amb[k] = 255.0f;
    out.cos[0][0] = 1.0f, out.dist[0][0] = 1.0f;
    for (int i = 1; i < 4; ++i) out.dist[i][0] = 1.0f;
    // Como antes de las escenas: la luz se enciende donde el material tiene alguna luz en su máscara.
    auto has = [](uint32_t ctrl) { return (((ctrl >> 2) & 0xF) | (((ctrl >> 11) & 0xF) << 4)) ? 1 : 0; };
    out.mask[0] = has(m.chans[0].ctrl_color);
    out.mask[1] = has(m.chans[0].ctrl_alpha);
    out.mask[2] = has(m.chans[1].ctrl_color);
    out.mask[3] = has(m.chans[1].ctrl_alpha);
    out.valid = true;
}

void ModelRenderer::light_uniforms(int set_index, const float view[16], LightUniforms& out) {
    out = LightUniforms{};
    for (int i = 0; i < 4; ++i) out.dist[i][0] = 1.0f;
    for (int k = 0; k < 4; ++k) out.amb[k] = 255.0f;
    const SceneLightSet& set = scene_->sets[set_index];
    if (set.amb >= 0) {
        const uint8_t* c = scene_->amb_color(uint32_t(set.amb), scene_frame_);
        for (int k = 0; k < 4; ++k) out.amb[k] = c[k];
    }
    const M34 v = m34_from_gl(view);
    // Las luces del conjunto y, detrás de cada difusa, su especular (como las carga LoadLightSet).
    int list[16], n = 0;
    for (int i = 0; i < set.count && i < 8; ++i) {
        const int li = set.light[i];
        if (li < 0) continue;
        list[n++] = li;
        if (scene_->lights[li].spec >= 0) list[n++] = scene_->lights[li].spec;
    }
    int slot = 0, over = 0;
    for (int i = 0; i < n; ++i) {
        const SceneLight& L = scene_->lights[list[i]];
        const SceneLightFrame& F = scene_->light(uint32_t(list[i]), scene_frame_);
        if (!F.enabled) continue;
        if (slot >= kMaxLights) {
            ++over;
            continue;
        }
        float dirv[3];
        m34_rot(v, F.dir, dirv);
        v3_norm(dirv);
        float* p = out.pos[slot];
        float* d = out.dir[slot];
        if (L.type == 1 || L.type == 3) {  // direccional / especular: hacia la luz, sin posición
            p[0] = -dirv[0], p[1] = -dirv[1], p[2] = -dirv[2], p[3] = 0.0f;
        } else {
            m34_point(v, F.pos, p);
            p[3] = 1.0f;
        }
        if (L.type == 3) {  // GXInitSpecularDir: vector medio con la vista
            float h[3] = {-dirv[0], -dirv[1], 1.0f - dirv[2]};
            v3_norm(h);
            d[0] = h[0], d[1] = h[1], d[2] = h[2];
        } else {  // GXInitLightDir guarda la dirección negada
            d[0] = -dirv[0], d[1] = -dirv[1], d[2] = -dirv[2];
        }
        for (int k = 0; k < 4; ++k) out.col[slot][k] = F.color[k] / 255.0f;
        for (int k = 0; k < 3; ++k) out.cos[slot][k] = F.a[k], out.dist[slot][k] = F.k[k];
        const int bit = 1 << slot;
        if (L.type == 3) {
            if (L.flags & 1) out.mask[2] |= bit;
            if (L.flags & 2) out.mask[3] |= bit;
        } else {
            if (L.flags & 1) out.mask[0] |= bit;
            if (L.flags & 2) out.mask[1] |= bit;
        }
        ++slot;
    }
    if (over) ++diag_.over_limit;
    out.valid = true;
}

// Matriz de la escena de una coordenada (ScnDependentMtxFunc de g3d_state.cpp): entorno (1, 3), proyección (2) y
// entorno especular (4), con la cámara (ref_camera) o la luz (ref_light) de la escena si hay. Sin escena, la cámara
// del visor y su luz, contado en el diagnóstico.
void ModelRenderer::scene_matrix(const TevMaterialRec& m, int slot, const float view[16], const float proj[16],
                                 float out[16]) {
    const unsigned mode = m.tex_mtx_mode[slot] ? m.tex_mtx_mode[slot] - 1 : 0;
    const M34 cam = m34_from_gl(view), inv = m34_rigid_inverse(cam);
    const bool refs = m.ref_valid & 1;
    const int cam_ref = refs ? m.cam_ref[slot] : -1, light_ref = refs ? m.light_ref[slot] : -1;
    int sc = -1, sl = -1;
    if (cam_ref >= 0) {
        sc = scene_ ? scene_->find_camera(cam_ref) : -1;
        (sc >= 0 ? diag_.ref_camera_scene : diag_.ref_camera_fallback)++;
    }
    if (light_ref >= 0 && (mode == 1 || mode == 3 || mode == 4)) {
        sl = scene_ ? scene_->find_light(light_ref) : -1;
        if (sl >= 0 && !scene_->light(uint32_t(sl), scene_frame_).enabled) sl = -1;
        (sl >= 0 ? diag_.ref_light_scene : diag_.ref_light_fallback)++;
    }
    auto ref_cam = [&](int i) {
        const SceneCameraFrame& f = scene_->camera(uint32_t(i), scene_frame_);
        M34 r;
        std::memcpy(r.m, f.view, sizeof r.m);
        return r;
    };
    // Luz de referencia en el mundo: la mirada de EnvironmentMapping (de la luz hacia la escena).
    auto light_look = [&](float look[3]) {
        if (sl >= 0) {
            const SceneLight& L = scene_->lights[sl];
            const SceneLightFrame& F = scene_->light(uint32_t(sl), scene_frame_);
            if (L.type == 0) {  // luz difusa (punto): -(InvCam · posición en vista) = cámara - posición, en el mundo
                look[0] = inv.m[0][3] - F.pos[0], look[1] = inv.m[1][3] - F.pos[1], look[2] = inv.m[2][3] - F.pos[2];
            } else {
                look[0] = F.dir[0], look[1] = F.dir[1], look[2] = F.dir[2];
            }
        } else {  // la del visor, que está en vista
            const float d[3] = {-kViewerLight[0], -kViewerLight[1], -kViewerLight[2]};
            m34_rot(inv, d, look);
        }
        if (look[0] == 0.0f && look[1] == 0.0f && look[2] == 0.0f) look[1] = -1.0f;
    };
    M34 r = m34_identity();
    if (mode == 1 || mode == 3) {
        if (sc >= 0) {
            r = m34_mul(env_tex_mtx(), zero_translation(m34_mul(ref_cam(sc), inv)));
        } else if (light_ref >= 0) {
            float look[3];
            light_look(look);
            r = m34_mul(env_tex_mtx(), zero_translation(m34_mul(look_basis(look, false), inv)));
        } else {
            r = env_tex_mtx();
        }
    } else if (mode == 2) {
        if (sc >= 0) {
            const SceneCameraFrame& f = scene_->camera(uint32_t(sc), scene_frame_);
            const M34 lp = scene_->cameras[sc].proj == 1
                               ? light_ortho(f.fovy_or_height, f.aspect)
                               : light_perspective(1.0f / std::tan(f.fovy_or_height * 0.5f * 3.14159265f / 180.0f),
                                                   f.aspect);
            r = m34_mul(lp, m34_mul(ref_cam(sc), inv));
        } else {  // la proyección del visor (cot y aspecto de su matriz)
            r = light_perspective(proj[5], proj[5] / (proj[0] != 0.0f ? proj[0] : 1.0f));
        }
    } else if (mode == 4) {
        float cam_look[3] = {-cam.m[2][0], -cam.m[2][1], -cam.m[2][2]}, look[3];
        bool done = false;
        if (light_ref >= 0) {
            float ll[3];
            light_look(ll);
            const bool spec = sl >= 0 && scene_->lights[sl].type == 3;
            if (!spec) half_angle(cam_look, ll, look);
            else look[0] = -ll[0], look[1] = -ll[1], look[2] = -ll[2];
            done = true;
        } else if (sc >= 0) {
            const M34 rc = ref_cam(sc);
            const float ll[3] = {-rc.m[2][0], -rc.m[2][1], -rc.m[2][2]};
            half_angle(cam_look, ll, look);
            done = true;
        }
        r = done ? m34_mul(env_tex_mtx(), zero_translation(m34_mul(look_basis(look, true), inv))) : env_tex_mtx();
    }
    m34_to_gl(r, out);
}

std::string SceneDiag::text() const {
    char b[320];
    std::snprintf(b, sizeof b,
                  "[escena] %s; materiales con sus luces: %d, con la luz del visor: %d, conjunto que falta: %d, "
                  "más de %d luces: %d; ref_camera escena/visor: %d/%d; ref_light escena/visor: %d/%d",
                  scene ? "SCN0 activa" : "sin escena (luz y cámara del visor, no es la del juego)", lit_materials,
                  viewer_light, missing_set, kMaxLights, over_limit, ref_camera_scene, ref_camera_fallback,
                  ref_light_scene, ref_light_fallback);
    return b;
}

}  // namespace wl
