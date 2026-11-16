"""ETC2/EAC completo (0.42.0): el decodificador de la CPU cubre los modos T, H y planar y el alfa de 1 bit en ellos,
revisa todos los niveles de mipmap y tamaños que no son múltiplo de 4, y la ruta de respaldo (descomprimir y subir sin
comprimir) da lo mismo que la nativa.

La referencia independiente es el decodificador ETC2 de Mesa (llvmpipe): choya_render --verificar compara cada nivel
como lo lee la GPU con etc_decode.cpp. Sin choya_render compilado (scripts/build-pc-render.sh) o sin EGL, las pruebas
de GPU se omiten; las de Python corren siempre. En la consola la misma prueba es `wl_view --verificar` con el paquete
de `choya prueba-etc` (pendiente de confirmar en la RG35XX H)."""
import os
import subprocess
import tempfile
import unittest

import numpy as np

from choya import optimize, pack, prueba_etc

AQUI = os.path.dirname(__file__)
REPO = os.path.dirname(AQUI)


def render_bin():
    for cand in (os.environ.get("CHOYA_RENDER"), os.path.join(REPO, "runtime", "build-render", "choya_render"),
                 os.path.join(REPO, "runtime", "build", "choya_render"),
                 os.path.join(REPO, "runtime", "build-pc", "choya_render")):
        if cand and os.path.isfile(cand):
            return cand
    return None


def verificar(path, env=None):
    r = subprocess.run([render_bin(), path, os.path.dirname(path), "--verificar"], capture_output=True, text=True,
                       env=dict(os.environ, EGL_PLATFORM="surfaceless", **(env or {})))
    if "EGL no disponible" in r.stderr or "contexto GLES" in r.stderr:
        raise unittest.SkipTest("sin EGL/Mesa")
    return r


class TestModos(unittest.TestCase):
    def test_bloques_al_azar_cubren_todos_los_modos(self):
        with tempfile.TemporaryDirectory() as d:
            s = prueba_etc.build(os.path.join(d, "a.wlpk"))
        for fmt in ("rgb8", "rgba8"):
            for m in ("individual", "diferencial", "T", "H", "planar"):
                self.assertGreater(s["modos"].get(f"{fmt}:{m}", 0), 0, (fmt, m, s["modos"]))
        for m in ("diferencial", "T", "H", "planar"):   # RGB8A1 no tiene modo individual
            self.assertGreater(s["modos"].get(f"rgb8a1:{m}", 0), 0, m)
        self.assertNotIn("rgb8a1:individual", s["modos"])

    def test_modo_por_bits(self):
        # R base 31 + dR +1 desborda: T. G desborda: H. B desborda: planar.
        diff = 1 << 33
        self.assertEqual(prueba_etc.mode_of((31 << 59) | (1 << 56) | diff), "T")
        self.assertEqual(prueba_etc.mode_of((31 << 51) | (1 << 48) | diff), "H")
        self.assertEqual(prueba_etc.mode_of((31 << 43) | (1 << 40) | diff), "planar")
        self.assertEqual(prueba_etc.mode_of(diff), "diferencial")
        self.assertEqual(prueba_etc.mode_of(0), "individual")
        self.assertEqual(prueba_etc.mode_of(0, punch=True), "diferencial")

    def test_tamanos_y_mipmaps(self):
        self.assertEqual(prueba_etc._levels(13, 7), [(13, 7), (6, 3), (3, 1), (1, 1)])
        self.assertEqual(prueba_etc._levels(1, 1), [(1, 1)])


@unittest.skipUnless(render_bin(), "sin choya_render compilado (scripts/build-pc-render.sh)")
class TestGpuMesa(unittest.TestCase):
    def test_todos_los_niveles_y_las_dos_rutas(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "assets.wlpk")
            s = prueba_etc.build(p)
            r = verificar(p)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("VERIFICACION OK", r.stdout)
            self.assertIn(f"niveles revisados por la ruta nativa: {s['niveles']}", r.stdout)
            self.assertIn(f"ruta de respaldo: {s['texturas']} revisadas, 0 con diferencias", r.stdout)
            self.assertIn("0 con descarte distinto", r.stdout)
            for name in ("GL_VERSION:", "GL_SHADING_LANGUAGE_VERSION:", "GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2",
                         "ruta para ETC2/EAC: nativa"):
                self.assertIn(name, r.stdout)

    def test_forzar_respaldo(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "assets.wlpk")
            prueba_etc.build(p)
            r = verificar(p, {"WL_SIN_ETC2": "1"})
            self.assertIn("WL_SIN_ETC2=1", r.stdout)
            self.assertIn("ruta para ETC2/EAC: respaldo", r.stdout)
            # la verificación revisa igual las dos rutas
            self.assertIn("VERIFICACION OK", r.stdout)

    def test_cadena_cortada_y_datos_cortos(self):
        """Una entrada con menos datos que los mipmaps que dice tener se revisa hasta el último nivel completo; una
        más corta que el nivel 0 se salta sin leer fuera del paquete."""
        rng = np.random.default_rng(1)
        w = pack.PackWriter(flags=pack.FLAG_GLES3)
        full = rng.integers(0, 256, 4 * 4 * 8 + 2 * 2 * 8, dtype=np.uint8).tobytes()   # 16x16 y 8x8 de 5 niveles
        w.add(pack.Entry(kind=pack.KIND_TEXTURE, fmt=optimize.OUT_ETC2_RGB8, count=5, flags=0, key=1, name="cortada",
                         data=full, width=16, height=16))
        w.add(pack.Entry(kind=pack.KIND_TEXTURE, fmt=optimize.OUT_ETC2_RGBA8, count=1, flags=0, key=2, name="corta",
                         data=b"\x00" * 10, width=8, height=8))
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "assets.wlpk")
            w.write(p, {})
            r = verificar(p)
            self.assertIn("niveles revisados por la ruta nativa: 2", r.stdout)
            self.assertIn("1 sin revisar", r.stdout)
            self.assertIn("VERIFICACION OK", r.stdout)


if __name__ == "__main__":
    unittest.main()
