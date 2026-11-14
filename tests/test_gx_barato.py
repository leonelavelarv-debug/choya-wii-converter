"""GX más barato por dibujo (0.22.2): la configuración del pipeline se rehúsa en vez de armarse desde cero y, con el
motor de Choya, el hash de 64 bits basta para reconocerla en el caché."""
import os
import shutil
import tempfile
import unittest

from choya import retarget as R

AURORA = "/home/claude/ext/brawlrecomp/wiicompiled/aurora-main"
REL = "lib/gx/command_processor.cpp"


@unittest.skipUnless(os.path.isfile(os.path.join(AURORA, REL)), "falta aurora de referencia")
class GxBaratoTest(unittest.TestCase):
    def test_parches_se_aplican(self):
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, "aurora-main", os.path.dirname(REL)))
            shutil.copy(os.path.join(AURORA, REL), os.path.join(d, "aurora-main", REL))
            saved, saved_ex = R.AURORA_EDITS, R.AURORA_EXCLUSIVOS
            R.AURORA_EXCLUSIVOS = []
            try:
                R.AURORA_EDITS = [e for e in saved if e[0] == REL]
                changes = []
                R.patch_aurora(d, changes)
            finally:
                R.AURORA_EDITS, R.AURORA_EXCLUSIVOS = saved, saved_ex
            self.assertNotIn("parche_fallido", [c.kind for c in changes])
            text = open(os.path.join(d, "aurora-main", REL), encoding="utf-8").read()
            self.assertIn("static PipelineConfig s_choyaConfig{};", text)
            self.assertNotIn("  PipelineConfig config{};\n  populate_pipeline_config(config, prim, fmt);", text)
            self.assertIn("choya::sin_gpu() || same_pipeline_config(entry.config, config)", text)
            self.assertIn("CHOYA_VERIFICAR_PIPELINE", text)
            self.assertIn("choya_gx.hpp", text)


if __name__ == "__main__":
    unittest.main()
