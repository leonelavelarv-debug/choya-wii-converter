"""Fiabilidad de la compilación del port (0.32.0): pruebas de comportamiento.

- Un parche obligatorio que falla detiene recomp-native; uno opcional no.
- Volver a aplicar parches compatibles funciona (ya aplicado) y no cambia nada.
- Un merge PGO fallido no deja usar en silencio el perfil anterior.
- Cambiar un recurso del disco invalida el assets.wlpk guardado (aunque tamaño y fecha sean iguales).
- Un port incompleto no reemplaza al anterior.
- La verificación detecta un archivo distribuido alterado (en Python y en PowerShell si hay pwsh).
- Las dependencias quedan fijadas y los cambios de Choya se distinguen de los ajenos.
"""
import io
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

AQUI = os.path.dirname(os.path.abspath(__file__))
RAIZ = os.path.dirname(AQUI)
sys.path.insert(0, RAIZ)
sys.path.insert(0, AQUI)

from choya import cli, huellas, manifiesto, recursos_cache  # noqa: E402
from choya import dependencias as deps  # noqa: E402
from choya import retarget as R  # noqa: E402
import test_recomp  # noqa: E402

REAL_WC = os.environ.get("CHOYA_WC_REAL", "/home/claude/ext/brawlrecomp/wiicompiled")  # WiiCompiled sin tocar


def _correr_cli(argv):
    """→ (código de salida, salida)."""
    buf = io.StringIO()
    code = 0
    with redirect_stdout(buf):
        try:
            cli.main(argv)
        except SystemExit as e:
            code = e.code if isinstance(e.code, int) else (0 if e.code is None else 1)
    return code, buf.getvalue()


# --- 1. parches obligatorios / opcionales -------------------------------------------------------------------------

