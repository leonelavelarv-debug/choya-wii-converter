"""scripts/build-wiisports-arm64.sh de punta a punta con un Docker falso (0.32.0).

El script corre de verdad (bash, recomp-native con el runtime real de WiiCompiled, dependencias con git, conversión de
recursos, manifiesto y publicación); solo lo que pasaría dentro de Docker (cmake, strip, readelf, llvm-profdata, clang)
lo hacen programas falsos que se pueden hacer fallar. Comprueba que:
- una compilación completa deja un port verificado con su manifiesto;
- un empaquetado que falla deja el port anterior intacto;
- un merge PGO que falla detiene la compilación sin usar el perfil anterior;
- un parche obligatorio que falla detiene la compilación antes de compilar;
- cambiar de revisión de WiiCompiled detiene la compilación.
Necesita el WiiCompiled real (runtime y aurora); sin él se salta.
"""
import json
import os
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import unittest

AQUI = os.path.dirname(os.path.abspath(__file__))
RAIZ = os.path.dirname(AQUI)
sys.path.insert(0, RAIZ)
sys.path.insert(0, AQUI)

from choya import huellas, manifiesto, recomp  # noqa: E402
from choya import retarget as R  # noqa: E402
import fixtures as fx  # noqa: E402
import test_recomp  # noqa: E402

REAL_WC = os.environ.get("CHOYA_WC_REAL", "/home/claude/ext/brawlrecomp/wiicompiled")  # WiiCompiled sin tocar
SCRIPT = os.path.join(RAIZ, "scripts", "build-wiisports-arm64.sh")

DOCKER = r'''#!/usr/bin/env python3
import os, subprocess, sys
a = sys.argv[1:]
if a[:1] in (["info"], ["build"]):
    sys.exit(0)
if a[:2] == ["image", "inspect"]:
    print("sha256:" + "f" * 64); sys.exit(0)
if a[:1] != ["run"]:
    sys.exit("docker falso: " + " ".join(a))
i, cwd, env = 1, None, dict(os.environ)
while a[i].startswith("-"):
    if a[i] == "--rm":
        i += 1; continue
    if a[i] == "-w":
        cwd = a[i + 1]
    elif a[i] == "-e":
        k, v = a[i + 1].split("=", 1); env[k] = v
    i += 2
i += 1  # la imagen
env["PATH"] = os.path.dirname(os.path.abspath(__file__)) + os.pathsep + env["PATH"]
sys.exit(subprocess.run(a[i:], cwd=cwd, env=env).returncode)
'''

CMAKE = r'''#!/usr/bin/env python3
import os, sys
a = sys.argv[1:]
if "-S" in a:
    b = a[a.index("-B") + 1]
    os.makedirs(b, exist_ok=True)
    defs = [x[2:] for x in a if x.startswith("-D")]
    with open(os.path.join(b, "CMakeCache.txt"), "w") as f:
        for d in defs:
            k, v = d.split("=", 1)
            f.write(f"{k.split(':')[0]}:STRING={v}\n")
        f.write("AURORA_SDL3_VERSION:STRING=3.2.10\n")
    open(os.path.join(b, "build.ninja"), "w").close()
    sys.exit(0)
if "--build" in a:
    b = a[a.index("--build") + 1]
    if "clean" in a:
        for n in ("WiiCompiled",):
            p = os.path.join(b, n)
            if os.path.exists(p): os.remove(p)
        sys.exit(0)
    if os.environ.get("FALSO_CMAKE") == "falla":
        print("x.cpp:1:1: error: falso"); sys.exit(1)
    cache = open(os.path.join(b, "CMakeCache.txt")).read()
    open(os.path.join(b, "WiiCompiled"), "w").write("ELF aarch64 falso\n" + cache)
    os.makedirs(os.path.join(b, "_deps", "png"), exist_ok=True)
    open(os.path.join(b, "_deps", "png", "libpng16.so.16.43.0"), "w").write("so")
    open(os.path.join(b, "dsp_coef.bin"), "wb").write(b"\x01\x02")
    sys.exit(0)
sys.exit("cmake falso: " + " ".join(a))
'''

