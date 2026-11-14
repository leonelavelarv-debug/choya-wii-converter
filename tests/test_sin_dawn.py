"""Sin Dawn (0.22.0): el WebGPU nulo de Choya (choya/data/aurora/sin_dawn) reemplaza a Dawn en aurora."""
import importlib.util
import os
import shutil
import subprocess
import tempfile
import unittest

from choya import retarget as R

AQUI = os.path.dirname(os.path.abspath(__file__))
RAIZ = os.path.join(AQUI, "..")
SIN_DAWN = os.path.join(RAIZ, "choya", "data", "aurora", "sin_dawn")
AURORA = "/home/claude/ext/brawlrecomp/wiicompiled/aurora-main"


def _generador():
    spec = importlib.util.spec_from_file_location("gen", os.path.join(RAIZ, "scripts", "generar-webgpu-nulo.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class TestWebgpuNulo(unittest.TestCase):
    def test_generado_al_dia(self):
        g = _generador()
        with open(g.CABECERA, encoding="utf-8") as f:
            cabecera = f.read()
        with open(g.A_MANO, encoding="utf-8") as f:
            mano = f.read()
        tipos, resto, _ = g.generar(cabecera, mano)
        with open(os.path.join(SIN_DAWN, "webgpu_nulo_tipos.inc"), encoding="utf-8") as f:
            self.assertEqual(f.read(), tipos)
        with open(os.path.join(SIN_DAWN, "webgpu_nulo_resto.inc"), encoding="utf-8") as f:
            self.assertEqual(f.read(), resto)
        # Toda la API C: lo escrito a mano más lo generado.
        exportadas = {n for _r, n, _p in g.funciones(cabecera)}
        self.assertGreater(len(exportadas), 250)
        for nombre in exportadas:
            self.assertTrue(f" {nombre}(" in resto or f"{nombre}(" in mano, nombre)

    @unittest.skipUnless(shutil.which("g++"), "sin g++")
    def test_api_cpp_como_aurora(self):
        with tempfile.TemporaryDirectory() as d:
            exe = os.path.join(d, "prueba_nulo")
            flags = ["-fsanitize=address,undefined"] if os.path.exists("/usr/lib/x86_64-linux-gnu/libasan.so.8") else []
            subprocess.run(["g++", "-std=c++20", "-O1", "-Wall", "-Werror", "-Wno-unused-parameter", *flags,
                            "-I", os.path.join(SIN_DAWN, "include"), os.path.join(AQUI, "sin_dawn", "prueba_nulo.cpp"),
                            os.path.join(SIN_DAWN, "webgpu_nulo.cpp"), "-o", exe], check=True)
            out = subprocess.run([exe], capture_output=True, text=True)
            self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
            self.assertIn("webgpu nulo: bien", out.stdout)

    def test_archivos_en_aurora(self):
        destinos = {dst for _src, dst in R.AURORA_FILES}
        for rel in ("lib/choya/sin_dawn/webgpu_nulo.cpp", "lib/choya/sin_dawn/include/webgpu/webgpu_cpp.h",
                    "lib/choya/sin_dawn/include/dawn/webgpu.h", "lib/choya/sin_dawn/include/dawn/native/DawnNative.h"):
            self.assertIn(rel, destinos)

    @unittest.skipUnless(os.path.isdir(AURORA), "sin aurora")
    def test_proveedor_choya(self):
        with tempfile.TemporaryDirectory() as d:
            for rel in R.archivos_aurora():
                os.makedirs(os.path.dirname(os.path.join(d, "aurora-main", rel)), exist_ok=True)
                shutil.copy(os.path.join(AURORA, rel), os.path.join(d, "aurora-main", rel))
            changes = []
            R.patch_aurora(d, changes)
            self.assertNotIn("parche_fallido", [c.kind for c in changes])
            cmake = open(os.path.join(d, "aurora-main/cmake/AuroraDawnProvider.cmake"), encoding="utf-8").read()
            self.assertIn('if (_aurora_dawn_provider STREQUAL "choya")', cmake)
            self.assertIn('elseif (_aurora_dawn_provider STREQUAL "vendor")', cmake)
            aurora = open(os.path.join(d, "aurora-main/lib/aurora.cpp"), encoding="utf-8").read()
            self.assertEqual(aurora.count("if (choya_gx_sin_gpu()) {"), 1)
            self.assertNotIn('#include "choya/choya_gx.hpp"', aurora)  # ahí `choya::` es ::choya::gx
            pipeline = open(os.path.join(d, "aurora-main/lib/gx/pipeline.cpp"), encoding="utf-8").read()
            self.assertIn("if (choya::sin_gpu())", pipeline)
        with open(os.path.join(RAIZ, "scripts", "build-wiisports-arm64.sh"), encoding="utf-8") as f:
            self.assertIn("-DAURORA_DAWN_PROVIDER=choya", f.read())

    @unittest.skipUnless(os.environ.get("CHOYA_DEPS") and os.path.isdir(AURORA) and shutil.which("cmake")
                         and shutil.which("ninja"), "prueba larga: CHOYA_DEPS=carpeta con las dependencias de aurora")
    def test_aurora_de_punta_a_punta(self):
        """aurora sin Dawn + motor de Choya en Mesa: triángulo, copia del EFB y textura (varios minutos)."""
        base = os.environ.get("CHOYA_SIN_DAWN_DIR") or tempfile.mkdtemp(prefix="choya-sin-dawn-")
        if not os.path.isdir(os.path.join(base, "aurora-main")):
            shutil.copytree(AURORA, os.path.join(base, "aurora-main"))
        changes = []
        R.patch_aurora(base, changes)
        self.assertNotIn("parche_fallido", [c.kind for c in changes])
        build = os.path.join(base, "build")
        subprocess.run(["cmake", "-S", os.path.join(AQUI, "sin_dawn"), "-B", build, "-G", "Ninja",
                        "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++",
                        f"-DCHOYA_DEPS={os.environ['CHOYA_DEPS']}", f"-DCHOYA_AURORA={base}",
                        "-DAURORA_DAWN_PROVIDER=choya", "-DAURORA_SDL3_PROVIDER=vendor", "-DSDL_X11=OFF",
                        "-DSDL_WAYLAND=OFF", "-DSDL_KMSDRM=OFF", "-DSDL_UNIX_CONSOLE_BUILD=ON"],
                       check=True, capture_output=True)
        subprocess.run(["cmake", "--build", build, "--target", "prueba_aurora", "banco_gx"], check=True,
                       capture_output=True)
        env = dict(os.environ, EGL_PLATFORM="surfaceless", SDL_VIDEODRIVER="offscreen")
        # Con y sin el hilo GX de Choya (0.23.0): los mismos píxeles.
        for hilo in ("0", "1"):
            for extra in ({}, {"PRUEBA_SOLO_TRIANGULO": "1"}):
                out = subprocess.run([os.path.join(build, "prueba_aurora")], cwd=build,
                                     env=dict(env, CHOYA_GX_HILO=hilo, **extra), capture_output=True, text=True,
                                     timeout=300)
                self.assertEqual(out.returncode, 0, out.stdout[-3000:] + out.stderr[-2000:])
                self.assertIn("sin Dawn: bien", out.stdout)
                self.assertIn("aurora: sin Dawn (WebGPU nulo de Choya)", out.stdout)
                self.assertEqual("hilo GX de Choya" in out.stdout, hilo == "1")
            out = subprocess.run([os.path.join(build, "banco_gx"), "40"], cwd=build,
                                 env=dict(env, CHOYA_GLES="1", CHOYA_GX_HILO=hilo, CHOYA_GX_HILO_INFORME="20"),
                                 capture_output=True, text=True, timeout=600)
            self.assertEqual(out.returncode, 0, out.stdout[-3000:] + out.stderr[-2000:])
            self.assertIn("dibujos por cuadro: 6000", out.stdout)
            if hilo == "1":
                self.assertIn("aurora: hilo GX: 20 cuadros", out.stdout + out.stderr)


if __name__ == "__main__":
    unittest.main()
