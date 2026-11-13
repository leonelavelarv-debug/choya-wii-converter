"""Pipeline completo: escanear -> optimizar -> escribir paquete y reporte."""
from __future__ import annotations

import json
import os
import time
import zlib
from concurrent.futures import ProcessPoolExecutor

from PIL import Image

from . import __version__, anim as an, audio as au, mdl0 as md, model_pack as mp, optimize as opt, pack, scan as sc
from .gxtex import FORMAT_NAMES, GXTexture, level_size
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
        if item.kind == "brsar":
            dec = item.decoded
        else:
            dec = au.read_brstm(item.data) if item.kind == "brstm" else au.read_dsp(item.data)
        before = dec.samples.shape[0] * dec.samples.shape[1] * 2
        if profile.keep_adpcm and au.can_keep_adpcm(dec, profile.audio_mono_sfx, profile.music_min_seconds):
            return item, dec, None, before, None
        res = au.optimize(dec, profile.audio_rate, profile.audio_mono_sfx, profile.music_min_seconds)
        return item, dec, res, before, None
    except Exception as e:  # noqa: BLE001
        return item, None, None, 0, f"{item.path}: {type(e).__name__}: {e}"


def _find_texture(model_path: str, tname: str, key_by_name: dict[str, int]) -> int:
    """Busca la textura de un material: mismo BRRES, luego el archivo más cercano en la ruta."""
    exact = key_by_name.get(f"{model_path}/{tname}")
    if exact is not None:
        return exact
    best, best_len = 0, -1
    for full, key in key_by_name.items():
        if full.rsplit("/", 1)[-1] != tname:
            continue
        common = len(os.path.commonprefix([full, model_path]))
        if common > best_len:
            best, best_len = key, common
    return best


def _convert_models(res, writer, key_by_name, preview_by_key, obj_dir, errors, anims: bool = True) -> dict:
    rows = []
    totals = dict(modelos=0, fallidos=0, objetos=0, triangulos=0, omitidos=0, sin_textura=0, avisos=0,
                  con_huesos=0, animaciones=0, animaciones_fallidas=0, animaciones_sin_modelo=0)
    loaded: list = []    # (BRRES, modelo, clave, nombre) de los modelos con huesos
    if obj_dir:
        os.makedirs(obj_dir, exist_ok=True)
    for item in res.models:
        full = f"{item.path}/{item.name}"
        try:
            m = md.read_mdl0(item.brres, item.offset, item.name)
        except Exception as e:  # noqa: BLE001
            totals["fallidos"] += 1
            errors.append(f"modelo {full}: {type(e).__name__}: {e}")
            continue
        keys, files, maps, missing = [], [], [], 0
        for mat in m.materials:
            mat_maps = [None] * 8
            for lay in mat.layers:
                k = _find_texture(item.path, lay.texture, key_by_name)
                if not k:
                    missing += 1
                if lay.texmap < 8:
                    mat_maps[lay.texmap] = (k, lay.wrap_s, lay.wrap_t)
            key = mat_maps[mat.layers[0].texmap][0] if mat.layers and mat.layers[0].texmap < 8 else 0
            keys.append(key)
            maps.append(mat_maps)
            files.append(f"../texturas/{preview_by_key[key]}" if key in preview_by_key else None)
        writer.add(pack.Entry(kind=pack.KIND_MODEL, fmt=mp.VERSION, count=len(m.submeshes), flags=0,
                              key=pack.path_key(full), name=full, data=mp.pack_model(m, keys, maps),
                              extra0=len(m.positions), extra1=len(m.indices), extra2=m.stats["triangulos"]))
        if m.skinned:
            totals["con_huesos"] += 1
            loaded.append((item.path, m, pack.path_key(full), full))
        if obj_dir and len(m.indices):
            mp.write_obj(m, os.path.join(obj_dir, _safe(full) + ".obj"), files)
        totals["modelos"] += 1
        for k in ("objetos", "triangulos", "omitidos"):
            totals[k] += m.stats[k]
        totals["sin_textura"] += missing
        totals["avisos"] += len(m.warnings)
        rows.append(dict(name=full, version=m.version, vertices=len(m.positions), triangles=m.stats["triangulos"],
                         objects=m.stats["objetos"], skipped=m.stats["omitidos"], materials=len(m.materials),
                         missing_textures=missing, warnings=m.warnings[:10]))
    anim_rows = _convert_anims(res, writer, loaded, totals, errors) if anims else []
    return dict(summary=totals, models=rows, anims=anim_rows)


