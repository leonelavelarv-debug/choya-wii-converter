"""0.33.0: resolución interna elegible en la consola y saltos del motor separados por causa."""
import os
import re
import subprocess
import tempfile
import unittest

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LANZADOR = os.path.join(RAIZ, "scripts", "knulli", "Wii Sports (Choya).sh")
CORE = os.path.join(RAIZ, "choya", "data", "aurora", "choya_gx_core.cpp")


def bloque_resolucion():
    texto = open(LANZADOR, encoding="utf-8").read()
    ini = texto.index('if [ -z "${CHOYA_RESOLUCION:-}" ]; then')
    fin = texto.index("\n", texto.index('case "$CHOYA_RESOLUCION" in'))
    return texto[ini:fin]


class ResolucionInterna(unittest.TestCase):
    def valor(self, archivos=(), env=None):
        """Corre el bloque del lanzador y escribe el Config.toml como él: → resolution_multiplier."""
        texto = open(LANZADOR, encoding="utf-8").read()
        conf = texto[texto.index('cat > "$CFG" <<CONF'):texto.index("\nCONF", texto.index('cat > "$CFG" <<CONF')) + 5]
        with tempfile.TemporaryDirectory() as d:
            for a in archivos:
                open(os.path.join(d, a), "w").close()
            script = f'DIR="{d}"; GAME=/juego; CFG="{d}/Config.toml"\n{bloque_resolucion()}\n{conf}\ncat "$CFG"\n'
            e = {k: v for k, v in os.environ.items() if k != "CHOYA_RESOLUCION"}
            e.update(env or {})
            r = subprocess.run(["bash", "-c", script], capture_output=True, text=True, env=e)
            self.assertEqual(r.returncode, 0, r.stderr)
            return re.search(r"^resolution_multiplier = (\S+)$", r.stdout, re.M).group(1)

    def test_por_omision_igual_que_antes(self):
        self.assertEqual(self.valor(), "1")

    def test_archivos(self):
        self.assertEqual(self.valor(["resolucion-075"]), "0.75")
        self.assertEqual(self.valor(["resolucion-05"]), "0.5")
        self.assertEqual(self.valor(["resolucion-075", "resolucion-05"]), "0.5")

    def test_variable_manda_y_se_valida(self):
        self.assertEqual(self.valor(["resolucion-05"], {"CHOYA_RESOLUCION": "0.75"}), "0.75")
        self.assertEqual(self.valor(env={"CHOYA_RESOLUCION": "0.6"}), "1")  # WiiCompiled no acepta 0.6
        self.assertEqual(self.valor(env={"CHOYA_RESOLUCION": "1; rm -rf /"}), "1")

    def test_valores_que_acepta_wiicompiled(self):
        cabecera = "/home/claude/ext/brawlrecomp/wiicompiled/runtime/include/runtime_config.h"
        if not os.path.isfile(cabecera):
            self.skipTest("falta el runtime de WiiCompiled")
        lista = re.search(r"IsSupportedResolutionMultiplier.*?values\{([^}]*)\}", open(cabecera).read(), re.S).group(1)
        aceptados = {float(v.strip().rstrip("f")) for v in lista.split(",")}
        for v in ("1", "0.75", "0.5"):
            self.assertIn(float(v), aceptados)


class SaltosPorCausa(unittest.TestCase):
    def test_tres_contadores(self):
        texto = open(CORE, encoding="utf-8").read()
        self.assertIn("r.cullAll = pc.cullMode == GX_CULL_ALL;", texto)
        self.assertIn("r.skipLine = pc.lineMode != 0;", texto)
        self.assertIn("++g_stats.skippedCull;", texto)
        self.assertIn("++g_stats.skippedInstanced;", texto)
        self.assertIn("cull todo (no pintan)", texto)


if __name__ == "__main__":
    unittest.main()
