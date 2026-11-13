"""Matrices de textura y canales de luz del material, modelos con piel y animaciones CHR0 (0.21.0).

Con MDL0 y CHR0 sintéticos (mismos campos que ogws: ResMatData, ResNodeData, NodeMix, ResAnmChr):
  - la SRT del material sale como la matriz de NW4R (Maya) y el canal de luz se lee tal cual;
  - los vértices guardan su hueso (objeto atado a un hueso) o sus pesos (NodeMix);
  - la animación horneada mueve los vértices como CalcWorld: el hijo gira 90° sobre su articulación;
  - el paquete CMDL 3 / CANM 1 se lee de vuelta, y el runtime (si está compilado con choya_render y hay Mesa) dibuja
    el modelo en reposo y animado."""
import math
import os
import shutil
import struct
import subprocess
import tempfile
import unittest

import numpy as np

from choya import anim, mdl0, model_pack as mp, pack
from tests import fixtures as fx

AQUI = os.path.dirname(__file__)
REPO = os.path.dirname(AQUI)

# Brazo: dos huesos (hombro en el origen, codo en y = 1). Cuatro vértices: dos del hombro, dos del codo.
QUAD = ([(-0.1, 0.0, 0), (0.1, 0.0, 0), (-0.1, 1.0, 0), (0.1, 1.0, 0)],
        [(0, 0, 1)] * 4, [(0, 0), (1, 0), (0, 1), (1, 1)], [(0, 1, 2), (2, 1, 3)])
SKEL = [("hombro", -1, (0.0, 0.0, 0.0)), ("codo", 0, (0.0, 1.0, 0.0))]


def brazo(obj_node=1, nodemix=None, **kw):
    return fx.build_mdl0(name="brazo", mesh=QUAD, skeleton=SKEL, obj_node=obj_node, nodemix=nodemix, **kw)


class TestMaterial(unittest.TestCase):
    def test_srt_y_canales(self):
        chans = [(0x3F, (200, 100, 50, 255), (10, 20, 30, 255), 0x2 | (1 << 2) | (2 << 7), 0x0),
                 (0x03, (255, 255, 255, 128), (0, 0, 0, 0), 0, 0)]
        d = fx.build_mdl0(tex_srt={0: (2.0, 2.0, 0.0, 0.25, 0.0), 1: (1.0, 1.0, 90.0, 0.0, 0.0)},
                          chans=chans, num_chans=2)
        m = mdl0.read_mdl0(d, 0, "box")
        mat = m.materials[0]
        r0 = np.array(mat.tex_mtx[0].rows).reshape(2, 4)
        # Maya: s' = 2s - 0.5 (traslación por la escala), t' = 2t - 1 (escala alrededor de t = 1)
        np.testing.assert_allclose(r0, [[2, 0, 0, -0.5], [0, 2, 0, -1]], atol=1e-6)
        r1 = np.array(mat.tex_mtx[1].rows).reshape(2, 4)
        np.testing.assert_allclose(r1, [[0, 1, 0, 0], [-1, 0, 0, 1]], atol=1e-6)   # 90° alrededor de (0.5, 0.5)
        self.assertIsNone(mat.tex_mtx[2])
        self.assertEqual(mat.num_chans, 2)
        self.assertEqual(mat.chans[0].mat, (200, 100, 50, 255))
        self.assertEqual(mat.chans[0].amb, (10, 20, 30, 255))
        self.assertEqual(mat.chans[0].ctrl_color, 0x2 | (1 << 2) | (2 << 7))
        # Al paquete: 1024 bytes por material con la matriz, su modo y los canales.
        blob = mp.pack_model(m, [0])
        u = mp.unpack_model(blob)
        self.assertEqual(u["version"], 3)
        rec = u["materials"][0]
        self.assertEqual(len(rec), 1024)
        np.testing.assert_allclose(struct.unpack_from("<8f", rec, 512), r0.reshape(-1), atol=1e-6)
        self.assertEqual(rec[768:770], bytes([1, 1]))
        self.assertEqual(rec[776], 2)
        flags, *rest = struct.unpack_from("<I4B4BII", rec, 780)
        self.assertEqual((flags, tuple(rest[:4]), tuple(rest[4:8]), rest[8]), (0x3F, (200, 100, 50, 255),
                                                                              (10, 20, 30, 255), chans[0][3]))

    def test_sin_srt_identidad(self):
        m = mdl0.read_mdl0(fx.build_mdl0(tex_srt={0: (1.0, 1.0, 0.0, 0.0, 0.0)}), 0, "box")
        # Escala 1, sin giro ni traslación pero sin las banderas de identidad: la matriz es la identidad.
        np.testing.assert_allclose(np.array(m.materials[0].tex_mtx[0].rows).reshape(2, 4),
                                   [[1, 0, 0, 0], [0, 1, 0, 0]], atol=1e-6)


