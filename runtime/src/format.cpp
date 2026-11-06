#include "wl/format.h"

#include <zlib.h>

namespace wl {

bool is_block_format(OutFormat f) {
    return f == OutFormat::ETC2_RGB8 || f == OutFormat::ETC2_RGBA8 || f == OutFormat::R11_EAC ||
           f == OutFormat::RG11_EAC;
}

size_t out_level_size(OutFormat f, uint32_t w, uint32_t h) {
    const size_t blocks = size_t((w + 3) / 4) * ((h + 3) / 4);
    switch (f) {
        case OutFormat::ETC2_RGB8:
        case OutFormat::R11_EAC: return blocks * 8;
        case OutFormat::ETC2_RGBA8:
        case OutFormat::RG11_EAC: return blocks * 16;
        case OutFormat::RGB565:
        case OutFormat::RGBA4444:
        case OutFormat::RGBA5551:
        case OutFormat::LA8: return size_t(w) * h * 2;
        case OutFormat::L8: return size_t(w) * h;
        case OutFormat::RGBA8: return size_t(w) * h * 4;
    }
    return 0;
}

const char* out_format_name(OutFormat f) {
    switch (f) {
        case OutFormat::ETC2_RGB8: return "ETC2_RGB8";
        case OutFormat::ETC2_RGBA8: return "ETC2_RGBA8_EAC";
        case OutFormat::RGB565: return "RGB565";
        case OutFormat::RGBA4444: return "RGBA4444";
        case OutFormat::RGBA5551: return "RGBA5551";
        case OutFormat::R11_EAC: return "R11_EAC";
        case OutFormat::RG11_EAC: return "RG11_EAC";
        case OutFormat::L8: return "L8";
        case OutFormat::LA8: return "LA8";
        case OutFormat::RGBA8: return "RGBA8";
    }
    return "?";
}

size_t gx_level_size(uint8_t fmt, uint32_t w, uint32_t h) {
    uint32_t bw = 4, bh = 4, bpp = 16;
    switch (fmt) {
        case GX_I4: case GX_C4: case GX_CMPR: bw = 8; bh = 8; bpp = 4; break;
        case GX_I8: case GX_IA4: case GX_C8: bw = 8; bh = 4; bpp = 8; break;
        case GX_IA8: case GX_RGB565: case GX_RGB5A3: case GX_C14X2: break;
        case GX_RGBA8: bpp = 32; break;
        default: return 0;
    }
    const size_t bx = (w + bw - 1) / bw, by = (h + bh - 1) / bh;
    return bx * by * bw * bh * bpp / 8;
}

uint64_t texture_key_from_crc(uint32_t crc, uint32_t w, uint32_t h, uint8_t gx_fmt) {
    return (uint64_t(crc) << 32) | (uint64_t((w - 1) & 0x3FF) << 22) | (uint64_t((h - 1) & 0x3FF) << 12) |
           (uint64_t(gx_fmt & 0xF) << 8);
}

uint64_t texture_key(const uint8_t* level0, size_t size, uint32_t w, uint32_t h, uint8_t gx_fmt) {
    uint32_t crc = uint32_t(crc32(0L, level0, uInt(size)));
    return texture_key_from_crc(crc, w, h, gx_fmt);
}

uint64_t path_key(const char* path) {
    uint64_t h = 0xCBF29CE484222325ull;
    auto mix = [&h](unsigned char c) {
        h ^= c;
        h *= 0x100000001B3ull;
    };
    const char* p = path;
    while (*p == '/' || *p == '\\') ++p;
    mix('/');
    for (; *p; ++p) {
        unsigned char c = static_cast<unsigned char>(*p);
        if (c == '\\') c = '/';
        if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
        mix(c);
    }
    return h;
}

}  // namespace wl
