"""Copias del EFB con el motor de Choya (0.21.1): un DCFlushRange sobre el destino de una copia no la retira si la
RAM del juego no cambió (las caras del público en Tenis salían como cuadros blancos)."""
import os
import shutil
import subprocess
import tempfile
import unittest

from choya import retarget as R

RUNTIME = "/home/claude/ext/brawlrecomp/wiicompiled/runtime"
AURORA = "/home/claude/ext/brawlrecomp/wiicompiled/aurora-main"

PRELUDE = r"""
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>
static uint8_t g_ram[0x400000];
static std::vector<uint32_t> g_destroyed;
static bool g_soloGpu = true;
void* GuestToHostPtr(uint32_t addr, size_t = 0) { return g_ram + addr; }
uint32_t CanonicalizeGxMainRamAddress(uint32_t addr) { return addr; }
void GXDestroyCopyTex(void* p) { g_destroyed.push_back(uint32_t(static_cast<uint8_t*>(p) - g_ram)); }
bool aurora_flush_efb_copy_to_ram(void*) { return false; }
extern "C" bool choya_gx_copias_solo_gpu() { return g_soloGpu; }
extern "C" void choya_gx_log(const char* line) { std::printf("log: %s\n", line); }
"""

MAIN = r"""
int main(int argc, char**) {
  g_soloGpu = argc == 1;
  for (size_t i = 0; i < sizeof(g_ram); ++i) g_ram[i] = uint8_t(i * 7);
  RememberEfbCopyDestination(0x1000, 0x4000);         // icono de 64x64 RGBA8
  RememberEfbCopyDestination(0x100000, 640 * 480 * 4); // copia grande (huella por muestras)
  InvalidateEfbCopyDestinationsForRange(0x1000, 0x4000);   // CpuTexture::flush, sin escribir
  std::printf("tras flush: %zu retiradas\n", g_destroyed.size());
  g_ram[0x1000 + 0x2000] ^= 0xFF;                      // la CPU escribe en el destino
  InvalidateEfbCopyDestinationsForRange(0x1000, 0x4000);
  std::printf("tras escribir: %zu retiradas\n", g_destroyed.size());
  InvalidateEfbCopyDestinationsForRange(0x100000, 64);  // sin escribir
  std::printf("grande sin escribir: %zu retiradas\n", g_destroyed.size());
  g_ram[0x100000 + 640 * 480 * 4 - 1] ^= 1;             // la última palabra siempre entra en la huella
  InvalidateEfbCopyDestinationsForRange(0x100000, 64);
  std::printf("grande escrita: %zu retiradas\n", g_destroyed.size());
  return 0;
}
"""


def _extract(text: str) -> str:
    start = text.index("namespace {\n")
    fn = text.index("void InvalidateEfbCopyDestinationsForRange(uint32_t addr, uint32_t size) {")
    end = text.index("\n}\n", fn) + 3
    return text[start:end]


@unittest.skipUnless(os.path.isfile(os.path.join(RUNTIME, "src/hle/gx/gx_copy.cpp")) and shutil.which("clang++"),
                     "sin WiiCompiled o sin clang++")
class TestCopiasEfb(unittest.TestCase):
    def _run(self, *args):
        with open(os.path.join(RUNTIME, "src/hle/gx/gx_copy.cpp"), encoding="utf-8") as f:
            src = f.read()
        changes = []
        out = R.apply_edits(src, "src/hle/gx/gx_copy.cpp", changes)
        self.assertEqual([c.kind for c in changes], ["edicion"] * 4)
        with tempfile.TemporaryDirectory() as d:
            cpp = os.path.join(d, "t.cpp")
            exe = os.path.join(d, "t")
            with open(cpp, "w", encoding="utf-8") as f:
                f.write(PRELUDE + _extract(out) + MAIN)
            subprocess.run(["clang++", "-std=c++20", "-O1", "-Wall", "-Werror", "-Wno-unused-function",
                            "-o", exe, cpp], check=True)
            return subprocess.run([exe, *args], check=True, capture_output=True, text=True).stdout

    def test_conserva_si_la_ram_no_cambio(self):
        out = self._run()
        self.assertIn("tras flush: 0 retiradas", out)
        self.assertIn("tras escribir: 1 retiradas", out)
        self.assertIn("grande sin escribir: 1 retiradas", out)
        self.assertIn("grande escrita: 2 retiradas", out)
        self.assertIn("motor: copias del EFB tras DCFlush/DCInvalidate: 1 conservadas", out)

    def test_sin_motor_como_antes(self):
        out = self._run("sin-motor")
        self.assertIn("tras flush: 1 retiradas", out)
        self.assertNotIn("log:", out)

    def test_aurora_no_apunta_copias_grandes(self):
        if not os.path.isfile(os.path.join(AURORA, "lib/dolphin/gx/GXFrameBuffer.cpp")):
            self.skipTest("sin aurora")
        with tempfile.TemporaryDirectory() as d:
            rel = "lib/dolphin/gx/GXFrameBuffer.cpp"
            os.makedirs(os.path.join(d, "aurora-main", os.path.dirname(rel)))
            shutil.copy(os.path.join(AURORA, rel), os.path.join(d, "aurora-main", rel))
            saved, saved_ex = R.AURORA_EDITS, R.AURORA_EXCLUSIVOS
            R.AURORA_EXCLUSIVOS = []
            try:
                R.AURORA_EDITS = [e for e in saved if e[0] == rel]
                self.assertEqual(len(R.AURORA_EDITS), 2)
                changes = []
                R.patch_aurora(d, changes)
            finally:
                R.AURORA_EDITS, R.AURORA_EXCLUSIVOS = saved, saved_ex
            self.assertNotIn("parche_fallido", [c.kind for c in changes])
            text = open(os.path.join(d, "aurora-main", rel), encoding="utf-8").read()
            self.assertIn("handle.dataSize <= 256", text)
            self.assertIn('#include "../../choya/choya_gx.hpp"', text)


if __name__ == "__main__":
    unittest.main()