STRIP = r'''#!/usr/bin/env python3
import os, shutil, sys
if os.environ.get("FALSO_STRIP") == "falla":
    sys.exit("strip: falla a propósito")
a = sys.argv[1:]
shutil.copyfile(a[-1], a[a.index("-o") + 1])
'''

READELF = r'''#!/usr/bin/env python3
import sys
if "-d" in sys.argv:
    print(" 0x0000000000000001 (NEEDED)             Shared library: [libpng16.so.16]")
else:
    print("  000000: Rev: 1  Flags: none  Index: 2  Cnt: 2  Name: GLIBC_2.34")
'''

CLANG = r'''#!/usr/bin/env python3
print("Ubuntu clang version 21.1.8 (++20250101)")
'''

PROFDATA = r'''#!/usr/bin/env python3
import os, sys
a = sys.argv[1:]
if a[0] == "merge":
    if os.environ.get("FALSO_PROFDATA") == "falla":
        print("error: wiisports.profraw: unsupported instrumentation profile format version", file=sys.stderr)
        sys.exit(1)
    salida = a[a.index("-o") + 1]
    open(salida, "wb").write(b"PERFIL" + b"".join(open(p, "rb").read() for p in a[a.index("-o") + 2:]))
    sys.exit(0)
print("Total functions: 7")
'''


def _ejecutable(ruta, texto):
    with open(ruta, "w") as f:
        f.write(texto)
    os.chmod(ruta, os.stat(ruta).st_mode | stat.S_IEXEC)


def _git(repo, *args):
    subprocess.run(["git", "-C", repo, *args], check=True, capture_output=True,
                   env=dict(os.environ, GIT_AUTHOR_NAME="p", GIT_AUTHOR_EMAIL="p@p", GIT_COMMITTER_NAME="p",
                            GIT_COMMITTER_EMAIL="p@p"))


@unittest.skipUnless(os.path.isdir(os.path.join(REAL_WC, "runtime")) and os.path.isdir(os.path.join(REAL_WC, "aurora-main"))
                     and shutil.which("git") and shutil.which("bash"), "falta el WiiCompiled real, git o bash")
