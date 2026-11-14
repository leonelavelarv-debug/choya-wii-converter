// Hilo GX de Choya (0.23.0): el procesador de comandos de aurora corre en otro núcleo, como el modo de doble núcleo
// de Dolphin. El hilo del juego solo escribe el FIFO de la API (barato) y lo entrega en lotes; el hilo GX lo decodifica
// (registros BP/CP/XF, dibujos, pipelines, texturas) y alimenta al motor de Choya.
//
// Orden exacto: lo que antes aurora procesaba al llamar a drain() (cada GXEnd, cada lista) se entrega en ese mismo
// punto; lo que antes cambiaba el estado de inmediato (GXApplyBPReg, GXSetSourceVtxDesc...) se anota en el punto ya
// entregado, antes del FIFO pendiente, igual que antes. Las funciones que leen o cambian el estado del procesador de
// forma directa (copias del EFB, GXDrawDone, inicio y fin de cuadro, lecturas) toman el procesador: esperan a que el
// hilo GX termine lo entregado y corren como antes en el hilo del juego (CHOYA_HILO_EXCLUSIVO).
//
// CHOYA_GX_HILO=1 lo enciende (solo con el motor de Choya sin Dawn).
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace choya::hilo {

bool activo() noexcept;     // CHOYA_GX_HILO=1 y el motor de Choya sin GPU
bool en_hilo() noexcept;    // este hilo es el hilo GX
bool exclusivo() noexcept;  // el hilo del juego tiene el procesador (el hilo GX está parado)
// El hilo del juego encola en vez de procesar.
inline bool asincrono() noexcept { return activo() && !en_hilo() && !exclusivo(); }

// fifo::drain(): cierra el tramo del FIFO de la API y lo entrega si ya hay bastante.
void fifo_listo() noexcept;
// GXCallDisplayList: la lista se copia (puede ser prestada) y va después del FIFO entregado.
void lista(const void* data, uint32_t nbytes, bool bigEndian) noexcept;
// GXApplyBPReg: se aplica en el punto ya entregado, antes del FIFO pendiente (como antes).
void bp(uint8_t reg, uint32_t value) noexcept;
// Cambio directo del estado del procesador (GXSetSourceVtxDesc, GXSetZScaleOffset...), en el punto ya entregado.
using Cierre = void (*)(const void* datos);
void cierre(Cierre fn, const void* datos, size_t size) noexcept;
template <class F>
inline void diferir(const F& f) noexcept {
  static_assert(std::is_trivially_copyable_v<F>, "el cierre se copia byte a byte");
  static_assert(alignof(F) <= 16, "alineación de más");
  cierre([](const void* d) { (*static_cast<const F*>(d))(); }, &f, sizeof(F));
}

// Toma el procesador para el hilo del juego: entrega lo cerrado, espera a que el hilo GX termine y deja que el código
// de aurora corra como antes (drain() procesa en este hilo) hasta el fin del alcance.
class Exclusivo {
public:
  explicit Exclusivo(const char* quien) noexcept;
  ~Exclusivo() noexcept;
  Exclusivo(const Exclusivo&) = delete;
  Exclusivo& operator=(const Exclusivo&) = delete;

private:
  bool tomado_ = false;
};

void cuadro() noexcept;   // fin de cuadro: estadísticas cada 300 cuadros
void detener() noexcept;  // aurora_shutdown
int tid_juego() noexcept; // para repartir los núcleos (0 si no se sabe)
int tid_gx() noexcept;

} // namespace choya::hilo

// Para el hilo GX: el cuerpo original de GXApplyBPReg (command_processor.cpp) y de la lista (GXDispList.cpp).
extern "C" void choya_gx_aplicar_bp(uint8_t reg, uint32_t value);
extern "C" void choya_gx_procesar_lista(const uint8_t* data, uint32_t nbytes, bool bigEndian);
// Para el runtime y aurora.cpp (sin incluir este archivo).
extern "C" bool choya_gx_hilo_en_hilo();
extern "C" void choya_gx_hilo_esperar();
// El runtime dice dónde está la RAM del juego (banco 0: MEM1, 1: MEM2): las listas de ahí no se copian.
extern "C" void choya_gx_hilo_memoria_juego(int banco, const void* base, uint64_t size);

#define CHOYA_HILO_EXCLUSIVO() ::choya::hilo::Exclusivo choyaHiloExclusivo(__func__)
