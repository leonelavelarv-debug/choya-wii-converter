// Subida de texturas del paquete a OpenGL ES 3.x (ETC2/EAC) o 2.0 (formatos sin comprimir).
#include <GLES3/gl3.h>

#include "wl/texcache.h"

namespace wl {

namespace {

struct GLFmt {
    bool compressed;
    GLenum internal;
    GLenum format;
    GLenum type;
};

GLFmt gl_format(OutFormat f) {
    switch (f) {
        case OutFormat::ETC2_RGB8: return {true, 0x9274 /*GL_COMPRESSED_RGB8_ETC2*/, 0, 0};
        case OutFormat::ETC2_RGBA8: return {true, 0x9278 /*GL_COMPRESSED_RGBA8_ETC2_EAC*/, 0, 0};
        case OutFormat::R11_EAC: return {true, 0x9270 /*GL_COMPRESSED_R11_EAC*/, 0, 0};
        case OutFormat::RG11_EAC: return {true, 0x9272 /*GL_COMPRESSED_RG11_EAC*/, 0, 0};
        case OutFormat::RGB565: return {false, GL_RGB, GL_RGB, GL_UNSIGNED_SHORT_5_6_5};
        case OutFormat::RGBA4444: return {false, GL_RGBA, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4};
        case OutFormat::RGBA5551: return {false, GL_RGBA, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1};
        case OutFormat::L8: return {false, GL_LUMINANCE, GL_LUMINANCE, GL_UNSIGNED_BYTE};
        case OutFormat::LA8: return {false, GL_LUMINANCE_ALPHA, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE};
        case OutFormat::RGBA8: return {false, GL_RGBA, GL_RGBA, GL_UNSIGNED_BYTE};
    }
    return {false, 0, 0, 0};
}

}  // namespace

uint32_t gl_upload(const PackEntry& e, const uint8_t* data, bool gles3) {
    const OutFormat of = OutFormat(e.format);
    const GLFmt gf = gl_format(of);
    if (gf.internal == 0 || (gf.compressed && !gles3)) return 0;

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    const int mips = e.count ? e.count : 1;
    size_t off = 0;
    for (int level = 0; level < mips; ++level) {
        const uint32_t w = e.width >> level ? e.width >> level : 1;
        const uint32_t h = e.height >> level ? e.height >> level : 1;
        const size_t size = out_level_size(of, w, h);
        if (off + size > e.data_size) break;
        if (gf.compressed)
            glCompressedTexImage2D(GL_TEXTURE_2D, level, gf.internal, GLsizei(w), GLsizei(h), 0,
                                   GLsizei(size), data + off);
        else
            glTexImage2D(GL_TEXTURE_2D, level, GLint(gf.internal), GLsizei(w), GLsizei(h), 0, gf.format,
                         gf.type, data + off);
        off += size;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mips > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    if (gles3) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, mips - 1);
        if (e.flags & TEXFLAG_SWIZZLE_I) {  // I4/I8: la intensidad va a los cuatro canales
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_RED);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_RED);
        } else if (e.flags & TEXFLAG_SWIZZLE_IA) {  // IA4/IA8: R = intensidad, G = alfa
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_RED);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_GREEN);
        }
    }
    if (glGetError() != GL_NO_ERROR) {
        glDeleteTextures(1, &tex);
        return 0;
    }
    return tex;
}

void gl_delete(uint32_t tex) {
    GLuint t = tex;
    glDeleteTextures(1, &t);
}

}  // namespace wl
