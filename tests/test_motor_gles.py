"""Prueba del motor GLES de Choya (paso 2a) en la PC: compila choya_gx.cpp contra cabeceras mínimas de aurora
(tests/motor_gles/lib) y dibuja cuadros sintéticos con Mesa (llvmpipe, GLES 3.2) leyendo los píxeles.
Se salta si no hay clang++, Mesa EGL o las cabeceras GX de aurora (BrawlRecompiled)."""
import ctypes.util
import os
import re
import shutil
import subprocess
import tempfile
import unittest

AQUI = os.path.dirname(__file__)
REPO = os.path.dirname(AQUI)
AURORA_INCLUDE = os.environ.get("CHOYA_AURORA_INCLUDE", os.path.expanduser(
    "~/BrawlRecompiled/wiicompiled/aurora-main/include"))
if not os.path.isdir(AURORA_INCLUDE):
    AURORA_INCLUDE = "/home/claude/ext/brawlrecomp/wiicompiled/aurora-main/include"


class TestMotorGles(unittest.TestCase):
    def test_cuadro_sintetico(self):
        if not shutil.which("clang++") or not os.path.isdir(AURORA_INCLUDE):
            self.skipTest("sin clang++ o sin cabeceras de aurora")
        if not ctypes.util.find_library("EGL") or not ctypes.util.find_library("GLESv2"):
            self.skipTest("sin Mesa EGL/GLES")
        with tempfile.TemporaryDirectory() as d:
            exe = os.path.join(d, "harness")
            data = os.path.join(REPO, "choya", "data", "aurora")
            subprocess.run(["clang++", "-std=c++20", "-I", data, "-I", AURORA_INCLUDE,
                            os.path.join(AQUI, "motor_gles", "harness.cpp"), os.path.join(data, "choya_gx_core.cpp"),
                            "-o", exe, "-ldl", "-lpthread"], check=True, capture_output=True)
            dump = os.path.join(d, "cuadro.bin")
            env = dict(os.environ, EGL_PLATFORM="surfaceless", CHOYA_PRUEBA_VOLCADO=dump)
            out = subprocess.run([exe], env=env, capture_output=True, text=True)
            if out.returncode == 2:
                self.skipTest("Mesa no dio contexto GLES 3.2: " + out.stdout.strip())
            self.assertIn("PRUEBA OK", out.stdout, out.stdout + out.stderr)
            # El volcado se vuelve a dibujar igual (rojo, verde, fondo azul; el segundo pase no se ve).
            ppm = os.path.join(d, "cuadro.ppm")
            out = subprocess.run([exe, dump, ppm], env=env, capture_output=True, text=True)
            self.assertIn("listo", out.stdout, out.stdout + out.stderr)
            with open(ppm, "rb") as f:
                raw = f.read()
            pix = raw[raw.index(b"255\n") + 4:]
            at = lambda x, y: tuple(pix[(y * 640 + x) * 3:(y * 640 + x) * 3 + 3])
            self.assertEqual((at(160, 240), at(480, 240), at(320, 10)), ((255, 0, 0), (0, 255, 0), (0, 0, 255)))
            # 0.23.3: el informe del motor dice dónde se va el tiempo de cada cuadro y cuántos cuadros dibuja.
            out = subprocess.run([exe, "repetir", dump, "305"], env=env, capture_output=True, text=True)
            self.assertIn("repetido", out.stdout, out.stdout + out.stderr)
            log = out.stdout + out.stderr
            m = re.search(r"motor: tiempo por cuadro dibujado ([\d.]+) ms \(CPU del hilo ([\d.]+) ms\): texturas ([\d.]+), "
                          r"subidas ([\d.]+), pases ([\d.]+), borrados ([\d.]+), dibujos ([\d.]+), copias del EFB ([\d.]+), "
                          r"pantalla ([\d.]+), cierre ([\d.]+), esperando a la GPU ([\d.]+), memoria de la Mali ([\d.]+)", log)
            self.assertIsNotNone(m, log[-3000:])
            total = float(m.group(1))
            partes = sum(float(m.group(i)) for i in range(3, 13))
            self.assertAlmostEqual(partes, total, delta=0.15 + 0.02 * total)  # las partes suman el total
            self.assertRegex(log, r"motor: cuadros por segundo: [\d.]+ dibujados por el motor")
            # 0.24.0: saltos del motor y tiempo de GPU por pase (1 de cada 30 cuadros, si el driver mide tiempos).
            self.assertRegex(log, r"motor: saltos: encendidos; [\d.]+ tomas por s")
            if "GL_EXT_disjoint_timer_query)" in log and "midiendo el tiempo de GPU por pase" in log:
                self.assertRegex(log, r"motor: GPU por pase \(\d+ cuadros medidos; suma [\d.]+ ms por cuadro\): "
                                      r"dibujos [\d.]+, copias del EFB [\d.]+, pantalla [\d.]+")
            self.assertRegex(log, r"motor: EFB sin recargar: (encendido|el driver no tiene); por cuadro: [\d.]+ borrados")
            self.assertNotIn("memoria de la Mali por partes", log)  # 0.24.0: apagada salvo CHOYA_GX_MALI=1
            # Caché de programas en disco: la primera vez compila y guarda, la segunda carga los binarios y dibuja igual.
            cache = os.path.join(d, "cache")
            env_cache = dict(env, CHOYA_GX_CACHE=cache)
            first = subprocess.run([exe], env=env_cache, capture_output=True, text=True)
            self.assertIn("PRUEBA OK", first.stdout, first.stdout + first.stderr)
            if "caché de programas apagado" in first.stdout:
                self.skipTest("Mesa sin binarios de programas")
            saved = sorted(os.listdir(cache))
            self.assertTrue(saved and all(n.endswith(".bin") for n in saved), saved)
            second = subprocess.run([exe], env=env_cache, capture_output=True, text=True)
            self.assertIn("PRUEBA OK", second.stdout, second.stdout + second.stderr)
            self.assertIn("programas cargados del caché de disco", second.stdout)
            self.assertEqual(sorted(os.listdir(cache)), saved)

            # Recursos convertidos: el conversor arma un assets.wlpk con texturas sintéticas (32x32 RGB565 en cuatro
            # colores e I8 gris) y el motor las encuentra por la clave de los datos GX, igual que aurora en el juego.
            import numpy as np
            from choya import gxtex, optimize, pack, profiles
            from tests.fixtures import encode_gx
            prof = profiles.get("rg35xxh")
            quad = np.zeros((32, 32, 4), np.uint8)
            quad[..., 3] = 255
            quad[:16, :16, 0] = 255                  # arriba a la izquierda rojo
            quad[:16, 16:, 1] = 255                  # arriba a la derecha verde
            quad[16:, :16, 2] = 255                  # abajo a la izquierda azul
            quad[16:, 16:, :3] = 255                 # abajo a la derecha blanco
            gray = np.full((32, 32, 4), 200, np.uint8)
            writer = pack.PackWriter(flags=pack.FLAG_GLES3)
            cases = []
            for fmt, img, name in ((gxtex.RGB565, quad, "cuatro"), (gxtex.I8, gray, "gris")):
                raw = encode_gx(fmt, img)
                tex = gxtex.GXTexture(name=name, fmt=fmt, width=32, height=32, mips=1, data=raw, source="modelo/" + name)
                conv = optimize.convert(tex, prof)
                flags = {optimize.OUT_R11_EAC: pack.TEXFLAG_SWIZZLE_I,
                         optimize.OUT_RG11_EAC: pack.TEXFLAG_SWIZZLE_IA}.get(conv.fmt, 0)
                writer.add(pack.Entry(kind=pack.KIND_TEXTURE, fmt=conv.fmt, count=conv.mips, flags=flags,
                                      key=pack.texture_key(raw, 32, 32, fmt), name=name, data=conv.data,
                                      width=conv.width, height=conv.height, extra0=fmt))
                path = os.path.join(d, name + ".gx")
                with open(path, "wb") as f:
                    f.write(raw)
                cases.append((path, fmt, conv.fmt))
            wl = os.path.join(d, "assets.wlpk")
            writer.write(wl, {"prueba": True})
            self.assertEqual(cases[0][2], optimize.OUT_ETC2_RGB8)  # de verdad comprimida en ETC2
            self.assertEqual(cases[1][2], optimize.OUT_R11_EAC)
            got = []
            for path, fmt, _ in cases:
                out = subprocess.run([exe, "paquete", wl, path, str(fmt), "32", "32"], env=env, capture_output=True,
                                     text=True)
                self.assertIn("cuadrantes", out.stdout, out.stdout + out.stderr)
                self.assertIn("texturas convertidas", out.stdout)
                got.append([int(x, 16) for x in out.stdout.split("cuadrantes")[1].split()])
            def near(c, rgb):
                return all(abs(((c >> s) & 0xFF) - v) <= 12 for s, v in zip((16, 8, 0), rgb))
            tl, tr, bl, br = got[0]
            self.assertTrue(near(tl, (255, 0, 0)) and near(tr, (0, 255, 0)) and near(bl, (0, 0, 255))
                            and near(br, (255, 255, 255)), [hex(x) for x in got[0]])
            self.assertTrue(all(near(c, (200, 200, 200)) for c in got[1]), [hex(x) for x in got[1]])
            # Una textura que no está en el paquete sigue por aurora.
            other = os.path.join(d, "otra.gx")
            with open(other, "wb") as f:
                f.write(encode_gx(gxtex.RGB565, quad[::-1].copy()))
            out = subprocess.run([exe, "paquete", wl, other, str(gxtex.RGB565), "32", "32"], env=env,
                                 capture_output=True, text=True)
            self.assertEqual(out.returncode, 3, out.stdout)

            # Borrado de aurora dentro del pase (GXCopyTex con borrado): como dibujo (por omisión) y con glClear, igual.
            for extra in ({}, {"CHOYA_GX_BORRAR_CON_GL": "1"}):
                out = subprocess.run([exe, "borrado"], env=dict(env, **extra), capture_output=True, text=True)
                self.assertIn("borrado 0000FF 000000", out.stdout, (extra, out.stdout + out.stderr))

            # Pantalla de carga: 40 lotes sin mostrar, cada uno con una copia distinta; no se pierde ninguna.
            out = subprocess.run([exe, "cola"], env=env, capture_output=True, text=True, timeout=60)
            m = re.search(r"cola (\d+) (\d+) (\d+) (\d+)", out.stdout)
            self.assertIsNotNone(m, out.stdout + out.stderr)
            self.assertEqual(m.group(3), "0", out.stdout)    # copias perdidas
            self.assertEqual(m.group(4), "40", out.stdout)   # todas hechas

            # Paleta sobre una copia del EFB (C8 con R*255 y C4 con R*15): la entrada 3 es magenta y la 5 cian.
            for variant, color in ((1, "FF00FF"), (2, "00FFFF")):
                out = subprocess.run([exe, "paleta", str(variant)], env=env, capture_output=True, text=True)
                self.assertIn(f"paleta {color} {color}", out.stdout, out.stdout + out.stderr)

            # TEV exacto (0.20.9): redondeo en enteros, desborde de 8 bits y resultado de la última etapa, matriz
            # posterior de las coordenadas, niebla y textura indirecta. En flotante (CHOYA_GX_TEV=flotante) los dos primeros cambian.
            for mode, exact, flot in ((0, "650000", "640000"), (1, "FEFEFE", None), (2, "FF0000", "FF0000"),
                                      (3, "808080", "808080"), (4, "00FF00", "00FF00")):
                out = subprocess.run([exe, "tev", str(mode)], env=env, capture_output=True, text=True)
                self.assertIn(f"tev {mode} {exact}", out.stdout, out.stdout + out.stderr)
                if flot:
                    out = subprocess.run([exe, "tev", str(mode)], env=dict(env, CHOYA_GX_TEV="flotante"),
                                         capture_output=True, text=True)
                    self.assertIn(f"tev {mode} {flot}", out.stdout, out.stdout + out.stderr)

            # Una copia hecha en un cuadro anterior (caras, texturas de luz) viaja en el volcado del siguiente.
            vieja = os.path.join(d, "vieja.bin")
            out = subprocess.run([exe, "copia-vieja", vieja], env=env, capture_output=True, text=True)
            self.assertIn("en vivo FF0000", out.stdout, out.stdout + out.stderr)
            self.assertIn("volcado listo", out.stdout, out.stdout + out.stderr)
            ppm = os.path.join(d, "vieja.ppm")
            out = subprocess.run([exe, vieja, ppm], env=env, capture_output=True, text=True)
            self.assertIn("listo", out.stdout, out.stdout + out.stderr)
            with open(ppm, "rb") as f:
                raw = f.read()
            pix = raw[raw.index(b"255\n") + 4:]
            self.assertEqual(tuple(pix[(240 * 640 + 320) * 3:(240 * 640 + 320) * 3 + 3]), (255, 0, 0))


if __name__ == "__main__":
    unittest.main()
