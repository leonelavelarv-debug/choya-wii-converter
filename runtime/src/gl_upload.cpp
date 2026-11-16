// Subida de texturas del paquete a OpenGL ES 3.x (ETC2/EAC) o 2.0 (formatos sin comprimir).
//
// Rutas (UploadRoute):
//   Native        el formato comprimido va tal cual a glCompressedTexImage2D (GLES 3 exige ETC2/EAC);
//   Decoded       el contexto no acepta ETC2/EAC (GLES 2, el driver rechazó el formato o WL_SIN_ETC2=1): cada nivel se
//                 descomprime en la CPU (etc_decode.cpp) a un búfer temporal que se libera al terminar la textura;
//   Uncompressed  la entrada ya estaba sin comprimir (RGB565, RGBA8, ...).
#include <GLES3/gl3.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

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
        case OutFormat::ETC2_RGB8A1: return {true, 0x9276 /*GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2*/, 0, 0};
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

void clear_errors() {
    for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {
    }
}

// Formato sin comprimir para el respaldo: lo mínimo que guarda lo que trae el bloque, en formatos que GLES 2 también
// acepta. Con las banderas de intensidad la textura queda igual que con el swizzle de GLES 3.
struct DecodedFmt {
    GLenum format;
    int bpp;
};
DecodedFmt decoded_format(OutFormat f, uint8_t flags) {
    switch (f) {
        case OutFormat::ETC2_RGB8: return {GL_RGB, 3};
        case OutFormat::R11_EAC:
            return (flags & TEXFLAG_SWIZZLE_I) ? DecodedFmt{GL_LUMINANCE_ALPHA, 2} : DecodedFmt{GL_RGB, 3};
        case OutFormat::RG11_EAC:
            return (flags & TEXFLAG_SWIZZLE_IA) ? DecodedFmt{GL_LUMINANCE_ALPHA, 2} : DecodedFmt{GL_RGBA, 4};
        default: return {GL_RGBA, 4};
    }
}

// RGBA8 decodificado (canales tal cual: R11 = R,0,0,255) al formato del respaldo, en el mismo búfer.
void repack(OutFormat f, uint8_t flags, uint8_t* px, size_t n) {
    const DecodedFmt d = decoded_format(f, flags);
    if (d.bpp == 4) return;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* s = px + i * 4;
        uint8_t* o = px + i * d.bpp;
        if (d.format == GL_RGB) {
            const uint8_t r = s[0], g = s[1], b = s[2];
            o[0] = r, o[1] = g, o[2] = b;
        } else if (f == OutFormat::R11_EAC) {  // intensidad: L = A = I
            const uint8_t r = s[0];
            o[0] = r, o[1] = r;
        } else {  // RG11 con IA: L = R (intensidad), A = G (alfa)
            const uint8_t r = s[0], g = s[1];
            o[0] = r, o[1] = g;
        }
    }
}

bool env_on(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && std::strcmp(v, "0") != 0;
}

}  // namespace

bool GlCaps::lists(uint32_t fmt) const {
    for (uint32_t f : compressed)
        if (f == fmt) return true;
    return false;
}

GlCaps gl_query_caps() {
    GlCaps c;
    c.queried = true;
    auto str = [](GLenum e) {
        const GLubyte* s = glGetString(e);
        return s ? std::string(reinterpret_cast<const char*>(s)) : std::string();
    };
    c.vendor = str(GL_VENDOR);
    c.renderer = str(GL_RENDERER);
    c.version = str(GL_VERSION);
    c.glsl = str(GL_SHADING_LANGUAGE_VERSION);
    // "OpenGL ES 3.2 ..." (o "OpenGL ES-CM"/"3.0 Mesa" en otros contextos): primer dígito con punto.
    const char* v = c.version.c_str();
    c.es = std::strstr(v, "OpenGL ES") != nullptr;
    while (*v && !(v[0] >= '0' && v[0] <= '9' && v[1] == '.')) ++v;
    if (*v) {
        c.major = v[0] - '0';
        c.minor = v[2] >= '0' && v[2] <= '9' ? v[2] - '0' : 0;
    }
    GLint n = 0, mx = 0;
    clear_errors();
    glGetIntegerv(GL_NUM_COMPRESSED_TEXTURE_FORMATS, &n);
    if (n > 0 && n < 4096) {
        std::vector<GLint> f(static_cast<size_t>(n), 0);
        glGetIntegerv(GL_COMPRESSED_TEXTURE_FORMATS, f.data());
        for (GLint x : f) c.compressed.push_back(uint32_t(x));
    }
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &mx);
    c.max_texture_size = mx;
    clear_errors();
    c.force_decode = env_on("WL_SIN_ETC2");
    c.etc2 = c.major >= 3 && !c.force_decode;  // GLES 3.0 exige ETC2/EAC en el núcleo
    c.swizzle = c.major >= 3;
    return c;
}

