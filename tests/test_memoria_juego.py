"""Memoria del juego (choya_perfil.cpp): con una memoria "de la Wii" falsa en la PC se arman montones de MEM como los
de Wii Sports (un montón expandible grande con un hijo casi lleno, y uno de cuadro) y se revisa que:
  - una petición que no cabe en el hijo se anota y se le presta al montón grande (r3 cambia),
  - el bloque prestado vuelve al montón que lo tiene (MEMFreeToExpHeap recibe el grande), y los del hijo no cambian,
  - nunca presta un montón que está dentro del que se quedó sin espacio,
  - EGG::Heap::alloc bloqueado por disableAllocationAllBut se anota,
  - al cerrarse por un salto a 0 se escribe el árbol de montones."""
import os
import shutil
import subprocess
import tempfile
import unittest

AQUI = os.path.dirname(__file__)
REPO = os.path.dirname(AQUI)

PRUEBA = r"""
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
extern "C" uint8_t* choya_prueba_base = nullptr;
extern "C" void choya_vigila_memoria(uint32_t target, void* ctx);
extern "C" void choya_informe_memoria(uint32_t target, const void* ctx);
static void w32(uint32_t a, uint32_t v) { v = __builtin_bswap32(v); std::memcpy(choya_prueba_base + a, &v, 4); }
static void w16(uint32_t a, uint16_t v) { v = __builtin_bswap16(v); std::memcpy(choya_prueba_base + a, &v, 2); }
// Montón de MEM: cabecera de 0x3C, luego la del expandible (lista libre) o la de cuadro (head, tail).
static void heap(uint32_t h, uint32_t magic, uint32_t start, uint32_t end) {
  w32(h, magic); w32(h + 0x18, start); w32(h + 0x1C, end);
  w32(h + 0xC, 0); w32(h + 0x10, 0); w16(h + 0x14, 0); w16(h + 0x16, 4);  // lista de hijos (enlace en +4)
}
static void free_block(uint32_t b, uint32_t size, uint32_t next) {
  w16(b, 0x4652); w32(b + 4, size); w32(b + 8, 0); w32(b + 0xC, next);
}
static void list(uint32_t l, uint32_t first, uint32_t last) { w32(l, first); w32(l + 4, last); w16(l + 8, 1); w16(l + 0xA, 4); }
int main() {
  void* m = mmap(nullptr, 0x94000000ull, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (m == MAP_FAILED) return 2;
  choya_prueba_base = static_cast<uint8_t*>(m);
  const uint32_t A = 0x90000000u, B = 0x90010000u, C = 0x91000000u;
  heap(A, 0x45585048u, A + 0x4C, 0x90800000u);           // 8 MB, expandible
  free_block(0x90200000u, 4u << 20, 0);                  // 4 MB libres en un bloque
  w32(A + 0x3C, 0x90200000u);
  heap(B, 0x45585048u, B + 0x4C, 0x90030000u);           // 128 KB, hijo de A, casi lleno
  free_block(0x90028000u, 4096, 0);
  w32(B + 0x3C, 0x90028000u);
  heap(C, 0x46524D48u, C + 0x4C, 0x91100000u);           // de cuadro, 1 MB, con 64 KB libres
  w32(C + 0x3C, 0x910F0000u); w32(C + 0x40, 0x91100000u);
  list(SROOT, A, C); w32(A + 4, 0); w32(A + 8, C); w32(C + 4, A); w32(C + 8, 0);  // raíz: A, C
  list(A + 0xC, B, B); w32(B + 4, 0); w32(B + 8, 0);                              // hijos de A: B
  // Pila del juego: r1 → marco anterior con el LR guardado.
  uint32_t ctx[64] = {};
  ctx[1] = 0x80500000u; w32(0x80500000u, 0x80500040u); w32(0x80500044u, 0x801B5FC0u); w32(0x80500040u, 0);
  ctx[33] = 0x8006D000u;
  // 1) 200 KB no caben en B: se anota y lo presta A.
  ctx[3] = B; ctx[4] = 200 * 1024; ctx[5] = 32;
  choya_vigila_memoria(MEM_EXP, ctx);
  std::printf("prestamo r3=%08X\n", ctx[3]);
  // 2) Se devuelve con MEMFreeToExpHeap(B, bloque de A): va a A. Uno de B se queda en B.
  ctx[3] = B; ctx[4] = 0x90200010u;
  choya_vigila_memoria(MEM_FREE, ctx);
  std::printf("devuelto r3=%08X\n", ctx[3]);
  ctx[3] = B; ctx[4] = 0x90028010u;
  choya_vigila_memoria(MEM_FREE, ctx);
  std::printf("propio r3=%08X\n", ctx[3]);
  // 3) 5 MB no caben en A y B está dentro de A: nadie presta.
  ctx[3] = A; ctx[4] = 5u << 20; ctx[5] = 4;
  choya_vigila_memoria(MEM_EXP, ctx);
  std::printf("sin prestamo r3=%08X\n", ctx[3]);
  // 4) Lo que sí cabe no se anota; las peticiones chicas no se revisan.
  ctx[3] = A; ctx[4] = 1u << 20; choya_vigila_memoria(MEM_EXP, ctx);
  ctx[3] = B; ctx[4] = 8192; choya_vigila_memoria(MEM_EXP, ctx);
  std::printf("igual r3=%08X\n", ctx[3]);
  // 5) De cuadro: 100 KB no caben en 64 KB; se anota sin prestar.
  ctx[3] = C; ctx[4] = 100 * 1024; ctx[5] = 4; choya_vigila_memoria(MEM_FRM, ctx);
  // 6) EGG solo deja asignar de otro montón.
  w32(EGG_ONLY, 0x80600000u); w32(EGG_CUR, 0x80610000u);
  ctx[3] = 64; ctx[4] = 4; ctx[5] = 0; choya_vigila_memoria(EGG_ALLOC, ctx);
  // 7) Cierre por un salto a 0.
  ctx[3] = 0; choya_informe_memoria(0, ctx);
  return 0;
}
"""


