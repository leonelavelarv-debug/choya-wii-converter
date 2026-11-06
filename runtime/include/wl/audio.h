// Audio: decodificador DSP-ADPCM (para la capa AX) y flujos PCM16 del paquete.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

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

enum AudioFormat : uint8_t { AUDIO_PCM16 = 1, AUDIO_DSPADPCM = 2 };

// Flujo de audio del paquete con bucle: PCM16 entrelazado o DSP-ADPCM decodificado al vuelo.
class AudioStream {
public:
    bool open(const Pack& pack, const PackEntry& e);
    // Escribe 'frames' cuadros en out (out_channels = 1 o 2). Devuelve cuadros escritos.
    size_t read(int16_t* out, size_t frames, int out_channels);
    void rewind();
    uint32_t rate() const { return rate_; }
    int channels() const { return channels_; }
    size_t frames() const { return frames_; }
    bool finished() const { return !loops_ && pos_ >= frames_; }

private:
    static constexpr int MAX_CH = 2;
    struct AdpcmCh {
        const uint8_t* data = nullptr;
        size_t bytes = 0;
        DspState st{};
        int16_t start_h1 = 0, start_h2 = 0, loop_h1 = 0, loop_h2 = 0;
    };
    bool decode_frame();   // ADPCM: llena buf_ con la trama que contiene pos_
    void seek(size_t sample, bool loop);

    uint8_t format_ = 0;
    const int16_t* pcm_ = nullptr;
    AdpcmCh ch_[MAX_CH];
    int16_t buf_[14 * MAX_CH];
    size_t buf_frame_ = SIZE_MAX;  // índice de trama decodificada en buf_
    size_t frames_ = 0;
    size_t pos_ = 0;
    size_t loop_start_ = 0;
    bool loops_ = false;
    uint32_t rate_ = 0;
    int channels_ = 0;
};

using PcmStream = AudioStream;  // nombre anterior

}  // namespace wl
