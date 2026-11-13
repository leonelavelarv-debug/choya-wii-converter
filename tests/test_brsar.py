"""Efectos de sonido de un BRSAR (0.21.0): los sonidos de onda salen como audios del paquete con su nombre."""
import os
import tempfile
import unittest

import numpy as np

from choya import audio, brsar, engine, pack, profiles
from tests import fixtures as fx


def _ondas():
    t = np.arange(980)
    a = (np.sin(t / 7) * 8000).astype(np.int16)
    b = (np.sin(t / 3) * 5000).astype(np.int16)
    return a, b


class TestBrsar(unittest.TestCase):
    def test_leer(self):
        a, b = _ondas()
        data, expected = fx.build_brsar([("SE_GOLPE", a, 32000, False), ("SE_RED", np.stack([a, b], 1), 22050, True)],
                                        seq_names=["SEQ_FANFARRIA"])
        r = brsar.read_brsar(data)
        self.assertEqual(r.counts, {"ondas": 2, "secuencias": 1})
        self.assertEqual([n for n, _ in r.sounds], ["SE_GOLPE", "SE_RED"])
        for (_n, dec), exp in zip(r.sounds, expected):
            np.testing.assert_array_equal(dec.samples, exp)          # igual que el decodificador del DSP
            self.assertEqual(len(dec.adpcm), dec.samples.shape[1])   # el ADPCM crudo se puede guardar sin recodificar
        self.assertEqual((r.sounds[0][1].rate, r.sounds[0][1].loop_start), (32000, None))
        self.assertEqual((r.sounds[1][1].rate, r.sounds[1][1].loop_start), (22050, 0))

    def test_direcciones_dsp(self):
        for s in (0, 1, 13, 14, 15, 700):
            self.assertEqual(brsar.dsp_sample(fx.dsp_address(s)), s)

    def test_dañado(self):
        with self.assertRaises(brsar.BrsarError):
            brsar.read_brsar(b"RSAR" + b"\x00" * 60)

    def test_al_paquete(self):
        a, b = _ondas()
        data, _ = fx.build_brsar([("SE_GOLPE", a, 32000, False)], seq_names=["SEQ_FANFARRIA"])
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "disco", "files", "Sound")
            os.makedirs(src)
            with open(os.path.join(src, "sonidos.brsar"), "wb") as f:
                f.write(data)
            out = os.path.join(d, "salida")
            rep = engine.run(os.path.join(d, "disco"), out, profiles.get("rg35xxh"), jobs=1, measure=False,
                             log=lambda *a: None)
            _man, entries = pack.read_pack(os.path.join(out, "assets.wlpk"))
            aud = [e for e in entries if e["kind"] == pack.KIND_AUDIO]
            self.assertEqual([e["name"] for e in aud], ["Sound/sonidos.brsar/SE_GOLPE"])
            self.assertEqual(aud[0]["key"], pack.path_key("Sound/sonidos.brsar/SE_GOLPE"))
            self.assertEqual(rep["counts"].get("BRSAR:secuencias"), 1)
            self.assertTrue(any("secuencia" in p for p in rep["pending"]))
            self.assertFalse(rep["errors"])


if __name__ == "__main__":
    unittest.main()
