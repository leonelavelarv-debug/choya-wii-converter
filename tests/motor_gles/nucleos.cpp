// Prueba del reparto de núcleos (0.36.0): imprime juego gx otros para cada caso.
#include "choya_gx_core.hpp"
#include <cstdio>
int main() {
  using choya::gx::repartir_nucleos;
  const struct { int n; bool gx, solo; } casos[] = {{4, true, false}, {4, true, true}, {4, false, false}, {3, false, false}, {8, true, false}};
  for (const auto& c : casos) {
    const auto r = repartir_nucleos(c.n, c.gx, c.solo);
    std::printf("%d %d %d %d %d %d\n", c.n, c.gx, c.solo, r.juego, r.gx, r.otros);
  }
  std::printf("prioridad %d\n", choya::gx::prioridad_gx());
  return 0;
}
