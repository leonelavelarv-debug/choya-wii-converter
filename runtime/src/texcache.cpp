#include "wl/texcache.h"

#include <cstring>

namespace wl {

TextureCache::TextureCache(const Pack& pack, Uploader up, Deleter del)
    : pack_(pack), upload_(std::move(up)), delete_(std::move(del)) {}

TextureCache::~TextureCache() { clear(); }

void TextureCache::clear() {
    if (delete_)
        for (auto& kv : by_key_)
            if (kv.second) delete_(kv.second);
    by_key_.clear();
    by_ptr_.clear();
}

uint64_t TextureCache::probe(const uint8_t* p, size_t n) {
    // Huella barata: 16 bytes del inicio, del centro y del final.
    uint64_t a = 0, b = 0, c = 0;
    const size_t k = n < 8 ? n : 8;
    std::memcpy(&a, p, k);
    if (n >= 16) {
        std::memcpy(&b, p + n / 2, 8);
        std::memcpy(&c, p + n - 8, 8);
    }
    return a ^ (b * 0x9E3779B97F4A7C15ull) ^ (c * 0xC2B2AE3D27D4EB4Full) ^ n;
}

uint32_t TextureCache::get(const GXTexDesc& d) {
    const size_t size = gx_level_size(d.format, d.width, d.height);
    if (!d.data || size == 0) return 0;
    const bool paletted = d.format == GX_C4 || d.format == GX_C8 || d.format == GX_C14X2;
    if (paletted && !d.palette) return 0;  // sin paleta no hay clave: lo decodifica la capa GX
    uint64_t pr = probe(d.data, size);
    if (paletted) pr ^= probe(d.palette, size_t(d.palette_entries) * 2) * 0x9E3779B97F4A7C15ull;
    auto it = by_ptr_.find(d.data);
    if (it != by_ptr_.end()) {
        if (it->second.probe == pr) {
            ++stats_.hits;
            return it->second.gl;
        }
        ++stats_.rehashes;  // misma dirección, otra textura
    }
    const uint64_t key = paletted ? texture_key(d.data, size, d.width, d.height, d.format, d.palette,
                                                size_t(d.palette_entries) * 2, d.palette_format)
                                  : texture_key(d.data, size, d.width, d.height, d.format);
    uint32_t gl = 0;
    auto kt = by_key_.find(key);
    if (kt != by_key_.end()) {
        gl = kt->second;
        ++stats_.hits;
    } else {
        const PackEntry* e = pack_.find(key);
        if (e && e->kind == uint8_t(Kind::Texture)) {
            gl = upload_ ? upload_(*e, pack_.data(*e)) : 0;
            ++stats_.uploads;
            stats_.gpu_bytes += e->data_size;
        } else {
            ++stats_.misses;
        }
        by_key_[key] = gl;
    }
    by_ptr_[d.data] = Slot{key, pr, gl};
    return gl;
}

}  // namespace wl
