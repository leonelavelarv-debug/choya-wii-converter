"""Prueba del motor GLES de Choya (paso 2a) en la PC: compila choya_gx.cpp contra cabeceras mínimas de aurora
(tests/motor_gles/lib) y dibuja cuadros sintéticos con Mesa (llvmpipe, GLES 3.2) leyendo los píxeles.
Se salta si no hay clang++, Mesa EGL o las cabeceras GX de aurora (BrawlRecompiled)."""
import ctypes.util
import os
import shutil
import subprocess
import tempfile
import unittest

AQUI = os.path.dirname(__file__)
REPO = os.path.dirname(AQUI)
AURORA_INCLUDE = os.environ.get("CHOYA_AURORA_INCLUDE", os.path.expanduser(
    "~/BrawlRecompiled/wiicompiled/aurora-main/include"))
if not os.path.isdir(AURORA_INCLUDE):
    AURORA_INCLUDE = "/home/claude/ext/brawlrecomp/wiicompiled/aurora-main/include"


class TestMotorGles(unittest.TestCase):
    def test_cuadro_sintetico(self):
        if not shutil.which("clang++") or not os.path.isdir(AURORA_INCLUDE):
            self.skipTest("sin clang++ o sin cabeceras de aurora")
        if not ctypes.util.find_library("EGL") or not ctypes.util.find_library("GLESv2"):
            self.skipTest("sin Mesa EGL/GLES")
        with tempfile.TemporaryDirectory() as d:
            exe = os.path.join(d, "harness")
            data = os.path.join(REPO, "choya", "data", "aurora")
            subprocess.run(["clang++", "-std=c++20", "-I", data, "-I", AURORA_INCLUDE,
                            os.path.join(AQUI, "motor_gles", "harness.cpp"), os.path.join(data, "choya_gx_core.cpp"),
                            "-o", exe, "-ldl"], check=True, capture_output=True)
            dump = os.path.join(d, "cuadro.bin")
            env = dict(os.environ, EGL_PLATFORM="surfaceless", CHOYA_PRUEBA_VOLCADO=dump)
            out = subprocess.run([exe], env=env, capture_output=True, text=True)
            if out.returncode == 2:
                self.skipTest("Mesa no dio contexto GLES 3.2: " + out.stdout.strip())
            self.assertIn("PRUEBA OK", out.stdout, out.stdout + out.stderr)
            # El volcado se vuelve a dibujar igual (rojo, verde, fondo azul; el segundo pase no se ve).
            ppm = os.path.join(d, "cuadro.ppm")
            out = subprocess.run([exe, dump, ppm], env=env, capture_output=True, text=True)
            self.assertIn("listo", out.stdout, out.stdout + out.stderr)
            with open(ppm, "rb") as f:
                raw = f.read()
            pix = raw[raw.index(b"255\n") + 4:]
            at = lambda x, y: tuple(pix[(y * 640 + x) * 3:(y * 640 + x) * 3 + 3])
            self.assertEqual((at(160, 240), at(480, 240), at(320, 10)), ((255, 0, 0), (0, 255, 0), (0, 0, 255)))


if __name__ == "__main__":
    unittest.main()
