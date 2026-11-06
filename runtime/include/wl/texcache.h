// Caché de texturas para la capa GX: cambia cada textura GX del juego por su versión optimizada.
//
// Uso desde la capa de compatibilidad (GXInitTexObj / GXLoadTexObj):
//   wl::GXTexDesc d{ptr, w, h, fmt};
//   if (uint32_t tex = cache.get(d)) { glBindTexture(GL_TEXTURE_2D, tex); }
//   else { /* textura no está en el paquete: decodificar por software */ }
#pragma once
#include <cstdint>
#include <functional>
#include <unordered_map>

#include "wl/pack.h"

namespace wl {

struct GXTexDesc {
    const uint8_t* data;  // puntero a los datos GX en la memoria del juego
    uint16_t width;
    uint16_t height;
    uint8_t format;       // GXFormat
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
// Implementación con OpenGL ES (gl_upload.cpp). gles3 = el contexto soporta ETC2 y swizzle.
uint32_t gl_upload(const PackEntry& e, const uint8_t* data, bool gles3);
void gl_delete(uint32_t tex);
#endif

}  // namespace wl