class TestParches(unittest.TestCase):
    def _arbol(self, t, syms=None):
        caso = test_recomp.TestRetargetWiiSports()
        if syms is not None:
            caso.SYMS = syms
        return caso.build(t)

    def test_obligatorio_fallido_detiene_recomp_native(self):
        with tempfile.TemporaryDirectory() as t:
            sym = self._arbol(t)
            wc = os.path.join(t, "wc")
            code, out = _correr_cli(["recomp-native", wc, "--simbolos", sym])
            self.assertEqual(code, 3, out)
            self.assertIn("ERROR: parche obligatorio no aplicado", out)
            self.assertIn("qué hacer:", out)
            rp = json.load(open(os.path.join(wc, "projects", "wiisports", "parches.json"), encoding="utf-8"))
            self.assertFalse(rp["ok"])
            fall = [p for p in rp["parches"] if p["estado"] == "obligatorio_fallido"]
            self.assertTrue(fall)
            for p in fall:   # archivo, parche, motivo y acción en cada uno
                self.assertTrue(p["archivo"] and p["parche"] and p["motivo"] and p["accion"])
            self.assertTrue(any(p["estado"] == "aplicado" for p in rp["parches"]))
            self.assertIn("opcional_no_aplicable", {p["estado"] for p in rp["parches"]})
            md = open(os.path.join(wc, "projects", "wiisports", "retarget.md"), encoding="utf-8").read()
            self.assertIn("Parches obligatorios no aplicados (detienen la compilación)", md)

    def test_estructura_del_sdk_incompatible_detiene(self):
        syms = test_recomp.TestRetargetWiiSports.SYMS.replace("DefaultThread = .bss:0x804201F8; // type:object size:0x318",
                                                               "DefaultThread = .bss:0x804201F8; // type:object size:0x300")
        self.assertNotEqual(syms, test_recomp.TestRetargetWiiSports.SYMS)
        with tempfile.TemporaryDirectory() as t:
            sym = self._arbol(t, syms)
            r = R.retarget(os.path.join(t, "wc"), simbolos=sym)
            self.assertEqual([s[0] for s in R.estructuras_incompatibles(r)], ["DefaultThread"])
            self.assertFalse(R.resultado_parches(r)["ok"])

    def test_opcional_no_detiene_y_obligatorio_si(self):
        cambios = []
        R.apply_edits("// nada que ver\n", "src/settings_overlay.cpp", cambios)
        self.assertTrue(cambios)
        self.assertEqual({c.kind for c in cambios}, {"parche_omitido"})
        self.assertEqual({R.estado_parche(c) for c in cambios}, {"opcional_no_aplicable"})
        cambios = []
        R.apply_edits("// nada que ver\n", "src/keybinds.cpp", cambios)
        self.assertEqual({R.estado_parche(c) for c in cambios}, {"obligatorio_fallido"})
        self.assertIn("revisión", R.accion_parche(cambios[0]))

    def test_texto_repetido_es_fallo_con_motivo(self):
        rel, old, _new, label = next(e for e in R.EDITS if e[0] == "src/keybinds.cpp")
        cambios = []
        R.apply_edits(old + "\n" + old, rel, cambios)
        self.assertEqual(cambios[0].kind, "parche_fallido")
        self.assertIn("2 veces", cambios[0].text)
        self.assertIn("ancla única", R.accion_parche(cambios[0]))

    def test_reaplicar_compatible(self):
        # Cada archivo de EDITS con el texto original de sus parches: la primera vez se aplican, la segunda ya están.
        por_archivo = {}
        for rel, old, _new, _label in R.EDITS:
            por_archivo.setdefault(rel, [])
            if old not in "".join(por_archivo[rel]):
                por_archivo[rel].append(old + "\n// ---\n")
        for rel, partes in por_archivo.items():
            texto = "".join(partes)
            c1, c2 = [], []
            una = R.apply_edits(texto, rel, c1)
            if any(c.kind == "parche_fallido" for c in c1):
                continue  # parches encadenados (el segundo necesita el primero): se prueban con el árbol real
            dos = R.apply_edits(una, rel, c2)
            self.assertEqual(una, dos, rel)
            self.assertTrue(all(R.estado_parche(c) == "ya_aplicado" for c in c2), (rel, [c.text for c in c2]))

    @unittest.skipUnless(os.path.isdir(os.path.join(REAL_WC, "aurora-main")), "falta el aurora de WiiCompiled")
    def test_reaplicar_aurora_real(self):
        with tempfile.TemporaryDirectory() as t:
            shutil.copytree(os.path.join(REAL_WC, "aurora-main"), os.path.join(t, "aurora-main"))
            c1, c2 = [], []
            R.patch_aurora(t, c1)
            self.assertEqual([c for c in c1 if c.kind in ("parche_fallido", "parche_omitido")], [])
            antes = huellas.huella_carpeta(os.path.join(t, "aurora-main"))
            R.patch_aurora(t, c2)
            self.assertEqual([c for c in c2 if c.kind in ("parche_fallido", "parche_omitido")], [])
            estados = {R.estado_parche(c) for c in c2 if c.text != "motor de Choya (archivo)"}
            self.assertEqual(estados, {"ya_aplicado"})
            self.assertEqual(huellas.huella_carpeta(os.path.join(t, "aurora-main")), antes)

    @unittest.skipUnless(os.path.isdir(os.path.join(REAL_WC, "runtime")), "falta el runtime de WiiCompiled")
    def test_runtime_real_solo_falla_lo_del_disco(self):
        # Con el WiiCompiled real y sin recomp-prep, los únicos obligatorios fallidos son los que necesitan el disco.
        with tempfile.TemporaryDirectory() as t:
            for d in ("runtime", "aurora-main", "projects"):
                shutil.copytree(os.path.join(REAL_WC, d), os.path.join(t, d), ignore=shutil.ignore_patterns("wiisports"))
            r = R.retarget(t)
            motivos = {c.text for c in R.fallidos_obligatorios(r)}
            self.assertEqual(motivos, {"falta correr recomp-prep"})
            self.assertEqual(R.estructuras_incompatibles(r), [])


# --- 2. PGO -------------------------------------------------------------------------------------------------------

FALSO_PROFDATA = r'''
import os, sys
modo = os.environ.get("FALSO_PROFDATA", "bien")
if sys.argv[1] == "merge":
    if modo == "merge_falla":
        print("error: no profile can be merged", file=sys.stderr); sys.exit(1)
    salida = sys.argv[sys.argv.index("-o") + 1]
    datos = b"".join(open(p, "rb").read() for p in sys.argv[sys.argv.index("-o") + 2:])
    open(salida, "wb").write(b"PERFIL" + datos)
    sys.exit(0)
if sys.argv[1] == "show":
    if modo == "show_falla":
        sys.exit(1)
    print("Instrumentation level: IR")
    print("Total functions: %d" % (0 if modo == "vacio" else 42))
    print("Maximum function count: 9")
'''


