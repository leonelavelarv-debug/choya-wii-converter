import os
import struct
import sys
import tempfile
import unittest

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from choya import audio, compress, containers, engine, etc, gxtex, optimize, pack, profiles, u8  # noqa: E402
import fixtures as fx  # noqa: E402


class TestCompress(unittest.TestCase):
    def test_yaz0_roundtrip(self):
        data = b"ABCABCABCABCXYZ" * 50 + bytes(range(256))
        enc = fx.yaz0_literal(data)
        self.assertLess(len(enc), len(data))
        self.assertEqual(compress.decompress(enc), (data, "yaz0"))

    def test_lz10(self):
        # LZ10 a mano: 'abc' literal, luego referencia de 6 bytes a distancia 3
        payload = bytes([0b00010000]) + b"abc" + bytes([(6 - 3) << 4, 2])
        blob = bytes([0x10, 9, 0, 0]) + payload
        self.assertEqual(compress.lz77(blob), b"abcabcabc")


class TestU8(unittest.TestCase):
    def test_roundtrip(self):
        files = [("a.txt", b"hola"), ("dir/b.bin", b"\x01\x02"), ("dir/sub/c.bin", b"xyz")]
        blob = u8.build_u8(files)
        self.assertEqual(sorted(u8.read_u8(blob)), sorted(files))


class TestGXDecode(unittest.TestCase):
    def check(self, fmt, img, tol):
        raw = fx.encode_gx(fmt, img)
        self.assertEqual(len(raw), gxtex.level_size(fmt, img.shape[1], img.shape[0]))
        dec = gxtex.decode_level(fmt, raw, img.shape[1], img.shape[0])
        diff = np.abs(dec.astype(int) - img.astype(int)).max()
        self.assertLessEqual(diff, tol, gxtex.FORMAT_NAMES[fmt])

    def test_formats(self):
        img = fx.gradient(24, 20, "full")
        gray = img.copy()
        gray[..., 1] = gray[..., 2] = gray[..., 0]
        self.check(gxtex.RGBA8, img, 0)
        self.check(gxtex.RGB565, fx.gradient(24, 20, "none"), 8)
        self.check(gxtex.RGB5A3, fx.gradient(24, 20, "none"), 8)
        self.check(gxtex.RGB5A3, img, 36)
        self.check(gxtex.IA8, gray, 0)
        i8 = np.repeat(gray[..., :1], 4, 2)
        self.check(gxtex.I8, i8, 0)
        self.check(gxtex.I4, i8, 17)

    def test_cmpr(self):
        red, blue = 0xF800, 0x001F
        sub = fx.cmpr_block_solid(red, blue, [0x00, 0x55, 0xAA, 0xFF])
        raw = sub * 4
        img = gxtex.decode_level(gxtex.CMPR, raw, 8, 8)
        self.assertEqual(tuple(img[0, 0]), (255, 0, 0, 255))
        self.assertEqual(tuple(img[1, 0]), (0, 0, 255, 255))
        self.assertEqual(tuple(img[2, 0]), (170, 0, 85, 255))   # 2/3 rojo + 1/3 azul
        # modo de 3 colores: c0 <= c1 -> índice 3 transparente
        sub3 = fx.cmpr_block_solid(blue, red, [0xFF] * 4)
        img3 = gxtex.decode_level(gxtex.CMPR, sub3 * 4, 8, 8)
        self.assertEqual(img3[0, 0, 3], 0)

    def test_palette_c8(self):
        pal_raw = struct.pack(">4H", 0xFC00 | 0x8000, 0x83E0, 0x801F, 0x0FFF)
        pal = gxtex.decode_palette(pal_raw, 4, gxtex.PAL_RGB5A3)
        idx = np.tile(np.arange(4, dtype=np.uint8), (4, 2))   # 8x4
        raw = fx.tile(idx, 8, 4).tobytes()
        img = gxtex.decode_level(gxtex.C8, raw, 8, 4, pal)
        self.assertEqual(tuple(img[0, 0]), (255, 0, 0, 255))
        self.assertEqual(img[0, 3, 3], 0)


