// Mallas del paquete (entrada KIND_MODEL, bloque "CMDL"; ver choya/model_pack.py).
#pragma once
#include <cstdint>

#include "wl/pack.h"

namespace wl {

constexpr uint8_t KIND_MODEL = 3;

#pragma pack(push, 1)
struct ModelVertex {
    float pos[3];
    float nrm[3];
    uint8_t col[4];
    float uv[2];
};
struct ModelSubmesh {
    uint32_t first;
    uint32_t count;
    uint64_t texture_key;  // 0 = sin textura
    uint8_t cull;          // 0 ninguno, 1 frente, 2 atrás, 3 todo
    uint8_t xlu;           // translúcido: se dibuja al final con mezcla
    uint8_t wrap_s;        // 0 clamp, 1 repetir, 2 espejo
    uint8_t wrap_t;
    uint8_t reserved[12];
};
#pragma pack(pop)
static_assert(sizeof(ModelVertex) == 36, "vértice de 36 bytes");
static_assert(sizeof(ModelSubmesh) == 32, "submalla de 32 bytes");

enum ModelFlags : uint32_t { MODEL_HAS_NORMALS = 1, MODEL_HAS_COLORS = 2 };

struct ModelView {
    const ModelVertex* vertices = nullptr;
    uint32_t vertex_count = 0;
    const uint32_t* indices = nullptr;
    uint32_t index_count = 0;
    const ModelSubmesh* submeshes = nullptr;
    uint32_t submesh_count = 0;
    uint32_t flags = 0;
    float bbox_min[3] = {0, 0, 0};
    float bbox_max[3] = {0, 0, 0};
};

// Interpreta una entrada de modelo sin copiar datos. false si está dañada.
bool model_view(const Pack& pack, const PackEntry& e, ModelView& out);

}  // namespace wl
