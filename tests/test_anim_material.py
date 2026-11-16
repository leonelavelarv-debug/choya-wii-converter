"""Animaciones de material, visibilidad y escena (0.42.0): SRT0, CLR0, PAT0, VIS0 y SCN0 se leen como ogws, se hornean
para el modelo (nombres a índices, claves de textura, máscaras de color) y se empaquetan en CMAN/CSCN. El runtime las
dibuja en tests/test_render_anim.py."""
import math
import os
import struct
import tempfile
import unittest

import numpy as np

from choya import anim_mat as am, engine, gxtex, mdl0 as md, model_pack as mp, pack, profiles, scn0 as sn
from tests import fixtures as fx

TEX_SRT = {0: (1.0, 1.0, 0.0, 0.0, 0.0)}


def modelo(**kw):
    blob = fx.build_mdl0(name="caja", tex_srt=TEX_SRT, chans=[(0x3F, (200, 100, 50, 255), (10, 20, 30, 255),
                                                               0x2 | (1 << 2) | (2 << 7), 0x2)], **kw)
    brres = fx.build_brres_folders({"3DModels(NW4R)": [("caja", blob)]})
    off = dict(md.find_models(brres, __import__("choya.containers", fromlist=["x"]).brres_sections(brres)))["caja"]
    return md.read_mdl0(brres, off, "caja")


def leer(kind, blob):
    return am.READERS[kind](blob, 0, "a")


class TestSrt0(unittest.TestCase):
    def test_traslacion_con_cuadros_clave(self):
        blob = fx.build_srt0({"mat": {0: (1.0, 1.0, 0.0, [(0, 0.0, 0.0), (10, 1.0, 0.0)], 0.0)}}, 10)
        a = leer(am.TYPE_SRT, blob)
        self.assertEqual(a.frames, 10)
        self.assertTrue(a.loop)
        m = modelo()
        b = am.bake(m, a)
        self.assertEqual(len(b.tracks), 1)
        target, slot, vals = b.tracks[0]
        self.assertEqual((target, slot), (0, 0))
        for f in (0, 3, 5, 9):
            tu = a.mats["mat"][0][1][3].at(float(f))
            want = md.tex_srt_matrix(1, 1, 0, tu, 0, 0)[:2].reshape(-1)
            np.testing.assert_allclose(vals[f], want, atol=1e-6)
        # Hermite con pendientes 0: a la mitad vale 0.5, y es suave (no lineal) a un cuarto
        self.assertAlmostEqual(a.mats["mat"][0][1][3].at(5.0), 0.5, places=6)
        self.assertLess(a.mats["mat"][0][1][3].at(2.5), 0.25)

    def test_constante_se_guarda_una_vez(self):
        blob = fx.build_srt0({"mat": {0: (2.0, 2.0, 0.0, 0.0, 0.0)}}, 30)
        b = am.bake(modelo(), leer(am.TYPE_SRT, blob))
        u = am.unpack(am.pack(77, b))
        self.assertEqual(u["model_key"], 77)
        self.assertEqual(u["kind"], am.TYPE_SRT)
        self.assertTrue(u["tracks"][0]["const"])
        self.assertEqual(u["tracks"][0]["values"].shape, (1, 8))
        self.assertAlmostEqual(float(u["tracks"][0]["values"][0][0]), 2.0)

    def test_material_que_no_existe_no_se_hornea(self):
        blob = fx.build_srt0({"otro": {0: (2.0, 2.0, 0.0, 0.0, 0.0)}}, 30)
        self.assertEqual(am.bake(modelo(), leer(am.TYPE_SRT, blob)).tracks, [])


