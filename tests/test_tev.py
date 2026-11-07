"""TEV de punta a punta: MDL0 sintético con la configuración TEV de materiales reales de Wii Sports
(tomada del diagnóstico) -> conversor -> paquete -> generador C++ -> GLSL compilado y ejecutado
en una GPU de software (Mesa llvmpipe)."""
import glob
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path[:0] = [ROOT, HERE]

from choya import engine, gxtex, profiles, u8  # noqa: E402
import fixtures as fx  # noqa: E402
import gles  # noqa: E402

BUILD = os.path.join(ROOT, "runtime", "build")
TEVGEN = os.path.join(BUILD, "wl_tevgen")
NONE = (0, 0, 0, 7)


def S(tref, color, alpha, kc=0x0C, ka=0x1C):
    return (tref, fx.color_env(*color[:4], **(color[4] if len(color) > 4 else {})),
            fx.alpha_env(*alpha[:4], **(alpha[4] if len(alpha) > 4 else {})), kc, ka)


# Configuraciones TEV leídas de Wii Sports con `choya modelo` (diag_tev.txt).
REAL = {
    "pin": fx.tev_config([
        S((0, 0, 1, 0), ("ZERO", "TEXC", "RASC", "ZERO"), ("ZERO", "TEXA", "KONST", "ZERO"), 31, 31),
        S((1, 1, 1, 7), ("ZERO", "TEXC", "KONST", "CPREV"), ("TEXA", "ZERO", "KONST", "APREV"), 31, 31),
        S((2, 2, 1, 7), ("ZERO", "TEXC", "CPREV", "ZERO"), ("APREV", "ZERO", "ZERO", "ZERO"), 12, 28),
    ], konst=[(0, 0, 0, 0), (0, 0, 0, 0), (0, 0, 0, 0), (0, 0, 0, 43)], texgens=(0x5280, 0x5086, 0x5006)),
    "ball": fx.tev_config([
        S((1, 1, 1, 7), ("ZERO", "KONST", "TEXC", "ZERO"), ("ZERO", "TEXA", "RASA", "ZERO"), 7, 28),
        S((0, 0, 1, 7), ("TEXC", "ONE", "CPREV", "CPREV"), ("APREV", "ZERO", "ZERO", "ZERO"), 28, 28),
        S((0, 0, 1, 7), ("CPREV", "TEXC", "TEXA", "ZERO"), ("APREV", "ZERO", "ZERO", "ZERO"), 12, 28),
        S((0, 0, 0, 0), ("ZERO", "RASC", "CPREV", "ZERO"), ("APREV", "ZERO", "ZERO", "ZERO"), 12, 28),
    ], texgens=(0x5280, 0x5086)),
    "mii": fx.tev_config([
        S((0, 0, 1, 7), ("TEXC", "C0", "TEXA", "ZERO", dict(dest=2)), ("ZERO", "ZERO", "ZERO", "ZERO"), 12, 16),
        S((1, 1, 1, 0), ("ZERO", "TEXC", "RASA", "RASC"), ("ZERO", "ZERO", "ZERO", "ZERO"), 27, 27),
        S((3, 3, 1, 7), ("ZERO", "C2", "CPREV", "RASC"), ("ZERO", "ZERO", "ZERO", "TEXA"), 23, 23),
        S((2, 2, 1, 0), ("ZERO", "TEXC", "RASA", "CPREV"), ("ZERO", "ZERO", "ZERO", "APREV"), 19, 19),
    ], cregs=[(0, 0, 0, 0), (255, 255, 0, 0), (0, 255, 255, 0), (0, 0, 0, 0)],
        konst=[(0, 0, 0, 0)] * 3 + [(63, 64, 64, 64)], texgens=(0x5280, 0x5086, 0x5086, 0x5280)),
    "racket": fx.tev_config([
        S((0, 0, 1, 0), ("ZERO", "TEXC", "RASC", "ZERO"), ("ZERO", "ZERO", "ZERO", "ZERO"), 12, 28),
        S((1, 1, 1, 0), ("ZERO", "KONST", "TEXC", "CPREV"), ("ZERO", "ZERO", "ZERO", "KONST"), 5, 7),
    ], texgens=(0x5280, 0x5086)),
    "xlu": fx.tev_config([
        S((0, 0, 1, 0), ("ZERO", "TEXC", "RASC", "ZERO"), ("ZERO", "TEXA", "RASA", "ZERO")),
    ], blend=1 | 8 | 16 | (4 << 8) | (5 << 5), zmode=0x07),
}


