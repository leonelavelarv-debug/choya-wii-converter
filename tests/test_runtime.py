"""Prueba cruzada: el runtime C++ lee lo que escribe el conversor Python.
Compila antes: cmake -S runtime -B runtime/build -DWL_WITH_GL=OFF && cmake --build runtime/build"""
import os
import subprocess
import sys
import tempfile
import unittest

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)
sys.path.insert(0, HERE)

from choya import audio, engine, gxtex, profiles  # noqa: E402
import fixtures as fx  # noqa: E402

BUILD = os.path.join(ROOT, "runtime", "build")


@unittest.skipUnless(os.path.isfile(os.path.join(BUILD, "wl_selftest")), "runtime sin compilar")
class TestRuntime(unittest.TestCase):
    def test_cross(self):
        with tempfile.TemporaryDirectory() as tmp:
            files = os.path.join(tmp, "disc", "files")
            os.makedirs(os.path.join(files, "Sound"))
            img = fx.gradient(64, 32, "full")
            gx = fx.encode_gx(gxtex.RGB5A3, img)
            with open(os.path.join(files, "m.brres"), "wb") as f:
                f.write(fx.build_brres([("t", gxtex.RGB5A3, 64, 32, gx, 1)]))
            dsp, sim = fx.build_dsp((np.sin(np.arange(420) / 6) * 7000).astype(np.int16), 32000)
            with open(os.path.join(files, "Sound", "golpe.dsp"), "wb") as f:
                f.write(dsp)
            out = os.path.join(tmp, "out")
            engine.run(os.path.join(tmp, "disc"), out, profiles.get("rg35xxh"), jobs=1, log=lambda *_: None)
            raw = os.path.join(tmp, "lvl0.gx")
            with open(raw, "wb") as f:
                f.write(gx)
            pack = os.path.join(out, "assets.wlpk")
            r = subprocess.run([os.path.join(BUILD, "wl_selftest"), pack, raw, "64", "32", str(gxtex.RGB5A3),
                                "/SOUND/golpe.dsp"], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r = subprocess.run([os.path.join(BUILD, "wl_info"), pack, "-l"], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout)
            self.assertIn("ETC2_RGBA8_EAC", r.stdout)
            # El decodificador DSP de C++ da exactamente lo mismo que el de Python.
            dsp_path = os.path.join(files, "Sound", "golpe.dsp")
            pcm_path = os.path.join(tmp, "pcm.raw")
            subprocess.run([os.path.join(BUILD, "wl_info"), "--dsp", dsp_path, pcm_path], check=True,
                           capture_output=True)
            cpp = np.fromfile(pcm_path, "<i2")
            with open(dsp_path, "rb") as fh:
                py = audio.read_dsp(fh.read()).samples[:, 0]
            self.assertTrue(np.array_equal(cpp, py))
            self.assertTrue(np.array_equal(cpp, sim))


if __name__ == "__main__":
    unittest.main()
