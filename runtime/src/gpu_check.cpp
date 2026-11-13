#include "wl/gpu_check.h"

#include <GLES3/gl3.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "wl/format.h"

namespace wl {

namespace {

const char* kVS = "#version 300 es\nvoid main() {\n"
                  "  vec2 p = vec2(float((gl_VertexID & 1) * 4 - 1), float((gl_VertexID & 2) * 2 - 1));\n"
                  "  gl_Position = vec4(p, 0.0, 1.0);\n}\n";
const char* kFS = "#version 300 es\nprecision highp float;\nuniform highp sampler2D t;\nout vec4 c;\n"
                  "void main() { c = texelFetch(t, ivec2(gl_FragCoord.xy), 0); }\n";

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

}  // namespace

GpuCheckResult gpu_check_textures(const Pack& pack, int max_textures, std::string* report) {
    GpuCheckResult r;
    GLuint prog = glCreateProgram();
    GLuint vs = shader(GL_VERTEX_SHADER, kVS), fs = shader(GL_FRAGMENT_SHADER, kFS);
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        r.first_problem = "no se pudo compilar el sombreador de la prueba";
        glDeleteProgram(prog);
        return r;
    }
    GLuint vao = 0, fbo = 0;
    glGenVertexArrays(1, &vao);
    glGenFramebuffers(1, &fbo);
    GLint prev_fbo = 0, vp[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_VIEWPORT, vp);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    std::vector<uint8_t> gpu, cpu;
    for (uint32_t i = 0; i < pack.size(); ++i) {
        if (max_textures && r.checked >= max_textures) break;
        const PackEntry& e = pack.entry(i);
        if (e.kind != uint8_t(Kind::Texture)) continue;
        const OutFormat f = OutFormat(e.format);
        const GLenum internal = internal_of(f);
        if (!internal || e.width == 0 || e.height == 0) continue;
        const uint32_t w = e.width, h = e.height;
        const size_t size = out_level_size(f, w, h);
        cpu.assign(size_t(w) * h * 4, 0);
        if (size > e.data_size || !decode_block_level(f, pack.data(e), size, w, h, cpu.data())) {
            ++r.skipped;
            continue;
        }
        GLuint tex = 0, dst = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glCompressedTexImage2D(GL_TEXTURE_2D, 0, internal, GLsizei(w), GLsizei(h), 0, GLsizei(size), pack.data(e));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glGenTextures(1, &dst);
        glBindTexture(GL_TEXTURE_2D, dst);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, GLsizei(w), GLsizei(h));
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst, 0);
        if (glGetError() != GL_NO_ERROR || glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            ++r.skipped;
            glDeleteTextures(1, &tex);
            glDeleteTextures(1, &dst);
            continue;
        }
        glViewport(0, 0, GLsizei(w), GLsizei(h));
        glUseProgram(prog);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glUniform1i(glGetUniformLocation(prog, "t"), 0);
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        gpu.assign(size_t(w) * h * 4, 0);
        glReadPixels(0, 0, GLsizei(w), GLsizei(h), GL_RGBA, GL_UNSIGNED_BYTE, gpu.data());
        // R11/RG11 pasan por 11 bits: la GPU puede redondear distinto al pasar a 8 (diferencia de 1).
        const bool eleven = f == OutFormat::R11_EAC || f == OutFormat::RG11_EAC;
        int worst = 0;
        size_t where = 0;
        for (size_t k = 0; k < gpu.size(); ++k) {
            const int d = std::abs(int(gpu[k]) - int(cpu[k]));
            if (d > worst) worst = d, where = k;
        }
        ++r.checked;
        r.by_format[int(f) & 15]++;
        if (worst == 0) {
            ++r.exact;
        } else if (eleven && worst <= 1) {
            ++r.close;
        } else {
            ++r.failed;
            r.bad_by_format[int(f) & 15]++;
            if (r.first_problem.empty()) {
                char buf[256];
                const size_t px = where / 4;
                std::snprintf(buf, sizeof buf, "%s (%s %ux%u): píxel (%zu, %zu) canal %zu: GPU %d, CPU %d",
                              pack.name(e), out_format_name(f), w, h, px % w, px / w, where % 4, int(gpu[where]),
                              int(cpu[where]));
                r.first_problem = buf;
            }
        }
        glDeleteTextures(1, &tex);
        glDeleteTextures(1, &dst);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, GLuint(prev_fbo));
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glDeleteFramebuffers(1, &fbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(prog);
    if (report) {
        for (int f = 0; f < 16; ++f) {
            if (!r.by_format[f]) continue;
            char buf[128];
            std::snprintf(buf, sizeof buf, "  %-16s %4d revisadas, %d con diferencias\n", out_format_name(OutFormat(f)),
                          r.by_format[f], r.bad_by_format[f]);
            *report += buf;
        }
    }
    return r;
}

}  // namespace wl
