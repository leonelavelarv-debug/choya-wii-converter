"""Efectos de sonido de un BRSAR (archivo de sonido de NW4R): los sonidos de onda (RWSD) salen como audios sueltos.

Se sigue a ogws (snd_SoundArchiveFile.h, snd_WsdFile.h, snd_WaveFile.h, snd_MemorySoundArchive.cpp):
  RSAR: cabecera con SYMB (nombres), INFO (tablas de sonidos, archivos y grupos) y FILE (los datos de los grupos).
  INFO: referencias de 8 bytes {tipo, tipo de dato, reservado, valor}; con tipo 1 el valor es relativo al cuerpo del
        bloque INFO. Sonido de onda (tipo 3): archivo RWSD + número de sonido dentro de él.
  Archivo → posición (grupo, elemento): el RWSD está en grupo.offset + elemento.offset y sus muestras en
        grupo.waveDataOffset + elemento.waveDataOffset.
  RWSD: DATA (sonidos: notas → índice de onda) y WAVE (información de cada onda: formato, canales, frecuencia,
        bucle y, por canal, el desplazamiento de las muestras y los coeficientes ADPCM).
Las secuencias (RSEQ, como MIDI con instrumentos de un RBNK) no son audio grabado: se cuentan y siguen en el disco.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field

import numpy as np

from .audio import DecodedAudio, DspChannel, decode_dsp_adpcm

SOUND_SEQ, SOUND_STRM, SOUND_WAVE = 1, 2, 3


class BrsarError(ValueError):
    pass


@dataclass
class BrsarResult:
    sounds: list[tuple[str, DecodedAudio]] = field(default_factory=list)   # (nombre del sonido, audio)
    counts: dict = field(default_factory=dict)                             # tipos de sonido y omitidos
    warnings: list[str] = field(default_factory=list)


def _u32(d, o):
    return struct.unpack_from(">I", d, o)[0]


def _ref(d: bytes, o: int, base: int) -> int | None:
    """Util::DataRef en o: dirección absoluta en el archivo, o None."""
    rtype, _dtype, _res, value = struct.unpack_from(">BBHI", d, o)
    if rtype == 1 or (rtype == 0 and value):   # en archivos anteriores a la 1.1 la tabla guarda el valor sin tipo
        return base + value
    return None


def _table(d: bytes, o: int | None, base: int, limit: int = 1 << 16) -> list[int | None]:
    if o is None or o + 4 > len(d):
        return []
    n = _u32(d, o)
    if n > limit:
        raise BrsarError("tabla dañada")
    return [_ref(d, o + 4 + 8 * i, base) for i in range(n)]


def _cstr(d: bytes, o: int) -> str:
    end = d.find(b"\x00", o)
    return d[o:end if end >= 0 else len(d)].decode("shift_jis", errors="replace")


def dsp_sample(addr: int) -> int:
    """AxVoice::GetSampleByDspAddress para ADPCM: dirección en nibbles (2 de cabecera por trama de 16) → muestra."""
    return (addr // 16) * 14 + max(0, (addr % 16) - 2)


def read_wave(d: bytes, info: int, wave_addr: int) -> DecodedAudio:
    """Una onda (WaveInfo en `info`; muestras desde wave_addr) a PCM, con el ADPCM crudo por canal."""
    fmt, loop, nch, rate24, rate = struct.unpack_from(">BBBBH", d, info)
    loop_start, loop_end, ch_table, data_off = struct.unpack_from(">IIII", d, info + 8)
    rate = (rate24 << 16) + rate
    if nch == 0 or nch > 8 or rate == 0:
        raise BrsarError("onda dañada")
    if fmt == 2:
        total = dsp_sample(loop_end) + 1
        start = dsp_sample(loop_start)
    else:
        total = loop_end + 1
        start = loop_start
    if total <= 0 or total > 50_000_000:
        raise BrsarError("onda con largo imposible")
    cols, raws, chans = [], [], []
    for c in range(nch):
        ci = info + _u32(d, info + ch_table + 4 * c)
        ch_data, adpcm_off = struct.unpack_from(">II", d, ci)
        pos = wave_addr + data_off + ch_data
        if fmt == 2:
            nbytes = (total + 13) // 14 * 8
            raw = bytes(d[pos:pos + nbytes])
            if len(raw) < nbytes:
                raise BrsarError("muestras fuera del archivo")
            a = info + adpcm_off
            coefs = list(struct.unpack_from(">16h", d, a))
            _gain, ps, h1, h2, lps, lh1, lh2 = struct.unpack_from(">Hhhhhhh", d, a + 0x20)
            ch = DspChannel(coefs, ps, h1, h2, lps, lh1, lh2)
            cols.append(decode_dsp_adpcm(raw, total, ch))
            raws.append(raw)
            chans.append(ch)
        elif fmt == 1:
            raw = d[pos:pos + 2 * total]
            cols.append(np.frombuffer(raw, ">i2").astype(np.int16))
        else:
            raw = d[pos:pos + total]
            cols.append(np.frombuffer(raw, np.int8).astype(np.int16) << 8)
    n = min(len(c) for c in cols)
    pcm = np.stack([c[:n] for c in cols], axis=1)
    out = DecodedAudio(pcm, rate, start if loop else None, {0: "pcm8", 1: "pcm16", 2: "dsp-adpcm"}.get(fmt, "?"))
    if fmt == 2:
        out.adpcm, out.adpcm_channels = raws, chans
    return out


def _rwsd_wave(d: bytes, rwsd: int, size: int, sub: int) -> int:
    """Índice de onda (en el bloque WAVE) del sonido `sub` de un RWSD: la primera nota de su tabla."""
    if d[rwsd:rwsd + 4] != b"RWSD":
        raise BrsarError("no es RWSD")
    data_blk = rwsd + _u32(d, rwsd + 0x10)
    base = data_blk + 8                       # las referencias son relativas a wsdCount
    count = _u32(d, base)
    if not 0 <= sub < count:
        raise BrsarError(f"sonido {sub} fuera del RWSD ({count})")
    wsd = _ref(d, base + 4 + 8 * sub, base)
    if wsd is None:
        raise BrsarError("sonido sin datos")
    notes = _table(d, _ref(d, wsd + 0x10, base), base)
    if not notes or notes[0] is None:
        raise BrsarError("sonido sin notas")
    return struct.unpack_from(">i", d, notes[0])[0]


def _rwsd_wave_info(d: bytes, rwsd: int, index: int) -> int:
    version, = struct.unpack_from(">H", d, rwsd + 6)
    wave_blk = rwsd + _u32(d, rwsd + 0x18)
    if version <= 0x0100:                     # WaveBlockOld: sin cuenta
        return wave_blk + _u32(d, wave_blk + 8 + 4 * index)
    count = _u32(d, wave_blk + 8)
    if not 0 <= index < count:
        raise BrsarError(f"onda {index} fuera del RWSD ({count})")
    return wave_blk + _u32(d, wave_blk + 0xC + 4 * index)


def read_brsar(d: bytes) -> BrsarResult:
    if d[:4] != b"RSAR":
        raise BrsarError("no es RSAR")
    if struct.unpack_from(">H", d, 4)[0] != 0xFEFF:
        raise BrsarError("orden de bytes no soportado")
    symb, _symb_size, info, _info_size = struct.unpack_from(">IIII", d, 0x10)
    res = BrsarResult()
    # Nombres: SYMB → tabla de desplazamientos (relativos al cuerpo del bloque).
    strings: list[int] = []
    if symb and d[symb:symb + 4] == b"SYMB":
        sbase = symb + 8
        table = sbase + _u32(d, sbase)
        n = _u32(d, table)
        strings = [sbase + _u32(d, table + 4 + 4 * i) for i in range(min(n, 1 << 16))]
    if d[info:info + 4] != b"INFO":
        raise BrsarError("sin bloque INFO")
    base = info + 8
    sounds = _table(d, _ref(d, base, base), base)
    files = _table(d, _ref(d, base + 0x18, base), base)
    groups = _table(d, _ref(d, base + 0x20, base), base)

    def file_pos(file_id: int):
        if not 0 <= file_id < len(files) or files[file_id] is None:
            return None
        fi = files[file_id]
        if _ref(d, fi + 0xC, base) is not None:          # archivo externo (fuera del BRSAR)
            return None
        pos = _table(d, _ref(d, fi + 0x14, base), base)
        if not pos or pos[0] is None:
            return None
        gid, idx = struct.unpack_from(">II", d, pos[0])
        if not 0 <= gid < len(groups) or groups[gid] is None:
            return None
        g = groups[gid]
        g_off, _g_size, g_wave, _g_wsize = struct.unpack_from(">IIII", d, g + 0x10)
        items = _table(d, _ref(d, g + 0x20, base), base)
        if not 0 <= idx < len(items) or items[idx] is None:
            return None
        _fid, i_off, i_size, i_wave, _i_wsize = struct.unpack_from(">IIIII", d, items[idx])
        return g_off + i_off, i_size, g_wave + i_wave

    seen: dict[tuple[int, int], DecodedAudio] = {}
    for sid, s in enumerate(sounds):
        if s is None:
            continue
        string_id, file_id = struct.unpack_from(">II", d, s)
        stype = d[s + 0x16]
        kind = {SOUND_SEQ: "secuencias", SOUND_STRM: "streams", SOUND_WAVE: "ondas"}.get(stype, "otros")
        res.counts[kind] = res.counts.get(kind, 0) + 1
        if stype != SOUND_WAVE:
            continue
        name = _cstr(d, strings[string_id]) if 0 <= string_id < len(strings) else f"sonido_{sid}"
        try:
            sub = struct.unpack_from(">i", d, _ref(d, s + 0x18, base))[0]
            where = file_pos(file_id)
            if where is None:
                raise BrsarError("archivo externo o sin grupo")
            rwsd, size, wave_addr = where
            wi = _rwsd_wave(d, rwsd, size, sub)
            key = (rwsd, wi)
            if key not in seen:
                seen[key] = read_wave(d, _rwsd_wave_info(d, rwsd, wi), wave_addr)
            res.sounds.append((name, seen[key]))
        except (BrsarError, struct.error, IndexError, ValueError) as e:
            res.counts["omitidos"] = res.counts.get("omitidos", 0) + 1
            if len(res.warnings) < 20:
                res.warnings.append(f"{name}: {e}")
    return res
