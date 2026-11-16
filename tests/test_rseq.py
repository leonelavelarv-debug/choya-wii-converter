"""Secuencias RSEQ + bancos RBNK (0.42.0): intérprete del MML de NW4R y prerender (ruta A), con datos sintéticos.
Notas, duración y tempo, instrumentos por rango e índice, envolvente, pan y volumen, flujo (llamadas, bucles con
contador, saltos, variables y ejecución condicional), detección del bucle de la música, final, entradas dañadas y el
paso al paquete con su bucle."""
import os
import struct
import tempfile
import unittest

import numpy as np

from choya import brsar, engine, pack, profiles, rseq
from tests import fixtures as fx

M = fx.Mml
RATE = 32000


def seno(freq, n=3200, amp=20000):
    return (np.sin(np.arange(n) * 2 * np.pi * freq / RATE) * amp).astype(np.int16)


def cuadrada(freq, n=3200, amp=20000):
    return (np.sign(np.sin(np.arange(n) * 2 * np.pi * freq / RATE)) * amp).astype(np.int16)


def banco(insts=None, waves=None):
    return fx.build_rbnk(insts or [dict(wave=0, key=69)], waves or [(seno(440), RATE, 0)])


def correr(mml, bk=None, vol=127, **kw):
    d = fx.build_brsar_seq([("SEQ", mml, vol)], bk or banco())
    sq = brsar.read_brsar(d).sequences[0]
    base, end = rseq.seq_base(d, sq.seq[0], sq.seq[1])
    return rseq.render(d, base, end, sq.data_offset, rseq.Bank(d, *sq.bank), RATE, sound_volume=vol / 127, **kw)


def freq(x):
    sp = np.abs(np.fft.rfft(x * np.hanning(len(x))))
    return np.argmax(sp) * RATE / len(x)


def rms(x):
    return float(np.sqrt(np.mean(x.astype(float) ** 2))) if len(x) else 0.0


class TestNotas(unittest.TestCase):
    def test_tono_y_duracion(self):
        r = correr(M.note(69, 127, 48) + M.note(81, 127, 48) + M.note(57, 127, 48) + M.fin)
        s = r.samples[:, 0]
        self.assertTrue(r.ended)
        self.assertIsNone(r.loop_start)
        # tempo 120, timebase 48: 96 ticks por segundo -> 48 ticks = 0.5 s
        self.assertAlmostEqual(len(s) / RATE, 1.5, delta=0.02)
        self.assertAlmostEqual(freq(s[1000:15000]), 440, delta=5)
        self.assertAlmostEqual(freq(s[17000:31000]), 880, delta=5)    # una octava arriba
        self.assertAlmostEqual(freq(s[33000:47000]), 220, delta=5)
        self.assertEqual(r.ops["nota"], 3)

    def test_tempo_y_espera(self):
        r = correr(M.tempo(60) + M.wait(48) + M.note(69, 127, 24) + M.fin)
        s = r.samples[:, 0]
        # tempo 60: 48 ticks por segundo; 1 s de silencio y 0.5 s de nota
        self.assertLess(np.abs(s[:31000]).max(), 1)
        self.assertGreater(rms(s[33000:46000]), 5000)
        self.assertAlmostEqual(len(s) / RATE, 1.5, delta=0.02)

    def test_instrumento_por_rango_e_indice(self):
        bk = banco([("rango", [(60, dict(wave=0, key=69)), (127, dict(wave=1, key=69))]),
                    ("indice", 60, [dict(wave=0, key=60), dict(wave=1, key=61)])],
                   [(seno(440), RATE, 0), (cuadrada(440), RATE, 0)])
        r = correr(M.note(60, 127, 48) + M.note(70, 127, 48) + M.prg(1) + M.note(60, 127, 48) +
                   M.note(61, 127, 48) + M.note(62, 127, 48) + M.fin, bk)
        s = r.samples[:, 0].astype(float)
        crest = lambda x: np.abs(x).max() / rms(x)
        self.assertGreater(crest(s[1000:15000]), 1.3)        # seno (tecla 60 <= 60)
        self.assertLess(crest(s[17000:31000]), 1.1)          # cuadrada (tecla 70)
        self.assertGreater(crest(s[33000:47000]), 1.3)       # programa 1, índice 60 -> onda 0
        self.assertLess(crest(s[49000:63000]), 1.1)          # índice 61 -> onda 1
        self.assertEqual(r.stats["notas sin instrumento"], 1)   # 62 está fuera del índice
        self.assertLess(np.abs(s[65000:]).max() if len(s) > 65000 else 0, 1)

    def test_afinacion_y_tecla_original(self):
        bk = banco([dict(wave=0, key=57, tune=2.0)])
        r = correr(M.note(57, 127, 48) + M.fin, bk)
        self.assertAlmostEqual(freq(r.samples[1000:15000, 0]), 880, delta=5)   # tune 2.0

    def test_envolvente(self):
        bk = banco([dict(wave=0, key=69, a=110, r=100)])
        r = correr(M.note(69, 127, 48) + M.wait(96) + M.fin, bk)
        s = r.samples[:, 0]
        self.assertLess(rms(s[:300]), rms(s[6000:9000]) * 0.7)          # ataque lento
        self.assertGreater(rms(s[16500:17500]), 300)                     # liberación: sigue sonando
        self.assertLess(rms(s[16500:17500]), rms(s[12000:15000]))
        self.assertLess(rms(s[-3000:]), 50)

    def test_volumen_y_pan(self):
        full = correr(M.note(69, 127, 48) + M.fin)
        half = correr(M.u8(0xC1, 64) + M.note(69, 127, 48) + M.fin)
        izq = correr(M.u8(0xC0, 0) + M.note(69, 127, 48) + M.fin)
        a, b = rms(full.samples[2000:14000, 0]), rms(half.samples[2000:14000, 0])
        self.assertAlmostEqual(b / a, (64 / 127) ** 2, delta=0.02)         # volumen al cuadrado
        self.assertGreater(rms(izq.samples[2000:14000, 0]), 10000)
        self.assertLess(rms(izq.samples[2000:14000, 1]), 200)             # todo a la izquierda
        vel = correr(M.note(69, 64, 48) + M.fin)
        self.assertAlmostEqual(rms(vel.samples[2000:14000, 0]) / a, (64 / 127) ** 2, delta=0.02)

    def test_pitch_bend(self):
        r = correr(M.u8(0xC4, 64) + M.note(69, 127, 48) + M.fin)   # 64/128 * 2 semitonos = 1 semitono
        self.assertAlmostEqual(freq(r.samples[1000:15000, 0]), 440 * 2 ** (1 / 12), delta=5)


