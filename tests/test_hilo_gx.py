"""Hilo GX de Choya (0.23.0): el procesador de comandos de aurora en otro núcleo. Las ediciones se aplican a aurora y
al runtime de referencia (y otra vez sin cambiar nada); el programa de la prueba larga (test_sin_dawn) dibuja igual con
el hilo GX encendido y apagado."""
import hashlib
import os
import shutil
import subprocess
import tempfile
import unittest

from choya import retarget as R

AURORA = "/home/claude/ext/brawlrecomp/wiicompiled/aurora-main"
RUNTIME = "/home/claude/ext/brawlrecomp/wiicompiled/runtime"
DATA = os.path.join(os.path.dirname(R.__file__), "data", "aurora")
LANZADOR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "scripts", "knulli",
                        "Wii Sports (Choya).sh")


def _huellas(raiz):
    out = {}
    for d, _sub, files in os.walk(raiz):
        for n in files:
            p = os.path.join(d, n)
            with open(p, "rb") as f:
                out[p] = hashlib.md5(f.read()).hexdigest()
    return out


class ExclusivosTest(unittest.TestCase):
    def test_inserta_al_principio(self):
        text = ('#include "a.h"\n#include "b.h"\n\n'
                "void Uno(int a, int b) {\n  hacer(a);\n}\n"
                "bool Dos() { return otra(); }\n"
                "void Tres(int x);\n")
        out, res = R.aplicar_exclusivos(text, "x.cpp", "c/hilo.hpp", ["Uno", "Dos", "Tres"])
        self.assertIn('#include "a.h"\n#include "c/hilo.hpp"  // choya: hilo GX\n', out)
        self.assertIn("void Uno(int a, int b) {\n  CHOYA_HILO_EXCLUSIVO();  // choya: hilo GX\n  hacer(a);", out)
        self.assertIn("bool Dos() { CHOYA_HILO_EXCLUSIVO(); return otra(); }", out)
        self.assertEqual([r[2] for r in res], ["edicion", "edicion", "no encontré"])  # Tres solo se declara
        again, res2 = R.aplicar_exclusivos(out, "x.cpp", "c/hilo.hpp", ["Uno", "Dos"])
        self.assertEqual(again, out)
        self.assertEqual([r[2] for r in res2], ["ya estaba", "ya estaba"])


