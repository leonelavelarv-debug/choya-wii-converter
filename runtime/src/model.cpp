#include "wl/model.h"

#include <algorithm>
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
    if (ver < 1 || ver > 4) return false;  // la 4 es la 3 con campos que antes iban en cero
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

namespace wl {

namespace {

uint32_t rd32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

size_t matanim_value_size(uint32_t type) {
    switch (type) {
        case MATANIM_SRT: return 32;
        case MATANIM_CLR: return 4;
        case MATANIM_PAT: return 2;
        case MATANIM_VIS: return 1;
        default: return 0;
    }
}

// Nombre de la animación sin la ruta, la extensión (.srt0, .clr0, ...) ni el modelo: "ruta/wait.srt0@modelo" -> wait.
std::string anim_base_name(const char* entry_name) {
    std::string n = entry_name;
    const size_t at = n.find('@');
    if (at != std::string::npos) n.resize(at);
    const size_t slash = n.rfind('/');
    if (slash != std::string::npos) n = n.substr(slash + 1);
    static const char* kExt[] = {".srt0", ".clr0", ".pat0", ".vis0"};
    for (const char* ext : kExt) {
        const size_t l = std::strlen(ext);
        if (n.size() > l && n.compare(n.size() - l, l, ext) == 0) {
            n.resize(n.size() - l);
            break;
        }
    }
    return n;
}

}  // namespace

const void* MatAnimView::value(const MatAnimTrack& t, uint32_t f) const {
    const uint32_t i = (t.flags & 1) || t.count <= 1 ? 0 : (f < t.count ? f : t.count - 1);
    return base + t.offset + size_t(i) * matanim_value_size(type);
}

bool mat_anim_view(const Pack& pack, const PackEntry& e, MatAnimView& out) {
    out = MatAnimView{};
    if (e.kind != KIND_MATANIM || e.data_size < 64) return false;
    const uint8_t* p = pack.data(e);
    if (std::memcmp(p, "CMAN", 4) != 0 || rd32(p + 4) != 1) return false;
    out.frames = rd32(p + 8);
    out.type = rd32(p + 12);
    std::memcpy(&out.model_key, p + 16, 8);
    out.loop = rd32(p + 24) & 1;
    std::memcpy(&out.fps, p + 28, 4);
    out.track_count = rd32(p + 32);
    out.key_count = rd32(p + 36);
    const uint32_t toff = rd32(p + 40), koff = rd32(p + 44);
    const size_t vsize = matanim_value_size(out.type);
    if (!vsize || out.frames == 0 || out.frames > 65536) return false;
    if (uint64_t(toff) + uint64_t(out.track_count) * sizeof(MatAnimTrack) > e.data_size) return false;
    if (out.key_count && (koff % 8 || uint64_t(koff) + uint64_t(out.key_count) * 8 > e.data_size)) return false;
    out.base = p;
    out.tracks = reinterpret_cast<const MatAnimTrack*>(p + toff);
    out.keys = out.key_count ? reinterpret_cast<const uint64_t*>(p + koff) : nullptr;
    for (uint32_t i = 0; i < out.track_count; ++i) {  // cada pista dentro del bloque, y alineada
        const MatAnimTrack& t = out.tracks[i];
        if (t.count == 0 || t.offset % (vsize >= 4 ? 4 : vsize) ||
            uint64_t(t.offset) + uint64_t(t.count) * vsize > e.data_size)
            return false;
        if (out.type == MATANIM_PAT) {  // los índices de textura dentro de la tabla
            const uint16_t* v = reinterpret_cast<const uint16_t*>(p + t.offset);
            for (uint32_t k = 0; k < t.count; ++k)
                if (v[k] != 0xFFFF && v[k] >= out.key_count) return false;
        }
    }
    return true;
}

const uint8_t* SceneView::amb_color(uint32_t i, uint32_t f) const {
    const SceneAmb& a = ambs[i];
    return base + a.offset + 4 * size_t(a.constant || a.count <= 1 ? 0 : (f < a.count ? f : a.count - 1));
}
const SceneLightFrame& SceneView::light(uint32_t i, uint32_t f) const {
    const SceneLight& l = lights[i];
    return reinterpret_cast<const SceneLightFrame*>(base + l.offset)[l.constant || l.count <= 1 ? 0
                                                                    : (f < l.count ? f : l.count - 1)];
}
const SceneCameraFrame& SceneView::camera(uint32_t i, uint32_t f) const {
    const SceneCamera& c = cameras[i];
    return reinterpret_cast<const SceneCameraFrame*>(base + c.offset)[c.constant || c.count <= 1 ? 0
                                                                      : (f < c.count ? f : c.count - 1)];
}
int SceneView::find_set(int ref) const {
    for (uint32_t i = 0; i < set_count; ++i)
        if (sets[i].ref == ref) return int(i);
    return -1;
}
int SceneView::find_light(int ref) const {
    for (uint32_t i = 0; i < light_count; ++i)
        if (lights[i].ref == ref) return int(i);
    return -1;
}
int SceneView::find_camera(int ref) const {
    for (uint32_t i = 0; i < camera_count; ++i)
        if (cameras[i].ref == ref) return int(i);
    return -1;
}

bool scene_view(const Pack& pack, const PackEntry& e, SceneView& out) {
    out = SceneView{};
    if (e.kind != KIND_SCENE || e.data_size < 64) return false;
    const uint8_t* p = pack.data(e);
    if (std::memcmp(p, "CSCN", 4) != 0 || rd32(p + 4) != 1) return false;
    out.frames = rd32(p + 8);
    out.loop = rd32(p + 12) & 1;
    std::memcpy(&out.fps, p + 16, 4);
    uint16_t n[6];
    std::memcpy(n, p + 20, 12);
    out.set_count = n[0], out.amb_count = n[1], out.light_count = n[2], out.camera_count = n[3], out.fog_count = n[4];
    const uint32_t os = rd32(p + 32), oa = rd32(p + 36), ol = rd32(p + 40), oc = rd32(p + 44);
    const uint64_t size = e.data_size;
    if (out.frames == 0 || uint64_t(os) + out.set_count * 32ull > size || uint64_t(oa) + out.amb_count * 12ull > size ||
        uint64_t(ol) + out.light_count * 16ull > size || uint64_t(oc) + out.camera_count * 16ull > size ||
        os % 4 || oa % 4 || ol % 4 || oc % 4)
        return false;
    out.base = p;
    out.sets = reinterpret_cast<const SceneLightSet*>(p + os);
    out.ambs = reinterpret_cast<const SceneAmb*>(p + oa);
    out.lights = reinterpret_cast<const SceneLight*>(p + ol);
    out.cameras = reinterpret_cast<const SceneCamera*>(p + oc);
    for (uint32_t i = 0; i < out.amb_count; ++i)
        if (out.ambs[i].count == 0 || uint64_t(out.ambs[i].offset) + out.ambs[i].count * 4ull > size) return false;
    for (uint32_t i = 0; i < out.light_count; ++i)
        if (out.lights[i].count == 0 || out.lights[i].offset % 4 ||
            uint64_t(out.lights[i].offset) + out.lights[i].count * 64ull > size ||
            (out.lights[i].spec >= 0 && uint32_t(out.lights[i].spec) >= out.light_count))
            return false;
    for (uint32_t i = 0; i < out.camera_count; ++i)
        if (out.cameras[i].count == 0 || out.cameras[i].offset % 4 ||
            uint64_t(out.cameras[i].offset) + out.cameras[i].count * 64ull > size)
            return false;
    for (uint32_t i = 0; i < out.set_count; ++i) {
        const SceneLightSet& s = out.sets[i];
        if (s.count > 8 || (s.amb >= 0 && uint32_t(s.amb) >= out.amb_count)) return false;
        for (int k = 0; k < s.count; ++k)
            if (s.light[k] >= 0 && uint32_t(s.light[k]) >= out.light_count) return false;
    }
    return true;
}

std::vector<AnimGroup> anim_groups_for(const Pack& pack, uint64_t model_key) {
    std::vector<AnimGroup> groups;
    auto group = [&](const std::string& name) -> AnimGroup& {
        for (AnimGroup& g : groups)
            if (g.name == name) return g;
        groups.push_back(AnimGroup{});
        groups.back().name = name;
        return groups.back();
    };
    for (uint32_t i = 0; i < pack.size(); ++i) {
        const PackEntry& e = pack.entry(i);
        if (e.kind == KIND_ANIM) {
            AnimView a;
            if (!anim_view(pack, e, a) || a.model_key != model_key) continue;
            AnimGroup& g = group(anim_base_name(pack.name(e)));
            if (g.chr < 0) g.chr = int32_t(i);
            g.frames = std::max(g.frames, a.frames);
            g.loop = g.loop || a.loop;
        } else if (e.kind == KIND_MATANIM) {
            MatAnimView a;
            if (!mat_anim_view(pack, e, a) || a.model_key != model_key) continue;
            AnimGroup& g = group(anim_base_name(pack.name(e)));
            g.mats.push_back(i);
            g.frames = std::max(g.frames, a.frames);
            g.loop = g.loop || a.loop;
        }
    }
    return groups;
}

std::vector<uint32_t> scenes_for(const Pack& pack, const char* model_name) {
    std::vector<std::pair<size_t, uint32_t>> found;
    const std::string mn = model_name ? model_name : "";
    for (uint32_t i = 0; i < pack.size(); ++i) {
        const PackEntry& e = pack.entry(i);
        if (e.kind != KIND_SCENE) continue;
        const char* n = pack.name(e);
        size_t common = 0;
        while (n[common] && common < mn.size() && n[common] == mn[common]) ++common;
        found.push_back({common, i});
    }
    std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<uint32_t> out;
    for (const auto& f : found) out.push_back(f.second);
    return out;
}

}  // namespace wl
