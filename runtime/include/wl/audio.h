// Audio: decodificador DSP-ADPCM (para la capa AX) y flujos PCM16 del paquete.
#pragma once
#include <cstddef>
#include <cstdint>

#include "wl/pack.h"

namespace wl {

struct DspState {
    int16_t coefs[16];
    int16_t hist1 = 0;
    int16_t hist2 = 0;
};

// Decodifica 'samples' muestras de DSP-ADPCM (tramas de 8 bytes = 14 muestras).
// Devuelve las muestras escritas. Actualiza el historial para poder seguir decodificando.
size_t dsp_decode(const uint8_t* src, size_t src_bytes, size_t samples, DspState& st, int16_t* out);

// Flujo PCM16 entrelazado desde el paquete, con bucle.
class PcmStream {
public:
    bool open(const Pack& pack, const PackEntry& e);
    // Escribe 'frames' cuadros mezclados en out (out_channels = 1 o 2). Devuelve cuadros escritos.
    size_t read(int16_t* out, size_t frames, int out_channels);
    void rewind() { pos_ = 0; }
    uint32_t rate() const { return rate_; }
    int channels() const { return channels_; }
    bool finished() const { return !loops_ && pos_ >= frames_; }

private:
    const int16_t* pcm_ = nullptr;
    size_t frames_ = 0;
    size_t pos_ = 0;
    size_t loop_start_ = 0;
    bool loops_ = false;
    uint32_t rate_ = 0;
    int channels_ = 0;
};

}  // namespace wl