def generate(tev, textures=("tex_a",)) -> str:
    """Convierte un modelo con esa configuración TEV y devuelve el fragment shader generado."""
    with tempfile.TemporaryDirectory() as tmp:
        files = os.path.join(tmp, "disc", "files")
        os.makedirs(files)
        img = fx.gradient(8, 8)
        texs = [(t, fx.tex0_blob(gxtex.RGB565, 8, 8, fx.encode_gx(gxtex.RGB565, img))) for t in textures]
        brres = fx.build_brres_folders({"3DModels(NW4R)": [("m", fx.build_mdl0(texture=textures[0], tev=tev,
                                                                             extra_textures=textures[1:]))],
                                        "Textures(NW4R)": texs})
        with open(os.path.join(files, "m.arc"), "wb") as f:
            f.write(u8.build_u8([("m.brres", brres)]))
        out = os.path.join(tmp, "out")
        engine.run(os.path.join(tmp, "disc"), out, profiles.get("rg35xxh"), jobs=1, log=lambda *_: None)
        sh = os.path.join(tmp, "sh")
        os.makedirs(sh)
        subprocess.run([TEVGEN, os.path.join(out, "assets.wlpk"), sh], check=True, capture_output=True)
        frags = glob.glob(os.path.join(sh, "*.frag"))
        assert len(frags) == 1, frags
        with open(frags[0]) as fh:
            return fh.read()


def vert() -> str:
    with tempfile.TemporaryDirectory() as tmp:
        pass
    # el vertex shader es fijo: se obtiene de cualquier paquete
    return VERT


VERT = None


