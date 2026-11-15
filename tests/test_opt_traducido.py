"""Optimización del código traducido (0.29.0): CHOYA_OPT=O3 / pgo-generar / pgo / pgo-O3 en build-wiisports-arm64.sh,
las dos ediciones de cmake/PublicProducts.cmake, el runtime de perfiles para aarch64 y el guardado periódico del
perfil en choya_gles.cpp."""
import os
import shutil
import subprocess
import tempfile
import unittest

from choya import retarget

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT = os.path.join(RAIZ, "scripts", "build-wiisports-arm64.sh")
RUNTIME_PGO = os.path.join(RAIZ, "scripts", "pgo-runtime-aarch64.sh")
LANZADOR = os.path.join(RAIZ, "scripts", "knulli", "Wii Sports (Choya).sh")
GLES = os.path.join(RAIZ, "choya", "data", "choya_gles.cpp")

PUBLIC_PRODUCTS = """function(mkw_apply_translated_compile_options target)
    target_compile_options(${target} PRIVATE
        -O2 ${MKW_TRANSLATED_PPC_FP_OPTIONS} -fno-slp-vectorize -w -pipe)
endfunction()
if(X)
    set(MKW_BASELINE_ARCH_FLAG -mcpu=native)
endif()
add_executable(WiiCompiled main.cpp)
mkw_configure_product(WiiCompiled)
"""


def leer(ruta):
    with open(ruta, encoding="utf-8") as f:
        return f.read()


class EdicionesCmake(unittest.TestCase):
    def test_opciones_extra_despues_de_O2(self):
        cambios = []
        out = retarget.apply_edits(PUBLIC_PRODUCTS, "cmake/PublicProducts.cmake", cambios)
        self.assertFalse([c for c in cambios if c.kind == "parche_fallido"], cambios)
        o2 = out.index("-O2 ${MKW_TRANSLATED_PPC_FP_OPTIONS}")
        extra = out.index("${CHOYA_OPT_TRADUCIDO})")
        self.assertLess(o2, extra)  # lo de CHOYA_OPT va después: -O3 gana sobre -O2
        self.assertIn("-mcpu=cortex-a53", out)

    def test_enlace_extra_solo_si_hay(self):
        out = retarget.apply_edits(PUBLIC_PRODUCTS, "cmake/PublicProducts.cmake", [])
        i = out.index("mkw_configure_product(WiiCompiled)")
        self.assertIn("if(CHOYA_ENLACE_EXTRA)", out[i:])
        self.assertIn("target_link_libraries(WiiCompiled PRIVATE ${CHOYA_ENLACE_EXTRA})", out[i:])


class ScriptDeCompilacion(unittest.TestCase):
    def modo(self, valor):
        """Corre solo el bloque que elige el modo y devuelve (código, BUILD, OPT_FLAGS, OPT_ENLACE)."""
        texto = leer(SCRIPT)
        ini = texto.index("BUILD=build-wiisports-arm64\n")
        fin = texto.index("esac\n", ini) + len("esac\n")
        prueba = ('WC=/wc\n' + texto[ini:fin] +
                  'echo "$BUILD|$OPT_FLAGS|$OPT_ENLACE|$OPT_MODO"\n')
        env = dict(os.environ, CHOYA_OPT=valor)
        r = subprocess.run(["bash", "-c", prueba], capture_output=True, text=True, env=env)
        partes = r.stdout.strip().split("|") if r.returncode == 0 else ["", "", "", ""]
        return (r.returncode, *partes)

    def test_sintaxis(self):
        for s in (SCRIPT, RUNTIME_PGO, LANZADOR):
            r = subprocess.run(["bash", "-n", s], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, (s, r.stderr))

    def test_modos(self):
        self.assertEqual(self.modo(""), (0, "build-wiisports-arm64", "", "", "O2"))
        self.assertEqual(self.modo("O3")[1:3], ("build-wiisports-arm64-o3", "-O3"))
        cod, build, flags, enlace, _ = self.modo("pgo-generar")
        self.assertEqual((cod, build, flags), (0, "build-wiisports-arm64-pgogen", "-fprofile-generate"))
        self.assertIn("libclang_rt.profile-aarch64.a", enlace)
        self.assertIn("-Wl,-u,__llvm_profile_runtime", enlace)
        cod, build, flags, enlace, _ = self.modo("pgo")
        self.assertEqual((cod, build, enlace), (0, "build-wiisports-arm64-pgo", ""))
        self.assertTrue(flags.startswith("-fprofile-use=/wc/choya-pgo/wiisports.profdata;"))
        self.assertTrue(self.modo("pgo-O3")[2].startswith("-O3;-fprofile-use="))
        self.assertNotEqual(self.modo("rapidisimo")[0], 0)

    def test_pasa_las_opciones_a_cmake(self):
        texto = leer(SCRIPT)
        self.assertIn("-DCHOYA_OPT_TRADUCIDO='$OPT_FLAGS'", texto)
        self.assertIn("-DCHOYA_ENLACE_EXTRA='$OPT_ENLACE'", texto)
        self.assertIn("llvm-profdata-21 merge", texto)
        self.assertIn("compilado.txt", texto)
        self.assertIn("-DCHOYA_COMPILADO='$COMPILADO'", texto)  # 0.30.0: también dentro del binario

    def test_modo_en_el_binario(self):
        out = retarget.apply_edits(PUBLIC_PRODUCTS, "cmake/PublicProducts.cmake", [])
        self.assertIn('file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/choya_compilado.cpp" @ONLY', out)
        self.assertIn('const char* const choya_compilado = \\"@CHOYA_COMPILADO@\\";', out)
        self.assertIn('target_sources(WiiCompiled PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/choya_compilado.cpp")', out)
        texto = leer(GLES)
        self.assertIn('extern "C" const char* const choya_compilado __attribute__((weak));', texto)
        self.assertIn('Log("compilado: código traducido con %s"', texto)
        self.assertIn("compilado: código traducido", leer(LANZADOR))
        if shutil.which("cmake") and shutil.which("c++"):
            # CMake de verdad genera el archivo con el modo (con espacios y paréntesis, como "pgo (perfil abc)").
            with tempfile.TemporaryDirectory() as tmp:
                with open(os.path.join(tmp, "main.cpp"), "w") as f:
                    f.write('#include <cstdio>\nextern "C" const char* const choya_compilado;\n'
                            'int main(){std::puts(choya_compilado);}\n')
                with open(os.path.join(tmp, "CMakeLists.txt"), "w") as f:
                    f.write("cmake_minimum_required(VERSION 3.20)\nproject(t CXX)\n"
                            "function(mkw_configure_product t)\nendfunction()\n"
                            "add_executable(WiiCompiled main.cpp)\n" + out[out.index("mkw_configure_product(WiiCompiled)"):])
                b = os.path.join(tmp, "b")
                subprocess.run(["cmake", "-S", tmp, "-B", b, "-DCHOYA_COMPILADO=pgo (perfil abc)"], check=True,
                               capture_output=True)
                subprocess.run(["cmake", "--build", b], check=True, capture_output=True)
                r = subprocess.run([os.path.join(b, "WiiCompiled")], capture_output=True, text=True)
                self.assertEqual(r.stdout.strip(), "pgo (perfil abc)")