std::string gl_caps_report(const GlCaps& c) {
    std::string r;
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "GL_VENDOR: %s\nGL_RENDERER: %s\nGL_VERSION: %s (versión %d.%d%s)\nGL_SHADING_LANGUAGE_VERSION: %s\n"
                  "GL_MAX_TEXTURE_SIZE: %d\n",
                  c.vendor.c_str(), c.renderer.c_str(), c.version.c_str(), c.major, c.minor, c.es ? ", ES" : "",
                  c.glsl.c_str(), c.max_texture_size);
    r += buf;
    static const struct {
        uint32_t id;
        const char* name;
    } kFmts[] = {{0x9274, "GL_COMPRESSED_RGB8_ETC2"},
                 {0x9276, "GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2"},
                 {0x9278, "GL_COMPRESSED_RGBA8_ETC2_EAC"},
                 {0x9270, "GL_COMPRESSED_R11_EAC"},
                 {0x9272, "GL_COMPRESSED_RG11_EAC"}};
    std::snprintf(buf, sizeof buf, "formatos comprimidos anunciados: %zu\n", c.compressed.size());
    r += buf;
    for (const auto& f : kFmts) {
        std::snprintf(buf, sizeof buf, "  %-44s 0x%04X %s\n", f.name, f.id, c.lists(f.id) ? "anunciado" : "NO anunciado");
        r += buf;
    }
    if (c.force_decode)
        r += "WL_SIN_ETC2=1: se fuerza la ruta de respaldo (descomprimir en la CPU)\n";
    r += c.etc2 ? "ruta para ETC2/EAC: nativa (glCompressedTexImage2D); si el driver rechaza un formato, respaldo\n"
                : "ruta para ETC2/EAC: respaldo (se descomprime en la CPU y se sube sin comprimir)\n";
    return r;
}

const char* upload_route_name(UploadRoute r) {
    switch (r) {
        case UploadRoute::Native: return "nativa";
        case UploadRoute::Decoded: return "respaldo (descomprimida en la CPU)";
        case UploadRoute::Uncompressed: return "sin comprimir";
        case UploadRoute::Failed: return "fallida";
    }
    return "?";
}

