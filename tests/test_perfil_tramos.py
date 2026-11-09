"""Medición por tramos (choya_perfil.cpp + choya perfil): un "juego" de prueba con el mismo esquema que Wii Sports
(EGG::SceneManager::calc y ::draw como funciones traducidas func_800A4C2C y func_800A4C78) que actualiza 6 ms,
dibuja 3 ms y duerme 4 ms por cuadro. La medición debe repartir el cuadro así, contar las llamadas y decir qué
llamada duerme, sin acortar el sleep (el hilo dormido no se interrumpe)."""
import os
import re
import shutil
import subprocess
import tempfile
import unittest

AQUI = os.path.dirname(__file__)
REPO = os.path.dirname(AQUI)

JUEGO = r"""
#include <chrono>
#include <cstdint>
#include <thread>
#include <unistd.h>
extern "C" volatile bool g_choyaMidiendo;
extern "C" void choya_medir_llamada(uint32_t, const void*);
extern "C" void choya_medir_empezar(int);
static uint32_t ctx[64];
__attribute__((noinline)) static void busy(double ms) {
  const auto t = std::chrono::steady_clock::now();
  while (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count() < ms) {}
}
extern "C" __attribute__((noinline)) void func_80100000() { busy(2.0); }
extern "C" __attribute__((noinline)) void func_800A4C2C() {
  for (int i = 0; i < 3; ++i) { if (g_choyaMidiendo) choya_medir_llamada(0x80100000u, ctx); func_80100000(); }
}
extern "C" __attribute__((noinline)) void func_800A4C78() { busy(3.0); }
int main() {
  std::thread([] {
    for (;;) {
      if (g_choyaMidiendo) choya_medir_llamada(0x800A4C2Cu, ctx);
      func_800A4C2C();
      if (g_choyaMidiendo) choya_medir_llamada(0x800A4C78u, ctx);
      func_800A4C78();
      usleep(4000);
    }
  }).detach();
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  choya_medir_empezar(2);
  std::this_thread::sleep_for(std::chrono::milliseconds(2700));
  return 0;
}
"""


class TestPerfilTramos(unittest.TestCase):
    def test_tramos(self):
        cxx = shutil.which("clang++") or shutil.which("g++")
        if not cxx or not (shutil.which("llvm-symbolizer") or shutil.which("llvm-symbolizer-18")):
            self.skipTest("sin compilador o sin llvm-symbolizer")
        from choya import perfil, retarget
        ws = retarget.load_wiisports_symbols()
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(REPO, "choya", "data", "choya_perfil.cpp"), encoding="utf-8") as f:
                src = retarget.perfil_marcas(f.read(), ws)
            self.assertIn("0x800A4C2Cu", src)
            with open(os.path.join(d, "perfil.cpp"), "w", encoding="utf-8") as f:
                f.write(src)
            with open(os.path.join(d, "juego.cpp"), "w", encoding="utf-8") as f:
                f.write(JUEGO)
            exe = os.path.join(d, "juego")
            subprocess.run([cxx, "-std=c++20", "-O1", "-g", "-fno-omit-frame-pointer", "-fno-optimize-sibling-calls",
                            os.path.join(d, "juego.cpp"), os.path.join(d, "perfil.cpp"), "-o", exe, "-lpthread"],
                           check=True, capture_output=True)
            out = subprocess.run([exe], cwd=d, capture_output=True, text=True, timeout=30)
            self.assertIn("perfil escrito", out.stderr)
            p = perfil.leer(os.path.join(d, "perfil.txt"))
            self.assertGreater(p.cuadros, 60)
            self.assertGreater(p.pared, 1500)
            names = perfil.nombrar(exe, perfil.desplazamientos(p))
            text = perfil.informe(p, names, top=8)
            ms = {k: float(re.search(r"([\d.]+) ms\s+[\d.]+%\s+" + k, text).group(1))
                  for k in ("Actualizar", "Preparar", "Esperar")}
            self.assertTrue(4.5 <= ms["Actualizar"] <= 8.0, text)
            self.assertTrue(2.0 <= ms["Preparar"] <= 4.5, text)
            self.assertTrue(3.0 <= ms["Esperar"] <= 6.0, text)  # el usleep(4000) no se acorta al medir
            self.assertRegex(text, r"3\.0×\s+\S+")                # 3 llamadas por cuadro a la "física"
            self.assertRegex(text, r"nanosleep ← .*main")       # quién duerme


if __name__ == "__main__":
    unittest.main()
