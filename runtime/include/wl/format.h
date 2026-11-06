// Formatos y claves compartidos con el conversor (choya/optimize.py y pack.py).
#pragma once
#include <cstddef>
#include <cstdint>

namespace wl {

enum class Kind : uint8_t { Texture = 1, Audio = 2 };

enum class OutFormat : uint8_t {
    ETC2_RGB8 = 1,
    ETC2_RGBA8 = 2,
    RGB565 = 3,
    RGBA4444 = 4,
    RGBA5551 = 5,
    R11_EAC = 6,   // intensidad: swizzle RRRR
    RG11_EAC = 7,  // intensidad + alfa: swizzle RRRG
    L8 = 8,
    LA8 = 9,
    RGBA8 = 10,
};

enum TexFlags : uint8_t { TEXFLAG_SWIZZLE_I = 1, TEXFLAG_SWIZZLE_IA = 2 };
enum AudioFlags : uint8_t { AUDIOFLAG_LOOP = 1 };

// Formatos GX de origen
enum GXFormat : uint8_t {
    GX_I4 = 0, GX_I8 = 1, GX_IA4 = 2, GX_IA8 = 3, GX_RGB565 = 4, GX_RGB5A3 = 5, GX_RGBA8 = 6,
    GX_C4 = 8, GX_C8 = 9, GX_C14X2 = 10, GX_CMPR = 14,
};

bool is_block_format(OutFormat f);
size_t out_level_size(OutFormat f, uint32_t w, uint32_t h);
const char* out_format_name(OutFormat f);

// Tamaño en bytes del nivel 0 de una textura GX (con relleno a bloques).
size_t gx_level_size(uint8_t gx_fmt, uint32_t w, uint32_t h);

// Clave de textura: crc32 del nivel 0 GX + dimensiones + formato (igual que pack.texture_key).
uint64_t texture_key(const uint8_t* level0, size_t size, uint32_t w, uint32_t h, uint8_t gx_fmt);
uint64_t texture_key_from_crc(uint32_t crc, uint32_t w, uint32_t h, uint8_t gx_fmt);

// Clave de ruta del disco: FNV-1a 64 de "/ruta" en minúsculas (igual que pack.path_key).
uint64_t path_key(const char* path);

}  // namespace wl