class TestFlujo(unittest.TestCase):
    def test_llamada_y_bucle_con_contador(self):
        # 0: call 20; loop_start 2 ... loop_end (3 vueltas: el contador baja a 0 en la tercera); fin
        body = M.call(20) + M.loop_start(3) + M.note(69, 127, 12) + M.loop_end + M.fin
        body += b"\x00" * (20 - len(body))
        body += M.note(81, 127, 12) + M.ret
        r = correr(body)
        self.assertEqual(r.ops["nota"], 4)          # 1 de la llamada + 3 del bucle
        self.assertEqual(r.ops["ret"], 1)
        self.assertTrue(r.ended)

    def test_variables_y_ejecucion_condicional(self):
        mml = (M.ex(0x80, 32, 5) +                  # VAR de pista 0 = 5
               M.ex(0x81, 32, 2) +                  # += 2 -> 7
               M.ex(0x90, 32, 7) +                  # ¿== 7? sí
               M.execif(M.note(69, 127, 12)) +
               M.ex(0x90, 32, 8) +                  # ¿== 8? no
               M.execif(M.note(81, 127, 12)) +
               M.fin)
        r = correr(mml)
        self.assertEqual(r.ops["nota"], 2)          # se leen las dos
        s = r.samples[:, 0]
        self.assertGreater(rms(s[500:3500]), 5000)  # la primera suena
        self.assertFalse(r.interactive)             # solo variables de pista

    def test_variable_del_juego_es_interactiva(self):
        r = correr(M.ex(0x90, 0, -1) + M.execif(M.note(69, 127, 12)) + M.fin)
        self.assertTrue(r.interactive)

    def test_pistas_paralelas(self):
        head = M.alloctrack(3) + M.opentrack(1, 0)
        main = M.note(69, 127, 48) + M.fin
        start1 = len(head) + len(main)
        mml = M.alloctrack(3) + M.opentrack(1, start1) + main + M.note(81, 127, 48) + M.fin
        r = correr(mml)
        sp = np.abs(np.fft.rfft(r.samples[1000:15000, 0] * np.hanning(14000)))
        f = np.argsort(sp)[-2:] * RATE / 14000
        self.assertTrue(any(abs(x - 440) < 5 for x in f) and any(abs(x - 880) < 5 for x in f), f)
        self.assertEqual(r.ops["opentrack"], 1)

    def test_bucle_de_musica(self):
        # intro de 48 ticks; el cuerpo (dos notas de 24 ticks) empieza en el byte 3 y salta a él
        intro = M.note(57, 127, 48)
        mml = intro + M.note(69, 127, 24) + M.note(81, 127, 24) + M.jump(len(intro))
        r = correr(mml)
        self.assertIsNotNone(r.loop_start)
        self.assertFalse(r.ended)
        self.assertAlmostEqual(r.loop_start / RATE, 0.5, delta=0.01)      # después de la intro
        self.assertAlmostEqual((len(r.samples) - r.loop_start) / RATE, 0.5, delta=0.01)
        body = r.samples[r.loop_start:, 0]
        self.assertAlmostEqual(freq(body[500:7500]), 440, delta=10)
        self.assertAlmostEqual(freq(body[8500:15500]), 880, delta=10)
        self.assertEqual(r.stats.get("sin final ni bucle: cortada al tope", 0), 0)

    def test_bucle_infinito_con_loop_start_0(self):
        mml = M.note(57, 127, 48) + M.loop_start(0) + M.note(69, 127, 24) + M.loop_end
        r = correr(mml)
        self.assertIsNotNone(r.loop_start)
        self.assertAlmostEqual((len(r.samples) - r.loop_start) / RATE, 0.25, delta=0.01)

    def test_sin_final_se_corta(self):
        r = correr(M.note(69, 127, 0x7FFF) + M.fin, max_seconds=2.0)
        self.assertLessEqual(len(r.samples) / RATE, 2.0 + rseq.TAIL_SECONDS + 0.01)
        self.assertEqual(r.stats["sin final ni bucle: cortada al tope"], 1)

    def test_comandos_no_aplicados_se_cuentan(self):
        r = correr(M.u8(0xD9, 40) + M.u8(0xD8, 30) + M.note(69, 127, 12) + M.fin)
        self.assertEqual(r.ignored["envío FX A"], 1)
        self.assertEqual(r.ignored["filtro pasabajos"], 1)


