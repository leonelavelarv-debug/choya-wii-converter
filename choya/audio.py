"""Audio de Wii: DSP-ADPCM, BRSTM (RSTM) y .dsp sueltos -> PCM16 optimizado."""
from __future__ import annotations

import struct
from dataclasses import dataclass

import numpy as np


@dataclass
class DspChannel:
    coefs: list[int]          # 16 coeficientes con signo
    ps: int = 0               # predictor/escala inicial
    hist1: int = 0
    hist2: int = 0
    loop_ps: int = 0
    loop_hist1: int = 0
    loop_hist2: int = 0


@dataclass
class DecodedAudio:
    samples: np.ndarray       # (n, canales) int16
    rate: int
    loop_start: int | None    # en muestras, o None si no hace bucle
    codec: str
    adpcm: list[bytes] | None = None          # datos DSP-ADPCM crudos por canal (para guardarlos sin recodificar)
    adpcm_channels: list[DspChannel] | None = None


def decode_dsp_adpcm(data: bytes, num_samples: int, ch: DspChannel) -> np.ndarray:
    """Decodifica DSP-ADPCM: tramas de 8 bytes = 1 byte de cabecera + 14 muestras de 4 bits."""
    out = np.empty(num_samples, np.int16)
    coefs = ch.coefs
    h1, h2 = ch.hist1, ch.hist2
    n = 0
    pos = 0
    ln = len(data)
    while n < num_samples and pos < ln:
        ps = data[pos]
        scale = 1 << (ps & 0x0F)
        ci = (ps >> 4) & 7
        c1, c2 = coefs[ci * 2], coefs[ci * 2 + 1]
        frame = data[pos + 1:pos + 8]
        pos += 8
        for byte in frame:
            for nib in (byte >> 4, byte & 0x0F):
                if n >= num_samples:
                    break
                if nib >= 8:
                    nib -= 16
                s = ((nib * scale) << 11) + 1024 + c1 * h1 + c2 * h2
                s >>= 11
                if s > 32767:
                    s = 32767
                elif s < -32768:
                    s = -32768
                out[n] = s
                h2, h1 = h1, s
                n += 1
    if n < num_samples:
        out[n:] = 0
    return out


def _coefs(data: bytes, off: int) -> list[int]:
    return list(struct.unpack_from(">16h", data, off))


def read_dsp(data: bytes) -> DecodedAudio:
    """Archivo .dsp estándar de Nintendo (cabecera de 0x60 bytes, mono)."""
    num_samples, _nibbles, rate, loop, fmt, loop_start, _loop_end, _cur = struct.unpack_from(">IIIHHIII", data, 0)
    if fmt != 0:
        raise ValueError("DSP con formato distinto de ADPCM")
    ch = DspChannel(_coefs(data, 0x1C), *struct.unpack_from(">Hhhhhh", data, 0x3E))
    pcm = decode_dsp_adpcm(data[0x60:], num_samples, ch)
    loop_sample = None
    if loop:
        frame, nib = divmod(loop_start, 16)
        loop_sample = frame * 14 + max(0, nib - 2)
    return DecodedAudio(pcm[:, None], rate, loop_sample, "dsp-adpcm", [bytes(data[0x60:])], [ch])


def is_brstm(data: bytes) -> bool:
    return data[:4] == b"RSTM"


def read_brstm(data: bytes) -> DecodedAudio:
    if not is_brstm(data):
        raise ValueError("no es BRSTM")
    head_off = struct.unpack_from(">I", data, 0x10)[0]
    if data[head_off:head_off + 4] != b"HEAD":
        raise ValueError("BRSTM sin HEAD")
    base = head_off + 8
    refs = [struct.unpack_from(">II", data, base + i * 8)[1] for i in range(3)]
    si = base + refs[0]
    (codec, loop_flag, channels, _pad, rate, _pad2, loop_start, total, data_off, blocks,
     block_size, _spb, last_size, _last_samples, last_padded) = struct.unpack_from(">BBBBHHIIIIIIIII", data, si)
    ci = base + refs[2]
    nch = data[ci]
    chans: list[DspChannel] = []
    for c in range(nch):
        entry = base + struct.unpack_from(">II", data, ci + 4 + c * 8)[1]
        info = base + struct.unpack_from(">II", data, entry)[1]
        if codec == 2:
            ps, h1, h2, lps, lh1, lh2 = struct.unpack_from(">Hhhhhh", data, info + 0x22)
            chans.append(DspChannel(_coefs(data, info), ps, h1, h2, lps, lh1, lh2))
        else:
            chans.append(DspChannel([0] * 16))
    channels = min(channels, nch) or nch
    # Desentrelazar bloques: [bloque0 ch0][bloque0 ch1]...[bloque1 ch0]...
    per_ch = [bytearray() for _ in range(channels)]
    pos = data_off
    for b in range(blocks):
        size = block_size if b < blocks - 1 else last_size
        stride = block_size if b < blocks - 1 else last_padded
        for c in range(channels):
            per_ch[c].extend(data[pos:pos + size])
            pos += stride
    cols = []
    for c in range(channels):
        raw = bytes(per_ch[c])
        if codec == 2:
            cols.append(decode_dsp_adpcm(raw, total, chans[c]))
        elif codec == 1:
            cols.append(np.frombuffer(raw[: total * 2], ">i2").astype(np.int16))
        elif codec == 0:
            cols.append((np.frombuffer(raw[:total], np.int8).astype(np.int16) << 8))
        else:
            raise ValueError(f"códec BRSTM desconocido {codec}")
    n = min(len(c) for c in cols)
    pcm = np.stack([c[:n] for c in cols], axis=1)
    names = {0: "pcm8", 1: "pcm16", 2: "dsp-adpcm"}
    res = DecodedAudio(pcm, rate, loop_start if loop_flag else None, names[codec])
    if codec == 2:
        res.adpcm = [bytes(b) for b in per_ch]
        res.adpcm_channels = chans[:channels]
    return res


