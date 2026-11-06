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

bool PcmStream::open(const Pack& pack, const PackEntry& e) {
    if (e.kind != uint8_t(Kind::Audio) || e.format != 1 || e.count == 0) return false;
    pcm_ = reinterpret_cast<const int16_t*>(pack.data(e));
    channels_ = e.count;
    frames_ = e.extra2;
    rate_ = e.extra0;
    loops_ = (e.flags & AUDIOFLAG_LOOP) != 0 && e.extra1 < frames_;
    loop_start_ = loops_ ? e.extra1 : 0;
    pos_ = 0;
    return frames_ * size_t(channels_) * 2 <= e.data_size;
}

size_t PcmStream::read(int16_t* out, size_t frames, int out_channels) {
    size_t written = 0;
    while (written < frames) {
        if (pos_ >= frames_) {
            if (!loops_) break;
            pos_ = loop_start_;
        }
        const size_t n = std::min(frames - written, frames_ - pos_);
        const int16_t* src = pcm_ + pos_ * size_t(channels_);
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