@unittest.skipUnless(os.path.isfile(TEVGEN), "runtime sin compilar")
class TestTevGenerator(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        global VERT
        with tempfile.TemporaryDirectory() as tmp:
            open(os.path.join(tmp, "x"), "w").close()
        # obtener common.vert generando un modelo simple
        with tempfile.TemporaryDirectory() as tmp:
            files = os.path.join(tmp, "disc", "files")
            os.makedirs(files)
            brres = fx.build_brres_folders({"3DModels(NW4R)": [("m", fx.build_mdl0())]})
            with open(os.path.join(files, "m.brres"), "wb") as f:
                f.write(brres)
            out = os.path.join(tmp, "out")
            engine.run(os.path.join(tmp, "disc"), out, profiles.get("rg35xxh"), jobs=1, log=lambda *_: None)
            sh = os.path.join(tmp, "sh")
            os.makedirs(sh)
            subprocess.run([TEVGEN, os.path.join(out, "assets.wlpk"), sh], check=True, capture_output=True)
            with open(os.path.join(sh, "common.vert")) as fh:
                VERT = fh.read()

    def test_real_materials_generate_expected_code(self):
        pin = generate(REAL["pin"], ("bwg_pin", "bwg_pin_env", "test_gra"))
        self.assertIn("uniform sampler2D tex2;", pin)
        self.assertIn("vec2 tc1 = env;", pin)                      # texgen 1 desde la normal
        self.assertIn("ka = 0.168627;", pin)                        # K3.A = 43/255
        mii = generate(REAL["mii"], ("a", "b", "c", "d"))
        self.assertIn("c1.rgb = clamp(((vec3(0.0) + mix(tex.rgb, c0.rgb, tex.aaa)", mii)   # destino C1
        self.assertIn("vec4 c0 = vec4(1.000000, 1.000000, 0.000000, 0.000000);", mii)

    @unittest.skipUnless(gles.available(), "sin OpenGL ES de software")
    def test_real_materials_compile_on_gles(self):
        for name, tev in REAL.items():
            texs = {"pin": ("a", "b", "c"), "ball": ("a", "b"), "mii": ("a", "b", "c", "d"),
                    "racket": ("a", "b")}.get(name, ("a",))
            with self.subTest(material=name):
                gles.program(VERT, generate(tev, texs))

    @unittest.skipUnless(gles.available(), "sin OpenGL ES de software")
    def test_pixels(self):
        # TEXC * RASC, con la luz de frente (ras = 1): sale el color de la textura
        p = gles.program(VERT, generate(fx.tev_config([S((0, 0, 1, 0), ("ZERO", "TEXC", "RASC", "ZERO"),
                                                       ("ZERO", "ZERO", "ZERO", "ZERO"), 12, 28)]), ))
        px = gles.draw(p, {0: (200, 100, 50, 255)})
        self.assertTrue(all(abs(a - b) <= 2 for a, b in zip(px[:3], (200, 100, 50))), px)
        # lerp(ZERO, TEXC, KONST K0) con K0 = (128, 64, 255)
        p = gles.program(VERT, generate(fx.tev_config(
            [S((0, 0, 1, 7), ("ZERO", "TEXC", "KONST", "ZERO"), ("ZERO", "ZERO", "ZERO", "KONST"), 0x0C, 0x1C)],
            konst=[(128, 64, 255, 200)])))
        px = gles.draw(p, {0: (200, 200, 200, 255)})
        want = (round(200 * 128 / 255), round(200 * 64 / 255), 200, 200)
        self.assertTrue(all(abs(a - b) <= 2 for a, b in zip(px, want)), (px, want))
        # Prueba de alfa: GREATER 128 con alfa 100 -> se descarta y queda el color de fondo
        p = gles.program(VERT, generate(fx.tev_config(
            [S((0, 0, 1, 7), ("ZERO", "ZERO", "ZERO", "TEXC"), ("ZERO", "ZERO", "ZERO", "TEXA"))],
            alpha_compare=128 | (4 << 16) | (4 << 19))))
        self.assertEqual(gles.draw(p, {0: (10, 20, 30, 100)}, clear=(1, 2, 3, 4)), (1, 2, 3, 4))
        px = gles.draw(p, {0: (10, 20, 30, 200)}, clear=(1, 2, 3, 4))
        self.assertTrue(abs(px[0] - 10) <= 1 and abs(px[3] - 200) <= 1, px)
        # Resta + escala x2 + bias: (d - lerp(a,b,c) + 0.5) * 2 con d=ONE, b=TEXC, c=ONE
        p = gles.program(VERT, generate(fx.tev_config(
            [S((0, 0, 1, 7), ("ZERO", "TEXC", "ONE", "HALF", dict(sub=1, bias=0, scale=1)),
               ("ZERO", "ZERO", "ZERO", "ZERO"))])))
        px = gles.draw(p, {0: (51, 0, 0, 255)})   # (0.5 - 0.2) * 2 = 0.6 -> 153 ; (0.5 - 0)*2 = 1 -> 255
        self.assertTrue(abs(px[0] - 153) <= 2 and px[1] >= 253, px)


if __name__ == "__main__":
    unittest.main()


RENDER = os.path.join(ROOT, "runtime", "build-render", "choya_render")


@unittest.skipUnless(os.path.isfile(RENDER) and os.path.isfile(TEVGEN) and gles.available(),
                     "choya_render sin compilar o sin EGL")
class TestRenderPipeline(unittest.TestCase):
    def test_pin_like_sphere_renders_texture_and_reflection(self):
        import numpy as np
        from PIL import Image
        with tempfile.TemporaryDirectory() as tmp:
            files = os.path.join(tmp, "disc", "files")
            os.makedirs(files)
            h = w = 64
            base = np.full((h, w, 4), 235, np.uint8)
            base[..., 3] = 255
            base[18:24, :, :3] = (200, 30, 30)
            env = np.full((h, w, 4), 255, np.uint8)
            white = np.full((h, w, 4), 255, np.uint8)
            tex = lambda img: fx.tex0_blob(gxtex.RGBA8, w, h, fx.encode_gx(gxtex.RGBA8, img))
            brres = fx.build_brres_folders({
                "3DModels(NW4R)": [("pin_like", fx.build_mdl0(name="pin_like", texture="bwg_pin", tev=REAL["pin"],
                                                              extra_textures=("bwg_pin_env", "test_gra"),
                                                              mesh=fx.sphere(), translate=(0, 0, 0)))],
                "Textures(NW4R)": [("bwg_pin", tex(base)), ("bwg_pin_env", tex(env)), ("test_gra", tex(white))]})
            with open(os.path.join(files, "pin.brres"), "wb") as f:
                f.write(brres)
            out = os.path.join(tmp, "out")
            engine.run(os.path.join(tmp, "disc"), out, profiles.get("rg35xxh"), jobs=1, log=lambda *_: None)
            png = os.path.join(tmp, "png")
            os.makedirs(png)
            r = subprocess.run([RENDER, os.path.join(out, "assets.wlpk"), png, "--size", "128"],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertNotIn("[tev]", r.stdout)
            img = np.asarray(Image.open(os.path.join(png, "pin.brres_pin_like.png")).convert("RGB")).astype(int)
            bg = img[0, 0]
            body = np.abs(img - bg).sum(-1) > 30
            self.assertGreater(body.mean(), 0.3)                      # la esfera ocupa buena parte
            red = (img[..., 0] > 120) & (img[..., 1] < 90) & (img[..., 2] < 90)
            self.assertGreater(red.sum(), 50)                          # se ven las franjas rojas
