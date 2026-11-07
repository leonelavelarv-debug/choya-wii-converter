import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from choya import dol, recomp  # noqa: E402
import fixtures as fx  # noqa: E402

START, INIT = 0x80006124, 0x80006290


def boot_dol(init_words):
    return fx.build_dol({START: [fx.ppc_bl(START, INIT), 0x4E800020],
                         INIT: init_words + [0x4E800020]})


CW_INIT = [fx.ppc_lis(1, 0x8040), fx.ppc_ori(1, 1, 0x1234),
           fx.ppc_lis(2, 0x8037), fx.ppc_ori(2, 2, 0xA0E0),
           fx.ppc_lis(13, 0x8037), fx.ppc_ori(13, 13, 0x8000)]


class TestDol(unittest.TestCase):
    def test_sections_and_boot(self):
        d = dol.read_dol(boot_dol(CW_INIT))
        self.assertEqual(d.entry, START)
        self.assertEqual([s.kind for s in d.sections], ["text", "data"])
        b = dol.boot_info(d)
        self.assertEqual((b.init_registers, b.sda_base, b.sda2_base, b.stack),
                         (INIT, 0x80378000, 0x8037A0E0, 0x80401234))

    def test_addi_negative(self):
        # lis r13,0x8038 ; addi r13,r13,-0x2000 → 0x8037E000
        words = [fx.ppc_lis(2, 0x8037), fx.ppc_addi(2, 2, 0x1000),
                 fx.ppc_lis(13, 0x8038), fx.ppc_addi(13, 13, -0x2000)]
        b = dol.boot_info(dol.read_dol(boot_dol(words)))
        self.assertEqual((b.sda_base, b.sda2_base), (0x8037E000, 0x80371000))

    def test_missing_sda(self):
        with self.assertRaises(ValueError):
            dol.boot_info(dol.read_dol(boot_dol([fx.ppc_lis(1, 0x8040)])))

    def test_bad_entry(self):
        with self.assertRaises(ValueError):
            dol.read_dol(fx.build_dol({}, entry=0x81000000))


class TestRecompPrep(unittest.TestCase):
    def test_symbols_parse(self):
        txt = ("__start = .init:0x80006124; // type:function size:0x16C scope:weak\n"
               "lbl_1 = .data:0x80300000; // type:object size:0x4\n"
               "main = .text:0x80156448; // type:function size:0x10\n")
        self.assertEqual(recomp.parse_symbols(txt), [(START, "__start"), (0x80156448, "main")])
        pad = "pad_00_80004188_init = .init:0x80004188; // type:function size:0x1F34 scope:local\n"
        self.assertEqual(recomp.parse_symbols(pad + txt)[0], (START, "__start"))
        self.assertEqual(recomp.parse_map("80004188 pad_00_80004188_init\n80006124 __start\n"),
                         [(START, "__start")])

    def test_bundled_map(self):
        funcs, _src = recomp.load_functions(None)
        names = {n: a for a, n in funcs}
        self.assertEqual(len(funcs), 14121)
        self.assertNotIn(0x80004188, dict(funcs))
        self.assertEqual(names["__start"], recomp.WIISPORTS_START)
        self.assertEqual(names["__init_registers"], recomp.WIISPORTS_INIT_REGISTERS)
        self.assertEqual(names["main"], 0x80183EE4)

    def test_prep_writes_project(self):
        with tempfile.TemporaryDirectory() as t:
            disc = os.path.join(t, "disco", "sys")
            os.makedirs(disc)
            with open(os.path.join(disc, "main.dol"), "wb") as f:
                f.write(boot_dol(CW_INIT))
            sym = os.path.join(t, "MAP.txt")
            with open(sym, "w") as f:
                f.write("80006124 __start\n80006290 __init_registers\n80001000 fuera\n")
            out = os.path.join(t, "wiicompiled")
            with self.assertRaises(ValueError):          # sin --simbolos ni --forzar: no es ogws
                recomp.prep(os.path.join(t, "disco"), out)
            r = recomp.prep(os.path.join(t, "disco"), out, simbolos=sym)
            self.assertEqual(r.functions, 3)
            self.assertEqual(r.outside_text, 1)
            yml = open(os.path.join(r.project_dir, "recomp.yml"), encoding="utf-8").read()
            self.assertIn("sda_base: 0x80378000", yml)
            self.assertIn("sda2_base: 0x8037A0E0", yml)
            self.assertIn("- 0x80006124", yml)
            self.assertIn("sha256: ", yml)
            self.assertTrue(os.path.isdir(os.path.join(r.project_dir, "native")))
            self.assertEqual(open(os.path.join(r.project_dir, "MAP.txt")).read().splitlines()[0],
                             "80006124 __start")

    def test_coverage(self):
        with tempfile.TemporaryDirectory() as t:
            os.makedirs(os.path.join(t, "runtime", "src", "hle", "gx"))
            os.makedirs(os.path.join(t, "projects", "otro"))
            with open(os.path.join(t, "projects", "otro", "MAP.txt"), "w") as f:
                f.write("801f409c GXSetTevOrder\n")
            with open(os.path.join(t, "runtime", "src", "hle", "gx", "tev.cpp"), "w") as f:
                f.write("PPC_NATIVE_OVERRIDE_VOID(801F409C, GX__SetTevOrder_8017214c, (a), (a))\n"
                        "REGISTER_NATIVE_FUNCTION(0x80001234, Solo_de_otro_juego)\n")
            cov = recomp.hle_coverage(t, [(0x800E0000, "GXSetTevOrder")])
            hit = [e for e in cov if e.new_address]
            self.assertEqual(len(cov), 2)
            self.assertEqual((hit[0].name, hit[0].new_address), ("GXSetTevOrder", 0x800E0000))
            self.assertIn("1 de 2", recomp.coverage_markdown(cov))


if __name__ == "__main__":
    unittest.main()