def resample(pcm: np.ndarray, src: int, dst: int) -> np.ndarray:
    if src == dst or pcm.shape[0] == 0:
        return pcm
    try:
        from math import gcd
        from scipy.signal import resample_poly
        g = gcd(src, dst)
        out = resample_poly(pcm.astype(np.float32), dst // g, src // g, axis=0)
    except ImportError:
        n_out = int(round(pcm.shape[0] * dst / src))
        x_old = np.arange(pcm.shape[0])
        x_new = np.linspace(0, pcm.shape[0] - 1, n_out)
        out = np.stack([np.interp(x_new, x_old, pcm[:, c]) for c in range(pcm.shape[1])], 1)
    return np.clip(np.rint(out), -32768, 32767).astype(np.int16)


def adpcm_payload(audio: DecodedAudio) -> bytes:
    """Datos para una entrada AUDIO_DSPADPCM del paquete (little-endian).
    Por canal: cabecera de 48 bytes {int16 coefs[16]; int16 hist1, hist2, loop_hist1, loop_hist2;
    uint8 loop_ps, pad[3]; uint32 bytes} seguida de los datos ADPCM, alineado a 16."""
    out = bytearray()
    for raw, ch in zip(audio.adpcm, audio.adpcm_channels):
        out += struct.pack("<16hhhhhB3xI", *ch.coefs, ch.hist1, ch.hist2, ch.loop_hist1, ch.loop_hist2,
                           ch.loop_ps & 0xFF, len(raw))
        out += raw
        out += b"\x00" * ((-len(out)) % 16)
    return bytes(out)


def can_keep_adpcm(audio: DecodedAudio, mono_sfx: bool, music_min_seconds: float) -> bool:
    """Se guarda sin recodificar si es ADPCM y no hay que cambiar el número de canales."""
    if audio.adpcm is None or audio.samples.shape[1] > 2:
        return False
    seconds = audio.samples.shape[0] / max(1, audio.rate)
    downmix = mono_sfx and seconds < music_min_seconds and audio.samples.shape[1] > 1
    return not downmix


def optimize(audio: DecodedAudio, rate: int, mono_sfx: bool, music_min_seconds: float) -> DecodedAudio:
    pcm = audio.samples
    seconds = pcm.shape[0] / max(1, audio.rate)
    is_music = seconds >= music_min_seconds
    if mono_sfx and not is_music and pcm.shape[1] > 1:
        pcm = pcm.astype(np.int32).mean(axis=1, keepdims=True).astype(np.int16)
    elif pcm.shape[1] > 2:
        pcm = pcm[:, :2]  # el equipo tiene salida estéreo
    target = min(rate, audio.rate)
    out = resample(pcm, audio.rate, target)
    loop = None if audio.loop_start is None else int(audio.loop_start * target / audio.rate)
    return DecodedAudio(out, target, loop, audio.codec)


def wav_bytes(audio: DecodedAudio) -> bytes:
    """WAV PCM16 (útil para revisar el resultado en la PC)."""
    pcm = audio.samples
    ch = pcm.shape[1]
    raw = pcm.astype("<i2").tobytes()
    return (b"RIFF" + struct.pack("<I", 36 + len(raw)) + b"WAVEfmt " +
            struct.pack("<IHHIIHH", 16, 1, ch, audio.rate, audio.rate * ch * 2, ch * 2, 16) +
            b"data" + struct.pack("<I", len(raw)) + raw)