def _profraw(contenido: bytes) -> bytes:
    return struct.pack("<Q", 0xFF6C70726F667281) + struct.pack("<Q", 10) + contenido


class TestPgo(unittest.TestCase):
    def setUp(self):
        self.t = tempfile.mkdtemp()
        self.crudos = os.path.join(self.t, "crudos")
        os.makedirs(self.crudos)
        with open(os.path.join(self.crudos, "wiisports-aaaaaaaaaaaa-1.profraw"), "wb") as f:
            f.write(_profraw(b"x" * 64))
        self.falso = os.path.join(self.t, "falso_profdata.py")
        with open(self.falso, "w") as f:
            f.write(FALSO_PROFDATA)
        self.perfil = os.path.join(self.t, "pgo", "wiisports.profdata")

    def tearDown(self):
        shutil.rmtree(self.t)

    def correr(self, modo="bien", extra=()):
        env = dict(os.environ, FALSO_PROFDATA=modo)
        return subprocess.run([sys.executable, os.path.join(RAIZ, "scripts", "pgo-perfil.py"), "--crudos", self.crudos,
                               "--perfil", self.perfil, "--profdata", f"{sys.executable} {self.falso}",
                               "--clang", "Ubuntu clang version 21.1.8", *extra],
                              capture_output=True, text=True, env=env)

    def test_merge_fallido_no_reutiliza_el_anterior(self):
        r = self.correr()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        antes = open(self.perfil, "rb").read()
        meta = open(self.perfil + ".json").read()
        with open(os.path.join(self.crudos, "wiisports-aaaaaaaaaaaa-2.profraw"), "wb") as f:
            f.write(_profraw(b"y" * 64))
        r = self.correr("merge_falla")
        self.assertEqual(r.returncode, 1)                       # el script de compilación se detiene
        self.assertIn("no se usa", r.stdout)
        self.assertEqual(open(self.perfil, "rb").read(), antes)  # el anterior se conserva intacto…
        self.assertEqual(open(self.perfil + ".json").read(), meta)
        self.assertFalse(os.path.exists(self.perfil + ".trabajo"))  # …y no quedan temporales

    def test_perfil_ilegible_o_vacio_falla(self):
        self.assertEqual(self.correr("show_falla").returncode, 1)
        self.assertFalse(os.path.exists(self.perfil))
        self.assertEqual(self.correr("vacio").returncode, 1)
        self.assertFalse(os.path.exists(self.perfil))

    def test_perfil_nuevo_con_identidad(self):
        ident = os.path.join(self.t, "identidad.json")
        gen = os.path.join(self.t, "generar.json")
        json.dump({"huella": "b" * 64}, open(ident, "w"))
        json.dump({"codigo_traducido": "a" * 64}, open(gen, "w"))
        r = self.correr(extra=("--identidad", ident, "--generar", gen))
        self.assertEqual(r.returncode, 0, r.stdout)
        meta = json.load(open(self.perfil + ".json"))
        self.assertEqual(meta["sha256"], huellas.sha256_archivo(self.perfil))
        self.assertEqual(meta["funciones"], 42)
        self.assertIn("21.1.8", meta["clang"])
        self.assertEqual(meta["codigo_traducido"], "b" * 64)
        self.assertTrue(any("cambió desde la última compilación pgo-generar" in a for a in meta["avisos"]))

    def test_reusar_a_proposito(self):
        self.assertEqual(self.correr().returncode, 0)
        r = self.correr("merge_falla", extra=("--reusar",))   # no junta nada
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("a propósito", json.load(open(self.perfil + ".json"))["estado"])
        with open(self.perfil, "ab") as f:                       # alguien lo tocó: ya no es el validado
            f.write(b"!")
        self.assertEqual(self.correr(extra=("--reusar",)).returncode, 1)

    def test_clang_de_otra_version_no_se_reusa(self):
        self.assertEqual(self.correr().returncode, 0)
        r = subprocess.run([sys.executable, os.path.join(RAIZ, "scripts", "pgo-perfil.py"), "--crudos", self.crudos,
                            "--perfil", self.perfil, "--profdata", "nada", "--clang", "clang version 22.0.0",
                            "--reusar"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 1)
        self.assertIn("Clang 21", r.stdout)


# --- 3. recursos (assets.wlpk) --------------------------------------------------------------------------------------

FALSO_CONVERSOR = r'''
import os, sys
sys.path.insert(0, sys.argv[1])
from choya import pack
disco, salida = sys.argv[2], sys.argv[3]
modo = os.environ.get("FALSO_CONVERSOR", "bien")
if modo == "falla":
    sys.exit(2)
os.makedirs(salida, exist_ok=True)
if modo == "roto":
    open(os.path.join(salida, "assets.wlpk"), "wb").write(b"basura")
    sys.exit(0)
w = pack.PackWriter()
datos = open(os.path.join(disco, "files", "a.brres"), "rb").read()
w.add(pack.Entry(kind=1, fmt=0, count=1, flags=0, key=1, name="a", data=datos))
w.write(os.path.join(salida, "assets.wlpk"), {"perfil": sys.argv[sys.argv.index("-p") + 1]})
open(os.path.join(salida, "report.md"), "w").write("texturas: 1\n")
'''


class TestRecursos(unittest.TestCase):
    def setUp(self):
        self.t = tempfile.mkdtemp()
        self.disco = os.path.join(self.t, "RSPE01")
        os.makedirs(os.path.join(self.disco, "files"))
        os.makedirs(os.path.join(self.disco, "sys"))
        self.recurso = os.path.join(self.disco, "files", "a.brres")
        with open(self.recurso, "wb") as f:
            f.write(b"A" * 100)
        with open(os.path.join(self.disco, "sys", "main.dol"), "wb") as f:
            f.write(b"D" * 10)
        self.conv = os.path.join(self.t, "conv.py")
        with open(self.conv, "w") as f:
            f.write(FALSO_CONVERSOR)
        self.dest = os.path.join(self.t, "paquete")

    def tearDown(self):
        shutil.rmtree(self.t)

    def preparar(self, modo="bien", perfil="rg35xxh"):
        os.environ["FALSO_CONVERSOR"] = modo
        try:
            with redirect_stdout(io.StringIO()):
                return recursos_cache.preparar(self.disco, self.dest, perfil, ["--sin-animaciones"],
                                               conversor_cmd=[sys.executable, self.conv, RAIZ])
        finally:
            del os.environ["FALSO_CONVERSOR"]

    def test_cambio_de_recurso_invalida(self):
        e1 = self.preparar()
        self.assertEqual(e1["estado"], "generado")
        e2 = self.preparar()
        self.assertEqual(e2["estado"], "reutilizado")            # nada cambió: no se presenta como nuevo
        self.assertEqual(e2["sha256"], e1["sha256"])
        st = os.stat(self.recurso)
        with open(self.recurso, "wb") as f:                       # mismo tamaño, misma fecha, otro contenido
            f.write(b"B" * 100)
        os.utime(self.recurso, ns=(st.st_atime_ns, st.st_mtime_ns))
        e3 = self.preparar()
        self.assertEqual(e3["estado"], "generado")
        self.assertIn("fuente", e3["motivo"])
        self.assertNotEqual(e3["sha256"], e1["sha256"])
        e4 = self.preparar(perfil="ligero")                       # otra opción de conversión
        self.assertEqual(e4["estado"], "generado")
        self.assertIn("perfil", e4["motivo"])

    def test_conversion_fallida_no_usa_el_anterior(self):
        e1 = self.preparar()
        with open(self.recurso, "wb") as f:
            f.write(b"C" * 100)
        for modo in ("falla", "roto"):
            e = self.preparar(modo)
            self.assertEqual(e["estado"], "fallido", modo)
            self.assertNotIn("paquete", e)                         # no se ofrece ningún paquete
            self.assertEqual(huellas.sha256_archivo(os.path.join(self.dest, "assets.wlpk")), e1["sha256"])
        self.assertEqual([n for n in os.listdir(self.t) if n.startswith("paquete.")], [])  # sin temporales

    def test_paquete_tocado_se_reconvierte(self):
        self.preparar()
        with open(os.path.join(self.dest, "assets.wlpk"), "ab") as f:
            f.write(b"\0")
        e = self.preparar()
        self.assertEqual(e["estado"], "generado")
        self.assertIn("otro SHA-256", e["motivo"])

    def test_cli_externo_y_fallido(self):
        e = self.preparar()
        est = os.path.join(self.t, "estado.json")
        code, out = _correr_cli(["recursos", "--externo", e["paquete"], "--estado", est])
        self.assertEqual(code, 0, out)
        self.assertEqual(json.load(open(est))["estado"], "externo")
        code, out = _correr_cli(["recursos", os.path.join(self.t, "no-existe"), self.dest, "--estado", est])
        self.assertEqual(code, 1)
        self.assertEqual(json.load(open(est))["estado"], "fallido")


# --- 4. manifiesto, verificación y publicación del port ------------------------------------------------------------

def _armar_port(raiz, contenido=b"ELF-falso"):
    os.makedirs(os.path.join(raiz, "wiisports-choya", "lib"))
    with open(os.path.join(raiz, "wiisports-choya", "WiiCompiled"), "wb") as f:
        f.write(contenido)
    with open(os.path.join(raiz, "wiisports-choya", "lib", "libpng16.so.16"), "wb") as f:
        f.write(b"so")
    with open(os.path.join(raiz, "Wii Sports (Choya).sh"), "w") as f:
        f.write("#!/bin/bash\n")
    return manifiesto.crear(raiz, {"compilacion": {"modo": "O2"}, "recursos": {"estado": "sin_paquete",
                                                                              "motivo": "prueba"}})


class TestPort(unittest.TestCase):
    def test_verificacion_detecta_archivo_alterado(self):
        with tempfile.TemporaryDirectory() as t:
            m = _armar_port(t)
            self.assertEqual(manifiesto.verificar(t)[1], [])
            # Archivos del usuario junto al port (partidas, opciones, disco) no cuentan ni se tocan.
            with open(os.path.join(t, "wiisports-choya", "con-reverb"), "w") as f:
                f.write("")
            self.assertEqual(manifiesto.verificar(t)[1], [])
            exe = os.path.join(t, "wiisports-choya", "WiiCompiled")
            with open(exe, "r+b") as f:                           # un byte distinto, mismo tamaño
                f.write(b"X")
            self.assertEqual(manifiesto.verificar(t)[1], ["distinto wiisports-choya/WiiCompiled (otro SHA-256)"])
            os.remove(exe)
            self.assertEqual(manifiesto.verificar(t)[1], ["falta wiisports-choya/WiiCompiled"])
            self.assertIn("port", open(os.path.join(t, "wiisports-choya", "version.txt")).read())
            self.assertEqual(len(m["identidad"]), 64)

    def test_identidad_por_contenido(self):
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            ma, mb = _armar_port(a), _armar_port(b)
            self.assertEqual(ma["identidad"], mb["identidad"])     # mismo contenido, otra carpeta y otro momento
            rutas = {e["ruta"] for e in ma["archivos"]}
            self.assertIn("wiisports-choya/version.txt", rutas)
            self.assertNotIn("wiisports-choya/manifiesto.json", rutas)  # el manifiesto no se describe a sí mismo
        with tempfile.TemporaryDirectory() as c:
            self.assertNotEqual(_armar_port(c, b"otro")["identidad"], ma["identidad"])

    def test_empaquetado_fallido_conserva_el_port_anterior(self):
        with tempfile.TemporaryDirectory() as t:
            destino, nuevo = os.path.join(t, "choya-port"), os.path.join(t, "choya-port.nuevo")
            _armar_port(destino, b"version-anterior")
            antes = huellas.huella_carpeta(destino)
            _armar_port(nuevo, b"version-nueva")
            os.remove(os.path.join(nuevo, "wiisports-choya", "lib", "libpng16.so.16"))  # quedó a medias
            code, out = _correr_cli(["publicar-port", nuevo, destino])
            self.assertEqual(code, 1)
            self.assertIn("sigue con el port anterior", out)
            self.assertEqual(huellas.huella_carpeta(destino), antes)
            shutil.rmtree(nuevo)
            os.makedirs(nuevo)                                       # sin manifiesto: tampoco
            self.assertTrue(manifiesto.publicar(nuevo, destino))
            self.assertEqual(huellas.huella_carpeta(destino), antes)
            shutil.rmtree(nuevo)
            _armar_port(nuevo, b"version-nueva")                     # completo: ahora sí
            self.assertEqual(manifiesto.publicar(nuevo, destino), [])
            self.assertEqual(open(os.path.join(destino, "wiisports-choya", "WiiCompiled"), "rb").read(), b"version-nueva")
            self.assertFalse(os.path.exists(nuevo))
            self.assertFalse(os.path.exists(destino + ".anterior"))

    def test_manifiesto_desde_la_cli(self):
        with tempfile.TemporaryDirectory() as t:
            wc, build, port = os.path.join(t, "wc"), os.path.join(t, "wc", "build-x"), os.path.join(t, "port")
            os.makedirs(os.path.join(wc, "projects", "wiisports"))
            os.makedirs(build)
            with open(os.path.join(build, "CMakeCache.txt"), "w") as f:
                f.write("AURORA_SDL3_VERSION:STRING=3.2.0\nCHOYA_OPT_TRADUCIDO:STRING=-O3\nOTRA:BOOL=ON\n")
            json.dump({"ok": True, "resumen": {"aplicado": 3, "ya_aplicado": 0, "opcional_no_aplicable": 1,
                                               "obligatorio_fallido": 0}, "estructuras_incompatibles": 0,
                       "parches": [{"archivo": "a", "parche": "p", "estado": "opcional_no_aplicable",
                                    "motivo": "m"}]},
                      open(os.path.join(wc, "projects", "wiisports", "parches.json"), "w"))
            _armar_port(port)
            os.remove(os.path.join(port, "wiisports-choya", "manifiesto.json"))
            with open(os.path.join(port, "wiisports-choya", "necesita.txt"), "w") as f:
                f.write("NEEDED libc\nGLIBC_2.34\n")
            ident = os.path.join(t, "ident.json")
            json.dump({"huella": "c" * 64, "main_dol_sha256": "d" * 64}, open(ident, "w"))
            code, out = _correr_cli(["manifiesto", port, "--wc", wc, "--build", build, "--modo", "O3",
                                     "--flags=-O3", "--clang=clang version 21.1.8", "--identidad", ident])
            self.assertEqual(code, 0, out)
            m = json.load(open(os.path.join(port, "wiisports-choya", "manifiesto.json")))
            self.assertEqual(m["compilacion"]["cmake"], {"AURORA_SDL3_VERSION": "3.2.0", "CHOYA_OPT_TRADUCIDO": "-O3"})
            self.assertEqual(m["compilador"]["glibc_requerida"], "GLIBC_2.34")
            self.assertEqual(m["juego"]["main_dol_sha256"], "d" * 64)
            self.assertEqual(m["parches"]["resumen"]["aplicado"], 3)
            self.assertEqual(m["recursos"]["estado"], "sin_paquete")
            txt = open(os.path.join(port, "wiisports-choya", "manifiesto.txt"), encoding="utf-8").read()
            self.assertIn("SIN assets.wlpk", txt)
            self.assertEqual(_correr_cli(["verificar-port", port])[0], 0)


def _pwsh():
    for c in (os.environ.get("CHOYA_PWSH"), shutil.which("pwsh"), "/tmp/pwsh/pwsh"):
        if c and os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    return None


@unittest.skipUnless(_pwsh(), "sin PowerShell (pwsh)")
class TestCopiarAConsola(unittest.TestCase):
    PS = os.path.join(RAIZ, "scripts", "copiar-a-consola.ps1")

    def correr(self, origen, destino, *extra):
        return subprocess.run([_pwsh(), "-NoProfile", "-File", self.PS, "-Origen", origen, "-Destino", destino, *extra],
                              capture_output=True, text=True)

    def test_copia_verifica_y_no_borra(self):
        with tempfile.TemporaryDirectory() as t:
            origen, sd = os.path.join(t, "choya-port"), os.path.join(t, "sd", "roms", "ports")
            _armar_port(origen)
            partida = os.path.join(sd, "wiisports-choya", "nand", "partida.bin")
            os.makedirs(os.path.dirname(partida))
            with open(partida, "wb") as f:
                f.write(b"partida")
            r = self.correr(origen, sd)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("son iguales", r.stdout)
            self.assertEqual(open(partida, "rb").read(), b"partida")   # lo del usuario sigue ahí
            with open(os.path.join(sd, "wiisports-choya", "lib", "libpng16.so.16"), "wb") as f:
                f.write(b"SO")                                           # se altera un archivo distribuido
            r = self.correr(origen, sd, "-SoloVerificar")
            self.assertEqual(r.returncode, 1)
            self.assertIn("DISTINTO  wiisports-choya/lib/libpng16.so.16", r.stdout)
            os.remove(os.path.join(sd, "Wii Sports (Choya).sh"))
            r = self.correr(origen, sd, "-SoloVerificar")
            self.assertIn("FALTA     Wii Sports (Choya).sh", r.stdout)

    def test_origen_incompleto_no_se_copia(self):
        with tempfile.TemporaryDirectory() as t:
            origen, sd = os.path.join(t, "choya-port"), os.path.join(t, "sd")
            _armar_port(origen)
            os.makedirs(sd)
            os.remove(os.path.join(origen, "wiisports-choya", "WiiCompiled"))
            r = self.correr(origen, sd)
            self.assertEqual(r.returncode, 1)
            self.assertIn("no coincide con su propio manifiesto", r.stdout)
            self.assertEqual(os.listdir(sd), [])


# --- 5. dependencias ------------------------------------------------------------------------------------------------

@unittest.skipUnless(shutil.which("git"), "sin git")
class TestDependencias(unittest.TestCase):
    def git(self, *args):
        subprocess.run(["git", "-C", self.repo, *args], check=True, capture_output=True,
                       env=dict(os.environ, GIT_AUTHOR_NAME="p", GIT_AUTHOR_EMAIL="p@p", GIT_COMMITTER_NAME="p",
                                GIT_COMMITTER_EMAIL="p@p"))

    def setUp(self):
        self.t = tempfile.mkdtemp()
        self.repo = os.path.join(self.t, "BrawlRecompiled")
        self.wc = os.path.join(self.repo, "wiicompiled")
        rel, old, _new, _l = next(e for e in R.AURORA_EDITS if e[3] == "GX barato (configuración reusada)")
        self.rel = rel
        ruta = os.path.join(self.wc, "aurora-main", rel)
        os.makedirs(os.path.dirname(ruta))
        with open(ruta, "w", newline="\n") as f:
            f.write('#include "gx.hpp"\n\nnamespace x {\n' + old + "}\n")
        os.makedirs(os.path.join(self.wc, "runtime", "src"))
        with open(os.path.join(self.wc, "runtime", "src", "main.cpp"), "w") as f:
            f.write("int main() {}\n")
        self.git("init", "-q")
        self.git("add", "-A")
        self.git("commit", "-qm", "base")

    def tearDown(self):
        shutil.rmtree(self.t)

    def test_cambios_de_choya_y_ajenos(self):
        R.patch_aurora(self.wc, [])                               # Choya edita aurora y agrega lib/choya
        os.makedirs(os.path.join(self.wc, "runtime-wiisports"))
        open(os.path.join(self.wc, "runtime-wiisports", "x.cpp"), "w").close()
        w = deps.estado(self.wc)["wiicompiled"]
        self.assertEqual(w["cambios_ajenos"], [])
        self.assertTrue(w["reproducible"])
        self.assertGreaterEqual(w["cambios_choya"], 3)
        with open(os.path.join(self.wc, "aurora-main", self.rel), "a") as f:   # un cambio ajeno en un archivo
            f.write("// mío\n")                                                # que Choya también edita
        with open(os.path.join(self.wc, "runtime", "src", "main.cpp"), "a") as f:
            f.write("// mío\n")
        w = deps.estado(self.wc)["wiicompiled"]
        self.assertEqual(sorted(w["cambios_ajenos"]), sorted([f"M aurora-main/{self.rel}", "M runtime/src/main.cpp"]))
        self.assertFalse(w["reproducible"])
        self.assertIn("no basta", w["nota"])

    def test_lock_detiene_si_cambia_la_revision(self):
        ok, est, _ = deps.comprobar(self.wc)
        self.assertTrue(ok)
        self.assertEqual(est["lock"]["estado"], "fijado ahora")
        ok, est, _ = deps.comprobar(self.wc)
        self.assertEqual(est["lock"]["estado"], "igual")
        with open(os.path.join(self.wc, "runtime", "src", "otro.cpp"), "w") as f:
            f.write("\n")
        self.git("add", "-A")
        self.git("commit", "-qm", "otro")                           # p. ej. un git pull en WiiCompiled
        code, out = _correr_cli(["dependencias", self.wc])
        self.assertEqual(code, 4)
        self.assertIn("WiiCompiled cambió desde que se fijó", out)
        self.assertIn("checkout", out)
        code, out = _correr_cli(["dependencias", self.wc, "--actualizar"])
        self.assertEqual(code, 0, out)
        self.assertEqual(deps.comprobar(self.wc)[1]["lock"]["estado"], "igual")


if __name__ == "__main__":
    unittest.main()
