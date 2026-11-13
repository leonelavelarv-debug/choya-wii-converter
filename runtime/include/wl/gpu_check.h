// Comprobación de las texturas comprimidas en la GPU: cada textura ETC2/EAC del paquete se sube, se dibuja tal cual
// (texelFetch del nivel 0) y se compara con el decodificador de la CPU (decode_block_level). Requiere un contexto
// GLES 3 activo. Así se confirma en la Mali-G31 que el códec del conversor se lee como se escribió.
#pragma once
#include <cstdint>
#include <string>

#include "wl/pack.h"

namespace wl {

struct GpuCheckResult {
    int checked = 0, exact = 0, close = 0, failed = 0, skipped = 0;
    int by_format[16] = {};       // revisadas por formato
    int bad_by_format[16] = {};   // con diferencias por formato
    std::string first_problem;    // nombre, formato y primer píxel distinto
};

// max_textures: 0 = todas. Escribe un renglón por formato en `report` si no es nullptr.
GpuCheckResult gpu_check_textures(const Pack& pack, int max_textures, std::string* report);

}  // namespace wl