class TestScriptDeCompilacion(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.base = tempfile.mkdtemp()
        repo = os.path.join(cls.base, "plantilla", "BrawlRecompiled")
        wc = os.path.join(repo, "wiicompiled")
        for d in ("runtime", "aurora-main", "projects"):
            shutil.copytree(os.path.join(REAL_WC, d), os.path.join(wc, d),
                            ignore=shutil.ignore_patterns("wiisports", "build*"))
        _git(repo, "init", "-q")
        _git(repo, "add", "-A")
        _git(repo, "commit", "-qm", "WiiCompiled")
        # Un disco falso con lo que recomp-prep y los parches buscan (entrada, SDA, arena, boot.bin, tabla de SC).
        disco = os.path.join(cls.base, "plantilla", "RSPE01")
        sysd = os.path.join(disco, "DATA", "sys")
        os.makedirs(sysd)
        os.makedirs(os.path.join(disco, "DATA", "files"))
        with open(os.path.join(disco, "DATA", "files", "a.bin"), "w") as f:
            f.write("recurso\n")
        init = [fx.ppc_lis(1, 0x804D), fx.ppc_ori(1, 1, 0x9C10), fx.ppc_lis(2, 0x804C), fx.ppc_ori(2, 2, 0x7A20),
                fx.ppc_lis(13, 0x804C), fx.ppc_ori(13, 13, 0x5380)]
        datos = b"\0" * 0x40 + b"\x00JPN\x00\x01USA\x00\x02EUR\x00" + b"\0" * 0xB1
        with open(os.path.join(sysd, "main.dol"), "wb") as f:
            f.write(fx.build_dol({test_recomp.START: [fx.ppc_bl(test_recomp.START, test_recomp.INIT), 0x4E800020],
                                  test_recomp.INIT: init + [0x4E800020]}, data=datos))
        with open(os.path.join(sysd, "boot.bin"), "wb") as f:
            f.write(b"RSPE01\x00\x01" + bytes(0x438))
        with open(os.path.join(sysd, "fst.bin"), "wb") as f:
            f.write(bytes(0x1A2B4))
        recomp.prep(disco, wc, force=True)
        gen = os.path.join(wc, "generated")
        os.makedirs(os.path.join(gen, "build_shards"))
        os.makedirs(os.path.join(gen, "functions"))
        with open(os.path.join(gen, "build_shards", "shards.cmake"), "w") as f:
            f.write("# falso\n")
        with open(os.path.join(gen, "functions", "f_80006124.cpp"), "w") as f:
            f.write("// código traducido falso\n")
        falsos = os.path.join(cls.base, "falsos")
        os.makedirs(falsos)
        for nombre, texto in (("docker", DOCKER), ("cmake", CMAKE), ("aarch64-linux-gnu-strip", STRIP),
                              ("aarch64-linux-gnu-readelf", READELF), ("clang-21", CLANG),
                              ("llvm-profdata-21", PROFDATA)):
            _ejecutable(os.path.join(falsos, nombre), texto)
        cls.falsos = falsos

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.base)

    def setUp(self):
        self.t = tempfile.mkdtemp(dir=self.base)
        shutil.copytree(os.path.join(self.base, "plantilla"), os.path.join(self.t, "p"), symlinks=True)
        self.repo = os.path.join(self.t, "p", "BrawlRecompiled")
        self.wc = os.path.join(self.repo, "wiicompiled")
        self.disco = os.path.join(self.t, "p", "RSPE01")
        self.home = os.path.join(self.t, "home")
        os.makedirs(self.home)
        self.port = os.path.join(self.home, "choya-port")

    def tearDown(self):
        shutil.rmtree(self.t, ignore_errors=True)

    def compilar(self, **env):
        e = dict(os.environ, HOME=self.home, CHOYA_DISCO=self.disco,
                 PATH=self.falsos + os.pathsep + os.environ["PATH"])
        for k in ("CHOYA_OPT", "CHOYA_PAQUETE", "CHOYA_SIN_PAQUETE", "CHOYA_PGO_DIR", "CHOYA_PGO_REUSAR",
                  "CHOYA_DEPS_ACTUALIZAR", "FALSO_STRIP", "FALSO_CMAKE", "FALSO_PROFDATA"):
            e.pop(k, None)
        e.update(env)
        r = subprocess.run(["bash", SCRIPT, self.wc], capture_output=True, text=True, env=e, timeout=600)
        return r.returncode, r.stdout + r.stderr

    def test_compilacion_completa_y_empaquetado_fallido(self):
        code, out = self.compilar()
        self.assertEqual(code, 0, out[-3000:])
        m, problemas = manifiesto.verificar(self.port)
        self.assertEqual(problemas, [])
        rutas = {e["ruta"] for e in m["archivos"]}
        for r in ("wiisports-choya/WiiCompiled", "wiisports-choya/lib/libpng16.so.16", "wiisports-choya/assets.wlpk",
                  "Wii Sports (Choya).sh", "wiisports-choya/version.txt", "wiisports-choya/compilado.txt"):
            self.assertIn(r, rutas)
        self.assertEqual(m["compilacion"]["modo"], "O2")
        self.assertIn("21.1.8", m["compilador"]["clang"])
        self.assertEqual(m["compilador"]["glibc_requerida"], "GLIBC_2.34")
        self.assertTrue(m["parches"]["ok"])
        self.assertEqual(m["parches"]["resumen"]["obligatorio_fallido"], 0)
        self.assertEqual(m["recursos"]["estado"], "generado")
        self.assertEqual(m["dependencias"]["lock"]["estado"], "fijado ahora")
        self.assertTrue(m["dependencias"]["wiicompiled"]["reproducible"], m["dependencias"]["wiicompiled"])
        self.assertEqual(m["juego"]["codigo_traducido"]["archivos"], 2)
        self.assertIn(m["identidad"][:12], open(os.path.join(self.port, "wiisports-choya", "version.txt")).read())
        self.assertFalse(os.path.exists(self.port + ".nuevo"))
        # Segunda vez: el paquete se reutiliza (nada cambió) y la identidad del port es la misma.
        code, out = self.compilar()
        self.assertEqual(code, 0, out[-3000:])
        m2 = json.load(open(os.path.join(self.port, manifiesto.MANIFIESTO)))
        self.assertEqual(m2["recursos"]["estado"], "reutilizado")
        self.assertEqual(m2["identidad"], m["identidad"])
        # Un empaquetado que falla deja el port anterior como estaba.
        antes = huellas.huella_carpeta(self.port)
        code, out = self.compilar(FALSO_STRIP="falla", CHOYA_OPT="O3")
        self.assertNotEqual(code, 0)
        self.assertIn("strip/readelf", out)
        self.assertEqual(huellas.huella_carpeta(self.port), antes)

    def test_pgo_merge_fallido_detiene(self):
        crudos = os.path.join(self.t, "choya-pgo")
        os.makedirs(crudos)
        with open(os.path.join(crudos, "wiisports-1.profraw"), "wb") as f:
            f.write(struct.pack("<QQ", 0xFF6C70726F667281, 10) + b"z" * 32)
        code, out = self.compilar(CHOYA_OPT="pgo", CHOYA_PGO_DIR=crudos, CHOYA_SIN_PAQUETE="1")
        self.assertEqual(code, 0, out[-3000:])
        m = json.load(open(os.path.join(self.port, manifiesto.MANIFIESTO)))
        perfil = os.path.join(self.wc, "choya-pgo", "wiisports.profdata")
        self.assertEqual(m["pgo"]["sha256"], huellas.sha256_archivo(perfil))
        self.assertEqual(m["recursos"]["estado"], "sin_paquete")
        self.assertIn("-fprofile-use=", m["compilacion"]["flags_codigo_traducido"])
        self.assertIn("SIN assets.wlpk", open(os.path.join(self.port, manifiesto.RESUMEN), encoding="utf-8").read())
        antes_port, antes_perfil = huellas.huella_carpeta(self.port), open(perfil, "rb").read()
        with open(os.path.join(crudos, "wiisports-2.profraw"), "wb") as f:
            f.write(struct.pack("<QQ", 0xFF6C70726F667281, 10) + b"w" * 32)
        code, out = self.compilar(CHOYA_OPT="pgo", CHOYA_PGO_DIR=crudos, CHOYA_SIN_PAQUETE="1", FALSO_PROFDATA="falla")
        self.assertNotEqual(code, 0)
        self.assertIn("perfil PGO", out)
        self.assertNotIn("4/6 Compilar", out)                       # no llegó a compilar con el perfil viejo
        self.assertEqual(open(perfil, "rb").read(), antes_perfil)
        self.assertEqual(huellas.huella_carpeta(self.port), antes_port)

    def test_parche_obligatorio_fallido_detiene(self):
        ruta = os.path.join(self.wc, "runtime", "src", "keybinds.cpp")
        _r, old, _n, _l = next(e for e in R.EDITS if e[0] == "src/keybinds.cpp")
        texto = open(ruta, encoding="utf-8").read()
        self.assertIn(old, texto)
        with open(ruta, "w", encoding="utf-8") as f:
            f.write(texto.replace(old, "// otra versión de WiiCompiled\n"))
        code, out = self.compilar(CHOYA_SIN_PAQUETE="1", CHOYA_DEPS_ACTUALIZAR="1")
        self.assertNotEqual(code, 0)
        self.assertIn("parche obligatorio no aplicado", out)
        self.assertIn("src/keybinds.cpp", out)
        self.assertNotIn("4/6 Compilar", out)
        self.assertFalse(os.path.exists(self.port))

    def test_revision_de_wiicompiled_cambiada_detiene(self):
        code, out = self.compilar(CHOYA_SIN_PAQUETE="1")
        self.assertEqual(code, 0, out[-3000:])
        with open(os.path.join(self.wc, "runtime", "NOTAS.txt"), "w") as f:
            f.write("nuevo\n")
        _git(self.repo, "add", "-A")
        _git(self.repo, "commit", "-qm", "git pull")
        antes = huellas.huella_carpeta(self.port)
        code, out = self.compilar(CHOYA_SIN_PAQUETE="1")
        self.assertNotEqual(code, 0)
        self.assertIn("WiiCompiled cambió desde que se fijó", out)
        self.assertEqual(huellas.huella_carpeta(self.port), antes)
        code, out = self.compilar(CHOYA_SIN_PAQUETE="1", CHOYA_DEPS_ACTUALIZAR="1")
        self.assertEqual(code, 0, out[-3000:])


if __name__ == "__main__":
    unittest.main()
