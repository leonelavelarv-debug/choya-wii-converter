"""Recorre el disco extraído y encuentra recursos, entrando en archivos comprimidos y U8."""
from __future__ import annotations

import os
import struct
from dataclasses import dataclass, field

import numpy as np

from . import anim, anim_mat, audio, brsar, compress, containers, mdl0, rseq, scn0, u8
from .gxtex import GXTexture


@dataclass
class AudioItem:
    path: str          # ruta dentro del disco (clave para el runtime); en un BRSAR, "archivo.brsar/NOMBRE"
    kind: str          # "brstm" | "dsp" | "brsar" (ya decodificado)
    data: bytes
    decoded: object = None   # audio.DecodedAudio de un sonido de onda de BRSAR


@dataclass
class ModelItem:
    path: str          # ruta del BRRES dentro del disco
    name: str          # nombre del modelo dentro del BRRES
    offset: int        # posición del MDL0 dentro del BRRES
    brres: bytes


@dataclass
class AnimItem:
    path: str          # ruta del BRRES dentro del disco
    name: str          # nombre de la animación CHR0
    offset: int
    brres: bytes


@dataclass
class MatAnimItem:
    path: str          # ruta del BRRES dentro del disco
    kind: int          # anim_mat.TYPE_SRT / CLR / PAT / VIS
    name: str
    offset: int
    brres: bytes


@dataclass
class SceneItem:
    path: str
    name: str
    offset: int
    brres: bytes


@dataclass
class SeqReport:
    path: str                  # "archivo.brsar/NOMBRE" (la misma clave que los efectos de onda)
    seconds: float
    loop: bool
    ended: bool
    interactive: bool
    ops: dict
    ignored: dict
    stats: dict
    error: str | None = None


@dataclass
class ScanResult:
    root: str
    textures: list[GXTexture] = field(default_factory=list)
    audio: list[AudioItem] = field(default_factory=list)
    models: list[ModelItem] = field(default_factory=list)
    anims: list[AnimItem] = field(default_factory=list)
    mat_anims: list[MatAnimItem] = field(default_factory=list)
    scenes: list[SceneItem] = field(default_factory=list)
    sequences: list[SeqReport] = field(default_factory=list)
    counts: dict[str, int] = field(default_factory=dict)
    pending: list[str] = field(default_factory=list)     # tipos conocidos aún no convertidos
    errors: list[str] = field(default_factory=list)
    files: int = 0
    disc_bytes: int = 0
    render_sequences: bool = True

    def bump(self, key: str, n: int = 1):
        self.counts[key] = self.counts.get(key, 0) + n


def find_files_root(path: str) -> str:
    """Acepta la carpeta de 'Extraer disco completo' de Dolphin o directamente la carpeta files/."""
    for cand in (os.path.join(path, "files"), os.path.join(path, "DATA", "files"),
                 os.path.join(path, "P-DATA", "files")):
        if os.path.isdir(cand):
            return cand
    return path


OGWS_SHA1 = "8bb422971b88b5551a37de98db69557df7b46637"   # Wii Sports US Rev 1 (doldecomp/ogws)


def _sys_dir(path: str) -> str | None:
    for cand in (os.path.join(path, "sys"), os.path.join(path, "DATA", "sys"), os.path.join(path, "..", "sys")):
        if os.path.isdir(cand):
            return cand
    return None


def disc_info(path: str) -> dict:
    """ID, revisión y SHA-1 de main.dol; dice si coincide con la versión que soporta ogws."""
    import hashlib
    info = {"game_id": None, "revision": None, "dol_sha1": None, "ogws_match": False}
    sysd = _sys_dir(path)
    if not sysd:
        return info
    boot = os.path.join(sysd, "boot.bin")
    if os.path.isfile(boot):
        with open(boot, "rb") as f:
            b = f.read(8)
        info["game_id"] = b[:6].decode("ascii", errors="replace")
        info["revision"] = b[7] if len(b) > 7 else None
    dol = os.path.join(sysd, "main.dol")
    if os.path.isfile(dol):
        with open(dol, "rb") as f:
            info["dol_sha1"] = hashlib.sha1(f.read()).hexdigest()
        info["ogws_match"] = info["dol_sha1"] == OGWS_SHA1
    return info


def game_id(path: str) -> str | None:
    """Lee el ID del juego (p. ej. RSPE01) desde sys/boot.bin si existe."""
    for cand in (os.path.join(path, "sys", "boot.bin"), os.path.join(path, "DATA", "sys", "boot.bin")):
        if os.path.isfile(cand):
            with open(cand, "rb") as f:
                return f.read(6).decode("ascii", errors="replace")
    return None


MAX_DEPTH = 6


SEQ_RATE = 32000          # salida del DSP del Wii