class TestETC(unittest.TestCase):
    def test_rgb_quality_and_validity(self):
        img = fx.gradient(64, 48)[..., :3]
        data = etc.encode_etc2_rgb(img)
        self.assertEqual(len(data), 16 * 12 * 8)
        self.assertGreater(etc.psnr(etc.decode_etc2_rgb(data, 64, 48), img), 36)

    def test_no_differential_overflow(self):
        rng = np.random.default_rng(3)
        img = rng.integers(0, 256, (64, 64, 3), dtype=np.uint8)
        img[::2] = 255
        img[1::2, ::3] = 0
        etc.decode_etc2_rgb(etc.encode_etc2_rgb(img), 64, 64)   # lanza si hay desbordamiento

    def test_eac(self):
        a = fx.gradient(32, 32, "full")[..., 3]
        d = etc.encode_eac(a)
        self.assertGreater(etc.psnr(etc.decode_eac(d, 32, 32), a), 40)
        flat = np.full((8, 8), 77, np.uint8)
        self.assertTrue(np.array_equal(etc.decode_eac(etc.encode_eac(flat), 8, 8), flat))

    def test_odd_sizes(self):
        img = fx.gradient(5, 3)
        d = etc.encode_etc2_rgba(img)
        self.assertEqual(len(d), 2 * 1 * 16)
        self.assertEqual(etc.decode_etc2_rgba(d, 5, 3).shape, (3, 5, 4))


class TestOptimize(unittest.TestCase):
    def tex(self, fmt, img, mips=1, source="files/x.brres"):
        data = b"".join(fx.encode_gx(fmt, img[::2 ** i, ::2 ** i]) for i in range(mips))
        return gxtex.GXTexture("t", fmt, img.shape[1], img.shape[0], mips, data, source=source)

    def test_downscale_and_format(self):
        p = profiles.get("rg35xxh")
        conv = optimize.convert(self.tex(gxtex.RGBA8, fx.gradient(512, 512), mips=3), p)
        self.assertEqual((conv.width, conv.height, conv.fmt), (256, 256, optimize.OUT_ETC2_RGB8))
        self.assertEqual(conv.mips, 3)
        expected = sum(optimize.out_level_size(conv.fmt, 256 >> i, 256 >> i) for i in range(3))
        self.assertEqual(len(conv.data), expected)
        alpha = optimize.convert(self.tex(gxtex.RGBA8, fx.gradient(64, 64, "full")), p)
        self.assertEqual(alpha.fmt, optimize.OUT_ETC2_RGBA8)

    def test_ui_keeps_resolution(self):
        p = profiles.get("rg35xxh")
        conv = optimize.convert(self.tex(gxtex.RGB5A3, fx.gradient(512, 256), source="files/lyt/timg/a.tpl"), p)
        self.assertEqual(conv.width, 512)

    def test_intensity_and_gles2(self):
        g = fx.gradient(32, 32)
        g[..., 1] = g[..., 2] = g[..., 3] = g[..., 0]
        self.assertEqual(optimize.convert(self.tex(gxtex.I8, g), profiles.get("rg35xxh")).fmt,
                         optimize.OUT_R11_EAC)
        la = optimize.convert(self.tex(gxtex.I8, g), profiles.get("gles2"))
        self.assertEqual(la.fmt, optimize.OUT_LA8)
        self.assertTrue(np.array_equal(np.frombuffer(la.data, np.uint8)[0::2], np.frombuffer(la.data, np.uint8)[1::2]))
        c = optimize.convert(self.tex(gxtex.RGBA8, fx.gradient(32, 32, "binary")), profiles.get("gles2"))
        self.assertEqual(c.fmt, optimize.OUT_RGBA5551)
        self.assertEqual(len(c.data), 32 * 32 * 2)


class TestModels(unittest.TestCase):
    def brres(self):
        img = fx.gradient(16, 16)
        return fx.build_brres_folders({
            "3DModels(NW4R)": [("box", fx.build_mdl0())],
            "Textures(NW4R)": [("tex_box", fx.tex0_blob(gxtex.RGB565, 16, 16, fx.encode_gx(gxtex.RGB565, img)))]})

    def test_read_mdl0(self):
        from choya import mdl0
        b = self.brres()
        (name, off), = mdl0.find_models(b, containers.brres_sections(b))
        m = mdl0.read_mdl0(b, off, name)
        self.assertEqual(m.warnings, [])
        self.assertEqual((m.stats["objetos"], m.stats["triangulos"]), (1, 2))
        # el hueso sube el modelo 1 unidad en Y
        self.assertAlmostEqual(float(m.positions[:, 1].min()), 0.0)
        self.assertAlmostEqual(float(m.positions[:, 1].max()), 2.0)
        self.assertEqual(m.materials[0].layers[0].texture, "tex_box")
        tris = m.positions[m.indices].reshape(-1, 3, 3)
        # ambos triángulos con el mismo sentido de giro
        cross = np.cross(tris[:, 1] - tris[:, 0], tris[:, 2] - tris[:, 0])[:, 2]
        self.assertTrue(np.all(cross > 0) or np.all(cross < 0))

    def test_engine_packs_model_with_texture_key(self):
        from choya import model_pack
        with tempfile.TemporaryDirectory() as tmp:
            files = os.path.join(tmp, "disc", "files", "Stage")
            os.makedirs(files)
            with open(os.path.join(files, "m.szs"), "wb") as f:
                f.write(fx.yaz0_literal(u8.build_u8([("g3d/box.brres", self.brres())])))
            out = os.path.join(tmp, "out")
            rep = engine.run(os.path.join(tmp, "disc"), out, profiles.get("rg35xxh"), jobs=1, preview=True,
                             log=lambda *_: None)
            self.assertEqual(rep["summary"]["models"]["modelos"], 1)
            self.assertEqual(rep["summary"]["models"]["sin_textura"], 0)
            _man, entries = pack.read_pack(os.path.join(out, "assets.wlpk"))
            model = [e for e in entries if e["kind"] == pack.KIND_MODEL][0]
            tex = [e for e in entries if e["kind"] == pack.KIND_TEXTURE][0]
            self.assertEqual(model["key"], pack.path_key("Stage/m.szs/g3d/box.brres/box"))
            mdl = model_pack.unpack_model(model["data"])
            self.assertEqual(mdl["submeshes"][0][2], tex["key"])
            self.assertEqual(len(mdl["indices"]), 6)
            objs = os.listdir(os.path.join(out, "preview", "modelos"))
            self.assertIn("Stage_m.szs_g3d_box.brres_box.obj", objs)
            with open(os.path.join(out, "preview", "modelos", "Stage_m.szs_g3d_box.brres_box.mtl")) as fh:
                self.assertIn("map_Kd ../texturas/", fh.read())