class TestMemoriaJuego(unittest.TestCase):
    def test_montones(self):
        cxx = shutil.which("clang++") or shutil.which("g++")
        if not cxx:
            self.skipTest("sin compilador")
        from choya import retarget
        ws = retarget.load_wiisports_symbols()
        get = lambda n: f"0x{ws.get(n):08X}u"
        defs = {"SROOT": get("sRootList"), "MEM_EXP": get("MEMAllocFromExpHeapEx"),
                "MEM_FRM": get("MEMAllocFromFrmHeapEx"), "MEM_FREE": get("MEMFreeToExpHeap"),
                "EGG_ALLOC": get("alloc__Q23EGG4HeapFUliPQ23EGG4Heap"),
                "EGG_ONLY": get("sAllocatableHeap__Q23EGG4Heap"), "EGG_CUR": get("sCurrentHeap__Q23EGG4Heap")}
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(REPO, "choya", "data", "choya_perfil.cpp"), encoding="utf-8") as f:
                src = retarget.perfil_marcas(f.read(), ws)
            self.assertNotIn("0x0u /*CHOYA_SYM", src)
            with open(os.path.join(d, "perfil.cpp"), "w", encoding="utf-8") as f:
                f.write(src)
            with open(os.path.join(d, "prueba.cpp"), "w", encoding="utf-8") as f:
                f.write(PRUEBA)
            exe = os.path.join(d, "prueba")
            subprocess.run([cxx, "-std=c++20", "-O1", "-DCHOYA_PRUEBA_MEMORIA",
                            *[f"-D{k}={v}" for k, v in defs.items()],
                            os.path.join(d, "prueba.cpp"), os.path.join(d, "perfil.cpp"), "-o", exe, "-lpthread"],
                           check=True, capture_output=True)
            out = subprocess.run([exe], cwd=d, capture_output=True, text=True, timeout=30)
            self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
            o, e = out.stdout, out.stderr
            self.assertIn("prestamo r3=90000000", o)
            self.assertIn("devuelto r3=90000000", o)
            self.assertIn("propio r3=90010000", o)
            self.assertIn("sin prestamo r3=90000000", o)
            self.assertIn("igual r3=90010000", o)
            self.assertRegex(e, r"no cabe un bloque de 200 KB en el montón expandible 0x90010000 \(127 KB, libre 4 KB "
                                r"en 1 bloques, el mayor 4 KB\); se lo presta otro montón; lo pidió 8006D000")
            self.assertIn("801B5FC0", e)                       # la cadena de marcos
            self.assertIn("ningún otro montón tiene el espacio", e)
            self.assertIn("no cabe un bloque de 100 KB en el montón de cuadro 0x91000000", e)
            self.assertIn("EGG no deja asignar 64 bytes del montón EGG 0x80610000 (solo del 0x80600000", e)
            self.assertIn("montones del juego (la primera vez que no cupo un bloque)", e)
            self.assertIn("montones del juego (al cerrarse)", e)
            self.assertRegex(e, r"0x90000000 expandible 8191 KB: libre 4096 KB en 1 bloques, el mayor 4096 KB")
            self.assertRegex(e, r"0x90000000 expandible.*\n\[choya\]     0x90010000 expandible.*\n"
                                r"\[choya\]   0x91000000 de cuadro")       # el hijo debajo de su padre, con sangría
            self.assertIn("el juego saltó a 0 desde 8006D000", e)
            self.assertEqual(e.count("se lo presta"), 1)


if __name__ == "__main__":
    unittest.main()
