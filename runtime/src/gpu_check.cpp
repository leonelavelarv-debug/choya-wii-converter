#include "wl/gpu_check.h"

#include <GLES3/gl3.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "wl/format.h"
#include "wl/texcache.h"

namespace wl {

namespace {

const char* kVS = "#version 300 es\nvoid main() {\n"
                  "  vec2 p = vec2(float((gl_VertexID & 1) * 4 - 1), float((gl_VertexID & 2) * 2 - 1));\n"
                  "  gl_Position = vec4(p, 0.0, 1.0);\n}\n";
// mode 0: el texel tal cual; mode 1: descarte con alfa < 0.5 (lo que sobrevive sale blanco).
const char* kFS = "#version 300 es\nprecision highp float;\nuniform highp sampler2D t;\nuniform int lod;\n"
                  "uniform int mode;\nout vec4 c;\n"
                  "void main() {\n  vec4 v = texelFetch(t, ivec2(gl_FragCoord.xy), lod);\n"
                  "  if (mode == 1) { if (v.a < 0.5) discard; c = vec4(1.0); } else { c = v; }\n}\n";

GLuint shader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    return s;
}

GLenum internal_of(OutFormat f) {
    switch (f) {
        case OutFormat::ETC2_RGB8: return 0x9274;
        case OutFormat::ETC2_RGB8A1: return 0x9276;
        case OutFormat::ETC2_RGBA8: return 0x9278;
        case OutFormat::R11_EAC: return 0x9270;
        case OutFormat::RG11_EAC: return 0x9272;
        default: return 0;
    }
}

struct Drawer {
    GLuint prog = 0, vao = 0, fbo = 0;
    GLint u_t = -1, u_lod = -1, u_mode = -1;
    // Dibuja el nivel `lod` de `tex` en un destino RGBA8 de w x h y lo lee. clear: color inicial (0 = negro).
    bool run(GLuint tex, int lod, uint32_t w, uint32_t h, int mode, std::vector<uint8_t>& out) {
        GLuint dst = 0;
        glGenTextures(1, &dst);
        glBindTexture(GL_TEXTURE_2D, dst);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, GLsizei(w), GLsizei(h));
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst, 0);
        bool ok = glGetError() == GL_NO_ERROR && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        if (ok) {
            glViewport(0, 0, GLsizei(w), GLsizei(h));
            glClearColor(0, 0, 0, 0);
            glClear(GL_COLOR_BUFFER_BIT);
            glUseProgram(prog);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, tex);
            glUniform1i(u_t, 0);
            glUniform1i(u_lod, lod);
            glUniform1i(u_mode, mode);
            glBindVertexArray(vao);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            out.assign(size_t(w) * h * 4, 0);
            glReadPixels(0, 0, GLsizei(w), GLsizei(h), GL_RGBA, GL_UNSIGNED_BYTE, out.data());
            ok = glGetError() == GL_NO_ERROR;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteTextures(1, &dst);
        return ok;
    }
};

// Peor diferencia entre la GPU y lo esperado; where = índice del byte.
int worst_diff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, size_t* where) {
    int worst = 0;
    for (size_t k = 0; k < a.size() && k < b.size(); ++k) {
        const int d = std::abs(int(a[k]) - int(b[k]));
        if (d > worst) worst = d, *where = k;
    }
    return worst;
}

// Lo que se espera leer de la textura de respaldo (formatos de luminancia: L,L,L,A).
void expected_fallback(OutFormat f, uint8_t flags, const std::vector<uint8_t>& raw, std::vector<uint8_t>& out) {
    out = raw;
    for (size_t i = 0; i + 3 < out.size(); i += 4) {
        uint8_t* p = &out[i];
        if (f == OutFormat::R11_EAC && (flags & TEXFLAG_SWIZZLE_I)) p[1] = p[2] = p[3] = p[0];
        else if (f == OutFormat::RG11_EAC && (flags & TEXFLAG_SWIZZLE_IA)) p[3] = p[1], p[1] = p[2] = p[0];
    }
}

}  // namespace