def _render_sequences(res: ScanResult, path: str, raw: bytes, seqs: list):
    """Ruta A: cada secuencia del BRSAR se prerenderiza con su banco (rseq.py) y entra al paquete como audio PCM con
    su bucle, con la misma clave "archivo.brsar/NOMBRE". Los bancos y sus ondas se leen una vez por BRSAR."""
    banks: dict = {}
    failed = 0
    for sq in seqs:
        full = f"{path}/{sq.name}"
        try:
            if sq.seq is None:
                raise rseq.SeqError("secuencia en un archivo externo")
            bank = None
            if sq.bank is not None:
                if sq.bank not in banks:
                    banks[sq.bank] = rseq.Bank(raw, sq.bank[0], sq.bank[1], sq.bank[2])
                bank = banks[sq.bank]
            base, end = rseq.seq_base(raw, sq.seq[0], sq.seq[1])
            r = rseq.render(raw, base, end, sq.data_offset, bank, SEQ_RATE, sound_volume=sq.volume / 127.0)
        except (rseq.SeqError, brsar.BrsarError, struct.error, IndexError, ValueError) as e:
            res.sequences.append(SeqReport(full, 0.0, False, False, False, {}, {}, {}, str(e)))
            res.bump("BRSAR:secuencias fallidas")
            failed += 1
            continue
        res.sequences.append(SeqReport(full, len(r.samples) / SEQ_RATE, r.loop_start is not None, r.ended,
                                       r.interactive, dict(r.ops), dict(r.ignored), dict(r.stats)))
        if len(r.samples) == 0 or not np.any(r.samples):
            res.bump("BRSAR:secuencias en silencio")
            continue
        res.audio.append(AudioItem(full, "brsar", b"", r.to_audio()))
        res.bump("BRSAR:secuencias prerenderizadas")
    if failed:
        res.pending.append(f"{path}: {failed} sonidos de secuencia no se pudieron prerenderizar (ver la tabla de "
                           "secuencias del reporte); siguen en el disco")


def _visit(res: ScanResult, path: str, data: bytes, depth: int):
    if depth > MAX_DEPTH:
        return
    try:
        raw, comp = compress.decompress(data)
    except Exception as e:  # noqa: BLE001 - un archivo dañado no detiene el escaneo
        res.errors.append(f"{path}: descompresión falló ({e})")
        return
    if comp:
        res.bump(f"comprimido:{comp}")
    lower = path.lower()
    try:
        if u8.is_u8(raw):
            res.bump("U8")
            for inner, blob in u8.read_u8(raw):
                _visit(res, f"{path}/{inner}", blob, depth + 1)
        elif containers.is_brres(raw):
            res.bump("BRRES")
            texs, counts = containers.read_brres(raw, source=path)
            res.textures.extend(texs)
            secs = containers.brres_sections(raw)
            for mname, moff in mdl0.find_models(raw, secs):
                res.models.append(ModelItem(path, mname, moff, raw))
            for aname, aoff in anim.find_chr0(raw, secs):
                res.anims.append(AnimItem(path, aname, aoff, raw))
            for kind, aname, aoff in anim_mat.find(raw, secs):
                res.mat_anims.append(MatAnimItem(path, kind, aname, aoff, raw))
            for sname, soff in scn0.find(raw, secs):
                res.scenes.append(SceneItem(path, sname, soff, raw))
            for folder, n in counts.items():
                res.bump(folder, n)
        elif containers.is_tpl(raw):
            res.bump("TPL")
            res.textures.extend(containers.read_tpl(raw, source=path))
        elif audio.is_brstm(raw):
            res.bump("BRSTM")
            res.audio.append(AudioItem(path, "brstm", raw))
        elif lower.endswith(".dsp") and len(raw) > 0x60:
            res.bump("DSP")
            res.audio.append(AudioItem(path, "dsp", raw))
        elif raw[:4] == b"RSAR":
            res.bump("BRSAR")
            br = brsar.read_brsar(raw)
            for name, dec in br.sounds:
                res.audio.append(AudioItem(f"{path}/{name}", "brsar", b"", dec))
            for kind, n in br.counts.items():
                res.bump(f"BRSAR:{kind}", n)
            res.errors.extend(f"{path}: {w}" for w in br.warnings)
            if br.sequences and res.render_sequences:
                _render_sequences(res, path, raw, br.sequences)
            elif br.counts.get("secuencias"):
                res.pending.append(f"{path}: {br.counts['secuencias']} sonidos de secuencia (RSEQ) no se "
                                   "prerenderizaron (--sin-secuencias); siguen en el disco")
        elif raw[:4] in (b"RLYT", b"RLAN"):
            res.bump("layout")
        else:
            res.bump("otros")
    except Exception as e:  # noqa: BLE001
        res.errors.append(f"{path}: {type(e).__name__}: {e}")


def scan(path: str, progress=None, sequences: bool = True) -> ScanResult:
    root = find_files_root(path)
    res = ScanResult(root=root, render_sequences=sequences)
    all_files = []
    for d, _dirs, files in os.walk(root):
        for name in sorted(files):
            all_files.append(os.path.join(d, name))
    for i, full in enumerate(sorted(all_files)):
        rel = os.path.relpath(full, root).replace(os.sep, "/")
        with open(full, "rb") as f:
            data = f.read()
        res.files += 1
        res.disc_bytes += len(data)
        _visit(res, rel, data, 0)
        if progress:
            progress(i + 1, len(all_files), rel)
    return res
