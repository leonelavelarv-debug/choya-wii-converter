// wl_selftest: comprueba que el runtime y el conversor coinciden (claves, caché, audio).
//   wl_selftest assets.wlpk nivel0.gx ancho alto formato_gx ruta_audio
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#include "wl/audio.h"
#include "wl/pack.h"
#include "wl/texcache.h"

static int fails = 0;
#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            std::printf("FALLA %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++fails;                                                \
        }                                                           \
    } while (0)

int main(int argc, char** argv) {
    // Valores calculados con choya.pack.path_key
    CHECK(wl::path_key("Sound/golpe.dsp") == wl::path_key("/sound/GOLPE.dsp"));
    CHECK(wl::path_key("\\Sound\\golpe.dsp") == wl::path_key("Sound/golpe.dsp"));
    if (argc < 7) {
        std::printf("uso: wl_selftest pack nivel0.gx w h fmt ruta_audio\n");
        return fails ? 1 : 0;
    }
    wl::Pack pack;
    CHECK(pack.open(argv[1]));
    std::ifstream f(argv[2], std::ios::binary);
    std::vector<uint8_t> gx((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const uint16_t w = uint16_t(std::atoi(argv[3])), h = uint16_t(std::atoi(argv[4]));
    const uint8_t fmt = uint8_t(std::atoi(argv[5]));
    CHECK(wl::gx_level_size(fmt, w, h) == gx.size());

    int uploads = 0;
    const wl::PackEntry* seen = nullptr;
    wl::TextureCache cache(pack, [&](const wl::PackEntry& e, const uint8_t*) {
        ++uploads;
        seen = &e;
        return uint32_t(100 + uploads);
    });
    const uint32_t id = cache.get({gx.data(), w, h, fmt});
    CHECK(id == 101);
    CHECK(seen && seen->width > 0);
    CHECK(cache.get({gx.data(), w, h, fmt}) == 101);       // acierto por puntero
    std::vector<uint8_t> copy = gx;
    CHECK(cache.get({copy.data(), w, h, fmt}) == 101);     // acierto por clave, otro puntero
    CHECK(uploads == 1);
    gx[gx.size() / 2] ^= 0xFF;                              // el juego reescribió esa memoria
    CHECK(cache.get({gx.data(), w, h, fmt}) == 0);          // ya no está en el paquete
    CHECK(cache.stats().rehashes == 1 && cache.stats().misses == 1);

    const wl::PackEntry* a = pack.find_audio(argv[6]);
    CHECK(a != nullptr);
    if (a) {
        wl::AudioStream s;
        CHECK(s.open(pack, *a));
        std::vector<int16_t> buf(size_t(a->extra2) * 2 + 64);
        const size_t n = s.read(buf.data(), buf.size() / 2, 2);
        CHECK(n == a->extra2);  // sin bucle: se detiene al final
        CHECK(s.finished());
        std::printf("audio %u Hz, %u cuadros\n", a->extra0, a->extra2);
    }
    std::printf(fails ? "%d fallas\n" : "selftest OK\n", fails);
    return fails ? 1 : 0;
}