MAX_MODELS_PER_ANIM = 8


def _convert_anims(res, writer, loaded, totals, errors) -> list:
    """Cada CHR0 se hornea para los modelos con huesos que tienen todos sus nodos (primero los del mismo BRRES)."""
    rows = []
    bone_sets = [(path, m, key, full, {b.name for b in m.bones}) for path, m, key, full in loaded]
    for item in getattr(res, "anims", []):
        full_anim = f"{item.path}/{item.name}"
        try:
            c = an.read_chr0(item.brres, item.offset, item.name)
        except Exception as e:  # noqa: BLE001
            totals["animaciones_fallidas"] += 1
            errors.append(f"animación {full_anim}: {type(e).__name__}: {e}")
            continue
        nodes = set(c.nodes)
        targets = [t for t in bone_sets if nodes and nodes <= t[4]]
        targets.sort(key=lambda t: (t[0] != item.path, len(t[4])))
        if not targets:
            totals["animaciones_sin_modelo"] += 1
            continue
        for path, m, key, full, _names in targets[:MAX_MODELS_PER_ANIM]:
            try:
                baked = an.bake(m, c)
            except Exception as e:  # noqa: BLE001
                totals["animaciones_fallidas"] += 1
                errors.append(f"animación {full_anim} en {full}: {type(e).__name__}: {e}")
                continue
            name = f"{full_anim}@{full}"
            writer.add(pack.Entry(kind=pack.KIND_ANIM, fmt=1, count=0, flags=1 if c.loop else 0,
                                  key=pack.path_key(name), name=name, data=an.pack_anim(key, baked, c.loop),
                                  extra0=c.frames, extra1=len(m.bones), extra2=0))
            totals["animaciones"] += 1
            rows.append(dict(name=name, frames=c.frames, bones=len(m.bones), loop=c.loop))
    return rows


def preview_png(conv: opt.ConvertedTexture) -> Image.Image:
    """El nivel 0 tal como lo verá la GPU, para revisarlo en la PC."""
    d = conv.data[:opt.out_level_size(conv.fmt, conv.width, conv.height)]
    return Image.fromarray(opt.decode_out(conv.fmt, d, conv.width, conv.height), "RGBA")


def _safe(name: str) -> str:
    return "".join(c if c.isalnum() or c in "-_." else "_" for c in name)[-120:]


