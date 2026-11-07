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


class TestRetarget(unittest.TestCase):
    SYMS = "\n".join([
        "GXSetTevOrder = .text:0x800E1A84; // type:function size:0x100 scope:global",
        "__GXSetDirtyState = .text:0x800DE85C; // type:function size:0x278 scope:global",
        "OSPanic = .text:0x800EEFA8; // type:function size:0x12C scope:global",
        "FifoObj = .bss:0x80412E50; // type:object size:0x80 scope:local",
        "HorVer = .bss:0x80429170; // type:object size:0x58 scope:local",
        "Dup = .bss:0x80400000; // type:object size:0x4 scope:local",
        "Dup = .bss:0x80400010; // type:object size:0x4 scope:local",
        "pad_00_80004188_init = .init:0x80004188; // type:function size:0x1F34 scope:local",
    ]) + "\n"

    SRC = """\
PPC_NATIVE_OVERRIDE_VOID(8017214c, GX__SetTevOrder_8017214c, (uint32_t s), (s));
PPC_NATIVE_OVERRIDE_VOID(80170000, GX__SetDirtyState_80170000, (), ());
PPC_NATIVE_OVERRIDE(80180000, OS__Panic_801A2660_Cpu, uint32_t,
    (uint32_t a),
    (a));
PPC_NATIVE_OVERRIDE_VOID(80190000, PAD__Init_HLE, (), ());
REGISTER_NATIVE_FUNCTION(0x80191000, GX__SetTevOrder_8017214c);
#if 0  // ya apagado por WiiCompiled
PPC_NATIVE_OVERRIDE_VOID(80192000, GX__SetTevOrder_8017214c, (), ());
#endif
extern "C" void func_8017214C(CpuContext* ctx);
constexpr uint32_t kFifo = 0x804DE6E0u;   // FifoObj (NSMBW)
constexpr uint32_t kHorW = 0x804de644;    // HorVer.DispSizeX
constexpr uint32_t kHorY = 0x804de65c;    // HorVer+0x1C (PanSizeY)
constexpr uint32_t kDup = 0x805A0000u;    // Dup
constexpr uint32_t kRaro = 0x80123456u;
void f() { Memory::Write32(0x804DE6E4, 0); InvokeIndirectCpu(0x801E9A2Cu, c); }  // VISetNextFrameBuffer's blr
"""

    def make_tree(self, t):
        src = os.path.join(t, "runtime", "src", "hle")
        os.makedirs(src)
        os.makedirs(os.path.join(t, "runtime", "include"))
        with open(os.path.join(src, "gx.cpp"), "w", newline="\n") as f:
            f.write(self.SRC)
        os.makedirs(os.path.join(t, "projects", "brawl"))
        with open(os.path.join(t, "projects", "brawl", "MAP.txt"), "w") as f:
            f.write("8017214c GXSetTevOrder\n80123456 fn_80123456\n")
        sym = os.path.join(t, "symbols.txt")
        with open(sym, "w") as f:
            f.write(self.SYMS)
        return sym

    def test_retarget(self):
        from choya import retarget as R
        with tempfile.TemporaryDirectory() as t:
            sym = self.make_tree(t)
            r = R.retarget(t, simbolos=sym)
            out = open(os.path.join(t, "runtime-wiisports", "src", "hle", "gx.cpp")).read()
            self.assertIn("PPC_NATIVE_OVERRIDE_VOID(800e1a84, GX__SetTevOrder_8017214c", out)
            self.assertIn("PPC_NATIVE_OVERRIDE_VOID(800de85c, GX__SetDirtyState", out)   # → __GXSetDirtyState
            self.assertIn("PPC_NATIVE_OVERRIDE(800eefa8, OS__Panic_801A2660_Cpu", out)  # varias líneas
            self.assertIn("#if 0  // choya: Wii Sports no tiene esta funcion (80190000 PAD__Init_HLE)", out)
            self.assertIn("#if 0  // choya: repetido", out)                             # segundo GXSetTevOrder
            self.assertIn("PPC_NATIVE_OVERRIDE_VOID(80192000", out)                     # el #if 0 viejo no se toca
            self.assertIn("func_800E1A84", out)                                         # llamada movida
            self.assertIn("kFifo = 0x80412E50u", out)
            self.assertIn("kHorW = 0x80429174", out)                                    # campo por el ancla +0x1C
            self.assertIn("kHorY = 0x8042918c", out)
            self.assertIn("kDup = 0x805A0000u", out)                                    # nombre ambiguo: no se toca
            self.assertIn("kRaro = 0x80123456u", out)                                   # sin pista: no se toca
            self.assertIn("Write32(0x80412E54", out)                                    # dentro de FifoObj (+4)
            self.assertIn("InvokeIndirectCpu(0x801E9A2Cu", out)                         # sin pista segura
            self.assertTrue(os.path.exists(os.path.join(t, "runtime-wiisports", R.MARKER)))
            self.assertEqual(r.count("duplicado"), 1)
            self.assertEqual(r.count("desactivado"), 1)
            unresolved = {c.old for c in r.changes if c.kind == "sin_resolver"}
            self.assertEqual(unresolved, {0x805A0000, 0x80123456, 0x801E9A2C})
            self.assertIn("Sin resolver", R.report_markdown(r))
            # el original queda igual y una segunda corrida rehace la copia
            self.assertIn("8017214c", open(os.path.join(t, "runtime", "src", "hle", "gx.cpp")).read())
            R.retarget(t, simbolos=sym)

    def test_refuses_foreign_dir(self):
        from choya import retarget as R
        with tempfile.TemporaryDirectory() as t:
            sym = self.make_tree(t)
            os.makedirs(os.path.join(t, "runtime-wiisports"))
            with self.assertRaises(FileExistsError):
                R.retarget(t, simbolos=sym)

    def test_manifest_switch(self):
        from choya import retarget as R
        b = dol.BootInfo(START, INIT, 0x80378000, 0x8037A0E0, None)
        with tempfile.TemporaryDirectory() as t:
            p = os.path.join(t, "recomp.yml")
            with open(p, "w") as f:
                f.write(recomp.manifest_yaml("C:/wii/main.dol", "ab" * 32, b))
            self.assertTrue(R.update_manifest(p))
            y = open(p).read()
            self.assertIn("native_registration_root: runtime-wiisports/src", y)
            self.assertIn("    - runtime-wiisports/src/hle/gx\n\noutput:", y)
            self.assertEqual(y.count("runtime:"), 1)

    def test_bundled_symbols(self):
        from choya import retarget as R
        s = R.load_wiisports_symbols()
        self.assertEqual(s.get("GXInit"), 0x800DB234)
        self.assertEqual(s.get("FifoObj"), 0x80412E50)
        self.assertNotIn("pad_00_80004188_init", s.funcs)