class TestClr0(unittest.TestCase):
    def test_mascara_sobre_el_color_del_material(self):
        frames = [0x00000000 | (i * 20) << 24 for i in range(11)]       # R animado de 0 a 200
        blob = fx.build_clr0({"mat": {0: (0x00FFFFFF, frames)}}, 10)
        b = am.bake(modelo(), leer(am.TYPE_CLR, blob))
        _t, slot, vals = b.tracks[0]
        self.assertEqual(slot, 0)
        # G, B y A salen del material (100, 50, 255); R del cuadro
        np.testing.assert_array_equal(vals[3], [60, 100, 50, 255])
        np.testing.assert_array_equal(vals[9], [180, 100, 50, 255])

    def test_registro_tev_y_constante(self):
        blob = fx.build_clr0({"mat": {4: (0, 0x11223344)}}, 5)      # TEV0 = registro C0
        b = am.bake(modelo(), leer(am.TYPE_CLR, blob))
        self.assertEqual(b.tracks[0][1], 4)
        np.testing.assert_array_equal(b.tracks[0][2][0], [0x11, 0x22, 0x33, 0x44])

    def test_interpolacion_de_15_bits(self):
        self.assertEqual(am.clr_at([0x000000FF, 0xFF0000FF], 0.5, 1), 0x7F0000FF)
        self.assertEqual(am.clr_at([0x000000FF, 0xFF0000FF], 5.0, 1), 0xFF0000FF)   # ClipFrame


class TestPat0(unittest.TestCase):
    def test_cambio_discreto_y_claves(self):
        blob = fx.build_pat0({"mat": {0: [(0, 0), (5, 1)]}}, ["tex_a", "tex_b"], 10)
        a = leer(am.TYPE_PAT, blob)
        self.assertEqual(a.textures, ["tex_a", "tex_b"])
        self.assertEqual(am.pat_at(a.mats["mat"][0][1], 4.99), (0, 0))
        self.assertEqual(am.pat_at(a.mats["mat"][0][1], 5.0), (1, 0))
        keys = {"tex_a": 0xAAAA, "tex_b": 0xBBBB}
        b = am.bake(modelo(), a, keys.get)
        self.assertEqual(b.keys, [0xAAAA, 0xBBBB])
        vals = b.tracks[0][2]
        self.assertEqual(list(vals[:5]), [0] * 5)
        self.assertEqual(list(vals[5:]), [1] * 5)

    def test_textura_que_falta(self):
        blob = fx.build_pat0({"mat": {0: 1}}, ["tex_a", "nada"], 4)
        b = am.bake(modelo(), leer(am.TYPE_PAT, blob), {"tex_a": 1}.get)
        self.assertEqual(list(b.tracks[0][2]), [am.NO_KEY] * 4)
        self.assertIn("texturas de PAT0 que no están en el paquete", b.notes)


class TestVis0(unittest.TestCase):
    def test_bits_por_cuadro(self):
        bits = [f % 2 == 0 for f in range(9)]
        blob = fx.build_vis0({"root": bits}, 8)
        a = leer(am.TYPE_VIS, blob)
        self.assertEqual(a.nodes["root"][:4], [True, False, True, False])
        b = am.bake(modelo(), a)
        self.assertEqual(b.tracks[0][0], 0)                 # submalla 0 (su nodo de visibilidad es root)
        self.assertEqual(list(b.tracks[0][2][:4]), [1, 0, 1, 0])

    def test_constante(self):
        b = am.bake(modelo(), leer(am.TYPE_VIS, fx.build_vis0({"root": False}, 8)))
        self.assertEqual(list(b.tracks[0][2]), [0] * 8)