GpuCheckResult gpu_check_textures(const Pack& pack, int max_textures, std::string* report) {
    GpuCheckResult r;
    Drawer dr;
    dr.prog = glCreateProgram();
    GLuint vs = shader(GL_VERTEX_SHADER, kVS), fs = shader(GL_FRAGMENT_SHADER, kFS);
    glAttachShader(dr.prog, vs);
    glAttachShader(dr.prog, fs);
    glLinkProgram(dr.prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint linked = 0;
    glGetProgramiv(dr.prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        r.first_problem = "no se pudo compilar el sombreador de la prueba";
        glDeleteProgram(dr.prog);
        return r;
    }
    dr.u_t = glGetUniformLocation(dr.prog, "t");
    dr.u_lod = glGetUniformLocation(dr.prog, "lod");
    dr.u_mode = glGetUniformLocation(dr.prog, "mode");
    glGenVertexArrays(1, &dr.vao);
    glGenFramebuffers(1, &dr.fbo);
    GLint prev_fbo = 0, vp[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_VIEWPORT, vp);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    GlCaps caps = gl_query_caps();
    GlCaps no_etc = caps;
    no_etc.etc2 = false;
    no_etc.force_decode = true;
    int route_native[16] = {}, route_decoded[16] = {};
    std::vector<uint8_t> gpu, cpu, want;
    auto problem = [&](const PackEntry& e, OutFormat f, int level, uint32_t w, size_t where, int g, int c,
                       const char* route) {
        if (!r.first_problem.empty()) return;
        char buf[320];
        const size_t px = where / 4;
        std::snprintf(buf, sizeof buf, "%s (%s %ux%u) ruta %s, nivel %d: píxel (%zu, %zu) canal %zu: GPU %d, CPU %d",
                      pack.name(e), out_format_name(f), e.width, e.height, route, level, px % w, px / w, where % 4,
                      g, c);
        r.first_problem = buf;
    };
    for (uint32_t i = 0; i < pack.size(); ++i) {
        if (max_textures && r.checked >= max_textures) break;
        const PackEntry& e = pack.entry(i);
        if (e.kind != uint8_t(Kind::Texture)) continue;
        const OutFormat f = OutFormat(e.format);
        const GLenum internal = internal_of(f);
        if (!internal || e.width == 0 || e.height == 0) continue;
        const int mips = e.count ? e.count : 1;
        // Niveles completos y su decodificación de referencia.
        std::vector<std::vector<uint8_t>> ref;
        std::vector<size_t> offs;
        {
            size_t off = 0;
            for (int level = 0; level < mips; ++level) {
                const uint32_t w = e.width >> level ? e.width >> level : 1;
                const uint32_t h = e.height >> level ? e.height >> level : 1;
                const size_t size = out_level_size(f, w, h);
                if (off + size > e.data_size) break;
                std::vector<uint8_t> px(size_t(w) * h * 4, 0);
                if (!decode_block_level(f, pack.data(e) + off, size, w, h, px.data())) break;
                ref.push_back(std::move(px));
                offs.push_back(off);
                off += size;
            }
        }
        if (ref.empty()) {
            ++r.skipped;
            continue;
        }
        const bool eleven = f == OutFormat::R11_EAC || f == OutFormat::RG11_EAC;
        bool bad = false;
        int worst_all = 0;
        // 1. Ruta nativa
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        while (glGetError() != GL_NO_ERROR) {
        }
        bool rejected = false;
        for (size_t level = 0; level < ref.size(); ++level) {
            const uint32_t w = e.width >> level ? e.width >> level : 1;
            const uint32_t h = e.height >> level ? e.height >> level : 1;
            glCompressedTexImage2D(GL_TEXTURE_2D, GLint(level), internal, GLsizei(w), GLsizei(h), 0,
                                   GLsizei(out_level_size(f, w, h)), pack.data(e) + offs[level]);
            if (glGetError() != GL_NO_ERROR) rejected = true;
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, GLint(ref.size()) - 1);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        if (rejected) {
            ++r.native_rejected;
            if (r.first_problem.empty())
                r.first_problem = std::string(pack.name(e)) + ": el driver rechazó " + out_format_name(f);
            bad = true;
        } else {
            route_native[int(f) & 15]++;
            for (size_t level = 0; level < ref.size(); ++level) {
                const uint32_t w = e.width >> level ? e.width >> level : 1;
                const uint32_t h = e.height >> level ? e.height >> level : 1;
                if (!dr.run(tex, int(level), w, h, 0, gpu)) {
                    bad = true;
                    break;
                }
                ++r.levels;
                size_t where = 0;
                const int worst = worst_diff(gpu, ref[level], &where);
                if (worst > worst_all) worst_all = worst;
                if (worst > (eleven ? 1 : 0)) {
                    bad = true;
                    problem(e, f, int(level), w, where, gpu[where], ref[level][where], "nativa");
                }
            }
            // 3. RGB8A1: el descarte por alfa deja justo los píxeles opacos (nivel 0).
            if (f == OutFormat::ETC2_RGB8A1 && dr.run(tex, 0, e.width, e.height, 1, gpu)) {
                ++r.punch_textures;
                int expect = 0, got = 0;
                for (size_t k = 0; k < ref[0].size(); k += 4) {
                    expect += ref[0][k + 3] >= 128;
                    got += gpu[k] == 255;
                    r.punch_transparent += ref[0][k + 3] < 128;
                }
                if (expect != got) {
                    ++r.punch_discard_failed;
                    if (r.first_problem.empty()) {
                        char buf[200];
                        std::snprintf(buf, sizeof buf, "%s: con descarte por alfa quedan %d píxeles, se esperaban %d",
                                      pack.name(e), got, expect);
                        r.first_problem = buf;
                    }
                }
            }
        }
        glDeleteTextures(1, &tex);
        // 2. Ruta de respaldo (como en un contexto sin ETC2)
        const UploadResult up = gl_upload_ex(e, pack.data(e), no_etc);
        if (up.tex) {
            ++r.fallback_checked;
            route_decoded[int(f) & 15]++;
            glBindTexture(GL_TEXTURE_2D, up.tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
            bool fb_bad = up.levels != int(ref.size());
            for (int level = 0; level < up.levels && !fb_bad; ++level) {
                const uint32_t w = e.width >> level ? e.width >> level : 1;
                const uint32_t h = e.height >> level ? e.height >> level : 1;
                expected_fallback(f, e.flags, ref[size_t(level)], want);
                if (!dr.run(up.tex, level, w, h, 0, gpu)) {
                    fb_bad = true;
                    break;
                }
                size_t where = 0;
                if (worst_diff(gpu, want, &where) > 0) {
                    fb_bad = true;
                    problem(e, f, level, w, where, gpu[where], want[where], "de respaldo");
                }
            }
            if (fb_bad) ++r.fallback_failed;
            gl_delete(up.tex);
        } else {
            ++r.fallback_failed;
            if (r.first_problem.empty())
                r.first_problem = std::string(pack.name(e)) + ": la ruta de respaldo falló (" + up.reason + ")";
        }
        ++r.checked;
        r.by_format[int(f) & 15]++;
        if (!bad && worst_all == 0) {
            ++r.exact;
        } else if (!bad) {
            ++r.close;
        } else {
            ++r.failed;
            r.bad_by_format[int(f) & 15]++;
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, GLuint(prev_fbo));
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glDeleteFramebuffers(1, &dr.fbo);
    glDeleteVertexArrays(1, &dr.vao);
    glDeleteProgram(dr.prog);
    if (report) {
        *report += gl_caps_report(caps);
        for (int f = 0; f < 16; ++f) {
            if (!r.by_format[f]) continue;
            char buf[200];
            std::snprintf(buf, sizeof buf, "  %-16s %4d revisadas (nativa %d, respaldo %d), %d con diferencias\n",
                          out_format_name(OutFormat(f)), r.by_format[f], route_native[f], route_decoded[f],
                          r.bad_by_format[f]);
            *report += buf;
        }
        char buf[300];
        std::snprintf(buf, sizeof buf,
                      "niveles revisados por la ruta nativa: %d; rechazadas por el driver: %d\n"
                      "ruta de respaldo: %d revisadas, %d con diferencias\n"
                      "RGB8A1: %d texturas, %d píxeles transparentes, %d con descarte distinto\n",
                      r.levels, r.native_rejected, r.fallback_checked, r.fallback_failed, r.punch_textures,
                      r.punch_transparent, r.punch_discard_failed);
        *report += buf;
    }
    return r;
}

}  // namespace wl
