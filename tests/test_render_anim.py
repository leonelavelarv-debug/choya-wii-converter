"""El runtime dibuja las animaciones de material, la visibilidad y las escenas (0.42.0), con choya_render y Mesa:
  - el modo con uniformes da lo mismo que el de constantes de la 0.41 sin animaciones ni escena;
  - SRT0, CLR0, PAT0 y VIS0 cambian lo que se ve en el cuadro pedido, solas y junto con la CHR0 del mismo nombre;
  - una escena SCN0 pone sus luces (color, ambiente) en los materiales de su conjunto y su cámara en ref_camera;
  - sin escena, la luz y la cámara del visor, y el diagnóstico lo dice;
  - un paquete con CMDL 3 (sin los campos nuevos) se dibuja igual;
  - con WL_SIN_ETC2=1 (respaldo: texturas descomprimidas en la CPU) sale igual que con ETC2 nativo.
Esto es "probado en la PC con Mesa"; en la RG35XX H sigue pendiente (choya_model)."""
import os
import struct
import subprocess
import tempfile
import unittest

import numpy as np
from PIL import Image

from choya import engine, gxtex, model_pack as mp, pack, profiles
from tests import fixtures as fx
from tests.test_etc2_completo import render_bin

LIT = 0x2 | (1 << 2) | (2 << 7)        # luz encendida, luz 0 en la máscara, difusa recortada, color del registro


def convertir(tmp, folders):
    files = os.path.join(tmp, "disc", "files")
    os.makedirs(files, exist_ok=True)
    with open(os.path.join(files, "m.brres"), "wb") as f:
        f.write(fx.build_brres_folders(folders))
    out = os.path.join(tmp, "out")
    rep = engine.run(os.path.join(tmp, "disc"), out, profiles.get("rg35xxh"), jobs=1, measure=False,
                     log=lambda *_: None)
    return os.path.join(out, "assets.wlpk"), rep


def dibujar(path, carpeta, *args, env=None):
    os.makedirs(carpeta, exist_ok=True)
    r = subprocess.run([render_bin(), path, carpeta, "--size", "64", "--doble", *args], capture_output=True,
                       text=True, env=dict(os.environ, EGL_PLATFORM="surfaceless", **(env or {})))
    if "EGL no disponible" in r.stderr or "contexto GLES" in r.stderr:
        raise unittest.SkipTest("sin EGL/Mesa")
    if r.returncode != 0:
        raise AssertionError(r.stdout + r.stderr)
    return r.stdout


def img(path):
    return np.asarray(Image.open(path).convert("RGB")).astype(int)


