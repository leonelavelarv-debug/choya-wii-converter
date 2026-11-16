// Comprobación de las texturas comprimidas en la GPU. Por cada textura ETC2/EAC del paquete y por cada nivel:
//   1. ruta nativa: se sube comprimida, se dibuja tal cual (texelFetch del nivel) y se compara con el decodificador
//      de la CPU (decode_block_level);
//   2. ruta de respaldo: se sube descomprimida (como en un contexto sin ETC2) y se compara igual, con el swizzle de
//      intensidad aplicado;
//   3. RGB8A1: además se dibuja con descarte por alfa (como la prueba de alfa del TEV) y se cuentan los píxeles que
//      sobreviven, que deben ser justo los opacos.
// Requiere un contexto GLES 3 activo. Así se confirma en la Mali-G31 que el códec del conversor se lee como se
// escribió, y que el respaldo da lo mismo.
#pragma once
#include <cstdint>
#include <string>

#include "wl/pack.h"

namespace wl {

struct GpuCheckResult {
    int checked = 0, exact = 0, close = 0, failed = 0, skipped = 0;  // texturas (todos sus niveles por la ruta nativa)
    int levels = 0;                // niveles revisados
    int native_rejected = 0;       // texturas que el driver no aceptó comprimidas
    int fallback_checked = 0, fallback_failed = 0;
    int punch_textures = 0, punch_transparent = 0, punch_discard_failed = 0;
    int by_format[16] = {};       // revisadas por formato
    int bad_by_format[16] = {};   // con diferencias por formato
    std::string first_problem;    // nombre, formato, nivel y primer píxel distinto
    bool ok() const { return checked > 0 && failed == 0 && fallback_failed == 0 && punch_discard_failed == 0; }
};

// max_textures: 0 = todas. Escribe un renglón por formato en `report` si no es nullptr.
GpuCheckResult gpu_check_textures(const Pack& pack, int max_textures, std::string* report);

}  // namespace wl