class TestEntradaDanada(unittest.TestCase):
    def test_cortado_da_error_claro(self):
        for kind, blob in ((am.TYPE_SRT, fx.build_srt0({"mat": {0: (1.0, 1.0, 0.0, [(0, 0.0, 0.0), (9, 1.0, 0.0)],
                                                                        0.0)}}, 9)),
                           (am.TYPE_CLR, fx.build_clr0({"mat": {0: (0, [1] * 11)}}, 10)),
                           (am.TYPE_PAT, fx.build_pat0({"mat": {0: [(0, 0), (5, 1)]}}, ["a", "b"], 10)),
                           (am.TYPE_VIS, fx.build_vis0({"root": [True] * 11}, 10))):
            for cut in (0x20, len(blob) // 2, len(blob) - 40):
                bad = bytearray(blob[:cut])
                struct.pack_into(">I", bad, 4, len(blob))     # la cabecera dice más de lo que hay
                with self.assertRaises((am.MatAnimError, struct.error), msg=(kind, cut)):
                    leer(kind, bytes(bad))

    def test_offset_fuera_de_rango(self):
        blob = bytearray(fx.build_vis0({"root": [True] * 11}, 10))
        struct.pack_into(">i", blob, 0x10, 0x7FFFFF00)
        with self.assertRaises(am.MatAnimError):
            leer(am.TYPE_VIS, bytes(blob))

    def test_version_no_soportada(self):
        blob = bytearray(fx.build_clr0({"mat": {0: (0, 1)}}, 2))
        struct.pack_into(">I", blob, 8, 9)
        with self.assertRaisesRegex(am.MatAnimError, "versión 9"):
            leer(am.TYPE_CLR, bytes(blob))


class TestScn0(unittest.TestCase):
    def escena(self):
        return fx.build_scn0(
            20,
            lightsets=[("set0", 0, "amb0", ["sol", "foco"])],
            ambs=[("amb0", 0, 0x406080FF)],
            lights=[dict(name="sol", ref=0, type=1, pos=(0, 10, 0), aim=(0, 0, 0), color=0xFFEEDDFF),
                    dict(name="foco", ref=1, type=2, pos=(0, 0, 5), aim=(0, 0, 0), cutoff=30.0, spot_fn=2,
                         dist=10.0, bright=0.5, dist_fn=1, spec_ref=2, shininess=8.0)],
            cameras=[dict(name="cam", ref=0, type=1, pos=(0, 2, 10), aim=(0, 2, 0), fovy=40.0, aspect=1.5,
                          near=0.5, far=500.0),
                     dict(name="rot", ref=1, type=0, pos=(1, 2, 3), rot=(10.0, [(0, 0.0, 0.0), (20, 90.0, 0.0)], 0.0))])

    def test_lectura(self):
        s = sn.read_scn0(self.escena(), 0, "esc")
        self.assertEqual(s.frames, 20)
        self.assertEqual([ls.lights for ls in s.lightsets], [["sol", "foco"]])
        self.assertEqual(s.lightsets[0].amb, "amb0")
        self.assertEqual(s.ambs[0].colors, [0x406080FF])
        self.assertEqual([lt.ref for lt in s.lights], [0, 1])
        self.assertEqual(len(s.cameras), 2)

    def test_horneado(self):
        s = sn.read_scn0(self.escena(), 0, "esc")
        u = sn.unpack(sn.bake(s).blob)
        self.assertEqual(u["frames"], 20)
        ref, amb, nl, *idx = u["sets"][0]
        self.assertEqual((ref, amb, nl), (0, 0, 2))
        self.assertEqual(idx[:2], [0, 1])
        self.assertEqual(u["ambs"][0]["colors"], [(0x40, 0x60, 0x80, 0xFF)])
        sol, foco, espec = u["lights"]
        self.assertEqual((sol["type"], foco["type"], espec["type"]), (1, 2, 3))
        self.assertEqual(foco["spec"], 2)
        self.assertEqual(espec["ref"], 2)
        fr = sol["frames"][0]
        np.testing.assert_allclose(fr[3:6], (0, -1, 0), atol=1e-6)      # de pos a aim
        self.assertEqual(fr[6:10], (0xFF, 0xEE, 0xDD, 0xFF))
        a = foco["frames"][0][10:13]
        np.testing.assert_allclose(a, sn.spot_coefs(30.0, 2), rtol=1e-6)
        c = math.cos(math.radians(30.0))
        np.testing.assert_allclose(a, (-c / (1 - c), 1 / (1 - c), 0.0), rtol=1e-5)
        k = foco["frames"][0][13:16]
        np.testing.assert_allclose(k, (1.0, (1 - 0.5) / (0.5 * 10.0), 0.0), rtol=1e-6)
        self.assertAlmostEqual(espec["frames"][0][17], 8.0)

    def test_camara_aim(self):
        s = sn.read_scn0(self.escena(), 0, "esc")
        u = sn.unpack(sn.bake(s).blob)
        cam = u["cameras"][0]
        self.assertTrue(cam["const"])
        v = np.array(cam["frames"][0][:12]).reshape(3, 4)
        np.testing.assert_allclose(v[:, :3] @ v[:, :3].T, np.eye(3), atol=1e-6)       # ortonormal
        target = v @ np.array([0, 2, 0, 1.0])
        np.testing.assert_allclose(target, (0, 0, -10), atol=1e-5)    # el objetivo queda delante (-z)
        self.assertEqual(cam["frames"][0][12:16], (40.0, 1.5, 0.5, 500.0))
        rot = u["cameras"][1]
        self.assertFalse(rot["const"])
        self.assertEqual(len(rot["frames"]), 20)

    def test_coeficientes_gx(self):
        self.assertEqual(sn.spot_coefs(95.0, 2), (1.0, 0.0, 0.0))          # fuera de rango: apagado
        self.assertEqual(sn.dist_coefs(10.0, 1.0, 1), (1.0, 0.0, 0.0))
        np.testing.assert_allclose(sn.dist_coefs(10.0, 0.25, 3), (1.0, 0.0, 0.75 / (10 * 0.25 * 10)))


class TestPaquete(unittest.TestCase):
    def test_conversion_de_un_disco(self):
        with tempfile.TemporaryDirectory() as tmp:
            files = os.path.join(tmp, "disc", "files")
            os.makedirs(files)
            w = h = 16
            img = np.full((h, w, 4), 200, np.uint8)
            tex = lambda: fx.tex0_blob(gxtex.RGBA8, w, h, fx.encode_gx(gxtex.RGBA8, img))
            img2 = img.copy()
            img2[..., 0] = 10
            tex2 = fx.tex0_blob(gxtex.RGBA8, w, h, fx.encode_gx(gxtex.RGBA8, img2))
            brres = fx.build_brres_folders({
                "3DModels(NW4R)": [("caja", fx.build_mdl0(name="caja", texture="tex_a", tex_srt=TEX_SRT))],
                "Textures(NW4R)": [("tex_a", tex()), ("tex_b", tex2)],
                "AnmTexSrt(NW4R)": [("mover", fx.build_srt0({"mat": {0: (1.0, 1.0, 0.0, [(0, 0.0, 0.0),
                                                                                        (10, 1.0, 0.0)], 0.0)}}, 10))],
                "AnmClr(NW4R)": [("mover", fx.build_clr0({"mat": {4: (0, 0xFF0000FF)}}, 10))],
                "AnmTexPat(NW4R)": [("mover", fx.build_pat0({"mat": {0: [(0, 0), (5, 1)]}}, ["tex_a", "tex_b"], 10))],
                "AnmVis(NW4R)": [("mover", fx.build_vis0({"root": [f < 5 for f in range(11)]}, 10))],
                "AnmScn(NW4R)": [("escena", TestScn0().escena())],
            })
            with open(os.path.join(files, "caja.brres"), "wb") as f:
                f.write(brres)
            out = os.path.join(tmp, "out")
            rep = engine.run(os.path.join(tmp, "disc"), out, profiles.get("rg35xxh"), jobs=1, log=lambda *_: None)
            mo = rep["summary"]["models"]
            self.assertEqual(mo["anim_material"], 4, rep["errors"])
            self.assertEqual(mo["escenas"], 1)
            _man, entries = pack.read_pack(os.path.join(out, "assets.wlpk"))
            mats = [e for e in entries if e["kind"] == pack.KIND_MATANIM]
            self.assertEqual(sorted(e["fmt"] for e in mats), [1, 2, 3, 4])
            model = next(e for e in entries if e["kind"] == pack.KIND_MODEL)
            for e in mats:
                u = am.unpack(e["data"])
                self.assertEqual(u["model_key"], model["key"])
                self.assertIn("@", e["name"])
                self.assertTrue(e["name"].split("@")[0].endswith("mover." + am.TYPE_NAMES[e["fmt"]]))
            pat = am.unpack(next(e for e in mats if e["fmt"] == am.TYPE_PAT)["data"])
            tex_keys = {e["name"].rsplit("/", 1)[-1]: e["key"] for e in entries if e["kind"] == pack.KIND_TEXTURE}
            self.assertEqual(pat["keys"], [tex_keys["tex_a"], tex_keys["tex_b"]])
            scene = next(e for e in entries if e["kind"] == pack.KIND_SCENE)
            self.assertEqual(sn.unpack(scene["data"])["frames"], 20)
            # CMDL 4: referencias de cámara y luz del material y su conjunto de luces
            u = mp.unpack_model(model["data"])
            self.assertEqual(u["version"], 4)
            mat = u["materials"][0]
            self.assertEqual(struct.unpack_from("<8b8bbbB", mat, 820)[-1], 1)
            md_ = open(os.path.join(out, "report.md"), encoding="utf-8").read()
            self.assertIn("Escenas SCN0: 1", md_)


if __name__ == "__main__":
    unittest.main()
