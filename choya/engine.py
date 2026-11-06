"""Pipeline completo: escanear -> optimizar -> escribir paquete y reporte."""
from __future__ import annotations

import json
import os
import time
import zlib
from concurrent.futures import ProcessPoolExecutor

import numpy as np
from PIL import Image

from . import __version__, audio as au, etc, optimize as opt, pack, scan as sc
from .gxtex import FORMAT_NAMES, INTENSITY_FORMATS, GXTexture, I4, I8, level_size
from .profiles import Profile


def _convert_one(args):
    tex, profile, measure = args
    try:
        return tex, opt.convert(tex, profile, measure), None
    except Exception as e:  # noqa: BLE001
        return tex, None, f"{tex.name}: {type(e).__name__}: {e}"


def _convert_audio(args):
    item, profile = args
    try:
        dec = au.read_brstm(item.data) if item.kind == "brstm" else au.read_dsp(item.data)
        before = dec.samples.shape[0] * dec.samples.shape[1] * 2
        res = au.optimize(dec, profile.audio_rate, profile.audio_mono_sfx, profile.music_min_seconds)
        return item, dec, res, before, None
    except Exception as e:  # noqa: BLE001
        return item, None, None, 0, f"{item.path}: {type(e).__name__}: {e}"


def preview_png(conv: opt.ConvertedTexture) -> Image.Image | None:
    """Decodifica el nivel 0 convertido para revisarlo en la PC."""
    w, h, d = conv.width, conv.height, conv.data[:opt.out_level_size(conv.fmt, conv.width, conv.height)]
    f = conv.fmt
    if f == opt.OUT_ETC2_RGB8:
        return Image.fromarray(etc.decode_etc2_rgb(d, w, h), "RGB")
    if f == opt.OUT_ETC2_RGBA8:
        return Image.fromarray(etc.decode_etc2_rgba(d, w, h), "RGBA")
    if f == opt.OUT_R11_EAC:
        return Image.fromarray(etc.decode_eac(d, w, h), "L")
    if f == opt.OUT_RG11_EAC:
        i = etc.decode_eac(d, w, h, 2, 0)
        a = etc.decode_eac(d, w, h, 2, 1)
        return Image.fromarray(np.stack([i, a], -1), "LA")
    if f == opt.OUT_L8:
        return Image.frombytes("L", (w, h), d)
    if f == opt.OUT_LA8:
        return Image.frombytes("LA", (w, h), d)
    if f == opt.OUT_RGBA8:
        return Image.frombytes("RGBA", (w, h), d)
    v = np.frombuffer(d, "<u2").reshape(h, w).astype(np.uint32)
    if f == opt.OUT_RGB565:
        rgb = np.stack([(v >> 11) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3], -1)
        return Image.fromarray(rgb.astype(np.uint8), "RGB")
    if f == opt.OUT_RGBA4444:
        rgba = np.stack([(v >> 12) * 17, ((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17], -1)
        return Image.fromarray(rgba.astype(np.uint8), "RGBA")
    if f == opt.OUT_RGBA5551:
        rgba = np.stack([(v >> 11) << 3, ((v >> 6) & 31) << 3, ((v >> 1) & 31) << 3, (v & 1) * 255], -1)
        return Image.fromarray(rgba.astype(np.uint8), "RGBA")
    return None


def _safe(name: str) -> str:
    return "".join(c if c.isalnum() or c in "-_." else "_" for c in name)[-120:]


def run(src: str, out_dir: str, profile: Profile, jobs: int = 0, preview: bool = False,
        measure: bool = True, log=print) -> dict:
    t0 = time.time()
    os.makedirs(out_dir, exist_ok=True)
    gid = sc.game_id(src)
    log(f"Escaneando {src} ...")
    res = sc.scan(src)
    log(f"  {res.files} archivos, {len(res.textures)} texturas, {len(res.audio)} audios")

    # Deduplicar texturas por clave antes de convertir (la misma textura aparece en varios archivos).
    unique: dict[int, GXTexture] = {}
    refs: dict[int, int] = {}
    for tex in res.textures:
        lvl0 = tex.data[:level_size(tex.fmt, tex.width, tex.height)]
        key = pack.texture_key(lvl0, tex.width, tex.height, tex.fmt)
        refs[key] = refs.get(key, 0) + 1
        unique.setdefault(key, tex)
    log(f"  {len(unique)} texturas únicas; convirtiendo con el perfil '{profile.name}' ...")

    writer = pack.PackWriter(flags=pack.FLAG_GLES3 if profile.gles3 else 0)
    tex_report = []
    errors = list(res.errors)
    prev_dir = os.path.join(out_dir, "preview")
    if preview:
        os.makedirs(os.path.join(prev_dir, "texturas"), exist_ok=True)
        os.makedirs(os.path.join(prev_dir, "audio"), exist_ok=True)

    jobs = jobs or max(1, (os.cpu_count() or 2))
    work = [(t, profile, measure) for t in unique.values()]
    if jobs == 1 or len(work) < 8:
        results = list(map(_convert_one, work))
    else:
        with ProcessPoolExecutor(max_workers=jobs) as ex:
            results = list(ex.map(_convert_one, work, chunksize=4))
    done = 0
    for tex, conv, err in results:
        done += 1
        if err:
            errors.append(err)
            continue
        lvl0 = tex.data[:level_size(tex.fmt, tex.width, tex.height)]
        key = pack.texture_key(lvl0, tex.width, tex.height, tex.fmt)
        flags = 0
        if tex.fmt in INTENSITY_FORMATS:
            flags = pack.TEXFLAG_SWIZZLE_I if tex.fmt in (I4, I8) else pack.TEXFLAG_SWIZZLE_IA
        writer.add(pack.Entry(kind=pack.KIND_TEXTURE, fmt=conv.fmt, count=conv.mips, flags=flags, key=key,
                              name=tex.name, data=conv.data, width=conv.width, height=conv.height,
                              extra0=tex.fmt, extra1=zlib.crc32(lvl0) & 0xFFFFFFFF, extra2=len(lvl0)))
        tex_report.append(dict(
            name=tex.name, src=f"{FORMAT_NAMES.get(tex.fmt)} {tex.width}x{tex.height} m{tex.mips}",
            out=f"{opt.OUT_NAMES[conv.fmt]} {conv.width}x{conv.height} m{conv.mips}",
            gx_bytes=len(tex.data), naive_vram=opt.gx_vram_bytes(tex), out_bytes=len(conv.data),
            alpha=conv.alpha, psnr=None if conv.psnr is None or conv.psnr == float("inf") else round(conv.psnr, 2),
            uses=refs.get(key, 1), notes=tex.notes))
        if preview:
            img = preview_png(conv)
            if img is not None:
                img.save(os.path.join(prev_dir, "texturas", f"{_safe(tex.name)}.png"))
        if done % 50 == 0:
            log(f"    {done}/{len(work)} texturas")

    audio_report = []
    for item, dec, conv, before, err in map(_convert_audio, [(a, profile) for a in res.audio]):
        if err:
            errors.append(err)
            continue
        pcm = conv.samples.astype("<i2").tobytes()
        loop = 0xFFFFFFFF if conv.loop_start is None else conv.loop_start
        writer.add(pack.Entry(kind=pack.KIND_AUDIO, fmt=pack.AUDIO_PCM16, count=conv.samples.shape[1],
                              flags=pack.AUDIOFLAG_LOOP if conv.loop_start is not None else 0,
                              key=pack.path_key(item.path), name=item.path, data=pcm,
                              extra0=conv.rate, extra1=loop, extra2=conv.samples.shape[0]))
        audio_report.append(dict(
            path=item.path, src=f"{dec.codec} {dec.rate} Hz {dec.samples.shape[1]} can.",
            out=f"PCM16 {conv.rate} Hz {conv.samples.shape[1]} can.",
            seconds=round(dec.samples.shape[0] / dec.rate, 1), disc_bytes=len(item.data),
            pcm_before=before, out_bytes=len(pcm)))
        if preview:
            with open(os.path.join(prev_dir, "audio", _safe(item.path) + ".wav"), "wb") as f:
                f.write(au.wav_bytes(conv))

    tex_report.sort(key=lambda r: -r["naive_vram"])
    summary = dict(
        textures=len(tex_report), texture_refs=len(res.textures),
        gx_bytes=sum(r["gx_bytes"] for r in tex_report),
        naive_vram=sum(r["naive_vram"] for r in tex_report),
        out_bytes=sum(r["out_bytes"] for r in tex_report),
        audio=len(audio_report),
        audio_pcm_before=sum(r["pcm_before"] for r in audio_report),
        audio_out=sum(r["out_bytes"] for r in audio_report),
        formats={},
        worst_psnr=sorted((r for r in tex_report if r["psnr"] is not None), key=lambda r: r["psnr"])[:10],
    )
    for r in tex_report:
        f = r["out"].split()[0]
        summary["formats"][f] = summary["formats"].get(f, 0) + 1

    manifest = dict(tool="choya-wii-converter", version=__version__, game_id=gid, profile=profile.to_dict(),
                    textures=summary["textures"], audio=summary["audio"])
    pack_path = os.path.join(out_dir, "assets.wlpk")
    size = writer.write(pack_path, manifest)
    report = dict(manifest=manifest, summary=summary, counts=res.counts, pending=res.pending,
                  errors=errors, textures=tex_report, audio=audio_report, pack_bytes=size,
                  disc_files=res.files, disc_bytes=res.disc_bytes, seconds=round(time.time() - t0, 1))
    with open(os.path.join(out_dir, "report.json"), "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=1)
    with open(os.path.join(out_dir, "report.md"), "w", encoding="utf-8") as f:
        f.write(render_markdown(report))
    log(f"Listo: {pack_path} ({size / 1048576:.1f} MiB) en {report['seconds']} s")
    return report


def _mb(n: int) -> str:
    return f"{n / 1048576:.2f} MiB"


def render_markdown(r: dict) -> str:
    s = r["summary"]
    m = r["manifest"]
    lines = [
        f"# Reporte de Choya Wii Converter — {m.get('game_id') or 'juego sin ID'}",
        "",
        f"Perfil **{m['profile']['name']}**: {m['profile']['description']}",
        "",
        "| Medida | Antes | Después |",
        "| --- | --- | --- |",
        f"| Texturas únicas | {s['textures']} ({s['texture_refs']} referencias) | {s['textures']} |",
        f"| Memoria de GPU (RGBA8 sin convertir vs. paquete) | {_mb(s['naive_vram'])} | {_mb(s['out_bytes'])} |",
        f"| Datos GX en el disco | {_mb(s['gx_bytes'])} | — |",
        f"| Audio PCM decodificado | {_mb(s['audio_pcm_before'])} | {_mb(s['audio_out'])} |",
        f"| Paquete .wlpk | — | {_mb(r['pack_bytes'])} |",
        "",
    ]
    if s["naive_vram"]:
        lines.append(f"La memoria de texturas en GPU baja **{100 - 100 * s['out_bytes'] / s['naive_vram']:.0f}%**.")
        lines.append("")
    lines += ["## Formatos de salida", "", "| Formato | Texturas |", "| --- | --- |"]
    lines += [f"| {k} | {v} |" for k, v in sorted(s["formats"].items(), key=lambda kv: -kv[1])]
    if s["worst_psnr"]:
        lines += ["", "## Texturas con menor calidad (PSNR)", "", "| Textura | Origen | Salida | PSNR dB |",
                  "| --- | --- | --- | --- |"]
        lines += [f"| {t['name']} | {t['src']} | {t['out']} | {t['psnr']} |" for t in s["worst_psnr"]]
    lines += ["", "## Contenido del disco", "", "| Tipo | Cantidad |", "| --- | --- |"]
    lines += [f"| {k} | {v} |" for k, v in sorted(r["counts"].items())]
    if r["pending"]:
        lines += ["", "## Pendiente (se lee tal cual desde el disco)", ""]
        lines += [f"- {p}" for p in r["pending"][:50]]
    if r["errors"]:
        lines += ["", f"## Errores ({len(r['errors'])})", ""]
        lines += [f"- {e}" for e in r["errors"][:100]]
    lines += ["", f"Tiempo: {r['seconds']} s. Archivos del disco: {r['disc_files']} ({_mb(r['disc_bytes'])})."]
    return "\n".join(lines) + "\n"