def textura(color, w=16, h=16, franja=None):
    a = np.zeros((h, w, 4), np.uint8)
    a[..., :3] = color
    a[..., 3] = 255
    if franja is not None:
        a[:, : w // 2, :3] = franja
    return fx.tex0_blob(gxtex.RGBA8, w, h, fx.encode_gx(gxtex.RGBA8, a))


@unittest.skipUnless(render_bin(), "sin choya_render compilado (scripts/build-pc-render.sh)")
class TestRender(unittest.TestCase):
    def test_uniformes_igual_que_constantes(self):
        with tempfile.TemporaryDirectory() as d:
            p, _ = convertir(d, {
                "3DModels(NW4R)": [("bola", fx.build_mdl0(name="bola", texture="t", mesh=fx.sphere(), translate=(0, 0, 0),
                                                          tex_srt={0: (2.0, 1.0, 30.0, 0.25, 0.0)},
                                                          chans=[(0x3F, (230, 200, 160, 255), (40, 40, 60, 255), LIT,
                                                                  0x2)]))],
                "Textures(NW4R)": [("t", textura((200, 60, 30), franja=(20, 90, 200)))]})
            dibujar(p, os.path.join(d, "a"))
            dibujar(p, os.path.join(d, "b"), env={"WL_TEV_CONSTANTES": "1"})
            a, b = img(os.path.join(d, "a", "m.brres_bola.png")), img(os.path.join(d, "b", "m.brres_bola.png"))
            self.assertLessEqual(np.abs(a - b).max(), 2)
            self.assertGreater(np.abs(a - a[0, 0]).sum(), 1000)          # se dibujó algo

    def test_animaciones_de_material_y_visibilidad(self):
        with tempfile.TemporaryDirectory() as d:
            chans = [(0x3F, (255, 255, 255, 255), (0, 0, 0, 255), 0, 0)]
            model = fx.build_mdl0(name="caja", texture="tex_a", translate=(0, 0, 0), tex_srt={0: (1.0, 1.0, 0.0, 0.0, 0.0)},
                                  chans=chans)
            p, rep = convertir(d, {
                "3DModels(NW4R)": [("caja", model)],
                "Textures(NW4R)": [("tex_a", textura((255, 255, 255), franja=(0, 0, 0))), ("tex_b", textura((40, 80, 230)))],
                "AnmClr(NW4R)": [("color", fx.build_clr0({"mat": {0: (0, [0xFF0000FF] * 5 + [0x00FF00FF] * 6)}}, 10))],
                "AnmTexPat(NW4R)": [("patron", fx.build_pat0({"mat": {0: [(0, 0), (5, 1)]}}, ["tex_a", "tex_b"], 10))],
                "AnmTexSrt(NW4R)": [("mover", fx.build_srt0({"mat": {0: (1.0, 1.0, 0.0, [(0, 0.0, 0.0), (10, 0.5, 0.0)],
                                                                         0.0)}}, 10))],
                "AnmVis(NW4R)": [("ocultar", fx.build_vis0({"root": [f < 6 for f in range(11)]}, 10))],
            })
            self.assertEqual(rep["summary"]["models"]["anim_material"], 4, rep["errors"])
            out0 = dibujar(p, os.path.join(d, "f0"), "--cuadro", "0")
            dibujar(p, os.path.join(d, "f7"), "--cuadro", "7")
            self.assertIn("1 de material", out0)
            g = lambda f, n: img(os.path.join(d, f, f"m.brres_caja@{n}.png"))
            bg = g("f0", "color")[0, 0]
            c0, c7 = g("f0", "color"), g("f7", "color")
            centro = (slice(24, 40), slice(36, 44))                     # en la mitad blanca de tex_a
            self.assertGreater(c0[centro][..., 0].mean(), 200)          # rojo
            self.assertLess(c0[centro][..., 1].mean(), 40)
            self.assertGreater(c7[centro][..., 1].mean(), 200)          # verde
            self.assertLess(c7[centro][..., 0].mean(), 40)
            p0, p7 = g("f0", "patron"), g("f7", "patron")               # tex_a (blanco y negro) -> tex_b (azul)
            self.assertGreater(p7[centro][..., 2].mean(), 180)
            self.assertLess(p7[centro][..., 0].mean(), 80)
            self.assertGreater(np.abs(p0 - p7).sum(), 5000)
            m0, m7 = g("f0", "mover"), g("f7", "mover")                 # la franja se corre
            self.assertGreater(np.abs(m0 - m7).sum(), 5000)
            o0, o7 = g("f0", "ocultar"), g("f7", "ocultar")
            self.assertGreater(np.abs(o0 - bg).sum(), 5000)
            self.assertEqual(np.abs(o7 - bg).max(), 0)                  # cuadro 7: oculta
            # el estado base vuelve: la imagen sin grupo es la del material
            base = img(os.path.join(d, "f7", "m.brres_caja.png"))
            np.testing.assert_array_equal(base, img(os.path.join(d, "f0", "m.brres_caja.png")))

    def test_material_junto_con_chr0(self):
        """Un modelo con piel: la CHR0 "doblar" y la CLR0 "doblar" forman un grupo y se dibujan juntas en el cuadro."""
        from tests.test_huesos_material import brazo
        with tempfile.TemporaryDirectory() as d:
            chans = [(0x3F, (255, 255, 255, 255), (0, 0, 0, 255), 0, 0)]
            chr0 = fx.build_chr0({"codo": {"r": [0.0, 0.0, [(0.0, 0.0, 0.0), (10.0, 90.0, 0.0)]],
                                           "t": [0.0, 1.0, 0.0]}}, frames=11)
            p, rep = convertir(d, {
                "3DModels(NW4R)": [("brazo", brazo(obj_node=2, nodemix={2: [(1, 1.0)]}, texture="tex_a", chans=chans))],
                "Textures(NW4R)": [("tex_a", textura((255, 255, 255)))],
                "AnmChr(NW4R)": [("doblar", chr0)],
                "AnmClr(NW4R)": [("doblar", fx.build_clr0({"mat": {0: (0, 0x0000FFFF)}}, 10))],
            })
            self.assertEqual(rep["summary"]["models"]["animaciones"], 1)
            out = dibujar(p, os.path.join(d, "f"), "--cuadro", "10")
            files = sorted(f for f in os.listdir(os.path.join(d, "f")) if "@" in f)
            self.assertEqual(files, ["m.brres_brazo@doblar.png"], out)
            self.assertIn("CHR0 + 1 de material", out)
            rest = img(os.path.join(d, "f", "m.brres_brazo.png"))
            a = img(os.path.join(d, "f", files[0]))
            self.assertGreater(np.abs(((np.abs(rest - rest[0, 0]).sum(-1) > 30).astype(int)) -
                                      ((np.abs(a - a[0, 0]).sum(-1) > 30).astype(int))).sum(), 50)   # se dobló
            body = np.abs(a - a[0, 0]).sum(-1) > 30
            self.assertGreater(a[body][:, 2].mean(), 200)               # y es azul (CLR0)
            self.assertLess(a[body][:, 0].mean(), 40)

    def _escena_modelo(self, d, escena=True, cam_ref=None):
        chans = [(0x3F, (255, 255, 255, 255), (255, 255, 255, 255), LIT, 0x2)]
        effect = {0: (cam_ref, -1, 1)} if cam_ref is not None else None
        model = fx.build_mdl0(name="bola", texture="t", mesh=fx.sphere(), translate=(0, 0, 0), chans=chans,
                              tex_srt={0: (1.0, 1.0, 0.0, 0.0, 0.0)} if cam_ref is not None else None,
                              tex_effect=effect,
                              tev=fx.tev_config([((0, 0, 1, 0), fx.color_env("ZERO", "TEXC", "RASC", "ZERO"),
                                                  fx.alpha_env("ZERO", "TEXA", "RASA", "ZERO"), 0x0C, 0x1C)],
                                                texgens=(0x5086,)) if cam_ref is not None else None)
        folders = {"3DModels(NW4R)": [("bola", model)],
                   "Textures(NW4R)": [("t", textura((255, 255, 255), franja=(0, 0, 0) if cam_ref is not None else None))]}
        if escena:
            folders["AnmScn(NW4R)"] = [("luz", fx.build_scn0(
                10, lightsets=[("set0", 0, "amb0", ["roja"])], ambs=[("amb0", 0, 0x000040FF)],
                lights=[dict(name="roja", ref=0, type=1, pos=(0, 0, 10), aim=(0, 0, 0), color=0xFF2020FF)],
                cameras=[dict(name="cam", ref=0, type=1, pos=(10, 0, 0), aim=(0, 0, 0))]))]
        return convertir(d, folders)

    def test_luces_de_la_escena(self):
        with tempfile.TemporaryDirectory() as d:
            p, rep = self._escena_modelo(d)
            self.assertEqual(rep["summary"]["models"]["escenas"], 1)
            sin = dibujar(p, os.path.join(d, "sin"))
            con = dibujar(p, os.path.join(d, "con"), "--escena")
            self.assertIn("sin escena", sin)
            self.assertIn("SCN0 activa; materiales con sus luces: 1", con)
            a, b = img(os.path.join(d, "sin", "m.brres_bola.png")), img(os.path.join(d, "con", "m.brres_bola.png"))
            body = np.abs(a - a[0, 0]).sum(-1) > 30
            ra, rb = a[body].mean(0), b[body].mean(0)
            self.assertLess(abs(ra[0] - ra[1]), 20)                     # visor: luz blanca
            self.assertGreater(rb[0], rb[1] + 60)                       # escena: roja
            self.assertGreater(rb[2], rb[1])                            # con el ambiente azul

    def test_ref_camera(self):
        with tempfile.TemporaryDirectory() as d:
            p, _ = self._escena_modelo(d, cam_ref=0)
            sin = dibujar(p, os.path.join(d, "sin"))
            con = dibujar(p, os.path.join(d, "con"), "--escena")
            self.assertIn("ref_camera escena/visor: 0/1", sin)
            self.assertIn("ref_camera escena/visor: 1/0", con)
            a, b = img(os.path.join(d, "sin", "m.brres_bola.png")), img(os.path.join(d, "con", "m.brres_bola.png"))
            self.assertGreater(np.abs(a - b).sum(), 5000)               # el reflejo sale desde otra cámara

    def test_cmdl3_viejo(self):
        with tempfile.TemporaryDirectory() as d:
            p, _ = self._escena_modelo(d, escena=False)
            man, entries = pack.read_pack(p)
            w = pack.PackWriter(flags=man["_flags"])
            for e in entries:
                data = bytearray(e["data"])
                if e["kind"] == pack.KIND_MODEL:            # como lo escribía la 0.41: versión 3, campos nuevos en 0
                    u = mp.unpack_model(bytes(data))
                    struct.pack_into("<I", data, 4, 3)
                    nv, ni, ns = len(u["vertices"]), len(u["indices"]), len(u["submeshes"])
                    so = 64 + nv * 44 + ni * 4
                    for i in range(ns):
                        data[so + 32 * i + 24:so + 32 * i + 32] = b"\0" * 8
                    mo = so + ns * 32
                    for i in range(len(u["materials"])):
                        data[mo + 1024 * i + 820:mo + 1024 * i + 840] = b"\0" * 20
                w.add(pack.Entry(kind=e["kind"], fmt=e["fmt"], count=e["count"], flags=e["flags"], key=e["key"],
                                 name=e["name"], data=bytes(data), width=e["width"], height=e["height"],
                                 extra0=e["extra0"], extra1=e["extra1"], extra2=e["extra2"]))
            viejo = os.path.join(d, "viejo.wlpk")
            w.write(viejo, man)
            dibujar(p, os.path.join(d, "n"))
            dibujar(viejo, os.path.join(d, "v"))
            np.testing.assert_array_equal(img(os.path.join(d, "n", "m.brres_bola.png")),
                                          img(os.path.join(d, "v", "m.brres_bola.png")))

    def test_respaldo_sin_etc2(self):
        with tempfile.TemporaryDirectory() as d:
            p, _ = convertir(d, {
                "3DModels(NW4R)": [("caja", fx.build_mdl0(name="caja", texture="t", translate=(0, 0, 0)))],
                "Textures(NW4R)": [("t", textura((180, 90, 30), w=32, h=32, franja=(10, 120, 200)))]})
            dibujar(p, os.path.join(d, "a"))
            dibujar(p, os.path.join(d, "b"), env={"WL_SIN_ETC2": "1"})
            np.testing.assert_array_equal(img(os.path.join(d, "a", "m.brres_caja.png")),
                                          img(os.path.join(d, "b", "m.brres_caja.png")))


if __name__ == "__main__":
    unittest.main()
