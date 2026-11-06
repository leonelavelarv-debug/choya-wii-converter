"""Recorre el disco extraído y encuentra recursos, entrando en archivos comprimidos y U8."""
from __future__ import annotations

import os
from dataclasses import dataclass, field

from . import audio, compress, containers, u8
from .gxtex import GXTexture


@dataclass
class AudioItem:
    path: str          # ruta dentro del disco (clave para el runtime)
    kind: str          # "brstm" | "dsp"
    data: bytes


@dataclass
class ScanResult:
    root: str
    textures: list[GXTexture] = field(default_factory=list)
    audio: list[AudioItem] = field(default_factory=list)
    counts: dict[str, int] = field(default_factory=dict)
    pending: list[str] = field(default_factory=list)     # tipos conocidos aún no convertidos
    errors: list[str] = field(default_factory=list)
    files: int = 0
    disc_bytes: int = 0

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
            res.pending.append(f"{path}: archivo de sonido BRSAR (efectos); se lee tal cual desde el disco")
        elif raw[:4] in (b"RLYT", b"RLAN"):
            res.bump("layout")
        else:
            res.bump("otros")
    except Exception as e:  # noqa: BLE001
        res.errors.append(f"{path}: {type(e).__name__}: {e}")


def scan(path: str, progress=None) -> ScanResult:
    root = find_files_root(path)
    res = ScanResult(root=root)
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
