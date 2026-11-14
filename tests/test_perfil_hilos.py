"""Perfil de hilos y núcleos (0.23.4, choya_gles.cpp): cada N segundos, qué tan ocupado estuvo cada núcleo y cada hilo
(CPU, ms por cuadro del juego, núcleo y espera de turno). Se compila la parte del perfil sola, con dos hilos ocupados."""
import os
import re
import shutil
import subprocess
import tempfile
import unittest

AQUI = os.path.dirname(os.path.abspath(__file__))
GLES = os.path.join(os.path.dirname(AQUI), "choya", "data", "choya_gles.cpp")

PRELUDE = r"""#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <pthread.h>
#include <string>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <vector>
namespace {
void Log(const char* fmt, const char* a = "", const char* b = "") { std::printf(fmt, a, b); std::printf("\n"); std::fflush(stdout); }
std::atomic<int> g_tidPantalla{0};
std::atomic<int> g_tidJuego{0};
std::atomic<uint64_t> g_cuadrosJuego{0};
}
"""

MAIN = r"""
extern "C" int choya_gx_hilo_tid_gx() { return 0; }
int main() {
  g_tidJuego = static_cast<int>(syscall(SYS_gettid));
  std::thread(PerfilHilos).detach();
  std::thread([] {
    pthread_setname_np(pthread_self(), "choya-pantalla");
    g_tidPantalla = static_cast<int>(syscall(SYS_gettid));
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(2600);
    volatile double x = 0;
    while (std::chrono::steady_clock::now() < end) x = x + 1;
  }).detach();
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(2600);
  while (std::chrono::steady_clock::now() < end) {
    g_cuadrosJuego.fetch_add(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return 0;
}
"""


@unittest.skipUnless(shutil.which("g++") and os.path.isdir("/proc/self/task"), "sin g++ o sin /proc")
class PerfilHilosTest(unittest.TestCase):
    def test_informe(self):
        with open(GLES, encoding="utf-8") as f:
            src = f.read()
        body = src[src.index("// --- Perfil de hilos y núcleos (0.23.4)"):src.index("// El motor pide dibujar sin")]
        with tempfile.TemporaryDirectory() as d:
            cpp, exe = os.path.join(d, "t.cpp"), os.path.join(d, "t")
            with open(cpp, "w", encoding="utf-8") as f:
                f.write(PRELUDE + body + MAIN)
            subprocess.run(["g++", "-std=c++20", "-O1", "-Wall", "-Werror", cpp, "-o", exe, "-lpthread"], check=True,
                           capture_output=True)
            out = subprocess.run([exe], env=dict(os.environ, CHOYA_HILOS_CADA="1"), capture_output=True, text=True,
                                 timeout=30).stdout
        self.assertRegex(out, r"hilos \(1 s, juego a [\d.]+ cuadros/s\): núcleos ocupados 0: \d+ %")
        m = re.search(r"pantalla (\d+) % CPU \(([\d.]+) ms por cuadro\) en el núcleo \d+, esperó turno \d+ %", out)
        self.assertIsNotNone(m, out)
        self.assertGreater(int(m.group(1)), 50)  # el hilo de la pantalla gira todo el tiempo
        self.assertIn("hilos: juego ", out)
        self.assertIn("hilos: otros:", out)


if __name__ == "__main__":
    unittest.main()
