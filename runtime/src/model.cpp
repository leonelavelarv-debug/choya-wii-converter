#include "wl/model.h"

#include <cstring>

namespace wl {

bool model_view(const Pack& pack, const PackEntry& e, ModelView& out) {
    out = ModelView{};
    if (e.kind != KIND_MODEL || e.data_size < 48) return false;
    const uint8_t* p = pack.data(e);
    if (std::memcmp(p, "CMDL", 4) != 0) return false;
    uint32_t hdr[5];
    std::memcpy(hdr, p + 4, sizeof hdr);
    const uint32_t ver = hdr[0];
    if (ver < 1 || ver > 3) return false;
    const uint64_t nv = hdr[1], ni = hdr[2], ns = hdr[3];
    const uint64_t base = ver >= 2 ? 64 : 48;
    uint32_t nm = 0, nb = 0;
    if (ver >= 2) std::memcpy(&nm, p + 48, 4);
    if (ver >= 3) std::memcpy(&nb, p + 52, 4);
    const uint64_t vsize = ver >= 3 ? sizeof(ModelVertex) : sizeof(ModelVertexV2);
    const uint64_t msize = ver >= 3 ? sizeof(TevMaterialRec) : kMaterialV2Size;
    const uint64_t need = base + nv * vsize + ni * 4 + ns * sizeof(ModelSubmesh) + uint64_t(nm) * msize +
                          uint64_t(nb) * sizeof(ModelBone);
    if (need > e.data_size || nb > 255) return false;
    out.version = ver;
    out.vertex_count = uint32_t(nv);
    out.index_count = uint32_t(ni);
    out.submesh_count = uint32_t(ns);
    out.flags = hdr[4];
    std::memcpy(out.bbox_min, p + 24, 12);
    std::memcpy(out.bbox_max, p + 36, 12);
    const uint8_t* vp = p + base;
    if (ver >= 3) {
        out.vertices = reinterpret_cast<const ModelVertex*>(vp);
    } else {
        out.owned_vertices.resize(nv);
        for (uint64_t i = 0; i < nv; ++i) {
            ModelVertex& v = out.owned_vertices[i];
            std::memset(&v, 0, sizeof v);
            std::memcpy(&v, vp + i * sizeof(ModelVertexV2), sizeof(ModelVertexV2));
        }
        out.vertices = out.owned_vertices.data();
        out.flags &= ~uint32_t(MODEL_SKINNED);
    }
    out.indices = reinterpret_cast<const uint32_t*>(vp + nv * vsize);
    out.submeshes = reinterpret_cast<const ModelSubmesh*>(vp + nv * vsize + ni * 4);
    out.material_count = nm;
    const uint8_t* mp = reinterpret_cast<const uint8_t*>(out.submeshes) + ns * sizeof(ModelSubmesh);
    if (nm && ver >= 3) {
        out.materials = reinterpret_cast<const TevMaterialRec*>(mp);
    } else if (nm) {
        out.owned_materials.resize(nm);
        for (uint32_t i = 0; i < nm; ++i) {
            TevMaterialRec& m = out.owned_materials[i];
            std::memset(&m, 0, sizeof m);
            std::memcpy(&m, mp + i * kMaterialV2Size, kMaterialV2Size);
        }
        out.materials = out.owned_materials.data();
    }
    out.bone_count = nb;
    out.bones = nb ? reinterpret_cast<const ModelBone*>(mp + uint64_t(nm) * msize) : nullptr;
    for (uint32_t i = 0; i < out.index_count; ++i)
        if (out.indices[i] >= out.vertex_count) return false;
    for (uint32_t i = 0; i < out.submesh_count; ++i)
        if (uint64_t(out.submeshes[i].first) + out.submeshes[i].count > ni ||
            (nm && out.submeshes[i].material >= nm))
            return false;
    if (out.flags & MODEL_SKINNED) {
        for (uint32_t i = 0; i < out.vertex_count; ++i)
            for (int k = 0; k < 4; ++k)
                if (out.vertices[i].weight[k] && out.vertices[i].bone[k] >= nb) return false;
    }
    return true;
}

bool anim_view(const Pack& pack, const PackEntry& e, AnimView& out) {
    out = AnimView{};
    if (e.kind != KIND_ANIM || e.data_size < 32) return false;
    const uint8_t* p = pack.data(e);
    if (std::memcmp(p, "CANM", 4) != 0) return false;
    uint32_t ver, frames, bones, flags;
    std::memcpy(&ver, p + 4, 4);
    std::memcpy(&frames, p + 8, 4);
    std::memcpy(&bones, p + 12, 4);
    std::memcpy(&out.model_key, p + 16, 8);
    std::memcpy(&flags, p + 24, 4);
    std::memcpy(&out.fps, p + 28, 4);
    if (ver != 1 || frames == 0 || bones == 0 || bones > 255) return false;
    if (32 + uint64_t(frames) * bones * 48 > e.data_size) return false;
    out.frames = frames;
    out.bones = bones;
    out.loop = flags & 1;
    out.matrices = reinterpret_cast<const float*>(p + 32);
    return true;
}

std::vector<uint32_t> anims_for(const Pack& pack, uint64_t model_key) {
    std::vector<uint32_t> out;
    for (uint32_t i = 0; i < pack.size(); ++i) {
        const PackEntry& e = pack.entry(i);
        AnimView a;
        if (e.kind == KIND_ANIM && anim_view(pack, e, a) && a.model_key == model_key) out.push_back(i);
    }
    return out;
}

}  // namespace wl
