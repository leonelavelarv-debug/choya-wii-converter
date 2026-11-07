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
void g(CpuContext* c) { func_80777777(c); }
static void func_80666666(CpuContext*) {}  // definida aquí: no se toca
void h(CpuContext* c) { func_80666666(c); }
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
            self.assertIn("choya_ausente_80777777(c)", out)                             # función de otro juego
            stubs = open(os.path.join(t, "runtime-wiisports", R.MISSING_FILE)).read()
            self.assertIn('void choya_ausente_80777777(CpuContext*)', stubs)
            self.assertIn("static void func_80666666(CpuContext*) {}", out)
            self.assertIn("{ func_80666666(c); }", out)
            self.assertNotIn("80666666", stubs)
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


class TestRetargetWiiSports(unittest.TestCase):
    """Parches revisados a mano, desfases SDA y funciones deducidas por sus vecinas."""
    SYMS = "\n".join([
        "OSGetCurrentContext = .text:0x800EE974; // type:function size:0x10 scope:global",
        "OSSaveContext = .text:0x800EE9DC; // type:function size:0x80 scope:global",
        "OSLoadContext = .text:0x800EEA5C; // type:function size:0x100 scope:global",
        "OSInitContext = .text:0x800EEBC0; // type:function size:0x100 scope:global",
        "GXCPInterruptHandler = .text:0x800DC048; // type:function size:0x160 scope:global",
        "AlarmQueue = .sbss:0x804BEF50; // type:object size:0x8 scope:local",
        "PowerCallback = .sbss:0x804BEFD8; // type:object size:0x4 scope:local",
        "axDspSlaveLength = .sdata:0x804BD8A4; // type:object size:0x2 scope:global",
        "DefaultThread = .bss:0x804201F8; // type:object size:0x318 scope:local",
    ]) + "\n"
    FILES = {
        "include/system_bridge.h": "inline constexpr uint32_t kDefaultEntryAddress = 0x8000403Cu;\n",
        "src/system_bridge.cpp": (
            "    constexpr uint32_t kMem1ArenaLoDefault = 0x805B5160u;\n"
            "    uint32_t mem1ArenaHiDefault = 0x817DA5A0u;  // MEM1 end - Brawl FST\n"
            '    entries.push_back({0x80000000u, 0x52534245u, "Disc game code"}); // SMNE\n'),
        "src/hle/gx/gx_init.cpp": "    constexpr uint32_t kCpInterruptHandlerAddr = 0x801ee720u;\n",
        "src/hle/os/os_internal.h": "constexpr uint32_t kAlarmQueueOffsetFromR13 = 0x3DA8u;   // SPM AlarmQueue = 0x8056DB30\n",
        "src/hle/os/os_misc.cpp": (
            "PPC_NATIVE_OVERRIDE_VOID(801D7FCC, OSGetCurrentContext_801a0000, (), ());\n"   # dos vecinas con el
            "PPC_NATIVE_OVERRIDE_VOID(801D80B4, OSLoadContext_801a0100, (), ());\n"         # mismo desfase
            "extern \"C\" void func_801D8034(CpuContext* ctx);\n"
            "const uint32_t a = r13 - 0x3d00u;    // PowerCallback (SPM)\n"
            "Memory::Write32(ctx->gpr[13] - 0x3d00u, 0);\n"
            "Memory::Read16(r13 - 0x5904u) /* axDspSlaveLength */;\n"),
    }

    def build(self, t):
        for rel, body in self.FILES.items():
            path = os.path.join(t, "wc", "runtime", rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", newline="\n") as f:
                f.write(body)
        sym = os.path.join(t, "symbols.txt")
        with open(sym, "w") as f:
            f.write(self.SYMS)
        sysd = os.path.join(t, "disco", "sys")
        os.makedirs(sysd)
        init = [fx.ppc_lis(1, 0x804D), fx.ppc_ori(1, 1, 0x9C10),       # pila 0x804D9C10 → arena 0x804D9C20
                fx.ppc_lis(2, 0x804C), fx.ppc_ori(2, 2, 0x7A20),
                fx.ppc_lis(13, 0x804C), fx.ppc_ori(13, 13, 0x5380)]
        with open(os.path.join(sysd, "main.dol"), "wb") as f:
            f.write(boot_dol(init))
        with open(os.path.join(sysd, "boot.bin"), "wb") as f:
            f.write(b"RSPE01\x00\x01" + bytes(0x438))
        with open(os.path.join(sysd, "fst.bin"), "wb") as f:
            f.write(bytes(0x1A2B4))
        recomp.prep(os.path.join(t, "disco"), os.path.join(t, "wc"), simbolos=sym)
        return sym

    def test_wiisports_layer(self):
        from choya import retarget as R
        with tempfile.TemporaryDirectory() as t:
            sym = self.build(t)
            r = R.retarget(os.path.join(t, "wc"), simbolos=sym)
            out = lambda rel: open(os.path.join(t, "wc", "runtime-wiisports", rel)).read()
            self.assertIn("kDefaultEntryAddress = 0x80006124u;  // choya:", out("include/system_bridge.h"))
            sb = out("src/system_bridge.cpp")
            self.assertIn("kMem1ArenaLoDefault = 0x804D9C20u", sb)
            self.assertIn("mem1ArenaHiDefault = 0x817E5D40u", sb)             # 0x81800000 - align32(0x1A2B4)
            self.assertIn("{0x80000000u, 0x52535045u", sb)                    # "RSPE"
            self.assertIn("kCpInterruptHandlerAddr = 0x800dc048u", out("src/hle/gx/gx_init.cpp"))
            # r13 = 0x804C5380: AlarmQueue 0x804BEF50 → 0x6430, PowerCallback → 0x63A8, axDspSlaveLength → 0x7ADC
            self.assertIn("kAlarmQueueOffsetFromR13 = 0x6430u", out("src/hle/os/os_internal.h"))
            misc = out("src/hle/os/os_misc.cpp")
            self.assertIn("r13 - 0x63a8u;    // PowerCallback", misc)
            self.assertIn("gpr[13] - 0x63a8u, 0)", misc)                      # mismo desfase sin comentario
            self.assertIn("r13 - 0x7adcu) /* axDspSlaveLength */", misc)
            self.assertIn("func_800EE9DC", misc)                               # vecinos → OSSaveContext
            failed = {c.name for c in r.changes if c.kind == "parche_fallido"}
            # solo fallan los parches cuyo código no está en estos archivos de prueba
            self.assertEqual(failed, {"fabricante y revision (boot.bin)", "ID del juego para OSGetAppGamename",
                                      "GXTokenInterruptHandler", "__savedGXdata", "OldCPUFifo"})
            self.assertEqual(r.count("vecinos"), 1)
            self.assertEqual(r.count("sda"), 4)
            self.assertIn("✓", R.report_markdown(r))


class TestReactivate(unittest.TestCase):
    def test_reenable_absent_in_other_game(self):
        from choya import retarget as R
        src = (
            'extern "C" uint32_t OSInitFont_HLE_801b0d60(uint32_t f) { return 0; }\n'
            "#if 0  // SPM: OSInitFont: name not in SPM\n"
            "#if 0  // BRAWL: no matching function in the Brawl dol\n"
            "PPC_NATIVE_OVERRIDE(801B0D60, OSInitFont_HLE_801b0d60, uint32_t, (uint32_t f), (f));\n"
            "#endif\n#endif\n"
            "#if 0  // NSMBW: MKW-only body with hardcoded addresses\n"     # motivo distinto: no se toca
            "PPC_NATIVE_OVERRIDE(801B0E00, OSLoadFont_HLE, uint32_t, (), ());\n#endif\n"
            "#if 0  // BRAWL: no matching function in the Brawl dol\n"      # trae más código: no se toca
            'extern "C" void Algo() {}\n'
            "PPC_NATIVE_OVERRIDE(801B0F00, OSGetFontTexel_HLE, uint32_t, (), ());\n#endif\n"
            "#if 0  // BRAWL: no matching function in the Brawl dol\n"      # Wii Sports no la tiene
            "PPC_NATIVE_OVERRIDE(80215E68, PAD__Init_HLE, int32_t, (), ());\n#endif\n"
            "#if 0  // NSMBW: RFL is not linked into NSMBW; this read an MKW pointer\n"   # cuerpo apagado:
            'extern "C" uint32_t RFLiIsWorking_HLE_800bd860() { return 0; }\n#endif\n'   # no se reactiva
            "#if 0  // NSMBW: RFLiIsWorking is not linked into NSMBW (was MKW-only)\n"
            "PPC_NATIVE_OVERRIDE(800BD860, RFLiIsWorking_HLE_800bd860, uint32_t, (), ());\n#endif\n")
        ws = R.Symbols({"OSInitFont": 0x800F14C8, "OSLoadFont": 0x800F0BEC, "OSGetFontTexel": 0x800F0FCC,
                        "RFLiIsWorking": 0x80169700},
                       {}, {}, set())
        res = R._Resolver(ws, {})
        changes = []
        out = R.reactivate(src, "x.cpp", res, changes)
        self.assertEqual([c.name for c in changes], ["OSInitFont"])
        self.assertIn("#if 1  // SPM: OSInitFont: name not in SPM  // choya: Wii Sports sí tiene OSInitFont", out)
        self.assertIn("#if 1  // BRAWL: no matching function in the Brawl dol  // choya:", out)
        self.assertIn("#if 0  // NSMBW: MKW-only", out)
        self.assertEqual(out.count("#if 0  // BRAWL"), 2)
        report = R.report_markdown(R.RetargetResult("x", None, changes))   # sin dirección nueva: no falla
        self.assertIn("OSInitFont", report)
        self.assertNotIn(4, {l for l in R.disabled_lines(out) if "OSInitFont" in out.split("\n")[l - 1]})


class TestChoyaNatives(unittest.TestCase):
    def test_sensor_bar(self):
        from choya import retarget as R
        ws = R.Symbols({"WPADSetSensorBarPower": 0x800CFF18}, {}, {}, set())
        changes = []
        src = R.choya_natives(ws, {}, changes)
        self.assertIn("PPC_NATIVE_OVERRIDE(800CFF18, WPADSetSensorBarPower_choya", src)
        self.assertEqual(len(changes), 1)
        # si WiiCompiled ya la reemplaza, Choya no la duplica
        self.assertNotIn("PPC_NATIVE_OVERRIDE", R.choya_natives(ws, {0x800CFF18: "otro.cpp"}, []))


class TestEdits(unittest.TestCase):
    def test_aurora_offscreen(self):
        import os, shutil, tempfile
        from choya import retarget as R
        real = "/home/claude/ext/brawlrecomp/wiicompiled/aurora-main"
        if not os.path.exists(real):
            self.skipTest("sin aurora")
        with tempfile.TemporaryDirectory() as d:
            for rel in ("lib/dawn/BackendBinding.cpp", "lib/webgpu/gpu.cpp"):
                os.makedirs(os.path.dirname(os.path.join(d, "aurora-main", rel)), exist_ok=True)
                shutil.copy(os.path.join(real, rel), os.path.join(d, "aurora-main", rel))
            for _ in range(2):  # la segunda vez no debe duplicar nada
                changes = []
                R.patch_aurora(d, changes)
                self.assertEqual([c.kind for c in changes], ["edicion"] * len(R.AURORA_EDITS))
            text = open(os.path.join(d, "aurora-main/lib/dawn/BackendBinding.cpp")).read()
            self.assertEqual(text.count("choyaDisplay;"), 1)
            gpu = open(os.path.join(d, "aurora-main/lib/webgpu/gpu.cpp")).read()
            self.assertEqual(gpu.count("Null solo admite RenderAttachment"), 1)

    def test_save_states_arm(self):
        from choya import retarget as R
        real = "/home/claude/ext/brawlrecomp/wiicompiled/runtime/src/save_states.cpp"
        import os
        if not os.path.exists(real):
            self.skipTest("sin WiiCompiled")
        changes = []
        out = R.apply_edits(open(real).read(), "src/save_states.cpp", changes)
        self.assertEqual([c.kind for c in changes], ["edicion"] * 4)
        self.assertEqual(out.count("#if"), open(real).read().count("#if") + 3)

    def test_exact_once(self):
        from choya import retarget as R
        changes = []
        src = "enum Action : int {\n    kMinus,\n    kActionCount,\n};\n"
        out = R.apply_edits(src, "include/keybinds.h", changes)
        self.assertIn("kButtonB,", out)
        self.assertEqual([c.kind for c in changes], ["edicion"])
        changes = []
        self.assertEqual(R.apply_edits("nada que ver\n", "include/keybinds.h", changes), "nada que ver\n")
        self.assertEqual([c.kind for c in changes], ["parche_fallido"])