UploadResult gl_upload_ex(const PackEntry& e, const uint8_t* data, const GlCaps& caps) {
    UploadResult res;
    const OutFormat of = OutFormat(e.format);
    const GLFmt gf = gl_format(of);
    if (gf.internal == 0 || e.width == 0 || e.height == 0) {
        res.reason = "formato desconocido o tamaño 0";
        return res;
    }
    if (caps.max_texture_size && (e.width > caps.max_texture_size || e.height > caps.max_texture_size)) {
        res.reason = "más grande que GL_MAX_TEXTURE_SIZE";
        return res;
    }
    const int mips = e.count ? e.count : 1;
    // Niveles completos dentro de la entrada (un nivel cortado no se sube: la textura queda con los anteriores).
    int levels = 0;
    {
        size_t off = 0;
        for (int level = 0; level < mips; ++level) {
            const uint32_t w = e.width >> level ? e.width >> level : 1;
            const uint32_t h = e.height >> level ? e.height >> level : 1;
            const size_t size = out_level_size(of, w, h);
            if (size == 0 || off + size > e.data_size) break;
            off += size;
            ++levels;
        }
    }
    if (levels == 0) {
        res.reason = "datos más cortos que el nivel 0";
        return res;
    }
    clear_errors();
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    bool decode = gf.compressed && !caps.etc2;
    if (gf.compressed && !decode) {
        size_t off = 0;
        for (int level = 0; level < levels; ++level) {
            const uint32_t w = e.width >> level ? e.width >> level : 1;
            const uint32_t h = e.height >> level ? e.height >> level : 1;
            const size_t size = out_level_size(of, w, h);
            glCompressedTexImage2D(GL_TEXTURE_2D, level, gf.internal, GLsizei(w), GLsizei(h), 0, GLsizei(size),
                                   data + off);
            const GLenum err = glGetError();
            if (err != GL_NO_ERROR) {  // el driver no lo acepta: respaldo para esta textura
                res.gl_error = err;
                res.reason = "el driver rechazó el formato comprimido";
                decode = true;
                glDeleteTextures(1, &tex);
                glGenTextures(1, &tex);
                glBindTexture(GL_TEXTURE_2D, tex);
                break;
            }
            off += size;
            res.gpu_bytes += size;
        }
        if (!decode) res.route = UploadRoute::Native;
    }
    if (decode) {
        res.gpu_bytes = 0;
        const DecodedFmt df = decoded_format(of, e.flags);
        std::vector<uint8_t> tmp;  // un solo búfer para todos los niveles; se libera al salir
        tmp.reserve(size_t(e.width) * e.height * 4);
        size_t off = 0;
        int ok_levels = 0;
        for (int level = 0; level < levels; ++level) {
            const uint32_t w = e.width >> level ? e.width >> level : 1;
            const uint32_t h = e.height >> level ? e.height >> level : 1;
            const size_t size = out_level_size(of, w, h);
            tmp.assign(size_t(w) * h * 4, 0);
            if (!decode_block_level(of, data + off, size, w, h, tmp.data())) break;
            repack(of, e.flags, tmp.data(), size_t(w) * h);
            glTexImage2D(GL_TEXTURE_2D, level, GLint(df.format), GLsizei(w), GLsizei(h), 0, df.format,
                         GL_UNSIGNED_BYTE, tmp.data());
            if (glGetError() != GL_NO_ERROR) break;
            off += size;
            res.gpu_bytes += size_t(w) * h * size_t(df.bpp);
            ++ok_levels;
        }
        res.temp_bytes = tmp.capacity();
        levels = ok_levels;
        res.route = UploadRoute::Decoded;
        if (!res.reason[0]) res.reason = caps.force_decode ? "WL_SIN_ETC2=1" : "el contexto no tiene ETC2/EAC";
    } else if (!gf.compressed) {
        size_t off = 0;
        for (int level = 0; level < levels; ++level) {
            const uint32_t w = e.width >> level ? e.width >> level : 1;
            const uint32_t h = e.height >> level ? e.height >> level : 1;
            const size_t size = out_level_size(of, w, h);
            glTexImage2D(GL_TEXTURE_2D, level, GLint(gf.internal), GLsizei(w), GLsizei(h), 0, gf.format, gf.type,
                         data + off);
            off += size;
            res.gpu_bytes += size;
        }
        res.route = UploadRoute::Uncompressed;
    }
    if (levels == 0) {
        glDeleteTextures(1, &tex);
        res.route = UploadRoute::Failed;
        res.reason = "no se pudo decodificar el nivel 0";
        return res;
    }
    res.levels = levels;
    // Cadena completa hasta 1x1: con GLES 2 no hay GL_TEXTURE_MAX_LEVEL, así que una cadena incompleta se muestrea sin
    // mipmaps (si no, la textura queda incompleta y sale negra).
    const uint32_t lw = e.width >> (levels - 1), lh = e.height >> (levels - 1);
    const bool full_chain = levels > 1 && lw <= 1 && lh <= 1;
    const bool use_mips = levels > 1 && (caps.major >= 3 || full_chain);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, use_mips ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    if (caps.major >= 3) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels - 1);
        // Con el respaldo la intensidad ya va en luminancia/alfa: el swizzle solo hace falta con el formato nativo.
        if (res.route == UploadRoute::Native && caps.swizzle) {
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
    }
    const GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        glDeleteTextures(1, &tex);
        res.gl_error = err;
        res.route = UploadRoute::Failed;
        res.reason = "error de GL al subir";
        res.levels = 0;
        return res;
    }
    res.tex = tex;
    return res;
}

uint32_t gl_upload(const PackEntry& e, const uint8_t* data, bool gles3) {
    static GlCaps caps;  // se consulta una vez, con el primer contexto
    if (!caps.queried) caps = gl_query_caps();
    GlCaps c = caps;
    if (!gles3) {  // el que llama dice que el contexto es GLES 2: sin ETC2 ni swizzle
        c.etc2 = false;
        c.swizzle = false;
        if (c.major >= 3) c.major = 2;
    }
    return gl_upload_ex(e, data, c).tex;
}

void gl_delete(uint32_t tex) {
    GLuint t = tex;
    glDeleteTextures(1, &t);
}

}  // namespace wl
