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
            shutil.copytree(os.path.join(AQUI, "motor_gles"), d, dirs_exist_ok=True)
            os.makedirs(os.path.join(d, "lib", "choya"))
            for name in ("choya_gx.hpp", "choya_gx.cpp"):
                shutil.copy(os.path.join(REPO, "choya", "data", "aurora", name), os.path.join(d, "lib", "choya"))
            os.makedirs(os.path.join(d, "test"))
            shutil.move(os.path.join(d, "harness.cpp"), os.path.join(d, "test", "harness.cpp"))
            exe = os.path.join(d, "harness")
            subprocess.run(["clang++", "-std=c++20", "-DTARGET_PC", "-I", os.path.join(d, "inc"), "-I", AURORA_INCLUDE,
                            os.path.join(d, "test", "harness.cpp"), os.path.join(d, "lib", "choya", "choya_gx.cpp"),
                            "-o", exe, "-ldl"], check=True, capture_output=True)
            env = dict(os.environ, CHOYA_GLES="1", EGL_PLATFORM="surfaceless")
            out = subprocess.run([exe], env=env, capture_output=True, text=True)
            if out.returncode == 2:
                self.skipTest("Mesa no dio contexto GLES 3.2: " + out.stdout.strip())
            self.assertIn("PRUEBA OK", out.stdout, out.stdout + out.stderr)


if __name__ == "__main__":
    unittest.main()
