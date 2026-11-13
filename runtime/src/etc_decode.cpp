// Decodificador ETC2/EAC de referencia (en la CPU) para comprobar lo que devuelve la GPU (wl_view --verificar,
// choya_render --verificar). Cubre lo que produce el conversor (choya/etc.py): ETC1 individual y diferencial sin
// desbordes, EAC de 8 bits (alfa de RGBA8_EAC), EAC de 11 bits (R11/RG11) y el alfa de 1 bit (RGB8A1) en modo
// diferencial. Un bloque en modo T, H o planar devuelve false: el conversor nunca los escribe.
#include <cstring>

#include "wl/format.h"

namespace wl {

namespace {

const int kEtc1[8][2] = {{2, 8}, {5, 17}, {9, 29}, {13, 42}, {18, 60}, {24, 80}, {33, 106}, {47, 183}};
const int kEac[16][8] = {
    {-3, -6, -9, -15, 2, 5, 8, 14}, {-3, -7, -10, -13, 2, 6, 9, 12}, {-2, -5, -8, -13, 1, 4, 7, 12},
    {-2, -4, -6, -13, 1, 3, 5, 12}, {-3, -6, -8, -12, 2, 5, 7, 11}, {-3, -7, -9, -11, 2, 6, 8, 10},
    {-4, -7, -8, -11, 3, 6, 7, 10}, {-3, -5, -8, -11, 2, 4, 7, 10}, {-2, -6, -8, -10, 1, 5, 7, 9},
    {-2, -5, -8, -10, 1, 4, 7, 9},  {-2, -4, -8, -10, 1, 3, 7, 9},  {-2, -5, -7, -10, 1, 4, 6, 9},
    {-3, -4, -7, -10, 2, 3, 6, 9},  {-1, -2, -3, -10, 0, 1, 2, 9},  {-4, -6, -8, -9, 3, 5, 7, 8},
    {-3, -5, -7, -9, 2, 4, 6, 8},
};

uint64_t be64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}
int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

// Color de un bloque ETC1/ETC2 (16 píxeles en orden de filas: out[y*4+x]). punch: formato RGB8A1.
bool color_block(uint64_t w, bool punch, uint8_t out[16][4]) {
    const uint32_t hi = uint32_t(w >> 32), lo = uint32_t(w);
    const bool diff = (hi >> 1) & 1;  // en RGB8A1, "opaco"
    const bool flip = hi & 1;
    const int t[2] = {int((hi >> 5) & 7), int((hi >> 2) & 7)};
    int base[2][3];
    if (punch || diff) {
        for (int c = 0; c < 3; ++c) {
            const int s5 = 27 - 8 * c;
            const int r5 = int((hi >> s5) & 31);
            int dd = int((hi >> (s5 - 3)) & 7);
            if (dd >= 4) dd -= 8;
            const int r5b = r5 + dd;
            if (r5b < 0 || r5b > 31) return false;  // modo T/H/planar
            base[0][c] = (r5 << 3) | (r5 >> 2);
            base[1][c] = (r5b << 3) | (r5b >> 2);
        }
    } else {
        for (int c = 0; c < 3; ++c) {
            base[0][c] = int((hi >> (28 - 8 * c)) & 15) * 17;
            base[1][c] = int((hi >> (24 - 8 * c)) & 15) * 17;
        }
    }
    const bool opaque = !punch || diff;
    for (int x = 0; x < 4; ++x) {
        for (int y = 0; y < 4; ++y) {
            const int j = x * 4 + y;
            const int idx = int((((lo >> (j + 16)) & 1) << 1) | ((lo >> j) & 1));
            const int sub = flip ? (y >= 2) : (x >= 2);
            const int a = kEtc1[t[sub]][0], b = kEtc1[t[sub]][1];
            uint8_t* px = out[y * 4 + x];
            if (!opaque && idx == 2) {
                px[0] = px[1] = px[2] = px[3] = 0;
                continue;
            }
            const int mod = opaque ? (idx == 0 ? a : idx == 1 ? b : idx == 2 ? -a : -b)
                                   : (idx == 0 ? 0 : idx == 1 ? b : -b);
            for (int c = 0; c < 3; ++c) px[c] = uint8_t(clamp255(base[sub][c] + mod));
            px[3] = 255;
        }
    }
    return true;
}

// EAC: 8 bits (alfa de RGBA8_EAC) o 11 bits (R11/RG11) pasado a 8 como lo lee un búfer RGBA8.
void eac_block(uint64_t w, bool eleven, uint8_t out[16]) {
    const int base = int(w >> 56), mult = int((w >> 52) & 15), table = int((w >> 48) & 15);
    for (int j = 0; j < 16; ++j) {
        const int idx = int((w >> (45 - 3 * j)) & 7);
        const int x = j / 4, y = j % 4;
        int v;
        if (eleven) {
            int v11 = base * 8 + 4 + kEac[table][idx] * (mult ? mult * 8 : 1);
            v11 = v11 < 0 ? 0 : v11 > 2047 ? 2047 : v11;
            v = int((v11 * 255 + 1023) / 2047);
        } else {
            v = clamp255(base + kEac[table][idx] * mult);
        }
        out[y * 4 + x] = uint8_t(v);
    }
}

}  // namespace

bool decode_block_level(OutFormat f, const uint8_t* data, size_t size, uint32_t w, uint32_t h, uint8_t* rgba) {
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    const size_t bsize = (f == OutFormat::ETC2_RGBA8 || f == OutFormat::RG11_EAC) ? 16 : 8;
    if (!is_block_format(f) || size < size_t(bw) * bh * bsize) return false;
    for (uint32_t by = 0; by < bh; ++by) {
        for (uint32_t bx = 0; bx < bw; ++bx) {
            const uint8_t* b = data + (size_t(by) * bw + bx) * bsize;
            uint8_t px[16][4];
            uint8_t ch[16];
            switch (f) {
                case OutFormat::ETC2_RGB8:
                    if (!color_block(be64(b), false, px)) return false;
                    break;
                case OutFormat::ETC2_RGB8A1:
                    if (!color_block(be64(b), true, px)) return false;
                    break;
                case OutFormat::ETC2_RGBA8:
                    if (!color_block(be64(b + 8), false, px)) return false;
                    eac_block(be64(b), false, ch);
                    for (int i = 0; i < 16; ++i) px[i][3] = ch[i];
                    break;
                case OutFormat::R11_EAC:
                    eac_block(be64(b), true, ch);
                    for (int i = 0; i < 16; ++i) px[i][0] = ch[i], px[i][1] = 0, px[i][2] = 0, px[i][3] = 255;
                    break;
                case OutFormat::RG11_EAC:
                    eac_block(be64(b), true, ch);
                    for (int i = 0; i < 16; ++i) px[i][0] = ch[i], px[i][2] = 0, px[i][3] = 255;
                    eac_block(be64(b + 8), true, ch);
                    for (int i = 0; i < 16; ++i) px[i][1] = ch[i];
                    break;
                default: return false;
            }
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x) {
                    const uint32_t X = bx * 4 + x, Y = by * 4 + y;
                    if (X < w && Y < h) std::memcpy(rgba + (size_t(Y) * w + X) * 4, px[y * 4 + x], 4);
                }
        }
    }
    return true;
}

}  // namespace wl
