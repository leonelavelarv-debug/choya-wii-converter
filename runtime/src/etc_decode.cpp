// Decodificador ETC2/EAC completo en la CPU (Khronos, OpenGL ES 3.0, anexo C). Sirve para dos cosas:
//   - comprobar lo que devuelve la GPU (wl_view --verificar, choya_render --verificar);
//   - el respaldo de gl_upload: si el contexto no acepta ETC2/EAC (GLES 2, driver que rechaza el formato o
//     WL_SIN_ETC2=1), cada nivel se descomprime aquí y se sube sin comprimir.
// Cubre ETC1 individual y diferencial, los modos T, H y planar de ETC2, el alfa de 1 bit (RGB8A1, también en T y H),
// EAC de 8 bits (alfa de RGBA8_EAC) y EAC de 11 bits sin signo (R11/RG11). Las variantes con signo y sRGB no las
// escribe el conversor y devuelven false.
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

const int kEtc2Dist[8] = {3, 6, 11, 16, 23, 32, 41, 64};

int ext4(int v) { return (v << 4) | v; }
int ext5(int v) { return (v << 3) | (v >> 2); }
int ext6(int v) { return (v << 2) | (v >> 4); }
int ext7(int v) { return (v << 1) | (v >> 6); }

// Índice de 2 bits del píxel (x, y) del bloque: bit menos significativo en j, el más significativo en j + 16.
int pixel_index(uint32_t lo, int x, int y) {
    const int j = x * 4 + y;
    return int((((lo >> (j + 16)) & 1) << 1) | ((lo >> j) & 1));
}

// Modos T y H: cuatro colores de pintura; con alfa de 1 bit y "opaco" en 0, el índice 2 es negro transparente.
void paint_block(const int paint[4][3], uint32_t lo, bool opaque, uint8_t out[16][4]) {
    for (int x = 0; x < 4; ++x)
        for (int y = 0; y < 4; ++y) {
            const int idx = pixel_index(lo, x, y);
            uint8_t* px = out[y * 4 + x];
            if (!opaque && idx == 2) {
                px[0] = px[1] = px[2] = px[3] = 0;
                continue;
            }
            for (int c = 0; c < 3; ++c) px[c] = uint8_t(clamp255(paint[idx][c]));
            px[3] = 255;
        }
}

// Color de un bloque ETC1/ETC2 (16 píxeles en orden de filas: out[y*4+x]). punch: formato RGB8A1 (el bit
// diferencial pasa a ser "opaco" y siempre se usa el modo diferencial o T/H/planar).
bool color_block(uint64_t w, bool punch, uint8_t out[16][4]) {
    const uint32_t hi = uint32_t(w >> 32), lo = uint32_t(w);
    const bool diff = punch || ((hi >> 1) & 1);
    const bool opaque = !punch || ((hi >> 1) & 1);
    const bool flip = hi & 1;
    const int t[2] = {int((hi >> 5) & 7), int((hi >> 2) & 7)};
    int base[2][3];
    if (diff) {
        int r5[3], d3[3];
        for (int c = 0; c < 3; ++c) {
            const int s5 = 27 - 8 * c;
            r5[c] = int((hi >> s5) & 31);
            d3[c] = int((hi >> (s5 - 3)) & 7);
            if (d3[c] >= 4) d3[c] -= 8;
        }
        const bool over_r = r5[0] + d3[0] < 0 || r5[0] + d3[0] > 31;
        const bool over_g = r5[1] + d3[1] < 0 || r5[1] + d3[1] > 31;
        const bool over_b = r5[2] + d3[2] < 0 || r5[2] + d3[2] > 31;
        if (over_r) {  // modo T
            const int c1[3] = {ext4(int(((w >> 59) & 3) << 2 | ((w >> 56) & 3))), ext4(int((w >> 52) & 15)),
                               ext4(int((w >> 48) & 15))};
            const int c2[3] = {ext4(int((w >> 44) & 15)), ext4(int((w >> 40) & 15)), ext4(int((w >> 36) & 15))};
            const int d = kEtc2Dist[((w >> 34) & 3) << 1 | ((w >> 32) & 1)];
            int paint[4][3];
            for (int c = 0; c < 3; ++c) {
                paint[0][c] = c1[c];
                paint[1][c] = c2[c] + d;
                paint[2][c] = c2[c];
                paint[3][c] = c2[c] - d;
            }
            paint_block(paint, lo, opaque, out);
            return true;
        }
        if (over_g) {  // modo H
            const int c1[3] = {ext4(int((w >> 59) & 15)), ext4(int(((w >> 56) & 7) << 1 | ((w >> 52) & 1))),
                               ext4(int(((w >> 51) & 1) << 3 | ((w >> 47) & 7)))};
            const int c2[3] = {ext4(int((w >> 43) & 15)), ext4(int((w >> 39) & 15)), ext4(int((w >> 35) & 15))};
            const int v1 = (c1[0] << 16) | (c1[1] << 8) | c1[2], v2 = (c2[0] << 16) | (c2[1] << 8) | c2[2];
            const int d = kEtc2Dist[((w >> 34) & 1) << 2 | ((w >> 32) & 1) << 1 | (v1 >= v2 ? 1 : 0)];
            int paint[4][3];
            for (int c = 0; c < 3; ++c) {
                paint[0][c] = c1[c] + d;
                paint[1][c] = c1[c] - d;
                paint[2][c] = c2[c] + d;
                paint[3][c] = c2[c] - d;
            }
            paint_block(paint, lo, opaque, out);
            return true;
        }
        if (over_b) {  // modo planar (siempre opaco)
            const int o[3] = {ext6(int((w >> 57) & 63)), ext7(int(((w >> 56) & 1) << 6 | ((w >> 49) & 63))),
                              ext6(int(((w >> 48) & 1) << 5 | ((w >> 43) & 3) << 3 | ((w >> 39) & 7)))};
            const int hh[3] = {ext6(int(((w >> 34) & 31) << 1 | ((w >> 32) & 1))), ext7(int((w >> 25) & 127)),
                               ext6(int((w >> 19) & 63))};
            const int v[3] = {ext6(int((w >> 13) & 63)), ext7(int((w >> 6) & 127)), ext6(int(w & 63))};
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x) {
                    uint8_t* px = out[y * 4 + x];
                    for (int c = 0; c < 3; ++c)
                        px[c] = uint8_t(clamp255((x * (hh[c] - o[c]) + y * (v[c] - o[c]) + 4 * o[c] + 2) >> 2));
                    px[3] = 255;
                }
            return true;
        }
        for (int c = 0; c < 3; ++c) {
            base[0][c] = ext5(r5[c]);
            base[1][c] = ext5(r5[c] + d3[c]);
        }
    } else {
        for (int c = 0; c < 3; ++c) {
            base[0][c] = int((hi >> (28 - 8 * c)) & 15) * 17;
            base[1][c] = int((hi >> (24 - 8 * c)) & 15) * 17;
        }
    }
    for (int x = 0; x < 4; ++x) {
        for (int y = 0; y < 4; ++y) {
            const int idx = pixel_index(lo, x, y);
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
