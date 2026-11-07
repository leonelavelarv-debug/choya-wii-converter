#include "wl/model.h"

#include <cstring>

namespace wl {

bool model_view(const Pack& pack, const PackEntry& e, ModelView& out) {
    if (e.kind != KIND_MODEL || e.data_size < 48) return false;
    const uint8_t* p = pack.data(e);
    if (std::memcmp(p, "CMDL", 4) != 0) return false;
    uint32_t hdr[5];
    std::memcpy(hdr, p + 4, sizeof hdr);
    if (hdr[0] != 1 && hdr[0] != 2) return false;
    const uint64_t nv = hdr[1], ni = hdr[2], ns = hdr[3];
    const uint64_t base = hdr[0] >= 2 ? 64 : 48;
    uint32_t nm = 0;
    if (hdr[0] >= 2) std::memcpy(&nm, p + 48, 4);
    const uint64_t need = base + nv * sizeof(ModelVertex) + ni * 4 + ns * sizeof(ModelSubmesh) +
                          uint64_t(nm) * sizeof(TevMaterialRec);
    if (need > e.data_size) return false;
    out.version = hdr[0];
    out.vertex_count = uint32_t(nv);
    out.index_count = uint32_t(ni);
    out.submesh_count = uint32_t(ns);
    out.flags = hdr[4];
    std::memcpy(out.bbox_min, p + 24, 12);
    std::memcpy(out.bbox_max, p + 36, 12);
    out.vertices = reinterpret_cast<const ModelVertex*>(p + base);
    out.indices = reinterpret_cast<const uint32_t*>(p + base + nv * sizeof(ModelVertex));
    out.submeshes = reinterpret_cast<const ModelSubmesh*>(p + base + nv * sizeof(ModelVertex) + ni * 4);
    out.material_count = nm;
    out.materials = nm ? reinterpret_cast<const TevMaterialRec*>(reinterpret_cast<const uint8_t*>(out.submeshes) +
                                                                ns * sizeof(ModelSubmesh))
                       : nullptr;
    for (uint32_t i = 0; i < out.index_count; ++i)
        if (out.indices[i] >= out.vertex_count) return false;
    for (uint32_t i = 0; i < out.submesh_count; ++i)
        if (uint64_t(out.submeshes[i].first) + out.submeshes[i].count > ni ||
            (nm && out.submeshes[i].material >= nm))
            return false;
    return true;
}

}  // namespace wl
