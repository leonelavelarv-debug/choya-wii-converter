#include "wl/audio.h"

#include <algorithm>

namespace wl {

size_t dsp_decode(const uint8_t* src, size_t src_bytes, size_t samples, DspState& st, int16_t* out) {
    size_t n = 0;
    int32_t h1 = st.hist1, h2 = st.hist2;
    for (size_t pos = 0; n < samples && pos + 8 <= src_bytes; pos += 8) {
        const uint8_t ps = src[pos];
        const int32_t scale = 1 << (ps & 0x0F);
        const int ci = (ps >> 4) & 7;
        const int32_t c1 = st.coefs[ci * 2], c2 = st.coefs[ci * 2 + 1];
        for (int i = 0; i < 14 && n < samples; ++i) {
            const uint8_t byte = src[pos + 1 + i / 2];
            int32_t nib = (i & 1) ? (byte & 0x0F) : (byte >> 4);
            if (nib >= 8) nib -= 16;
            int32_t s = (((nib * scale) << 11) + 1024 + c1 * h1 + c2 * h2) >> 11;
            s = std::clamp(s, -32768, 32767);
            out[n++] = int16_t(s);
            h2 = h1;
            h1 = s;
        }
    }
    st.hist1 = int16_t(h1);
    st.hist2 = int16_t(h2);
    return n;
}

bool AudioStream::open(const Pack& pack, const PackEntry& e) {
    if (e.kind != uint8_t(Kind::Audio) || e.count == 0 || e.count > MAX_CH) return false;
    format_ = e.format;
    channels_ = e.count;
    frames_ = e.extra2;
    rate_ = e.extra0;
    loops_ = (e.flags & AUDIOFLAG_LOOP) != 0 && e.extra1 < frames_;
    loop_start_ = loops_ ? e.extra1 : 0;
    const uint8_t* p = pack.data(e);
    if (format_ == AUDIO_PCM16) {
        pcm_ = reinterpret_cast<const int16_t*>(p);
        pos_ = 0;
        return frames_ * size_t(channels_) * 2 <= e.data_size;
    }
    if (format_ != AUDIO_DSPADPCM) return false;
    // Por canal: int16 coefs[16]; int16 hist1, hist2, loop_hist1, loop_hist2; u8 loop_ps; pad[3]; u32 bytes; datos.
    size_t off = 0;
    for (int c = 0; c < channels_; ++c) {
        if (off + 48 > e.data_size) return false;
        const uint8_t* h = p + off;
        AdpcmCh& ch = ch_[c];
        std::memcpy(ch.st.coefs, h, 32);
        std::memcpy(&ch.start_h1, h + 32, 2);
        std::memcpy(&ch.start_h2, h + 34, 2);
        std::memcpy(&ch.loop_h1, h + 36, 2);
        std::memcpy(&ch.loop_h2, h + 38, 2);
        uint32_t bytes = 0;
        std::memcpy(&bytes, h + 44, 4);
        ch.data = h + 48;
        ch.bytes = bytes;
        off += 48 + bytes;
        off = (off + 15) & ~size_t(15);
        if (off > e.data_size + 15) return false;
    }
    rewind();
    return true;
}

void AudioStream::rewind() { seek(0, false); }

void AudioStream::seek(size_t sample, bool loop) {
    pos_ = sample;
    buf_frame_ = SIZE_MAX;
    if (format_ != AUDIO_DSPADPCM) return;
    for (int c = 0; c < channels_; ++c) {
        // El historial guardado corresponde al inicio del archivo o al punto de bucle.
        ch_[c].st.hist1 = loop ? ch_[c].loop_h1 : ch_[c].start_h1;
        ch_[c].st.hist2 = loop ? ch_[c].loop_h2 : ch_[c].start_h2;
    }
    // Si el bucle no cae al inicio de una trama, se decodifica desde su trama (aproximación habitual).
}

bool AudioStream::decode_frame() {
    const size_t frame = pos_ / 14;
    if (frame == buf_frame_) return true;
    for (int c = 0; c < channels_; ++c) {
        AdpcmCh& ch = ch_[c];
        const size_t byte = frame * 8;
        if (byte + 8 > ch.bytes) return false;
        int16_t tmp[14] = {};
        dsp_decode(ch.data + byte, 8, 14, ch.st, tmp);
        for (int i = 0; i < 14; ++i) buf_[i * channels_ + c] = tmp[i];
    }
    buf_frame_ = frame;
    return true;
}

size_t AudioStream::read(int16_t* out, size_t frames, int out_channels) {
    size_t written = 0;
    while (written < frames) {
        if (pos_ >= frames_) {
            if (!loops_) break;
            seek(loop_start_, true);
        }
        const int16_t* src;
        size_t n;
        if (format_ == AUDIO_PCM16) {
            n = std::min(frames - written, frames_ - pos_);
            src = pcm_ + pos_ * size_t(channels_);
        } else {
            if (!decode_frame()) break;
            const size_t in_frame = pos_ % 14;
            n = std::min({frames - written, frames_ - pos_, size_t(14) - in_frame});
            src = buf_ + in_frame * size_t(channels_);
        }
        int16_t* dst = out + written * size_t(out_channels);
        for (size_t i = 0; i < n; ++i) {
            if (channels_ == out_channels) {
                for (int c = 0; c < channels_; ++c) dst[i * out_channels + c] = src[i * channels_ + c];
            } else if (channels_ == 1) {
                for (int c = 0; c < out_channels; ++c) dst[i * out_channels + c] = src[i];
            } else {  // estéreo -> mono
                dst[i] = int16_t((int32_t(src[i * 2]) + src[i * 2 + 1]) / 2);
            }
        }
        pos_ += n;
        written += n;
    }
    return written;
}

}  // namespace wl
