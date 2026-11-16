// Caché de texturas para la capa GX: cambia cada textura GX del juego por su versión optimizada.
//
// Uso desde la capa de compatibilidad (GXInitTexObj / GXLoadTexObj):
//   wl::GXTexDesc d{ptr, w, h, fmt};
//   if (uint32_t tex = cache.get(d)) { glBindTexture(GL_TEXTURE_2D, tex); }
//   else { /* textura no está en el paquete: decodificar por software */ }
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "wl/pack.h"

namespace wl {

struct GXTexDesc {
    const uint8_t* data;  // puntero a los datos GX en la memoria del juego
    uint16_t width;
    uint16_t height;
    uint8_t format;       // GXFormat
    const uint8_t* palette = nullptr;  // C4/C8/C14X2: la paleta cargada (GXInitTlutObj), big-endian
    uint16_t palette_entries = 0;
    uint8_t palette_format = 0;
};

// Sube una entrada del paquete a la GPU y devuelve el id de textura (0 = error).
using Uploader = std::function<uint32_t(const PackEntry&, const uint8_t* data)>;
using Deleter = std::function<void(uint32_t)>;

struct TexCacheStats {
    uint64_t hits = 0;
    uint64_t misses = 0;        // texturas que no están en el paquete
    uint64_t uploads = 0;
    uint64_t rehashes = 0;      // el juego reutilizó memoria para otra textura
    uint64_t gpu_bytes = 0;
};

class TextureCache {
public:
    TextureCache(const Pack& pack, Uploader up, Deleter del = nullptr);
    ~TextureCache();

    uint32_t get(const GXTexDesc& d);
    void clear();
    const TexCacheStats& stats() const { return stats_; }

private:
    struct Slot {
        uint64_t key;
        uint64_t probe;     // huella rápida de los extremos del bloque
        uint32_t gl;
    };
    static uint64_t probe(const uint8_t* p, size_t n);

    const Pack& pack_;
    Uploader upload_;
    Deleter delete_;
    std::unordered_map<const void*, Slot> by_ptr_;
    std::unordered_map<uint64_t, uint32_t> by_key_;
    TexCacheStats stats_;
};

#ifdef WL_WITH_GL
// Lo que el contexto GL actual ofrece para texturas (gl_query_caps requiere un contexto activo).
struct GlCaps {
    bool queried = false;
    int major = 2, minor = 0;
    bool es = true;
    bool etc2 = false;          // se suben ETC2/EAC comprimidas (GLES 3 las exige en el núcleo)
    bool swizzle = false;       // GL_TEXTURE_SWIZZLE_* (GLES 3)
    bool force_decode = false;  // WL_SIN_ETC2=1: probar la ruta de respaldo en un equipo con ETC2
    int max_texture_size = 0;
    std::string vendor, renderer, version, glsl;
    std::vector<uint32_t> compressed;  // GL_COMPRESSED_TEXTURE_FORMATS
    bool lists(uint32_t gl_format) const;
};
GlCaps gl_query_caps();
std::string gl_caps_report(const GlCaps& caps);

enum class UploadRoute : uint8_t { Native, Decoded, Uncompressed, Failed };
const char* upload_route_name(UploadRoute r);
struct UploadResult {
    uint32_t tex = 0;
    UploadRoute route = UploadRoute::Failed;
    int levels = 0;           // niveles subidos
    uint32_t gl_error = 0;    // error de GL que obligó al respaldo (0 si no hubo)
    size_t gpu_bytes = 0;     // lo que ocupa en la GPU
    size_t temp_bytes = 0;    // búfer temporal usado (ya liberado)
    const char* reason = "";
};
// Sube la textura por la ruta que el contexto permite; si ETC2/EAC no está (o el driver lo rechaza), descomprime cada
// nivel en la CPU y lo sube sin comprimir.
UploadResult gl_upload_ex(const PackEntry& e, const uint8_t* data, const GlCaps& caps);
// Implementación con OpenGL ES (gl_upload.cpp). gles3 = el contexto soporta ETC2 y swizzle; si no, se usa el
// respaldo.
uint32_t gl_upload(const PackEntry& e, const uint8_t* data, bool gles3);
void gl_delete(uint32_t tex);
#endif

}  // namespace wl