class TestQualityFallback(unittest.TestCase):
    def tex(self, fmt, img, source):
        return gxtex.GXTexture("t", fmt, img.shape[1], img.shape[0], 1, fx.encode_gx(fmt, img), source=source)

    def test_tiny_color_table_stays_lossless(self):
        rng = np.random.default_rng(5)
        img = rng.integers(0, 256, (4, 4, 4), dtype=np.uint8)
        img[..., 3] = 255
        c = optimize.convert(self.tex(gxtex.RGBA8, img, "files/a.brres"), profiles.get("rg35xxh"))
        self.assertEqual(c.fmt, optimize.OUT_RGBA8)
        self.assertTrue(np.array_equal(np.frombuffer(c.data, np.uint8).reshape(4, 4, 4), img))
        self.assertIn("diminuta", c.fallback)

    def test_sharp_ui_falls_back(self):
        rng = np.random.default_rng(6)
        img = rng.integers(0, 256, (32, 24, 4), dtype=np.uint8)
        img[..., 3] = np.where(img[..., 3] > 128, 255, 0)
        c = optimize.convert(self.tex(gxtex.RGBA8, img, "files/x.arc/timg/count.tpl"), profiles.get("rg35xxh"))
        self.assertEqual(c.fmt, optimize.OUT_RGBA8)
        self.assertGreaterEqual(c.psnr, 32)

    def test_noisy_3d_uses_rgb565(self):
        rng = np.random.default_rng(7)
        img = rng.integers(0, 256, (64, 64, 4), dtype=np.uint8)
        img[..., 3] = 255
        c = optimize.convert(self.tex(gxtex.RGBA8, img, "files/a.brres"), profiles.get("rg35xxh"))
        self.assertEqual(c.fmt, optimize.OUT_RGB565)

    def test_smooth_texture_stays_etc2(self):
        c = optimize.convert(self.tex(gxtex.RGBA8, fx.gradient(64, 64), "files/a.brres"), profiles.get("rg35xxh"))
        self.assertEqual(c.fmt, optimize.OUT_ETC2_RGB8)
        self.assertIsNone(c.fallback)


class TestContainers(unittest.TestCase):
    def test_tpl(self):
        img = fx.gradient(16, 16)
        blob = containers.build_tpl([(gxtex.RGB565, 16, 16, fx.encode_gx(gxtex.RGB565, img), 1)])
        texs = containers.read_tpl(blob, "a.tpl")
        self.assertEqual(len(texs), 1)
        self.assertEqual((texs[0].fmt, texs[0].width), (gxtex.RGB565, 16))

    def test_brres(self):
        img = fx.gradient(16, 8, "full")
        blob = fx.build_brres([("tex_a", gxtex.RGBA8, 16, 8, fx.encode_gx(gxtex.RGBA8, img), 1),
                               ("tex_b", gxtex.IA8, 16, 8, fx.encode_gx(gxtex.IA8, img), 1)])
        texs, counts = containers.read_brres(blob, "m.brres")
        self.assertEqual([t.name for t in texs], ["m.brres/tex_a", "m.brres/tex_b"])
        self.assertEqual(counts["Textures(NW4R)"], 2)
        dec = gxtex.decode_level(texs[0].fmt, texs[0].data, 16, 8)
        self.assertTrue(np.array_equal(dec, img))


