"""ETC2/EAC en la GPU (0.21.0): las texturas que escribe el conversor se leen en OpenGL ES igual que en el
decodificador de la CPU (choya_render --verificar, con Mesa; en la consola, wl_view --verificar con la Mali).
También: CMPR con transparencia sale en ETC2 RGB8A1 (4 bpp) con el alfa exacto."""
import os
import struct
import subprocess
import tempfile
import unittest

import numpy as np

from choya import etc, gxtex, optimize, pack, profiles
from tests import fixtures as fx

AQUI = os.path.dirname(__file__)
REPO = os.path.dirname(AQUI)


def cmpr_con_huecos(w: int, h: int, seed: int = 3, ruido: bool = False) -> bytes:
    """CMPR con sub-bloques de 3 colores + transparente (c0 <= c1) y otros de 4 colores. Sin ruido, los colores de
    cada sub-bloque siguen un degradado (como una textura de verdad); con ruido, cualquier cosa."""
    rng = np.random.default_rng(seed)
    out = bytearray()
    k = 0
    for _ in range((w // 8) * (h // 8) * 4):
        if ruido:
            a, b = sorted(int(v) for v in rng.integers(0, 0x10000, 2))
        else:
            r, g = (k * 3) % 28, (k * 5) % 56
            a = (r << 11) | (g << 5) | 12
            b = ((r + 3) << 11) | ((g + 6) << 5) | 14
        k += 1
        if rng.random() < 0.5:
            a, b = max(a, b), min(a, b)      # c0 > c1: 4 colores, opaco
        rows = [int(v) for v in rng.integers(0, 256, 4)]
        out += fx.cmpr_block_solid(a, b, rows)
    return bytes(out)


class TestRgb8A1(unittest.TestCase):
    def test_cmpr_con_transparencia(self):
        data = cmpr_con_huecos(32, 32)
        tex = gxtex.GXTexture(name="hueco", fmt=gxtex.CMPR, width=32, height=32, mips=1, data=data, source="m/hueco")
        src = gxtex.decode_mips(tex)[0]
        self.assertEqual(optimize.classify_alpha(src[..., 3]), "binary")
        conv = optimize.convert(tex, profiles.get("rg35xxh"))
        self.assertEqual(conv.fmt, optimize.OUT_ETC2_RGB8A1, conv.fallback)
        self.assertEqual(len(conv.data), (32 // 4) ** 2 * 8)       # 4 bpp
        dec = optimize.decode_out(conv.fmt, conv.data, 32, 32)
        np.testing.assert_array_equal(dec[..., 3] >= 128, src[..., 3] >= 128)
        self.assertGreater(conv.psnr, 30)

    def test_bloque_opaco_usa_la_tabla_completa(self):
        img = fx.gradient(8, 8)
        d = etc.encode_etc2_rgba1(img)
        words = np.frombuffer(d, ">u8")
        self.assertTrue(all((int(w) >> 33) & 1 for w in words))   # opaco
        np.testing.assert_array_equal(etc.decode_etc2_rgba1(d, 8, 8)[..., 3], 255)


class TestGpu(unittest.TestCase):
    def test_mesa_lee_lo_mismo(self):
        render = os.environ.get("CHOYA_RENDER") or os.path.join(REPO, "runtime", "build", "choya_render")
        if not os.path.isfile(render):
            self.skipTest("sin choya_render compilado (cmake -DWL_BUILD_RENDER=ON)")
        prof = profiles.get("rg35xxh")
        rng = np.random.default_rng(7)
        noise = rng.integers(0, 256, (64, 64, 4)).astype(np.uint8)
        smooth = fx.gradient(64, 64, "full")
        casos = [(gxtex.RGB565, smooth), (gxtex.RGB5A3, smooth), (gxtex.I8, noise), (gxtex.IA8, noise),
                 (gxtex.RGB565, noise)]
        writer = pack.PackWriter(flags=pack.FLAG_GLES3)
        formats = set()
        for i, (fmt, img) in enumerate(casos):
            tex = gxtex.GXTexture(name=f"t{i}", fmt=fmt, width=64, height=64, mips=1, data=fx.encode_gx(fmt, img),
                                  source=f"m/t{i}")
            conv = optimize.convert(tex, prof, measure=False)
            formats.add(conv.fmt)
            writer.add(pack.Entry(kind=pack.KIND_TEXTURE, fmt=conv.fmt, count=conv.mips, flags=0, key=1000 + i,
                                  name=f"t{i}", data=conv.data, width=conv.width, height=conv.height))
        tex = gxtex.GXTexture(name="cmpr", fmt=gxtex.CMPR, width=64, height=64, mips=1, data=cmpr_con_huecos(64, 64, ruido=True),
                              source="m/cmpr")
        conv = optimize.convert(tex, prof, measure=False)
        formats.add(conv.fmt)
        writer.add(pack.Entry(kind=pack.KIND_TEXTURE, fmt=conv.fmt, count=1, flags=0, key=2000, name="cmpr",
                              data=conv.data, width=64, height=64))
        self.assertTrue({optimize.OUT_ETC2_RGB8, optimize.OUT_ETC2_RGBA8, optimize.OUT_R11_EAC,
                         optimize.OUT_RG11_EAC, optimize.OUT_ETC2_RGB8A1} <= formats, formats)
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "assets.wlpk")
            writer.write(p, {"tool": "prueba"})
            res = subprocess.run([render, p, d, "--verificar"], capture_output=True, text=True,
                                 env=dict(os.environ, EGL_PLATFORM="surfaceless"))
            if "EGL no disponible" in res.stderr or "contexto GLES" in res.stderr:
                self.skipTest("sin EGL/Mesa")
            self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
            self.assertIn("6 revisadas", res.stdout)
            self.assertIn("0 con diferencias", res.stdout)
            self.assertIn("ETC2_RGB8A1", res.stdout)


if __name__ == "__main__":
    unittest.main()