class TestDanado(unittest.TestCase):
    def test_lectura_fuera(self):
        with self.assertRaises(rseq.SeqError):
            correr(M.note(69, 127, 12) + bytes([0x89, 0x7F]))     # salto cortado

    def test_bucle_sin_espera(self):
        with self.assertRaisesRegex(rseq.SeqError, "sin espera"):
            correr(M.jump(0))

    def test_rbnk_danado(self):
        rbnk, waves = banco()
        bad = bytearray(rbnk)
        struct.pack_into(">I", bad, 0x20 + 8, 1 << 20)
        with self.assertRaises(rseq.SeqError):
            d = fx.build_brsar_seq([("SEQ", M.note(69, 127, 12) + M.fin, 127)], (bytes(bad), waves))
            sq = brsar.read_brsar(d).sequences[0]
            rseq.Bank(d, *sq.bank)

    def test_rseq_cortado(self):
        with self.assertRaises(rseq.SeqError):
            rseq.seq_base(b"RSEQ" + b"\0" * 20, 0, 24)


class TestPaquete(unittest.TestCase):
    def test_al_paquete_con_bucle(self):
        mml = M.note(57, 127, 48) + M.note(69, 127, 24) + M.note(81, 127, 24) + M.jump(3)
        with tempfile.TemporaryDirectory() as d:
            sd = os.path.join(d, "disc", "files", "Sound")
            os.makedirs(sd)
            with open(os.path.join(sd, "musica.brsar"), "wb") as f:
                f.write(fx.build_brsar_seq([("BGM_PRUEBA", mml, 100), ("SE_CORTO", M.note(69, 127, 12) + M.fin, 127)],
                                           banco()))
            out = os.path.join(d, "out")
            rep = engine.run(os.path.join(d, "disc"), out, profiles.get("rg35xxh"), jobs=1, measure=False,
                             log=lambda *_: None)
            self.assertEqual(rep["counts"].get("BRSAR:secuencias prerenderizadas"), 2, rep["errors"])
            _man, entries = pack.read_pack(os.path.join(out, "assets.wlpk"))
            aud = {e["name"]: e for e in entries if e["kind"] == pack.KIND_AUDIO}
            bgm = aud["Sound/musica.brsar/BGM_PRUEBA"]
            self.assertEqual(bgm["key"], pack.path_key("Sound/musica.brsar/BGM_PRUEBA"))
            self.assertTrue(bgm["flags"] & pack.AUDIOFLAG_LOOP)
            rate = bgm["extra0"]
            self.assertAlmostEqual(bgm["extra1"] / rate, 0.5, delta=0.02)          # inicio del bucle
            self.assertAlmostEqual(bgm["extra2"] / rate, 1.0, delta=0.02)          # intro + una vuelta
            se = aud["Sound/musica.brsar/SE_CORTO"]
            self.assertFalse(se["flags"] & pack.AUDIOFLAG_LOOP)
            md = open(os.path.join(out, "report.md"), encoding="utf-8").read()
            self.assertIn("## Secuencias (RSEQ + RBNK, prerenderizadas en la PC)", md)
            self.assertIn("2 de 2 prerenderizadas: 1 con bucle, 1 que terminan", md)

    def test_sin_secuencias(self):
        with tempfile.TemporaryDirectory() as d:
            sd = os.path.join(d, "disc", "files")
            os.makedirs(sd)
            with open(os.path.join(sd, "m.brsar"), "wb") as f:
                f.write(fx.build_brsar_seq([("BGM", M.note(69, 127, 12) + M.fin, 127)], banco()))
            rep = engine.run(os.path.join(d, "disc"), os.path.join(d, "o"), profiles.get("rg35xxh"), jobs=1,
                             measure=False, log=lambda *_: None, sequences=False)
            self.assertTrue(any("--sin-secuencias" in p for p in rep["pending"]))
            self.assertEqual(rep["summary"]["audio"], 0)


if __name__ == "__main__":
    unittest.main()
