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
            self.assertIn("modelos: 0", r.stdout)
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
            # El .dsp mono se guardó como DSP-ADPCM y el runtime lo decodifica igual que Python.
            r = subprocess.run([os.path.join(BUILD, "wl_info"), "--pcm", pack, "Sound/golpe.dsp", pcm_path],
                               capture_output=True, text=True)
            self.assertIn("formato 2", r.stdout)
            self.assertTrue(np.array_equal(np.fromfile(pcm_path, "<i2"), sim))

    def test_model_cross(self):
        from choya import u8
        with tempfile.TemporaryDirectory() as tmp:
            files = os.path.join(tmp, "disc", "files")
            os.makedirs(files)
            img = fx.gradient(16, 16)
            brres = fx.build_brres_folders({
                "3DModels(NW4R)": [("box", fx.build_mdl0())],
                "Textures(NW4R)": [("tex_box", fx.tex0_blob(gxtex.RGB565, 16, 16, fx.encode_gx(gxtex.RGB565, img)))]})
            with open(os.path.join(files, "m.arc"), "wb") as f:
                f.write(u8.build_u8([("box.brres", brres)]))
            out = os.path.join(tmp, "out")
            engine.run(os.path.join(tmp, "disc"), out, profiles.get("rg35xxh"), jobs=1, log=lambda *_: None)
            r = subprocess.run([os.path.join(BUILD, "wl_info"), os.path.join(out, "assets.wlpk"), "-l"],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout)
            self.assertIn("modelos: 1 (2 triángulos)", r.stdout)
            self.assertIn("m.arc/box.brres/box", r.stdout)

    def test_adpcm_stereo_loop(self):
        import dataclasses
        with tempfile.TemporaryDirectory() as tmp:
            files = os.path.join(tmp, "disc", "files", "Sound")
            os.makedirs(files)
            t = np.arange(560)
            l = (np.sin(t / 7) * 9000).astype(np.int16)
            r = (np.cos(t / 11) * 9000).astype(np.int16)
            blob, sims = fx.build_brstm([l, r], 32000, block_size=0x40, loop=True)
            with open(os.path.join(files, "bgm.brstm"), "wb") as f:
                f.write(blob)
            prof = dataclasses.replace(profiles.get("rg35xxh"), music_min_seconds=0.001)
            out = os.path.join(tmp, "out")
            rep = engine.run(os.path.join(tmp, "disc"), out, prof, jobs=1, log=lambda *_: None)
            self.assertEqual(rep["summary"]["audio_adpcm"], 1)
            pcm_path = os.path.join(tmp, "pcm.raw")
            res = subprocess.run([os.path.join(BUILD, "wl_info"), "--pcm", os.path.join(out, "assets.wlpk"),
                                  "sound/BGM.brstm", pcm_path, "100"], capture_output=True, text=True)
            self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
            got = np.fromfile(pcm_path, "<i2").reshape(-1, 2)
            want = np.stack(sims, 1)
            self.assertEqual(got.shape[0], 660)            # 560 + 100 tras volver al inicio del bucle
            self.assertTrue(np.array_equal(got[:560], want))
            self.assertTrue(np.array_equal(got[560:], want[:100]))


if __name__ == "__main__":
    unittest.main()
