"""Texturas con paleta (0.21.0): la clave del paquete incluye la paleta. La misma imagen C8 con dos paletas son dos
texturas (antes se juntaban en una), y la clave del runtime en C++ (wl::texture_key con paleta y el motor del juego)
es la misma que la del conversor."""
import os
import shutil
import struct
import subprocess
import tempfile
import unittest

import numpy as np

from choya import engine, gxtex, pack, profiles
from tests import fixtures as fx

AQUI = os.path.dirname(__file__)
REPO = os.path.dirname(AQUI)


def plt0_blob(pal_raw: bytes, pfmt: int) -> bytes:
    n = len(pal_raw) // 2
    hdr = bytearray(b"PLT0" + struct.pack(">IIi", 0x40 + len(pal_raw), 1, 0))
    hdr += struct.pack(">II", 0x40, 0) + struct.pack(">IHH", pfmt, n, 0)
    hdr += b"\x00" * (0x40 - len(hdr))
    return bytes(hdr) + pal_raw


def c8_indices(w: int, h: int) -> bytes:
    idx = (np.add.outer(np.arange(h), np.arange(w)) % 4).astype(np.uint8)
    return fx.tile(idx, 8, 4).tobytes()


PAL_A = struct.pack(">4H", 0xFC00, 0x83E0, 0x801F, 0xFFFF)   # RGB5A3 opacos: rojo, verde, azul, blanco
PAL_B = struct.pack(">4H", 0x8000, 0xFFE0, 0x83FF, 0xFC1F)


class TestClavePaleta(unittest.TestCase):
    def test_dos_paletas_dos_texturas(self):
        idx = c8_indices(16, 16)
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "disco", "files")
            os.makedirs(src)
            for name, pal in (("a", PAL_A), ("b", PAL_B)):
                brres = fx.build_brres_folders({"Textures(NW4R)": [("icono", fx.tex0_blob(gxtex.C8, 16, 16, idx))],
                                                "Palettes(NW4R)": [("icono", plt0_blob(pal, gxtex.PAL_RGB5A3))]})
                with open(os.path.join(src, f"{name}.brres"), "wb") as f:
                    f.write(brres)
            out = os.path.join(d, "salida")
            engine.run(os.path.join(d, "disco"), out, profiles.get("rg35xxh"), jobs=1, measure=False,
                       log=lambda *a: None)
            _m, entries = pack.read_pack(os.path.join(out, "assets.wlpk"))
            keys = sorted(e["key"] for e in entries if e["kind"] == pack.KIND_TEXTURE)
            self.assertEqual(keys, sorted({pack.texture_key(idx, 16, 16, gxtex.C8, PAL_A, gxtex.PAL_RGB5A3),
                                           pack.texture_key(idx, 16, 16, gxtex.C8, PAL_B, gxtex.PAL_RGB5A3)}))
        # Sin paleta, la clave no cambia (las demás texturas siguen igual que antes).
        self.assertEqual(pack.texture_key(idx, 16, 16, gxtex.I8), pack.texture_key(idx, 16, 16, gxtex.I8, None))

    def test_clave_en_cpp(self):
        cxx = shutil.which("clang++") or shutil.which("g++")
        if not cxx:
            self.skipTest("sin compilador")
        idx = c8_indices(16, 16)
        prog = r"""
#include <cstdio>
#include <vector>
#include "wl/format.h"
int main() {
  std::vector<unsigned char> idx(%d), pal = {%s};
  for (size_t i = 0; i < idx.size(); ++i) idx[i] = (unsigned char)(%s)[i];
  std::printf("%%llu\n", (unsigned long long)wl::texture_key(idx.data(), idx.size(), 16, 16, wl::GX_C8, pal.data(),
                                                         pal.size(), 2));
}
""" % (len(idx), ",".join(str(b) for b in PAL_A), "(const char*)\"" + "".join(f"\\x{b:02x}" for b in idx) + "\"")
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "k.cpp"), "w") as f:
                f.write(prog)
            exe = os.path.join(d, "k")
            subprocess.run([cxx, "-std=c++17", "-I", os.path.join(REPO, "runtime", "include"), os.path.join(d, "k.cpp"),
                            os.path.join(REPO, "runtime", "src", "format.cpp"), "-lz", "-o", exe], check=True,
                           capture_output=True)
            got = int(subprocess.run([exe], capture_output=True, text=True).stdout)
        self.assertEqual(got, pack.texture_key(idx, 16, 16, gxtex.C8, PAL_A, 2))


if __name__ == "__main__":
    unittest.main()