@unittest.skipUnless(os.path.isdir(AURORA), "falta aurora de referencia")
class AuroraTest(unittest.TestCase):
    def test_aurora_se_ajusta_una_vez(self):
        with tempfile.TemporaryDirectory() as d:
            shutil.copytree(AURORA, os.path.join(d, "aurora-main"))
            changes = []
            R.patch_aurora(d, changes)
            self.assertEqual([c for c in changes if c.kind == "parche_fallido"], [])
            antes = _huellas(d)
            changes = []
            R.patch_aurora(d, changes)  # otra vez: nada cambia (ni cabeceras repetidas)
            self.assertEqual([c for c in changes if c.kind == "parche_fallido"], [])
            self.assertEqual(antes, _huellas(d))

            def leer(rel):
                with open(os.path.join(d, "aurora-main", rel), encoding="utf-8") as f:
                    return f.read()

            fifo = leer("lib/gx/fifo.cpp")
            self.assertIn("::choya::hilo::fifo_listo();", fifo)
            listas = leer("lib/dolphin/gx/GXDispList.cpp")
            self.assertEqual(listas.count("::choya::hilo::lista(data, nbytes"), 2)
            self.assertIn('extern "C" void choya_gx_procesar_lista(', listas)
            cp = leer("lib/gx/command_processor.cpp")
            self.assertIn("::choya::hilo::bp(reg, value);", cp)
            self.assertIn('extern "C" void choya_gx_aplicar_bp(', cp)
            for rel, nombre in [("lib/dolphin/gx/GXFrameBuffer.cpp", "void GXCopyTex(void* dest, GXBool clear) {"),
                                ("lib/dolphin/gx/GXManage.cpp", "void GXDrawDone() {"),
                                ("lib/gx/command_processor.cpp", "bool submit_raw_draw("),
                                ("lib/aurora.cpp", "bool aurora_begin_frame() {")]:
                texto = leer(rel)
                i = texto.index(nombre)
                self.assertIn("CHOYA_HILO_EXCLUSIVO();", texto[i:texto.index("\n", texto.index("{", i)) + 60], rel)
            au = leer("lib/aurora.cpp")
            self.assertIn("callback != nullptr && !::choya::hilo::en_hilo()", au)
            self.assertIn("::choya::hilo::cuadro();", au)
            self.assertIn("::choya::hilo::detener();", au)
            self.assertEqual(au.count('#include "choya/choya_hilo.hpp"'), 1)
            geo = leer("lib/dolphin/gx/GXGeometry.cpp")
            self.assertIn("::choya::hilo::diferir([attr, type]", geo)
            tex = leer("lib/dolphin/gx/GXTexture.cpp")  # 0.23.1: con el reemplazo de texturas encendido
            self.assertIn("texture_replacement::register_tlut(c.obj,", tex)
            self.assertIn("::choya::hilo::diferir([obj_, idx]", tex)
            with open(os.path.join(DATA, "choya_hilo.cpp"), encoding="utf-8") as f:
                self.assertNotIn("allowTextureReplacements", f.read())
            self.assertIn("lib/choya/choya_hilo.cpp", leer("cmake/aurora_gx.cmake"))
            self.assertTrue(os.path.isfile(os.path.join(d, "aurora-main", "lib/choya/choya_hilo.cpp")))

    def test_hilo_compila(self):
        """choya_hilo.cpp con las cabeceras de aurora (sin enlazar): la sintaxis y los tipos."""
        if shutil.which("g++") is None:
            self.skipTest("sin g++")
        with tempfile.TemporaryDirectory() as d:
            shutil.copytree(AURORA, os.path.join(d, "aurora-main"))
            R.patch_aurora(d, [])
            base = os.path.join(d, "aurora-main")
            deps = os.environ.get("CHOYA_DEPS", "/tmp/deps")
            inc = ["-I" + os.path.join(base, "include"), "-I" + os.path.join(base, "lib")]
            for sub in ("fmt/include", "xxhash", "abseil-cpp", "sdl/include", "tracy/public"):
                if os.path.isdir(os.path.join(deps, sub)):
                    inc.append("-I" + os.path.join(deps, sub))
            sin_dawn = os.path.join(base, "lib/choya/sin_dawn/include")
            out = subprocess.run(["g++", "-std=gnu++20", "-fsyntax-only", "-DTARGET_PC", "-DAURORA",
                                  "-DAURORA_ENABLE_GX", "-DWEBGPU_DAWN", *inc, "-I" + sin_dawn,
                                  os.path.join(base, "lib/choya/choya_hilo.cpp")], capture_output=True, text=True)
            if out.returncode != 0 and "No such file" in out.stderr:
                self.skipTest("faltan cabeceras de las dependencias de aurora: " + out.stderr[:200])
            self.assertEqual(out.returncode, 0, out.stderr[-3000:])


@unittest.skipUnless(os.path.isdir(RUNTIME), "falta el runtime de referencia")
class RuntimeTest(unittest.TestCase):
    def test_ediciones_del_runtime(self):
        for rel, marca in [("src/hle/gx/gx_stubs.cpp", "choya_gx_hilo_esperar();"),
                           ("src/hle/gx/gx_guest_write_hooks.cpp", "choya_gx_hilo_memoria_juego(i, ")]:
            with open(os.path.join(RUNTIME, rel), encoding="utf-8") as f:
                text = f.read()
            changes = []
            out = R.apply_edits(text, rel, changes)
            self.assertEqual([c for c in changes if c.kind == "parche_fallido"], [], rel)
            self.assertIn(marca, out)
            self.assertIn("__attribute__((weak))", out)


class LanzadorTest(unittest.TestCase):
    def test_lanzador(self):
        with open(LANZADOR, encoding="utf-8") as f:
            text = f.read()
        self.assertIn('if [ -f "$DIR/sin-hilo-gx" ]; then export CHOYA_GX_HILO=0', text)
        self.assertIn("aurora: hilo GX", text)
        self.assertIn('"$DIR/run.log" "$DIR/choya-gles.txt"', text[text.index("aurora: hilo GX (de Choya|apagado)") - 80:][:200])

    def test_archivos(self):
        for n in ("choya_hilo.hpp", "choya_hilo.cpp"):
            self.assertIn((n, "lib/choya/" + n), R.AURORA_FILES)
            self.assertTrue(os.path.isfile(os.path.join(DATA, n)))


if __name__ == "__main__":
    unittest.main()
