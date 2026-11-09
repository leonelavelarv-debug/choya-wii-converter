// Choya Wii Converter - el mando de la consola leído como en Nebula (SDL2 + mapeo de PortMaster).
#pragma once

// Estado del mando con los nombres de un game controller de SDL (south = botón de abajo, east = derecha...).
struct ChoyaMando {
    bool south, east, west, north;
    bool back, guide, start;
    bool leftshoulder, rightshoulder;
    bool dpup, dpdown, dpleft, dpright;
    float leftx, lefty;               // -1..1, +y abajo (como SDL)
    float lefttrigger, righttrigger;  // 0..1
};

// Lee el mando de la consola directo de /dev/input con el mapeo de PortMaster (SDL_GAMECONTROLLERCONFIG), con los
// números de botón como los cuenta SDL2, que es para lo que PortMaster hace sus mapeos (SDL3 los cuenta distinto y
// con el mismo mapeo A salía en X y B en L1). Devuelve false si no hay un mando con mapeo de PortMaster.
extern "C" bool choya_mando_leer(ChoyaMando* out);
// Nombre del mando que se lee así ("" si ninguno): SDL3 debe ignorarlo para no contarlo dos veces.
extern "C" const char* choya_mando_nombre();