class TestAudio(unittest.TestCase):
    def test_dsp_decode_matches_encoder(self):
        t = np.arange(700)
        pcm = (np.sin(t / 9) * 12000).astype(np.int16)
        blob, sim = fx.build_dsp(pcm, 32000)
        dec = audio.read_dsp(blob)
        self.assertEqual(dec.rate, 32000)
        self.assertTrue(np.array_equal(dec.samples[:, 0], sim))
        snr = 10 * np.log10(np.mean(pcm.astype(float) ** 2) / np.mean((pcm.astype(float) - sim) ** 2))
        self.assertGreater(snr, 20)

    def test_brstm_stereo_blocks(self):
        t = np.arange(600)
        l = (np.sin(t / 7) * 9000).astype(np.int16)
        r = (np.cos(t / 11) * 9000).astype(np.int16)
        blob, sims = fx.build_brstm([l, r], 48000, block_size=0x40)
        dec = audio.read_brstm(blob)
        self.assertEqual(dec.samples.shape, (600, 2))
        self.assertTrue(np.array_equal(dec.samples[:, 0], sims[0]))
        self.assertTrue(np.array_equal(dec.samples[:, 1], sims[1]))
        opt = audio.optimize(dec, 32000, True, 20.0)
        self.assertEqual((opt.rate, opt.samples.shape[1]), (32000, 1))
        self.assertEqual(opt.samples.shape[0], 400)


class TestEndToEnd(unittest.TestCase):
    def test_convert_fake_disc(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = os.path.join(tmp, "disc", "DATA")
            files = os.path.join(root, "files")
            os.makedirs(os.path.join(files, "Stage"))
            os.makedirs(os.path.join(files, "Sound"))
            os.makedirs(os.path.join(root, "sys"))
            with open(os.path.join(root, "sys", "boot.bin"), "wb") as f:
                f.write(b"TEST01\x00\x01" + b"\x00" * 24)
            with open(os.path.join(root, "sys", "main.dol"), "wb") as f:
                f.write(b"no es un dol real")
            img = fx.gradient(128, 128)
            brres = fx.build_brres([("suelo", gxtex.RGB565, 128, 128, fx.encode_gx(gxtex.RGB565, img), 1)])
            tpl = containers.build_tpl([(gxtex.RGB5A3, 32, 32,
                                         fx.encode_gx(gxtex.RGB5A3, fx.gradient(32, 32, "full")), 1)])
            arc = u8.build_u8([("g3d/model.brres", brres), ("timg/boton.tpl", tpl),
                               ("copia/model.brres", brres)])
            with open(os.path.join(files, "Stage", "stage.szs"), "wb") as f:
                f.write(fx.yaz0_literal(arc))
            dsp, _ = fx.build_dsp((np.sin(np.arange(280) / 5) * 8000).astype(np.int16), 32000)
            with open(os.path.join(files, "Sound", "golpe.dsp"), "wb") as f:
                f.write(dsp)
            out = os.path.join(tmp, "out")
            rep = engine.run(os.path.join(tmp, "disc"), out, profiles.get("rg35xxh"), jobs=1, preview=True,
                             log=lambda *_: None)
            self.assertEqual(rep["manifest"]["game_id"], "TEST01")
            self.assertEqual(rep["manifest"]["revision"], 1)
            with open(os.path.join(out, "report.md"), encoding="utf-8") as fh:
                self.assertIn("no coincide con ogws", fh.read())
            self.assertEqual(rep["summary"]["textures"], 2)        # el BRRES duplicado se deduplica
            self.assertEqual(rep["summary"]["texture_refs"], 3)
            self.assertEqual(rep["summary"]["audio"], 1)
            self.assertFalse(rep["errors"], rep["errors"])
            man, entries = pack.read_pack(os.path.join(out, "assets.wlpk"))
            keys = [e["key"] for e in entries]
            self.assertEqual(keys, sorted(keys))
            tex = [e for e in entries if e["kind"] == pack.KIND_TEXTURE]
            lvl0 = fx.encode_gx(gxtex.RGB565, img)
            want = pack.texture_key(lvl0, 128, 128, gxtex.RGB565)
            self.assertIn(want, keys)
            aud = [e for e in entries if e["kind"] == pack.KIND_AUDIO][0]
            self.assertEqual(aud["key"], pack.path_key("Sound/golpe.dsp"))
            self.assertTrue(os.path.isfile(os.path.join(out, "report.md")))
            self.assertTrue(os.listdir(os.path.join(out, "preview", "texturas")))
            self.assertEqual(len(tex), 2)


if __name__ == "__main__":
    unittest.main()
