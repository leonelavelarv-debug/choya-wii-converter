"""Generadores de datos sintéticos con formato de Wii (no contienen datos de ningún juego)."""
from __future__ import annotations

import struct

import numpy as np

from choya import gxtex
from choya.u8 import build_u8


def tile(img: np.ndarray, bw: int, bh: int) -> np.ndarray:
    """Inverso de gxtex._untile: imagen -> valores en orden de bloque."""
    h, w = img.shape[:2]
    ph, pw = (-h) % bh, (-w) % bw
    img = np.pad(img, ((0, ph), (0, pw)) + ((0, 0),) * (img.ndim - 2))
    H, W = img.shape[:2]
    rest = img.shape[2:]
    v = img.reshape((H // bh, bh, W // bw, bw) + rest).transpose((0, 2, 1, 3) + tuple(range(4, 4 + len(rest))))
    return v.reshape((-1,) + rest)


def encode_gx(fmt: int, rgba: np.ndarray) -> bytes:
    bw, bh, _ = gxtex.BLOCK[fmt]
    r, g, b, a = (rgba[..., i].astype(np.uint32) for i in range(4))
    if fmt == gxtex.I4:
        v = tile((r >> 4).astype(np.uint8), bw, bh)
        return ((v[0::2] << 4) | v[1::2]).astype(np.uint8).tobytes()
    if fmt == gxtex.I8:
        return tile(r.astype(np.uint8), bw, bh).tobytes()
    if fmt == gxtex.IA4:
        return tile((((a >> 4) << 4) | (r >> 4)).astype(np.uint8), bw, bh).tobytes()
    if fmt == gxtex.IA8:
        return tile(((a << 8) | r).astype(">u2"), bw, bh).tobytes()
    if fmt == gxtex.RGB565:
        return tile((((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)).astype(">u2"), bw, bh).tobytes()
    if fmt == gxtex.RGB5A3:
        opaque = 0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)
        trans = ((a >> 5) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)
        return tile(np.where(a == 255, opaque, trans).astype(">u2"), bw, bh).tobytes()
    if fmt == gxtex.RGBA8:
        px = tile(rgba, 4, 4).reshape(-1, 16, 4)
        out = np.empty((px.shape[0], 2, 16, 2), np.uint8)
        out[:, 0, :, 0] = px[..., 3]
        out[:, 0, :, 1] = px[..., 0]
        out[:, 1, :, 0] = px[..., 1]
        out[:, 1, :, 1] = px[..., 2]
        return out.tobytes()
    raise NotImplementedError(fmt)


def cmpr_block_solid(c0: int, c1: int, idx_rows: list[int]) -> bytes:
    """Un sub-bloque DXT1 de CMPR: 2 colores RGB565 y 4 filas de índices."""
    return struct.pack(">HH", c0, c1) + bytes(idx_rows)


def gradient(w: int, h: int, alpha: str = "none") -> np.ndarray:
    y, x = np.mgrid[0:h, 0:w]
    img = np.zeros((h, w, 4), np.uint8)
    img[..., 0] = (x * 255 // max(1, w - 1))
    img[..., 1] = (y * 255 // max(1, h - 1))
    img[..., 2] = 128
    if alpha == "none":
        img[..., 3] = 255
    elif alpha == "binary":
        img[..., 3] = np.where((x // 8 + y // 8) % 2 == 0, 255, 0)
    else:
        img[..., 3] = (x * 255 // max(1, w - 1))
    return img


def yaz0_literal(data: bytes) -> bytes:
    """Yaz0 válido: literales y una referencia hacia atrás por cada grupo cuando hay repetición."""
    out = bytearray(b"Yaz0" + struct.pack(">I", len(data)) + b"\x00" * 8)
    i = 0
    while i < len(data):
        code_pos = len(out)
        out.append(0)
        code = 0
        for bit in range(7, -1, -1):
            if i >= len(data):
                break
            # referencia de 3+ bytes a distancia 1..16 si existe
            best = (0, 0)
            for dist in range(1, min(16, i) + 1):
                n = 0
                while i + n < len(data) and n < 17 and data[i + n] == data[i - dist + n]:
                    n += 1
                if n > best[0]:
                    best = (n, dist)
            if best[0] >= 3:
                n, dist = best
                out.extend(bytes([((n - 2) << 4) | ((dist - 1) >> 8), (dist - 1) & 0xFF]))
                i += n
            else:
                code |= 1 << bit
                out.append(data[i])
                i += 1
        out[code_pos] = code
    return bytes(out)


def build_brres(textures: list[tuple[str, int, int, int, bytes, int]]) -> bytes:
    """BRRES mínimo con 'Textures(NW4R)'. textures = [(nombre, fmt, w, h, datos, mips)]."""
    strings = bytearray()
    str_off = {}

    def s(name: str) -> int:
        if name not in str_off:
            strings.extend(struct.pack(">I", len(name)))
            str_off[name] = len(strings)
            strings.extend(name.encode() + b"\x00")
            strings.extend(b"\x00" * ((-len(strings)) % 4))
        return str_off[name]

    def group(entries: list[tuple[str, int]]) -> tuple[bytes, list[int]]:
        size = 8 + 16 * (len(entries) + 1)
        return size, entries

    root_grp_off = 0x18
    root_size = 8 + 16 * 2
    sub_grp_off = root_grp_off + root_size
    sub_size = 8 + 16 * (len(textures) + 1)
    tex_offs = []
    cur = (sub_grp_off + sub_size + 0x1F) & ~0x1F
    tex_blobs = []
    for name, fmt, w, h, data, mips in textures:
        tex_offs.append(cur)
        hdr = bytearray(b"TEX0" + struct.pack(">IIi", 0x40 + len(data), 1, -cur))
        hdr += struct.pack(">II", 0x40, 0)          # data offset, name offset (se fija después)
        hdr += struct.pack(">IHHII", 0, w, h, fmt, mips)
        hdr += struct.pack(">ff", 0.0, float(mips - 1))
        hdr += b"\x00" * (0x40 - len(hdr))
        tex_blobs.append(bytes(hdr) + data)
        cur = (cur + len(tex_blobs[-1]) + 0x1F) & ~0x1F
    str_base = cur
    s("Textures(NW4R)")
    for t in textures:
        s(t[0])

    out = bytearray(b"bres" + struct.pack(">HHIHH", 0xFEFF, 0, 0, 0x10, 1))
    out += b"root" + struct.pack(">I", root_size + 8)
    # grupo raíz
    out += struct.pack(">II", root_size, 1)
    out += struct.pack(">HHHHII", 0xFFFF, 0, 1, 0, 0, 0)
    out += struct.pack(">HHHHII", 0, 0, 0, 0, str_base + str_off["Textures(NW4R)"] - root_grp_off,
                       sub_grp_off - root_grp_off)
    # grupo de texturas
    out += struct.pack(">II", sub_size, len(textures))
    out += struct.pack(">HHHHII", 0xFFFF, 0, 1, 0, 0, 0)
    for (name, *_), off in zip(textures, tex_offs):
        out += struct.pack(">HHHHII", 0, 0, 0, 0, str_base + str_off[name] - sub_grp_off, off - sub_grp_off)
    for off, blob in zip(tex_offs, tex_blobs):
        out += b"\x00" * (off - len(out))
        out += blob
    out += b"\x00" * (str_base - len(out))
    out += strings
    struct.pack_into(">I", out, 8, len(out))
    for (name, *_), off in zip(textures, tex_offs):
        struct.pack_into(">I", out, off + 0x14, str_base + str_off[name] - off)
    return bytes(out)


COEFS = [0, 0, 2048, 0, 3840, -1920, 3600, -1680, 1024, 1024, 4000, -2000, 1800, 0, 3000, -1500]


def dsp_encode(pcm: np.ndarray, coefs=COEFS) -> tuple[bytes, np.ndarray]:
    """Codificador DSP-ADPCM simple (fuerza bruta por trama). Devuelve (datos, salida_simulada)."""
    pcm = pcm.astype(np.int64)
    out = bytearray()
    sim = []
    h1 = h2 = 0
    for f in range(0, len(pcm), 14):
        frame = list(pcm[f:f + 14]) + [0] * max(0, 14 - len(pcm[f:f + 14]))
        best = None
        for ci in range(8):
            c1, c2 = coefs[ci * 2], coefs[ci * 2 + 1]
            for sc in range(12):
                scale = 1 << sc
                a, b = h1, h2
                nibs, outs, err = [], [], 0
                for x in frame:
                    pred = 1024 + c1 * a + c2 * b
                    best_n, best_v, best_e = 0, 0, None
                    for n in range(-8, 8):
                        v = (((n * scale) << 11) + pred) >> 11
                        v = max(-32768, min(32767, v))
                        e = (v - x) ** 2
                        if best_e is None or e < best_e:
                            best_n, best_v, best_e = n, v, e
                    nibs.append(best_n & 15)
                    outs.append(best_v)
                    err += best_e
                    b, a = a, best_v
                if best is None or err < best[0]:
                    best = (err, (ci << 4) | sc, nibs, outs, a, b)
        _, ps, nibs, outs, h1, h2 = best
        out.append(ps)
        for i in range(0, 14, 2):
            out.append((nibs[i] << 4) | nibs[i + 1])
        sim.extend(outs)
    return bytes(out), np.array(sim[:len(pcm)], np.int16)


def build_dsp(pcm: np.ndarray, rate: int) -> tuple[bytes, np.ndarray]:
    data, sim = dsp_encode(pcm)
    hdr = struct.pack(">IIIHHIII", len(pcm), len(pcm) // 14 * 16 + 2, rate, 0, 0, 2, 0, 2)
    hdr += struct.pack(">16h", *COEFS) + struct.pack(">HHhhHhh", 0, data[0], 0, 0, 0, 0, 0)
    hdr += b"\x00" * (0x60 - len(hdr))
    return hdr + data, sim


def dsp_address(sample: int) -> int:
    """Muestra → dirección en nibbles de DSP-ADPCM (2 nibbles de cabecera por trama de 14 muestras)."""
    return (sample // 14) * 16 + sample % 14 + 2


def build_brsar(sounds: list, seq_names=()) -> tuple[bytes, list[np.ndarray]]:
    """BRSAR 1.3 con un grupo y un RWSD: sounds = [(nombre, pcm mono o estéreo (n, c), frecuencia, bucle)].
    Los sonidos de onda llevan una nota cada uno; seq_names agrega sonidos de secuencia (solo se cuentan).
    Devuelve el archivo y lo que debe salir al decodificar (la simulación del codificador)."""
    ref = lambda off: struct.pack(">BBHI", 1, 0, 0, off)
    # --- RWSD: DATA (sonido → nota → onda) y WAVE (información de cada onda); las muestras van aparte ---
    wave_data = bytearray()
    infos, expected = [], []
    for name, pcm, rate, loop in sounds:
        pcm = pcm if pcm.ndim == 2 else pcm[:, None]
        chans, sims = [], []
        for c in range(pcm.shape[1]):
            data, sim = dsp_encode(pcm[:, c])
            wave_data.extend(b"\x00" * ((-len(wave_data)) % 32))
            chans.append((len(wave_data), data))
            wave_data.extend(data)
            sims.append(sim)
        expected.append(np.stack(sims, 1))
        n = len(pcm)
        info = bytearray(struct.pack(">BBBBHHII", 2, 1 if loop else 0, pcm.shape[1], 0, rate, 0,
                                     dsp_address(0), dsp_address(n - 1)))
        info += struct.pack(">III", 0x1C, 0, 0)               # tabla de canales en +0x1C, datos desde 0
        table = 0x1C
        ch_info = table + 4 * len(chans)
        adpcm = ch_info + 0x1C * len(chans)
        body = bytearray()
        for k, (off, data) in enumerate(chans):
            body += struct.pack(">I", ch_info + 0x1C * k)
        for k, (off, data) in enumerate(chans):
            body += struct.pack(">II5I", off, adpcm + 0x30 * k, 0, 0, 0, 0, 0)
        for k, (off, data) in enumerate(chans):
            body += struct.pack(">16h", *COEFS) + struct.pack(">HHhhHhhH", 0, data[0], 0, 0, data[0], 0, 0, 0)
        infos.append(bytes(info) + bytes(body))
    count = len(sounds)
    # DATA: wsdCount, refWsd[]; cada Wsd {info, pistas, notas}; notas: tabla con una NoteInfo (onda i).
    data_body = bytearray(struct.pack(">I", count) + b"\x00" * (8 * count))
    for i in range(count):
        wsd = len(data_body)
        data_body += b"\x00" * 0x18
        notes = len(data_body)
        data_body += struct.pack(">I", 1) + b"\x00" * 8
        note = len(data_body)
        data_body += struct.pack(">i", i) + b"\x00" * 0x2C
        data_body[4 + 8 * i:12 + 8 * i] = ref(wsd)
        data_body[wsd + 0x10:wsd + 0x18] = ref(notes)
        data_body[notes + 4:notes + 12] = ref(note)
    data_blk = b"DATA" + struct.pack(">I", 8 + len(data_body)) + bytes(data_body)
    data_blk += b"\x00" * ((-len(data_blk)) % 32)
    wave_body = bytearray(struct.pack(">I", count) + b"\x00" * (4 * count))
    for i, info in enumerate(infos):
        wave_body.extend(b"\x00" * ((-(len(wave_body) + 8)) % 4))
        struct.pack_into(">I", wave_body, 4 + 4 * i, 8 + len(wave_body))
        wave_body += info
    wave_blk = b"WAVE" + struct.pack(">I", 8 + len(wave_body)) + bytes(wave_body)
    rwsd = bytearray(b"RWSD" + struct.pack(">HHIHH", 0xFEFF, 0x0102, 0, 0x20, 2))
    rwsd += struct.pack(">IIII", 0x20, len(data_blk), 0x20 + len(data_blk), len(wave_blk))
    rwsd += bytes(data_blk) + wave_blk
    rwsd += b"\x00" * ((-len(rwsd)) % 32)
    struct.pack_into(">I", rwsd, 8, len(rwsd))
    # --- RSAR ---
    names = [n for n, *_ in sounds] + list(seq_names)
    symb_body = bytearray(struct.pack(">5I", 0x14, 0, 0, 0, 0))
    table = len(symb_body)
    symb_body += struct.pack(">I", len(names)) + b"\x00" * (4 * len(names))
    for i, n in enumerate(names):
        struct.pack_into(">I", symb_body, table + 4 + 4 * i, len(symb_body))
        symb_body += n.encode() + b"\x00"
    symb = b"SYMB" + struct.pack(">I", 8 + len(symb_body)) + bytes(symb_body)
    symb += b"\x00" * ((-len(symb)) % 32)
    info = bytearray(b"\x00" * 0x30)
    def put(blob):
        info.extend(b"\x00" * ((-len(info)) % 4))
        off = len(info)
        info.extend(blob)
        return off
    sound_offs = []
    for i, n in enumerate(names):
        wave = i < count
        sub = put(struct.pack(">iIBB2x", i, 1, 64, 0))
        common = bytearray(struct.pack(">III", i, 0, 0) + b"\x00" * 8 + struct.pack(">BBBB", 127, 64,
                                                                                    3 if wave else 1, 0))
        common += ref(sub) + b"\x00" * 10
        sound_offs.append(put(bytes(common)))
    sound_tbl = put(struct.pack(">I", len(names)) + b"".join(ref(o) for o in sound_offs))
    pos = put(struct.pack(">II", 0, 0))
    pos_tbl = put(struct.pack(">I", 1) + ref(pos))
    finfo = put(struct.pack(">III", len(rwsd), len(wave_data), 1) + b"\x00" * 8 + ref(pos_tbl))
    file_tbl = put(struct.pack(">I", 1) + ref(finfo))
    header_size = 0x40
    file_blk_off = header_size + len(symb) + ((8 + 0x200 + 31) // 32) * 32  # se corrige abajo
    item = put(struct.pack(">IIIII", 0, 0, len(rwsd), 0, len(wave_data)))
    item_tbl = put(struct.pack(">I", 1) + ref(item))
    group = put(struct.pack(">Ii", 0, 1) + b"\x00" * 8 + struct.pack(">IIII", 0, 0, 0, 0) + ref(item_tbl))
    group_tbl = put(struct.pack(">I", 1) + ref(group))
    info[0:8] = ref(sound_tbl)
    info[0x18:0x20] = ref(file_tbl)
    info[0x20:0x28] = ref(group_tbl)
    info_blk = bytearray(b"INFO" + struct.pack(">I", 8 + len(info)) + bytes(info))
    info_blk += b"\x00" * ((-len(info_blk)) % 32)
    file_blk_off = header_size + len(symb) + len(info_blk)
    rwsd_off = file_blk_off + 0x20
    wave_off = rwsd_off + len(rwsd)
    # posiciones del grupo (absolutas en el archivo)
    struct.pack_into(">IIII", info_blk, 8 + group + 0x10, rwsd_off, len(rwsd), wave_off, len(wave_data))
    file_blk = b"FILE" + struct.pack(">I", 0x20 + len(rwsd) + len(wave_data)) + b"\x00" * 0x18 + bytes(rwsd) + \
        bytes(wave_data)
    out = bytearray(b"RSAR" + struct.pack(">HHIHH", 0xFEFF, 0x0103, 0, header_size, 3))
    out += struct.pack(">IIIIII", header_size, len(symb), header_size + len(symb), len(info_blk), file_blk_off,
                       len(file_blk))
    out += b"\x00" * (header_size - len(out))
    out += symb + bytes(info_blk) + file_blk
    struct.pack_into(">I", out, 8, len(out))
    return bytes(out), expected


def build_brstm(channels: list[np.ndarray], rate: int, block_size: int = 0x2000, loop: bool = False) -> tuple[bytes, list[np.ndarray]]:
    """BRSTM DSP-ADPCM con varios canales y bloques entrelazados."""
    enc = [dsp_encode(c) for c in channels]
    total = len(channels[0])
    nch = len(channels)
    streams = [e[0] for e in enc]
    ln = len(streams[0])
    blocks = (ln + block_size - 1) // block_size
    last = ln - (blocks - 1) * block_size
    last_pad = (last + 0x1F) & ~0x1F
    head = bytearray(b"HEAD" + b"\x00" * 4)
    base = 8
    # 3 referencias
    refs_pos = len(head)
    head += b"\x00" * 24
    si = len(head) - base
    head += struct.pack(">BBBBHHIIIIIIIIIII", 2, int(loop), nch, 0, rate, 0, 0, total, 0, blocks, block_size,
                        block_size // 8 * 14, last, last // 8 * 14, last_pad, 0, 0)
    ti = len(head) - base
    head += struct.pack(">BBH", 1, 0, 0) + b"\x00" * 8
    ci = len(head) - base
    head += struct.pack(">B3x", nch)
    entry_refs = len(head)
    head += b"\x00" * (8 * nch)
    for c in range(nch):
        entry = len(head) - base
        struct.pack_into(">II", head, entry_refs + c * 8, 0x01000000, entry)
        info = entry + 8
        head += struct.pack(">II", 0x01000000, info)
        head += struct.pack(">16h", *COEFS) + struct.pack(">HHhhHhh", 0, streams[c][0], 0, 0, 0, 0, 0)
        head += b"\x00" * 2
    for i, off in enumerate((si, ti, ci)):
        struct.pack_into(">II", head, refs_pos + i * 8, 0x01000000, off)
    head += b"\x00" * ((-len(head)) % 0x20)
    struct.pack_into(">I", head, 4, len(head))
    head_off = 0x40
    data_chunk_off = head_off + len(head)
    data_start = data_chunk_off + 0x20
    struct.pack_into(">I", head, base + si + 0x10, data_start)
    body = bytearray()
    for b in range(blocks):
        size = block_size if b < blocks - 1 else last
        stride = block_size if b < blocks - 1 else last_pad
        for c in range(nch):
            chunk = streams[c][b * block_size: b * block_size + size]
            body += chunk + b"\x00" * (stride - len(chunk))
    data_chunk = b"DATA" + struct.pack(">II", 0x20 + len(body), 0x18) + b"\x00" * 0x14 + bytes(body)
    hdr = bytearray(b"RSTM" + struct.pack(">HHIHH", 0xFEFF, 0x0100, 0, 0x40, 2))
    hdr += struct.pack(">IIIIII", head_off, len(head), 0, 0, data_chunk_off, len(data_chunk))
    hdr += b"\x00" * (0x40 - len(hdr))
    out = bytes(hdr) + bytes(head) + data_chunk
    out = out[:8] + struct.pack(">I", len(out)) + out[12:]
    return out, [e[1] for e in enc]


__all__ = ["encode_gx", "tile", "gradient", "yaz0_literal", "build_brres", "build_u8", "build_dsp",
           "build_brstm", "dsp_encode", "cmpr_block_solid"]


# ---------------------------------------------------------------------------- modelos MDL0 sintéticos

def build_brres_folders(folders: dict) -> bytes:
    """BRRES con varias carpetas. folders = {"3DModels(NW4R)": [(nombre, blob)], ...}."""
    names = list(folders)
    strings = bytearray()
    str_off = {}

    def s(name):
        if name not in str_off:
            strings.extend(struct.pack(">I", len(name)))
            str_off[name] = len(strings)
            strings.extend(name.encode() + b"\x00")
            strings.extend(b"\x00" * ((-len(strings)) % 4))
        return str_off[name]

    root_grp = 0x18
    root_size = 8 + 16 * (len(names) + 1)
    cur = root_grp + root_size
    sub_offs = {}
    for f in names:
        sub_offs[f] = cur
        cur += 8 + 16 * (len(folders[f]) + 1)
    cur = (cur + 0x1F) & ~0x1F
    blob_offs = {}
    for f in names:
        for n, b in folders[f]:
            blob_offs[(f, n)] = cur
            cur = (cur + len(b) + 0x1F) & ~0x1F
    str_base = cur
    for f in names:
        s(f)
        for n, _ in folders[f]:
            s(n)
    out = bytearray(b"bres" + struct.pack(">HHIHH", 0xFEFF, 0, 0, 0x10, 1))
    out += b"root" + struct.pack(">I", root_size + 8)
    out += struct.pack(">II", root_size, len(names)) + struct.pack(">HHHHII", 0xFFFF, 0, 1, 0, 0, 0)
    for f in names:
        out += struct.pack(">HHHHii", 0, 0, 0, 0, str_base + str_off[f] - root_grp, sub_offs[f] - root_grp)
    for f in names:
        g = sub_offs[f]
        out += struct.pack(">II", 8 + 16 * (len(folders[f]) + 1), len(folders[f]))
        out += struct.pack(">HHHHII", 0xFFFF, 0, 1, 0, 0, 0)
        for n, _ in folders[f]:
            out += struct.pack(">HHHHii", 0, 0, 0, 0, str_base + str_off[n] - g, blob_offs[(f, n)] - g)
    for f in names:
        for n, b in folders[f]:
            out += b"\x00" * (blob_offs[(f, n)] - len(out))
            out += b
    out += b"\x00" * (str_base - len(out))
    out += strings
    struct.pack_into(">I", out, 8, len(out))
    return bytes(out)


def tex0_blob(fmt: int, w: int, h: int, data: bytes, mips: int = 1) -> bytes:
    hdr = bytearray(b"TEX0" + struct.pack(">IIi", 0x40 + len(data), 1, 0))
    hdr += struct.pack(">II", 0x40, 0) + struct.pack(">IHHII", 0, w, h, fmt, mips)
    hdr += struct.pack(">ff", 0.0, float(mips - 1))
    hdr += b"\x00" * (0x40 - len(hdr))
    return bytes(hdr) + data


class _Blob:
    def __init__(self):
        self.b = bytearray()

    def alloc(self, size: int, align: int = 4) -> int:
        self.b.extend(b"\x00" * ((-len(self.b)) % align))
        off = len(self.b)
        self.b.extend(b"\x00" * size)
        return off

    def put(self, fmt: str, off: int, *vals):
        struct.pack_into(fmt, self.b, off, *vals)


# ---------------------------------------------------------------------------- comandos TEV sintéticos

from choya.tev import ALPHA_IN, COLOR_IN  # noqa: E402


def bp(reg: int, val: int) -> bytes:
    return bytes([0x61, reg, (val >> 16) & 255, (val >> 8) & 255, val & 255])


def color_env(a, b, c, d, dest=0, bias=0, sub=0, clamp=1, scale=0) -> int:
    ix = COLOR_IN.index
    return ix(d) | (ix(c) << 4) | (ix(b) << 8) | (ix(a) << 12) | (bias << 16) | (sub << 18) | (clamp << 19) | (
        scale << 20) | (dest << 22)


def alpha_env(a, b, c, d, dest=0, bias=0, sub=0, clamp=1, scale=0, rswap=0, tswap=0) -> int:
    ix = ALPHA_IN.index
    return rswap | (tswap << 2) | (ix(d) << 4) | (ix(c) << 7) | (ix(b) << 10) | (ix(a) << 13) | (bias << 16) | (
        sub << 18) | (clamp << 19) | (scale << 20) | (dest << 22)


def tref(s0, s1=(0, 0, 0, 7)) -> int:
    f = lambda m, c, e, r: m | (c << 3) | (e << 6) | (r << 7)
    return f(*s0) | (f(*s1) << 12)


def ksel(kc0=0x0C, ka0=0x1C, kc1=0x0C, ka1=0x1C, swap=0) -> int:
    return swap | (kc0 << 4) | (ka0 << 9) | (kc1 << 14) | (ka1 << 19)


def tevreg(i: int, rgba, konst=False) -> bytes:
    r, g, b, a = rgba
    t = (1 << 23) if konst else 0
    return bp(0xE0 + 2 * i, (r & 0x7FF) | ((a & 0x7FF) << 12) | t) + bp(0xE1 + 2 * i, (b & 0x7FF) | ((g & 0x7FF) << 12) | t)


def xf(addr: int, vals) -> bytes:
    return bytes([0x10]) + struct.pack(">HH", len(vals) - 1, addr) + b"".join(struct.pack(">I", v) for v in vals)


def tev_config(stages, konst=None, cregs=None, alpha_compare=None, blend=0x18 | (4 << 8) | (5 << 5), zmode=0x17,
               texgens=(0x5280,)):
    """stages = [(tref(map,coord,en,ras), color_env, alpha_env, kcsel, kasel)] -> (dl_shader, dl_material, n, ntg)."""
    sh = bytearray()
    swap = (0x4, 0xE, 0x0, 0xC, 0x5, 0xD, 0xA, 0xE)      # tablas por defecto de GX
    for i in range(8):                                    # como NW4R: primero las tablas, enmascaradas
        sh += bp(0xFE, 0x00000F) + bp(0xF6 + i, swap[i] | 0xFFFFF0)
    for i in range(0, len(stages), 2):
        pair = stages[i:i + 2]
        t1 = pair[1][0] if len(pair) > 1 else (0, 0, 0, 7)
        sh += bp(0x28 + i // 2, tref(pair[0][0], t1))
        k1 = pair[1][3:5] if len(pair) > 1 else (0x0C, 0x1C)
        sh += bp(0xFE, 0xFFFFF0) + bp(0xF6 + i // 2, ksel(pair[0][3], pair[0][4], k1[0], k1[1], 0))
    for i, st in enumerate(stages):
        sh += bp(0xC0 + 2 * i, st[1]) + bp(0xC1 + 2 * i, st[2])
    mt = bytearray()
    mt += bp(0xF3, alpha_compare if alpha_compare is not None else (7 << 16) | (7 << 19))
    mt += bp(0x40, zmode) + bp(0x41, blend)
    for i, rgba in enumerate(cregs or []):
        mt += tevreg(i, rgba)
    for i, rgba in enumerate(konst or []):
        mt += tevreg(i, rgba, konst=True)
    for i, v in enumerate(texgens):
        mt += xf(0x1040 + i, [v])
    return bytes(sh), bytes(mt), len(stages), len(texgens)


def sphere(rings=12, segs=16, radius=1.0):
    """Esfera UV: (posiciones, normales, uv, triángulos) para modelos de prueba."""
    import math
    pos, nrm, uv = [], [], []
    for r in range(rings + 1):
        th = math.pi * r / rings
        for sgi in range(segs + 1):
            ph = 2 * math.pi * sgi / segs
            n = (math.sin(th) * math.cos(ph), math.cos(th), math.sin(th) * math.sin(ph))
            nrm.append(n)
            pos.append(tuple(radius * c for c in n))
            uv.append((sgi / segs, r / rings))
    tris = []
    for r in range(rings):
        for sgi in range(segs):
            a = r * (segs + 1) + sgi
            b = a + segs + 1
            tris += [(a, a + 1, b), (a + 1, b + 1, b)]   # sentido horario visto desde afuera (GX)
    return pos, nrm, uv, tris


def build_mdl0(name: str = "box", texture: str = "tex_box", translate=(0.0, 1.0, 0.0), tev=None,
               extra_textures=(), mesh=None, tex_srt=None, srt_mode=0, chans=None, num_chans=1,
               skeleton=None, obj_node=0, nodemix=None) -> bytes:
    """MDL0 v9 con un hueso, 4 vértices (tira de 2 triángulos), UV y un material con textura.
    tex_srt = {coordenada: (su, sv, giro, tu, tv)} (ResTexSrtData, modo srt_mode); chans = [(banderas, mat, amb,
    control del color, control del alfa)] (ResChanData); skeleton = [(nombre, padre, traslación)] reemplaza al hueso
    único (id de matriz = índice); obj_node = matriz del objeto; nodemix = {id: [(id de hueso, peso)]}."""
    m = _Blob()
    m.alloc(0x40)
    m.b[0:4] = b"MDL0"
    m.put(">I", 8, 9)
    strings = {}

    def string(txt):
        if txt not in strings:
            off = m.alloc(4 + len(txt) + 1)
            m.put(">I", off, len(txt))
            m.b[off + 4:off + 4 + len(txt)] = txt.encode()
            strings[txt] = off + 4
        return strings[txt]

    def group(entries):            # entries = [(nombre, offset_absoluto)]
        g = m.alloc(8 + 16 * (len(entries) + 1))
        m.put(">II", g, 8 + 16 * (len(entries) + 1), len(entries))
        m.put(">HHHHii", g + 8, 0xFFFF, 0, 1, 0, 0, 0)
        for i, (n, off) in enumerate(entries, 1):
            m.put(">HHHHii", g + 8 + i * 16, 0, 0, 0, 0, string(n) - g, off - g)
        return g

    # huesos (ResNodeData: id, id de matriz, SRT, padre relativo y matriz del modelo)
    skeleton = skeleton or [("root", -1, translate)]
    bone_offs, worlds = [], []
    for bi, (bname, parent, tr) in enumerate(skeleton):
        bone = m.alloc(0xD0)
        bone_offs.append(bone)
        w = list(tr) if parent < 0 else [a + b for a, b in zip(worlds[parent], tr)]
        worlds.append(w)
        m.put(">i", bone + 4, -bone)
        m.put(">ii", bone + 0x0C, bi, bi)
        m.put(">3f", bone + 0x20, 1, 1, 1)
        m.put(">3f", bone + 0x38, *tr)
        if parent >= 0:
            m.put(">i", bone + 0x5C, bone_offs[parent] - bone)
        m.put(">12f", bone + 0x70, 1, 0, 0, w[0], 0, 1, 0, w[1], 0, 0, 1, w[2])
        m.put(">12f", bone + 0xA0, 1, 0, 0, -w[0], 0, 1, 0, -w[1], 0, 0, 1, -w[2])
    bone = bone_offs[0]
    # posiciones (s16, 8 bits de fracción), normales (float) y UV (float)
    if mesh:
        pos, nrms, uvs, tris = mesh
    else:
        pos = [(-1, -1, 0), (1, -1, 0), (-1, 1, 0), (1, 1, 0)]
        uvs = [(0, 1), (1, 1), (0, 0), (1, 0)]
        nrms, tris = None, None
    vb = m.alloc(0x40 + 6 * len(pos))
    m.put(">iiii", vb, 0x40 + 6 * len(pos), -vb, 0x40, 0)
    m.put(">IIBBH", vb + 0x14, 1, 3, 8, 6, len(pos))
    for i, p in enumerate(pos):
        m.put(">3h", vb + 0x40 + i * 6, *(int(round(c * 256)) for c in p))
    ub = m.alloc(0x40 + 8 * len(uvs))
    m.put(">iiii", ub, 0x40 + 8 * len(uvs), -ub, 0x40, 0)
    m.put(">IIBBH", ub + 0x14, 1, 4, 0, 8, len(uvs))
    for i, uv in enumerate(uvs):
        m.put(">2f", ub + 0x40 + i * 8, *uv)
    nb = None
    if nrms:
        nb = m.alloc(0x40 + 12 * len(nrms))
        m.put(">iiii", nb, 0x40 + 12 * len(nrms), -nb, 0x40, 0)
        m.put(">IIBBH", nb + 0x14, 0, 4, 0, 12, len(nrms))
        for i, n in enumerate(nrms):
            m.put(">3f", nb + 0x40 + i * 12, *n)
    # material con sus capas de textura y su lista de comandos; shader con las etapas TEV
    if tev is None:
        tev = tev_config([((0, 0, 1, 0), color_env("ZERO", "TEXC", "RASC", "ZERO"),
                           alpha_env("ZERO", "TEXA", "RASA", "ZERO"), 0x0C, 0x1C)])
    sh_dl, mt_dl, nst, ntg = tev
    textures = [texture] + list(extra_textures)
    mat_head = 0x418                                     # ResMatData completo (ogws g3d_resmat.h)
    mat_size = (mat_head + 0x34 * len(textures) + 0x1F) & ~0x1F
    dl_rel = mat_size
    mat_size += (len(mt_dl) + 0x1F) & ~0x1F
    mat = m.alloc(mat_size, 32)
    shader = m.alloc(0x200, 32)
    m.put(">I", mat, mat_size)
    m.put(">i", mat + 4, -mat)
    m.put(">ii", mat + 0x0C, 0, 0)
    m.b[mat + 0x14:mat + 0x18] = bytes([ntg, num_chans, nst, 0])
    if tex_srt:
        flag = 0
        for i, (su, sv, r, tu, tv) in tex_srt.items():
            flag |= 1 << (4 * i)
            m.put(">5f", mat + 0x1A4 + 8 + 0x14 * i, su, sv, r, tu, tv)
            eff = mat + 0x1A4 + 0xA8 + 0x34 * i
            m.b[eff:eff + 4] = bytes([0xFF, 0xFF, 0, 1])           # sin cámara ni luz, mapeo normal, efecto identidad
            m.put(">12f", eff + 4, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0)
        m.put(">II", mat + 0x1A4, flag, srt_mode)
    for c, (flags, matc, ambc, ctrl_c, ctrl_a) in enumerate(chans or []):
        o = mat + 0x3EC + 0x14 * c
        m.put(">I", o, flags)
        m.b[o + 4:o + 8] = bytes(matc)
        m.b[o + 8:o + 12] = bytes(ambc)
        m.put(">II", o + 0xC, ctrl_c, ctrl_a)
    m.put(">I", mat + 0x18, 2)
    m.put(">i", mat + 0x28, shader - mat)
    m.put(">i", mat + 0x38, dl_rel)
    m.b[mat + dl_rel:mat + dl_rel + len(mt_dl)] = mt_dl
    m.put(">I", shader, 0x200)
    m.put(">i", shader + 4, -shader)
    m.b[shader + 0x0C] = nst
    m.b[shader + 0x20:shader + 0x20 + len(sh_dl)] = sh_dl
    layers = []
    for i, tname in enumerate(textures):
        layer = mat + mat_head + 0x34 * i
        m.put(">i", layer, string(tname) - layer)
        m.put(">I", layer + 0x10, i)
        m.put(">II", layer + 0x18, 1, 1)
        layers.append(layer)
    layer = layers[0]
    # objeto: POS índice8 + TEX0 índice8, una tira de triángulos (o la malla dada, con índices de 16 bits)
    if mesh:
        body = bytearray([0x90]) + struct.pack(">H", 3 * len(tris))
        for t in tris:
            for k in t:
                body += struct.pack(">HHH", k, k, k)       # pos, nrm, tex0
        dl = bytes(body) + b"\x00" * ((-len(body)) % 32)
        vcd_lo, vcd_hi = (3 << 9) | (3 << 11), 3
    else:
        dl = bytes([0x20, 0x00, 0x00, 0xB0, 0x00]) + bytes([0x98, 0x00, 0x04]) + bytes(
            [0, 0, 1, 1, 2, 2, 3, 3]) + b"\x00" * 3
        vcd_lo, vcd_hi = 2 << 9, 2
    obj = m.alloc(0x80)
    prim = m.alloc(len(dl), 32)
    m.b[prim:prim + len(dl)] = dl
    m.put(">i", obj + 4, -obj)
    m.put(">i", obj + 0x08, obj_node)
    m.put(">II", obj + 0x0C, vcd_lo, vcd_hi)
    m.put(">IIi", obj + 0x24, len(dl), len(dl), prim - (obj + 0x24))
    m.put(">i", obj + 0x3C, 0)
    m.put(">hhhh8h", obj + 0x48, 0, 0 if nrms else -1, -1, -1, 0, -1, -1, -1, -1, -1, -1, -1)
    # enlaces de texturas (uno por textura)
    tex_links = []
    for tname, lay in zip(textures, layers):
        tl = m.alloc(4 + 8)
        m.put(">Iii", tl, 1, mat - tl, lay - tl)
        tex_links.append((tname, tl))
    # definiciones
    draw = m.alloc(16)
    m.b[draw:draw + 9] = bytes([0x04, 0, 0, 0, 0, 0, 0, 0, 0x01])
    defs = [("DrawOpa", draw)]
    if nodemix:
        body = bytearray()
        for target, lst in nodemix.items():
            body += bytes([0x03]) + struct.pack(">HB", target, len(lst))
            for mid, w in lst:
                body += struct.pack(">Hf", mid, w)
        body += b"\x01"
        nm = m.alloc(len(body))
        m.b[nm:nm + len(body)] = body
        defs.insert(0, ("NodeMix", nm))
    groups = {
        "defs": group(defs), "bones": group([(sk[0], bo) for sk, bo in zip(skeleton, bone_offs)]),
        "verts": group([("pos", vb)]), "uvs": group([("uv", ub)]),
        **({"norms": group([("nrm", nb)])} if nb else {}), "mats": group([("mat", mat)]),
        "shaders": group([("mat", shader)]),
        "objs": group([("obj", obj)]), "texs": group(tex_links),
    }
    order = ["defs", "bones", "verts", "norms", "colors", "uvs", "mats", "shaders", "objs", "texs", "pals"]
    for i, sname in enumerate(order):
        m.put(">i", 0x10 + 4 * i, groups.get(sname, 0))
    m.put(">i", 0x10 + 44, string(name))
    m.b.extend(b"\x00" * ((-len(m.b)) % 32))
    m.put(">I", 4, len(m.b))
    return bytes(m.b)


def build_chr0(nodes: dict, frames: int, loop: bool = True) -> bytes:
    """CHR0 versión 4 (Wii Sports). nodes = {hueso: {"r": [pista x, y, z], "t": [...], "s": [...]}} donde cada pista
    es un número (constante) o [(cuadro, valor, pendiente)] (FVS96)."""
    m = _Blob()
    m.alloc(0x2C)
    m.b[0:4] = b"CHR0"
    m.put(">I", 8, 4)
    m.put(">HHII", 0x1C, frames, len(nodes), 1 if loop else 0, 0)
    names = list(nodes)
    str_offs = {}
    for n in names:
        off = m.alloc(4 + len(n) + 1)
        m.put(">I", off, len(n))
        m.b[off + 4:off + 4 + len(n)] = n.encode()
        str_offs[n] = off + 4
    dic = m.alloc(8 + 16 * (len(names) + 1))
    m.put(">II", dic, 8 + 16 * (len(names) + 1), len(names))
    m.put(">HHHHii", dic + 8, 0xFFFF, 0, 1, 0, 0, 0)
    m.put(">i", 0x10, dic)
    for i, n in enumerate(names, 1):
        spec = nodes[n]
        flags = 1          # ANM_EXISTS
        comps = []         # (valor constante | lista de claves) en orden S, R, T
        fmt_bits = {"s": 25, "r": 27, "t": 30}
        has = {"s": 1 << 22, "r": 1 << 23, "t": 1 << 24}
        const_bits = {"s": (13, 14, 15), "r": (16, 17, 18), "t": (19, 20, 21)}
        for kind in ("s", "r", "t"):
            if kind not in spec:
                continue
            flags |= has[kind]
            tracks = spec[kind]
            if any(isinstance(t, list) for t in tracks):
                flags |= 3 << fmt_bits[kind]                         # FVS96
            for axis, t in enumerate(tracks):
                if not isinstance(t, list):
                    if any(isinstance(x, list) for x in tracks):
                        flags |= 1 << const_bits[kind][axis]
                comps.append(t)
        nd = m.alloc(8 + 4 * len(comps))
        m.put(">iI", nd, str_offs[n] - nd, flags)
        for k, t in enumerate(comps):
            if isinstance(t, list):
                fv = m.alloc(8 + 12 * len(t))
                m.put(">HHf", fv, len(t), 0, 1.0 / max(1.0, t[-1][0] - t[0][0]))
                for j, (fr, val, sl) in enumerate(t):
                    m.put(">fff", fv + 8 + 12 * j, fr, val, sl)
                m.put(">i", nd + 8 + 4 * k, fv - nd)
            else:
                m.put(">f", nd + 8 + 4 * k, t)
        m.put(">HHHHii", dic + 8 + 16 * i, 0, 0, 0, 0, str_offs[n] - dic, nd - dic)
    m.b.extend(b"\x00" * ((-len(m.b)) % 32))
    m.put(">I", 4, len(m.b))
    return bytes(m.b)


# --- main.dol sintético ------------------------------------------------------------

def ppc_lis(rd, v):
    return (15 << 26) | (rd << 21) | (v & 0xFFFF)


def ppc_ori(ra, rs, v):
    return (24 << 26) | (rs << 21) | (ra << 16) | (v & 0xFFFF)


def ppc_addi(rd, ra, v):
    return (14 << 26) | (rd << 21) | (ra << 16) | (v & 0xFFFF)


def ppc_bl(pc, target):
    return (18 << 26) | ((target - pc) & 0x03FFFFFC) | 1


def build_dol(code: dict, text_base=0x80004000, text_size=0x3000, entry=0x80006124,
              data_base=0x80300000, data=b"\0" * 0x100):
    """code = {dirección: [instrucciones]} dentro de la única sección de código."""
    import struct as st
    text = bytearray(text_size)
    for addr, words in code.items():
        for i, w in enumerate(words):
            st.pack_into(">I", text, addr - text_base + 4 * i, w)
    hdr = bytearray(0x100)
    offs, addrs, sizes = [0] * 18, [0] * 18, [0] * 18
    offs[0], addrs[0], sizes[0] = 0x100, text_base, text_size
    offs[7], addrs[7], sizes[7] = 0x100 + text_size, data_base, len(data)
    st.pack_into(">18I", hdr, 0x00, *offs)
    st.pack_into(">18I", hdr, 0x48, *addrs)
    st.pack_into(">18I", hdr, 0x90, *sizes)
    st.pack_into(">3I", hdr, 0xD8, 0x80400000, 0x1000, entry)
    return bytes(hdr) + bytes(text) + data