def run(src: str, out_dir: str, profile: Profile, jobs: int = 0, preview: bool = False,
        measure: bool = True, log=print, anims: bool = True) -> dict:
    t0 = time.time()
    os.makedirs(out_dir, exist_ok=True)
    disc = sc.disc_info(src)
    gid = disc["game_id"]
    log(f"Escaneando {src} ...")
    res = sc.scan(src)
    log(f"  {res.files} archivos, {len(res.textures)} texturas, {len(res.audio)} audios")

    # Deduplicar texturas por clave antes de convertir (la misma textura aparece en varios archivos).
    unique: dict[int, GXTexture] = {}
    refs: dict[int, int] = {}
    key_by_name: dict[str, int] = {}
    for tex in res.textures:
        lvl0 = tex.data[:level_size(tex.fmt, tex.width, tex.height)]
        key = pack.texture_key(lvl0, tex.width, tex.height, tex.fmt, tex.palette_raw, tex.palette_fmt or 0)
        refs[key] = refs.get(key, 0) + 1
        unique.setdefault(key, tex)
        key_by_name[tex.name] = key
    preview_by_key: dict[int, str] = {}
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
        key = pack.texture_key(lvl0, tex.width, tex.height, tex.fmt, tex.palette_raw, tex.palette_fmt or 0)
        flags = {opt.OUT_R11_EAC: pack.TEXFLAG_SWIZZLE_I, opt.OUT_RG11_EAC: pack.TEXFLAG_SWIZZLE_IA}.get(conv.fmt, 0)
        writer.add(pack.Entry(kind=pack.KIND_TEXTURE, fmt=conv.fmt, count=conv.mips, flags=flags, key=key,
                              name=tex.name, data=conv.data, width=conv.width, height=conv.height,
                              extra0=tex.fmt, extra1=zlib.crc32(lvl0) & 0xFFFFFFFF, extra2=len(lvl0)))
        tex_report.append(dict(
            name=tex.name, src=f"{FORMAT_NAMES.get(tex.fmt)} {tex.width}x{tex.height} m{tex.mips}",
            out=f"{opt.OUT_NAMES[conv.fmt]} {conv.width}x{conv.height} m{conv.mips}",
            gx_bytes=len(tex.data), naive_vram=opt.gx_vram_bytes(tex), out_bytes=len(conv.data),
            alpha=conv.alpha, psnr=None if conv.psnr is None or conv.psnr == float("inf") else round(conv.psnr, 2),
            uses=refs.get(key, 1), notes=tex.notes, fallback=conv.fallback))
        if preview:
            fname = f"{_safe(tex.name)}.png"
            preview_png(conv).save(os.path.join(prev_dir, "texturas", fname))
            preview_by_key[key] = fname
        if done % 50 == 0:
            log(f"    {done}/{len(work)} texturas")

    audio_report = []
    for item, dec, conv, before, err in map(_convert_audio, [(a, profile) for a in res.audio]):
        if err:
            errors.append(err)
            continue
        if conv is None:   # DSP-ADPCM tal cual: el runtime lo decodifica al vuelo
            payload = au.adpcm_payload(dec)
            n, ch, rate, loop_start, fmt, kind = (dec.samples.shape[0], dec.samples.shape[1], dec.rate,
                                                  dec.loop_start, pack.AUDIO_DSPADPCM, "DSP-ADPCM")
        else:
            payload = conv.samples.astype("<i2").tobytes()
            n, ch, rate, loop_start, fmt, kind = (conv.samples.shape[0], conv.samples.shape[1], conv.rate,
                                                  conv.loop_start, pack.AUDIO_PCM16, "PCM16")
        writer.add(pack.Entry(kind=pack.KIND_AUDIO, fmt=fmt, count=ch,
                              flags=pack.AUDIOFLAG_LOOP if loop_start is not None else 0,
                              key=pack.path_key(item.path), name=item.path, data=payload,
                              extra0=rate, extra1=0xFFFFFFFF if loop_start is None else loop_start, extra2=n))
        audio_report.append(dict(
            path=item.path, src=f"{dec.codec} {dec.rate} Hz {dec.samples.shape[1]} can.",
            out=f"{kind} {rate} Hz {ch} can.", seconds=round(dec.samples.shape[0] / dec.rate, 1),
            disc_bytes=len(item.data), pcm_before=before, out_bytes=len(payload)))
        if preview:
            with open(os.path.join(prev_dir, "audio", _safe(item.path) + ".wav"), "wb") as f:
                f.write(au.wav_bytes(conv or dec))

    model_report = _convert_models(res, writer, key_by_name, preview_by_key,
                                   os.path.join(prev_dir, "modelos") if preview else None, errors, anims=anims)

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
        fallbacks=[r for r in tex_report if r["fallback"]],
        audio_adpcm=sum(1 for r in audio_report if r["out"].startswith("DSP")),
        worst_psnr=sorted((r for r in tex_report if r["psnr"] is not None), key=lambda r: r["psnr"])[:10],
    )
    for r in tex_report:
        f = r["out"].split()[0]
        summary["formats"][f] = summary["formats"].get(f, 0) + 1

    manifest_models = model_report["summary"]["modelos"]
    manifest = dict(tool="choya-wii-converter", models=manifest_models, version=__version__, game_id=gid, revision=disc["revision"],
                    dol_sha1=disc["dol_sha1"], profile=profile.to_dict(),
                    textures=summary["textures"], audio=summary["audio"])
    pack_path = os.path.join(out_dir, "assets.wlpk")
    size = writer.write(pack_path, manifest)
    summary["models"] = model_report["summary"]
    report = dict(manifest=manifest, summary=summary, models=model_report["models"],
                  anims=model_report.get("anims", []), counts=res.counts, pending=res.pending,
                  errors=errors, textures=tex_report, audio=audio_report, pack_bytes=size,
                  disc_files=res.files, disc_bytes=res.disc_bytes, seconds=round(time.time() - t0, 1))
    with open(os.path.join(out_dir, "report.json"), "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=1)
    with open(os.path.join(out_dir, "report.md"), "w", encoding="utf-8") as f:
        f.write(render_markdown(report))
    log(f"Listo: {pack_path} ({size / 1048576:.1f} MiB) en {report['seconds']} s")
    return report


def _version_line(m: dict) -> str:
    from .scan import OGWS_SHA1
    rev = m.get("revision")
    sha = m.get("dol_sha1")
    head = f"Disco **{m.get('game_id') or '?'}**, revisión **{rev if rev is not None else '?'}**"
    if not sha:
        return head + ". No se encontró sys/main.dol, así que no se pudo comparar con ogws."
    if sha == OGWS_SHA1:
        return head + f". main.dol SHA-1 `{sha}`: **coincide con ogws** (Wii Sports US Rev 1)."
    return (head + f". main.dol SHA-1 `{sha}`: **no coincide con ogws**, que necesita "
            f"`{OGWS_SHA1}` (Wii Sports US Rev 1).")


def _mb(n: int) -> str:
    return f"{n / 1048576:.2f} MiB"


def render_markdown(r: dict) -> str:
    s = r["summary"]
    m = r["manifest"]
    lines = [
        f"# Reporte de Choya Wii Converter — {m.get('game_id') or 'juego sin ID'}",
        "",
        f"Choya Wii Converter {m.get('version')}. Perfil **{m['profile']['name']}**: {m['profile']['description']}",
        "",
        _version_line(m),
        "",
        "| Medida | Antes | Después |",
        "| --- | --- | --- |",
        f"| Texturas únicas | {s['textures']} ({s['texture_refs']} referencias) | {s['textures']} |",
        f"| Memoria de GPU (RGBA8 sin convertir vs. paquete) | {_mb(s['naive_vram'])} | {_mb(s['out_bytes'])} |",
        f"| Datos GX en el disco | {_mb(s['gx_bytes'])} | — |",
        f"| Audio (PCM decodificado vs. paquete) | {_mb(s['audio_pcm_before'])} | {_mb(s['audio_out'])} "
        f"({s['audio_adpcm']} de {s['audio']} en DSP-ADPCM) |",
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
    if s["fallbacks"]:
        fb = s["fallbacks"]
        extra = sum(r["out_bytes"] for r in fb)
        lines += ["", f"## Sin compresión por calidad ({len(fb)} texturas, {_mb(extra)})", "",
                  "| Textura | Origen | Salida | Motivo |", "| --- | --- | --- | --- |"]
        lines += [f"| {t['name']} | {t['src']} | {t['out']} | {t['fallback']} |" for t in fb[:30]]
    mo = s.get("models")
    if mo:
        lines += ["", "## Modelos 3D", "",
                  f"{mo['modelos']} modelos leídos, {mo['fallidos']} fallidos. {mo['objetos']} objetos y "
                  f"{mo['triangulos']} triángulos; {mo['omitidos']} objetos omitidos, "
                  f"{mo['sin_textura']} materiales sin su textura, {mo['avisos']} avisos.",
                  "",
                  f"{mo.get('con_huesos', 0)} modelos con huesos; {mo.get('animaciones', 0)} animaciones CHR0 "
                  f"horneadas, {mo.get('animaciones_sin_modelo', 0)} sin un modelo con todos sus huesos, "
                  f"{mo.get('animaciones_fallidas', 0)} fallidas."]
        problem = [x for x in r.get("models", []) if x["warnings"] or x["skipped"] or x["missing_textures"]]
        if problem:
            lines += ["", "| Modelo | Triángulos | Omitidos | Sin textura | Primer aviso |", "| --- | --- | --- | --- | --- |"]
            lines += [f"| {x['name']} | {x['triangles']} | {x['skipped']} | {x['missing_textures']} | "
                      f"{(x['warnings'] or [''])[0]} |" for x in problem[:40]]
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
