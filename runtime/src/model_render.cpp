#include "wl/model_render.h"

#include <GLES3/gl3.h>

#include <chrono>
#include <cstdio>

#include "wl/texcache.h"

namespace wl {

namespace {

const char* kSimpleVS = R"(#version 300 es
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

}  // namespace

ModelRenderer::~ModelRenderer() {
    unload();
    for (auto& kv : textures_)
        if (kv.second) gl_delete(kv.second);
    for (auto& kv : programs_)
        if (kv.second.id) glDeleteProgram(kv.second.id);
    if (simple_) glDeleteProgram(simple_);
    if (white_) glDeleteTextures(1, &white_);
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
    u_mvp_ = glGetUniformLocation(simple_, "mvp");
    u_tex_ = glGetUniformLocation(simple_, "tex");
    u_use_tex_ = glGetUniformLocation(simple_, "use_tex");
    u_vcol_ = glGetUniformLocation(simple_, "use_vcol");
    u_lit_ = glGetUniformLocation(simple_, "lit");
    u_atest_ = glGetUniformLocation(simple_, "alpha_test");
    u_light_ = glGetUniformLocation(simple_, "light");
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
    p.mvp = glGetUniformLocation(p.id, "mvp");
    p.mv = glGetUniformLocation(p.id, "mv");
    p.light = glGetUniformLocation(p.id, "light");
    for (int i = 0; i < 8; ++i) p.tex[i] = glGetUniformLocation(p.id, ("tex" + std::to_string(i)).c_str());
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
    for (GLuint a = 0; a < 4; ++a) glEnableVertexAttribArray(a);
    glBindVertexArray(0);

    gen_.has_normals = view_.flags & MODEL_HAS_NORMALS;
    gen_.has_vertex_color = view_.flags & MODEL_HAS_COLORS;
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
    glBindVertexArray(vao_);
    glFrontFace(GL_CW);  // GX usa sentido horario como cara frontal
    for (int pass = 0; pass < 2; ++pass) {  // 0: opacos, 1: translúcidos
        for (uint32_t s = 0; s < view_.submesh_count; ++s) {
            const ModelSubmesh& sm = view_.submeshes[s];
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
                glUniformMatrix4fv(tp->mv, 1, GL_FALSE, view);
                glUniform3f(tp->light, 0.30f, 0.55f, 0.78f);
                for (int k = 0; k < 8; ++k) {
                    if (tp->tex[k] < 0) continue;
                    const TevMapRec& mp = mat->maps[k];
                    const GLuint t = (mp.present && mp.texture_key) ? texture(mp.texture_key) : 0;
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

}  // namespace wl
