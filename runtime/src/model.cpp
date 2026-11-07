#include "wl/model.h"

#include <cstring>

namespace wl {

bool model_view(const Pack& pack, const PackEntry& e, ModelView& out) {
    if (e.kind != KIND_MODEL || e.data_size < 48) return false;
    const uint8_t* p = pack.data(e);
    if (std::memcmp(p, "CMDL", 4) != 0) return false;
    uint32_t hdr[5];
    std::memcpy(hdr, p + 4, sizeof hdr);
    if (hdr[0] != 1) return false;
    const uint64_t nv = hdr[1], ni = hdr[2], ns = hdr[3];
    const uint64_t need = 48 + nv * sizeof(ModelVertex) + ni * 4 + ns * sizeof(ModelSubmesh);
    if (need > e.data_size) return false;
    out.vertex_count = uint32_t(nv);
    out.index_count = uint32_t(ni);
    out.submesh_count = uint32_t(ns);
    out.flags = hdr[4];
    std::memcpy(out.bbox_min, p + 24, 12);
    std::memcpy(out.bbox_max, p + 36, 12);
    out.vertices = reinterpret_cast<const ModelVertex*>(p + 48);
    out.indices = reinterpret_cast<const uint32_t*>(p + 48 + nv * sizeof(ModelVertex));
    out.submeshes = reinterpret_cast<const ModelSubmesh*>(p + 48 + nv * sizeof(ModelVertex) + ni * 4);
    for (uint32_t i = 0; i < out.index_count; ++i)
        if (out.indices[i] >= out.vertex_count) return false;
    for (uint32_t i = 0; i < out.submesh_count; ++i)
        if (uint64_t(out.submeshes[i].first) + out.submeshes[i].count > ni) return false;
    return true;
}

}  // namespace wl