class GuardadoDelPerfil(unittest.TestCase):
    def test_lanzador(self):
        texto = leer(LANZADOR)
        self.assertIn('export LLVM_PROFILE_FILE="$DIR/pgo/wiisports-%m.profraw"', texto)
        self.assertIn("compilado.txt", texto)

    def test_choya_gles(self):
        texto = leer(GLES)
        self.assertIn('extern "C" int __llvm_profile_write_file(void) __attribute__((weak));', texto)
        self.assertIn("if (__llvm_profile_write_file) std::thread(EscribirPerfilPgo).detach();", texto)

    def test_escritura_con_suma_y_contadores_a_cero(self):
        texto = leer(GLES)
        self.assertIn('extern "C" void __llvm_profile_reset_counters(void) __attribute__((weak));', texto)
        self.assertIn("if (r == 0 && __llvm_profile_reset_counters) __llvm_profile_reset_counters();", texto)

    def test_preparar_perfiles(self):
        """Vacíos fuera; de varios perfiles seguidos (0.29.0/0.30.x: el runtime agrega al final) solo el último."""
        import struct
        magia = struct.pack("<Q", 0xFF6C70726F667281) + struct.pack("<Q", 0x0100000000000000 | 10)
        with tempfile.TemporaryDirectory() as tmp:
            ent, sal = os.path.join(tmp, "e"), os.path.join(tmp, "s")
            os.makedirs(ent)
            open(os.path.join(ent, "vacio.profraw"), "wb").close()
            with open(os.path.join(ent, "varios.profraw"), "wb") as f:
                f.write(magia + b"A" * 100 + magia + b"B" * 100 + magia + b"C" * 104)
            with open(os.path.join(ent, "cortado.profraw"), "wb") as f:
                f.write(magia + b"A" * 100 + magia + b"B" * 100 + magia + b"C" * 10)
            with open(os.path.join(ent, "uno.profraw"), "wb") as f:
                f.write(magia + b"Z" * 50)
            r = subprocess.run(["python3", "-I", os.path.join(RAIZ, "scripts", "pgo-preparar.py"), ent, sal],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertEqual(sorted(os.listdir(sal)), ["cortado.profraw", "uno.profraw", "varios.profraw"])
            self.assertEqual(open(os.path.join(sal, "varios.profraw"), "rb").read(), magia + b"C" * 104)
            self.assertEqual(open(os.path.join(sal, "cortado.profraw"), "rb").read(), magia + b"B" * 100)
            self.assertEqual(open(os.path.join(sal, "uno.profraw"), "rb").read(), magia + b"Z" * 50)
        self.assertIn('python3 "$CHOYA/scripts/pgo-preparar.py" "$CRUDOS" "$PGO/crudos"', leer(SCRIPT))

    @unittest.skipUnless(os.environ.get("CHOYA_PRUEBA_PGO_RED") == "1" and shutil.which("clang") and
                         shutil.which("llvm-profdata"), "CHOYA_PRUEBA_PGO_RED=1 (baja compiler-rt de github.com)")
    def test_runtime_de_punta_a_punta(self):
        """Con el clang de la PC: compila el runtime, guarda un perfil con __llvm_profile_write_file y lo usa."""
        with tempfile.TemporaryDirectory() as tmp:
            env = dict(os.environ, CHOYA_PGO_CC="clang", CHOYA_PGO_CXX="clang++",
                       CHOYA_PGO_AR=shutil.which("llvm-ar") or "ar", CHOYA_PGO_TARGET="x86_64-linux-gnu")
            subprocess.run(["bash", RUNTIME_PGO, os.path.join(tmp, "rt")], check=True, env=env,
                           capture_output=True)
            lib = os.path.join(tmp, "rt", "libclang_rt.profile-aarch64.a")
            with open(os.path.join(tmp, "hot.cpp"), "w") as f:
                f.write("int hot(int x){int s=0;for(int i=0;i<x;i++)s+=(i%3)?i:-i;return s;}\n")
            with open(os.path.join(tmp, "main.cpp"), "w") as f:
                # Como choya_gles.cpp: escribir y poner los contadores a cero, dos veces, sin salir por exit().
                f.write('#include <cstdlib>\nextern "C" int __llvm_profile_write_file(void) __attribute__((weak));\n'
                        'extern "C" void __llvm_profile_reset_counters(void) __attribute__((weak));\n'
                        "int hot(int);\nint main(){int w=0,r=0;for(int k=0;k<2;++k){r+=hot(1000);"
                        "w|=__llvm_profile_write_file?__llvm_profile_write_file():9;__llvm_profile_reset_counters();}"
                        "_Exit(w|(r&0));}\n")
            c = lambda *a: subprocess.run(["clang++", *a], check=True, cwd=tmp, capture_output=True)
            c("-O2", "-fprofile-generate", "-c", "hot.cpp", "-o", "hot.o")
            c("-O2", "-c", "main.cpp", "-o", "main.o")
            c("-fuse-ld=lld", "main.o", "hot.o", "-Wl,-u,__llvm_profile_runtime", lib, "-o", "a")
            perfil = os.path.join(tmp, "pgo", "j-%m.profraw")  # como el lanzador: suma en el mismo archivo
            r = subprocess.run([os.path.join(tmp, "a")], env=dict(os.environ, LLVM_PROFILE_FILE=perfil))
            self.assertEqual(r.returncode, 0)  # el guardado explícito funcionó (sin atexit: _Exit)
            crudos = os.listdir(os.path.join(tmp, "pgo"))
            self.assertEqual(len(crudos), 1)
            subprocess.run(["llvm-profdata", "merge", "-o", "w.profdata", os.path.join("pgo", crudos[0])],
                           check=True, cwd=tmp)
            show = subprocess.run(["llvm-profdata", "show", "--counts", "--function=hot", "w.profdata"], check=True,
                                  cwd=tmp, capture_output=True, text=True).stdout
            self.assertIn("2000", show, show)  # las dos tandas sumadas una sola vez (1000 + 1000)
            c("-O2", "-fprofile-use=w.profdata", "-Werror=profile-instr-unprofiled", "-c", "hot.cpp", "-o", "h2.o")


if __name__ == "__main__":
    unittest.main()


class VersionEnLaConsola(unittest.TestCase):
    """0.31.2: el resumen dice qué lanzador y qué port hay en la consola, y hay un script que copia y comprueba."""

    def test_lanzador_con_la_version_del_paquete(self):
        import re
        version = re.search(r'^version = "([^"]+)"', leer(os.path.join(RAIZ, "pyproject.toml")), re.M).group(1)
        self.assertIn(f'LANZADOR_VERSION="{version}"', leer(LANZADOR))
        self.assertIn('echo "choya: lanzador $LANZADOR_VERSION; port', leer(LANZADOR))

    def test_port_con_version_txt(self):
        texto = leer(SCRIPT)
        self.assertIn('> "$PORT/wiisports-choya/version.txt"', texto)
        self.assertIn("Choya $VERSION_CHOYA", texto)

    def test_script_de_copia(self):
        ruta = os.path.join(RAIZ, "scripts", "copiar-a-consola.ps1")
        with open(ruta, "rb") as f:
            datos = f.read()
        self.assertTrue(datos.startswith(b"\xef\xbb\xbf"))  # con BOM: PowerShell 5 lo lee como UTF-8
        texto = datos.decode("utf-8-sig")
        for algo in ("/IS /IT", "Get-FileHash", "wiisports-choya\\version.txt", "Wii Sports (Choya).sh"):
            self.assertIn(algo, texto)
