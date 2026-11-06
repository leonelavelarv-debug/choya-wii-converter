// wl_info: revisa un paquete .wlpk en la consola o en la PC (no necesita GPU).
//   wl_info assets.wlpk            resumen
//   wl_info assets.wlpk -l         lista de entradas
//   wl_info --dsp archivo.dsp out.raw   decodifica un .dsp a PCM16 (para pruebas)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <vector>

#include "wl/audio.h"
#include "wl/pack.h"

static int decode_dsp_file(const char* in, const char* out) {
    std::ifstream f(in, std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 0x60) return 1;
    auto be32 = [&](size_t o) { return uint32_t(d[o]) << 24 | uint32_t(d[o + 1]) << 16 | uint32_t(d[o + 2]) << 8 | d[o + 3]; };
    auto be16 = [&](size_t o) { return int16_t(uint16_t(d[o]) << 8 | d[o + 1]); };
    const uint32_t samples = be32(0);
    wl::DspState st{};
    for (int i = 0; i < 16; ++i) st.coefs[i] = be16(0x1C + i * 2);
    st.hist1 = be16(0x40);
    st.hist2 = be16(0x42);
    std::vector<int16_t> pcm(samples);
    const size_t n = wl::dsp_decode(d.data() + 0x60, d.size() - 0x60, samples, st, pcm.data());
    std::ofstream o(out, std::ios::binary);
    o.write(reinterpret_cast<const char*>(pcm.data()), std::streamsize(n * 2));
    std::printf("%zu muestras\n", n);
    return 0;
}

// Vuelca a PCM16 un audio del paquete (PCM16 o DSP-ADPCM), con 'extra' cuadros de más para probar el bucle.
static int dump_audio(const char* pack_path, const char* disc_path, const char* out, size_t extra) {
    wl::Pack pack;
    if (!pack.open(pack_path)) return 1;
    const wl::PackEntry* e = pack.find_audio(disc_path);
    wl::AudioStream s;
    if (!e || !s.open(pack, *e)) {
        std::fprintf(stderr, "audio no encontrado o dañado: %s\n", disc_path);
        return 1;
    }
    std::vector<int16_t> buf((s.frames() + extra) * size_t(s.channels()));
    const size_t n = s.read(buf.data(), s.frames() + extra, s.channels());
    std::ofstream o(out, std::ios::binary);
    o.write(reinterpret_cast<const char*>(buf.data()), std::streamsize(n * size_t(s.channels()) * 2));
    std::printf("%zu cuadros, %d canales, formato %u\n", n, s.channels(), e->format);
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 4 && std::strcmp(argv[1], "--dsp") == 0) return decode_dsp_file(argv[2], argv[3]);
    if (argc >= 5 && std::strcmp(argv[1], "--pcm") == 0)
        return dump_audio(argv[2], argv[3], argv[4], argc > 5 ? size_t(std::atol(argv[5])) : 0);
    if (argc < 2) {
        std::fprintf(stderr, "uso: wl_info assets.wlpk [-l]\n");
        return 2;
    }
    wl::Pack pack;
    if (!pack.open(argv[1])) {
        std::fprintf(stderr, "error: %s\n", pack.error().c_str());
        return 1;
    }
    const bool list = argc > 2 && std::strcmp(argv[2], "-l") == 0;
    std::map<std::string, std::pair<int, uint64_t>> by_fmt;
    uint64_t tex_bytes = 0, audio_bytes = 0;
    int textures = 0, audio = 0, bad_order = 0;
    for (uint32_t i = 0; i < pack.size(); ++i) {
        const wl::PackEntry& e = pack.entry(i);
        if (i && pack.entry(i - 1).key >= e.key) ++bad_order;
        if (e.kind == uint8_t(wl::Kind::Texture)) {
            ++textures;
            tex_bytes += e.data_size;
            auto& slot = by_fmt[wl::out_format_name(wl::OutFormat(e.format))];
            slot.first++;
            slot.second += e.data_size;
            // La clave debe coincidir con crc/dimensiones/formato guardados.
            const uint64_t k = wl::texture_key_from_crc(e.extra1, (e.key >> 22 & 0x3FF) + 1,
                                                        (e.key >> 12 & 0x3FF) + 1, uint8_t(e.extra0));
            if (k != e.key) ++bad_order;
            if (list)
                std::printf("TEX %016llx %-14s %4ux%-4u m%u %8llu B  %s\n", (unsigned long long)e.key,
                            wl::out_format_name(wl::OutFormat(e.format)), e.width, e.height, e.count,
                            (unsigned long long)e.data_size, pack.name(e));
        } else if (e.kind == uint8_t(wl::Kind::Audio)) {
            ++audio;
            audio_bytes += e.data_size;
            if (list)
                std::printf("AUD %016llx %-9s %u Hz %u can. %.1f s  %s\n", (unsigned long long)e.key,
                            e.format == wl::AUDIO_DSPADPCM ? "DSP-ADPCM" : "PCM16", e.extra0,
                            e.count, double(e.extra2) / e.extra0, pack.name(e));
        }
    }
    std::printf("paquete: %s\n%u entradas, banderas=%u\n", argv[1], pack.size(), pack.flags());
    std::printf("texturas: %d (%.2f MiB)\n", textures, tex_bytes / 1048576.0);
    for (auto& kv : by_fmt)
        std::printf("  %-14s %5d  %.2f MiB\n", kv.first.c_str(), kv.second.first, kv.second.second / 1048576.0);
    std::printf("audio: %d (%.2f MiB)\n", audio, audio_bytes / 1048576.0);
    if (bad_order) {
        std::printf("ERROR: %d entradas fuera de orden o con clave inconsistente\n", bad_order);
        return 1;
    }
    std::printf("OK\n");
    return 0;
}