class TestHuesos(unittest.TestCase):
    def test_atado_a_un_hueso(self):
        m = mdl0.read_mdl0(brazo(), 0, "brazo")
        self.assertTrue(m.skinned)
        self.assertEqual([b.parent for b in m.bones], [-1, 0])
        # Objeto atado al codo (y = 1): los vértices pasan al espacio del modelo y van al hueso 1 con todo el peso.
        self.assertAlmostEqual(float(m.positions[:, 1].max()), 2.0, places=5)
        self.assertTrue(np.all(m.skin_idx[:, 0] == 1) and np.all(m.skin_w[:, 0] == 255))

    def test_pesos_nodemix(self):
        m = mdl0.read_mdl0(brazo(obj_node=2, nodemix={2: [(0, 0.25), (1, 0.75)]}), 0, "brazo")
        # Matriz 2 = mezcla: vértices en espacio del modelo, dos huesos con 64 y 191 (suman 255).
        self.assertAlmostEqual(float(m.positions[:, 1].max()), 1.0, places=5)
        row = sorted(zip(m.skin_idx[0][:2], m.skin_w[0][:2]))
        self.assertEqual(row, [(0, 64), (1, 191)])

    def test_animacion_horneada(self):
        m = mdl0.read_mdl0(brazo(obj_node=2, nodemix={2: [(1, 1.0)]}), 0, "brazo")
        # El codo gira en Z de 0° (cuadro 0) a 90° (cuadro 10), Hermite sin pendiente; el hombro queda quieto.
        c0 = fx.build_chr0({"codo": {"r": [0.0, 0.0, [(0.0, 0.0, 0.0), (10.0, 90.0, 0.0)]],
                                     "t": [0.0, 1.0, 0.0]}}, frames=11)
        c = anim.read_chr0(c0, 0, "doblar")
        self.assertTrue(anim.matches(m, c))
        self.assertAlmostEqual(c.nodes["codo"].r[2].at(5.0), 45.0, places=4)
        baked = anim.bake(m, c)
        self.assertEqual(baked.shape, (11, 2, 3, 4))
        np.testing.assert_allclose(baked[0, 1], np.eye(4)[:3], atol=1e-6)          # cuadro 0 = reposo
        # Cuadro 10: un punto en (0, 2, 0) (1 arriba del codo) termina en (-1, 1, 0).
        p = baked[10, 1] @ np.array([0.0, 2.0, 0.0, 1.0])
        np.testing.assert_allclose(p, [-1.0, 1.0, 0.0], atol=1e-5)
        np.testing.assert_allclose(baked[10, 0], np.eye(4)[:3], atol=1e-6)
        blob = anim.pack_anim(1234, baked, c.loop)
        u = anim.unpack_anim(blob)
        self.assertEqual((u["frames"], u["bones"], u["model_key"], u["loop"]), (11, 2, 1234, True))
        np.testing.assert_allclose(u["matrices"], baked)

    def test_formatos_de_cuadros(self):
        t = anim.Track(keys=[(0.0, 0.0, 0.0), (4.0, 8.0, 2.0)])
        self.assertAlmostEqual(t.at(4.0), 8.0)
        self.assertAlmostEqual(t.at(-1.0), 0.0)
        self.assertAlmostEqual(t.at(2.0), anim._hermite(0.0, 0.0, 8.0, 2.0, 0.5, 2.0))
        cv = anim.Track(values=[0.0, 10.0, 20.0])
        self.assertAlmostEqual(cv.at(1.5), 15.0)
        self.assertAlmostEqual(cv.at(9.0), 20.0)


class TestPaqueteYRender(unittest.TestCase):
    def test_convertir_y_dibujar(self):
        from choya import engine, profiles
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "disco", "files")
            os.makedirs(src)
            chr0 = fx.build_chr0({"codo": {"r": [0.0, 0.0, [(0.0, 0.0, 0.0), (10.0, 90.0, 0.0)]],
                                           "t": [0.0, 1.0, 0.0]}}, frames=11)
            brres = fx.build_brres_folders({"3DModels(NW4R)": [("brazo", brazo(obj_node=2,
                                                                                 nodemix={2: [(1, 1.0)]}))],
                                            "AnmChr(NW4R)": [("doblar", chr0)]})
            with open(os.path.join(src, "brazo.brres"), "wb") as f:
                f.write(brres)
            out = os.path.join(d, "salida")
            rep = engine.run(os.path.join(d, "disco"), out, profiles.get("rg35xxh"), jobs=1, measure=False,
                             log=lambda *a: None)
            self.assertEqual(rep["summary"]["models"]["con_huesos"], 1)
            self.assertEqual(rep["summary"]["models"]["animaciones"], 1)
            _man, entries = pack.read_pack(os.path.join(out, "assets.wlpk"))
            anims = [e for e in entries if e["kind"] == pack.KIND_ANIM]
            model = [e for e in entries if e["kind"] == pack.KIND_MODEL][0]
            self.assertEqual(len(anims), 1)
            self.assertEqual(anim.unpack_anim(anims[0]["data"])["model_key"], model["key"])
            self.assertEqual(mp.unpack_model(model["data"])["version"], 3)
            render = os.environ.get("CHOYA_RENDER") or os.path.join(REPO, "runtime", "build", "choya_render")
            if not os.path.isfile(render):
                self.skipTest("sin choya_render compilado (cmake -DWL_BUILD_RENDER=ON)")
            png = os.path.join(d, "png")
            os.makedirs(png)
            res = subprocess.run([render, os.path.join(out, "assets.wlpk"), png, "--cuadro", "10", "--size", "96", "--doble"],
                                 capture_output=True, text=True, env=dict(os.environ, EGL_PLATFORM="surfaceless"))
            if "EGL no disponible" in res.stderr or "contexto GLES" in res.stderr:
                self.skipTest("sin EGL/Mesa")
            self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
            files = sorted(os.listdir(png))
            self.assertEqual(len(files), 2, files)
            from PIL import Image
            rest, bent = (np.asarray(Image.open(os.path.join(png, f)).convert("RGB")).astype(int) for f in files)
            self.assertGreater(np.abs(rest - bent).sum(), 1000, "la animación no movió nada")


if __name__ == "__main__":
    unittest.main()
