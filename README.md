# Choya Wii Converter

Motor que adapta los recursos de juegos de Wii para que corran ligeros en la **Anbernic RG35XX H** con KNULLI y PortMaster.

Se usa desde la línea de comandos como `choya` (`python -m choya`).

Tiene dos partes:

| Parte | Dónde corre | Qué hace |
| --- | --- | --- |
| Conversor (`choya/`, Python) | Tu PC con Windows 11 | Lee el disco extraído, abre los archivos comprimidos y los U8, decodifica texturas GX, las reduce y recodifica a ETC2/EAC, convierte el audio y escribe `assets.wlpk` con un reporte |
| Runtime (`runtime/`, C++17) | La consola (aarch64) | Mapea el paquete en memoria, cambia cada textura GX del juego por la optimizada (caché por clave), la sube a GLES y reproduce el audio |

El repositorio **no incluye datos de ningún juego**. Necesitas tu propia copia.

## 1. Instalar el conversor (Windows 11)

```powershell
pip install numpy pillow scipy
cd choya-wii-converter
python -m choya profiles
```

`scipy` es opcional y solo mejora el cambio de frecuencia del audio.

## 2. Extraer el disco con Dolphin

En Dolphin, haz clic derecho en el juego, elige **Propiedades → Sistema de archivos**, haz clic derecho en el disco y elige **Extraer disco completo**. Debe quedar una carpeta con `DATA/files` y `DATA/sys` (o `files` y `sys`).

## 3. Convertir

```powershell
python -m choya scan  C:\wii\RSPE01
python -m choya convert C:\wii\RSPE01 C:\wii\salida --preview
```

- `salida/assets.wlpk` es el paquete para la consola.
- `salida/report.md` compara memoria antes y después, muestra los formatos usados y las texturas con menor calidad (PSNR).
- `--preview` guarda PNG y WAV del resultado para revisarlos en la PC.

### Perfiles (`-p`)

| Perfil | Texturas 3D | Interfaz | Formatos | Audio |
| --- | --- | --- | --- | --- |
| `rg35xxh` (predeterminado) | 256 px | 512 px | ETC2 / EAC | música en DSP-ADPCM original, efectos cortos a PCM mono de 32 kHz |
| `ligero` | 128 px | 256 px | ETC2 / EAC | 22 kHz |
| `calidad` | 512 px | 1024 px | ETC2 / EAC | 32 kHz estéreo |
| `gles2` | 256 px | 512 px | RGB565 / RGBA4444 / RGBA5551 / LA8 | 32 kHz |

### Qué decide el optimizador por textura

| Origen GX | Alfa | Salida GLES 3 | bpp |
| --- | --- | --- | --- |
| CMPR, RGB565, RGB5A3, RGBA8, C4/C8/C14X2 | sin alfa | ETC2_RGB8 | 4 |
| las mismas | con alfa | ETC2_RGBA8_EAC | 8 |
| I4, I8 | intensidad en RGBA | R11_EAC + swizzle RRRR | 4 |
| IA4, IA8 | intensidad + alfa | RG11_EAC + swizzle RRRG | 8 |

**Respaldo por calidad.** Después de comprimir, mide la calidad (PSNR). Si queda por debajo de 28 dB en 3D o de 32 dB en menús, usa RGB565 o RGBA8 sin pérdida de bloques. Las texturas de 16×16 px o menos (tablas de color, puntos de UI) siempre quedan en RGBA8, porque comprimirlas no ahorra casi nada. El reporte lista cada caso en "Sin compresión por calidad".

Las texturas de interfaz (rutas con `timg/`, `lyt`, `layout`, `menu`) se reducen menos, porque la pantalla es de 640×480 igual que la Wii. Los mips se regeneran desde la imagen reducida, y el alfa se reduce premultiplicado para evitar halos.

## 4. Compilar el runtime para la consola

En WSL2 (Ubuntu) con Docker Desktop encendido:

```sh
./scripts/build-aarch64.sh
```

Usa Debian 12 (glibc 2.36). Si en la consola aparece `GLIBC_2.3x not found`, revisa la versión de KNULLI con `ldd --version`. Salen `wl_view`, `wl_info` y `wl_selftest` en `runtime/build-aarch64/`.

Para probar en la PC sin GPU: `cmake -S runtime -B runtime/build -DWL_WITH_GL=OFF && cmake --build runtime/build`.

## 5. Probar en la consola (puerta 2 del plan)

```powershell
python -m choya package C:\wii\ports --pack-dir C:\wii\salida --files C:\wii\RSPE01\DATA\files ^
    --runtime runtime\build-aarch64\wl_view --binary wl_view
```

Copia `Wii Sports.sh` y la carpeta `wiisports/` a `ports/` en la microSD. Al abrirlo, `wl_view` sube todas las texturas, imprime en `log.txt` el tiempo y la memoria, y deja recorrerlas con la cruceta (L1/R1 saltan 10, A cambia el zoom, Start sale). Si alguna textura sale negra o rota, el registro dice cuál.

`wl_info assets.wlpk -l` revisa el paquete por SSH sin abrir la pantalla.

## 6. Ver modelos 3D en la consola (puerta 1)

Desde la versión 0.2, `convert` también lee los modelos MDL0 de los BRRES. Los guarda en el paquete ya listos para la GPU: posiciones en la pose de reposo, normales, color de vértice, UV y submallas por material con su textura principal.

- **En la PC:** con `--preview`, cada modelo se exporta a `salida/preview/modelos/*.obj`, con sus texturas. Ábrelos con el Visor 3D de Windows o con Blender para comprobar la geometría antes de ir a la consola.
- **En el reporte:** la sección "Modelos 3D" cuenta modelos, triángulos, objetos omitidos y avisos del lector.
- **En la consola:** `choya_model` muestra los modelos con cámara orbital y escribe los FPS en `log.txt`:

```powershell
python -m choya package C:\wii\ports --title "Choya Modelos" --folder choyamodel --pack-dir C:\wii\salida --runtime C:\wii\bin\choya_model --binary choya_model
```

| Botón | Acción |
| --- | --- |
| Cruceta o stick izquierdo | Girar la cámara |
| A / B | Acercar / alejar |
| L1 / R1 | Modelo anterior / siguiente |
| X | Activar o desactivar el descarte de caras traseras |
| Y | Material TEV / sombreado simple |
| Select | Texturas sí / no (modo simple) |
| Start | Salir |

Desde la 0.3, cada material se dibuja con su **TEV** traducido a GLSL ES 3.00: etapas, texturas por unidad, coordenadas de reflejo calculadas desde la normal, registros de color y konstantes, tablas de intercambio, prueba de alfa, mezcla y profundidad. El botón Y alterna entre TEV y el sombreado simple para comparar. La iluminación del canal COLOR0 es aproximada (luz fija), y aún no se aplican las matrices de textura (escala/rotación de UV) de cada material.

### Dibujar modelos en la PC sin la consola (WSL2)

`choya_render` usa el mismo código de render que `choya_model` y guarda PNG con Mesa (llvmpipe). En Ubuntu:

```sh
sh scripts/build-pc-render.sh
mkdir -p /mnt/c/wii/render
runtime/build-pc/choya_render /mnt/c/wii/salida/assets.wlpk /mnt/c/wii/render bwg_pin --max 5
```

Agrega `--simple` para ver el sombreado simple y `--size 1024` para imágenes más grandes.

## 7. Conectar con la capa GX del juego

El juego (recompilado o decompilado de ogws) llama a `GXInitTexObj` con un puntero a datos GX. La capa de compatibilidad hace esto:

```cpp
wl::Pack pack;  pack.open(getenv("WL_PACK"));
wl::TextureCache cache(pack,
    [](const wl::PackEntry& e, const uint8_t* d) { return wl::gl_upload(e, d, /*gles3=*/true); },
    wl::gl_delete);

// dentro de GXLoadTexObj:
if (uint32_t tex = cache.get({ptr, width, height, gx_format}))
    glBindTexture(GL_TEXTURE_2D, tex);
else
    decodificar_por_software(ptr, ...);   // textura que no estaba en el paquete
```

La clave es `crc32(nivel 0 GX) | ancho | alto | formato`, igual en Python (`pack.texture_key`) y en C++ (`wl::texture_key`). Si el juego reutiliza la misma dirección para otra textura, la caché lo detecta y vuelve a buscar.

Para audio: `pack.find_audio("Sound/strm/bgm.brstm")` y `wl::AudioStream` dan PCM16 listo para SDL. La música se guarda en su DSP-ADPCM original (3.5 veces más chica que PCM16) y se decodifica al vuelo, con bucle. `wl::dsp_decode` sirve también para lo que siga en BRSAR.

## 8. Recompilar Wii Sports con WiiCompiled (puerta 3)

[WiiCompiled](https://github.com/patchzyy/Wiicompiled) traduce el código PowerPC del `main.dol` a C++. Choya no trae nada de WiiCompiled (es GPLv3): solo le prepara el proyecto.

```powershell
choya recomp-prep D:\Wii\RSPE01 C:\choya\BrawlRecompiled\wiicompiled
```

Escribe en `projects\wiisports\`:

| Archivo | Qué es |
|---|---|
| `recomp.yml` | manifiesto: bases SDA (r13/r2) leídas de `__init_registers`, entrada `__start` 0x80006124, SHA-256 de tu `main.dol` |
| `MAP.txt` | 14 121 funciones con nombre, de los símbolos de [ogws](https://github.com/doldecomp/ogws) (CC0, incluidos en `choya/data`) |
| `native\` | carpeta vacía para las funciones nativas propias de Wii Sports |
| `hle_cobertura.md` | qué funciones nativas de WiiCompiled existen en Wii Sports y en qué dirección |

Solo acepta Wii Sports USA Rev 1 (el mismo `main.dol` que ogws). Con `--simbolos` se le da otro `symbols.txt` o `MAP.txt`.

### Mover el runtime de WiiCompiled a Wii Sports

```powershell
choya recomp-native C:\choya\BrawlRecompiled\wiicompiled
```

El runtime de WiiCompiled tiene ~400 funciones nativas (gráficos, sistema, disco, control, sonido) y decenas de variables del SDK, todas en direcciones de otros juegos (Brawl, NSMBW, MKW, SPM). El comando:

1. Copia `runtime\` a `runtime-wiisports\` (el original no se toca; la copia se rehace cada vez).
2. Busca cada función nativa por su nombre en los símbolos de ogws y le pone la dirección de Wii Sports.
3. Desactiva con `#if 0` las que Wii Sports no tiene (por ejemplo el control de GameCube) y las repetidas.
4. Mueve las variables del SDK que el comentario nombra (`// FifoObj`, `// HorVer+0x1C`, `// HorVer.DispSizeX`).
5. Aplica los parches revisados a mano para Wii Sports: entrada `__start`, inicio y fin de la memoria (MEM1 arena, calculados de tu `main.dol` y `fst.bin`), ID del disco (`boot.bin`), manejadores de interrupción de GX, tarea del DSP de sonido, `__DVDFSInit`, tablas de SC (buscadas en tu `main.dol`), etc.
6. Recalcula los desfases desde r13/r2 (`r13 - 0x3d00 // PowerCallback`) con la base SDA de Wii Sports.
7. Deduce funciones sin nombre por sus vecinas: si las dos funciones nativas más cercanas se movieron con el mismo desfase y ese desfase cae justo en el inicio de una función de Wii Sports, es esa.
8. Comprueba que los objetos del SDK de 2006 (`OSThread`, `OSContext`, `GXFifoObj`, `GXData`) miden lo que el runtime espera.
9. Cambia `recomp.yml` para que el traductor use `runtime-wiisports\src`.
10. Renombra las llamadas a funciones de otros juegos que Wii Sports no tiene (`choya_ausente_XXXXXXXX`) y las define en `src/choya_ausentes.cpp`: así enlaza, y si alguna llegara a llamarse el programa se detiene diciendo cuál.
11. Reactiva las funciones nativas que WiiCompiled apagó solo porque Brawl, SPM o NSMBW no las tenían (`#if 0  // BRAWL: no matching function...`) cuando Wii Sports sí las tiene, como `OSInitFont` o las de NAND y caché.
12. Agrega funciones nativas propias para lo que Wii Sports usa y los juegos de WiiCompiled no (`src/choya_wiisports_hle.cpp`); la primera es `WPADSetSensorBarPower`, que en la consola enciende la barra de sensores por un registro de hardware.
13. Hace unos pocos cambios de código revisados a mano (cada uno debe encontrar su texto exacto o se anota sin tocar nada): botón **B** en el teclado (S o B), porque Wii Sports lo pide desde la pantalla de título; el teclado y los mandos hacen de Wii Remote 1 (BrawlRecompiled los dejaba solo como mandos de GameCube); los controles del mando al estilo de Nebula (ver abajo); la pantalla de inicio dice Wii Sports y conserva el crédito de BrawlRecompiled y WiiCompiled; y el mensaje de `OSSetPowerCallback`, que Wii Sports llama en cada cuadro, solo se anota cuando cambia.
14. Escribe `projects\wiisports\retarget.md` con todo lo que cambió y lo que no pudo resolver.

Ante la duda no cambia nada: una dirección sin pista segura queda igual y aparece en la lista "Sin resolver" para revisarla a mano.


### Traducir y compilar en Ubuntu (WSL2)

Linux x86_64 es lo más parecido a la consola (Linux ARM64) y WiiCompiled lo soporta tal cual. Un solo script hace todo: `recomp-prep`, `recomp-native`, traducir, `generate-data-init`, `emit-build-shards`, configurar CMake (descarga Dawn la primera vez) y compilar con Clang sin detenerse en el primer error.

```sh
sudo apt install -y clang lld cmake ninja-build git pkg-config dotnet-sdk-8.0 python3-numpy python3-pil \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev \
  libxkbcommon-dev libwayland-dev wayland-protocols libegl1-mesa-dev libgles-dev libdrm-dev libgbm-dev \
  libasound2-dev libpulse-dev libudev-dev libdbus-1-dev libvulkan-dev mesa-vulkan-drivers
bash scripts/build-wiisports-wsl.sh /mnt/c/wii/RSPE01/DATA/sys/main.dol ~/BrawlRecompiled/wiicompiled
```

Con `RAPIDO=1` delante no vuelve a traducir: solo rehace el runtime y enlaza. Todo queda en `~/choya-build.log` (y una copia en `C:\Users\Public\choya-build.log`); al final imprime cuántos errores hubo y en qué archivos.


Controles con teclado: **A** = A, **S** o **B** = B, **Z**/Shift = 1, **X**/Espacio = 2, flechas = cruz, Enter/Esc = +, guion/Tab = −, **C**/Ctrl = agitar.

Controles con mando (los de Nebula, el port de Super Mario Galaxy): el mando de la consola con el mapeo de PortMaster, leído de /dev/input con la numeración de botones de SDL2 como Nebula; en la RG35XX H con KNULLI los botones de cara, hombros, Select, Start y MENU van con una tabla propia medida en la consola (con el mapeo de PortMaster X salía como A y L1 como B; `CHOYA_MANDO_CODIGOS` la cambia), y el Wii Remote vertical: **A** = A (derecha), **B** = B (abajo), **X** = 1, **Y** = 2, **R1** = agitar, **Start** = +, **Select**/L1/L2 = −, cruceta o stick izquierdo = cruz. Salir: **MENU + Start** en KNULLI y muOS, **Select + Start** en ArkOS (el juego se cierra limpio; gptokeyb mata con la misma combinación si no contesta). En el lanzador: `CHOYA_FACE_LAYOUT=labels` si A y B salen al revés en tu firmware y `CHOYA_QUIT_COMBO=menu|select|both` para forzar la salida.

Para arrancarlo y ver dónde se detiene:

```sh
bash scripts/run-wiisports-wsl.sh /mnt/c/wii/RSPE01/DATA
python3 -m choya donde 0x80009D90      # qué función de Wii Sports hay en una dirección
```

### Wii Sports en la RG35XX H (en progreso)

El driver de la Mali-G31 de KNULLI solo ofrece Vulkan 1.0 y Dawn (los gráficos de WiiCompiled) pide 1.1, así que la primera prueba en la consola usa el modo de gráficos **null**: el juego corre completo sin imagen (con sonido) y el log dice cuántos cuadros por segundo da el procesador.

```sh
bash scripts/build-wiisports-wsl.sh /mnt/c/wii/RSPE01/DATA/sys/main.dol ~/BrawlRecompiled/wiicompiled   # una vez
bash scripts/build-wiisports-arm64.sh ~/BrawlRecompiled/wiicompiled
```

Compila con Docker (Ubuntu 24.04, glibc 2.39, Clang 21, `-mcpu=cortex-a53`, libstdc++ estática) y deja `C:\Users\Public\choya-port\`: `Wii Sports (Choya).sh` y la carpeta `wiisports-choya` van a la carpeta `ports` de la microSD, y la carpeta `DATA` del disco extraído va en `wiisports-choya/game/`. Al abrirlo desde Ports corre 90 segundos y escribe `run.log` y `resumen.txt`.

`scripts/knulli/Choya-Info.sh` (desde Ports) escribe `choya-info.txt` con el sistema, la GPU, Vulkan/GLES y el espacio libre.

#### Motor de Choya (gráficos en la consola)

En la consola los gráficos los dibuja el motor GLES de Choya en vez de Dawn. aurora sigue leyendo el GX del juego y armando cada cuadro; Choya copia ese cuadro (vértices crudos, uniformes, índices, arreglos y pases) y lo dibuja con OpenGL ES 3.2 sobre el framebuffer de la Mali (EGL fbdev, sin X11/Wayland/DRM). Se activa con `CHOYA_GLES=1`, que ya pone el lanzador.

- `choya/data/choya_gles.cpp` (runtime): pantalla EGL, presentación y registro `choya-gles.txt`.
- `choya/data/aurora/choya_gx_core.{hpp,cpp}`: el núcleo del motor, sin nada de aurora. Generador GLSL ES 3.20, reproducción del cuadro en un framebuffer del tamaño del EFB y presentación de la copia a pantalla (GXCopyDisp). Los vértices y arreglos se leen de texturas R32UI porque la Mali no permite buffers de almacenamiento en el vertex shader.
- `choya/data/aurora/choya_gx.{hpp,cpp}`: adaptador que traduce los pipelines y cuadros de aurora al núcleo. Los cuatro archivos se copian a `aurora-main/lib/choya/`.
- `CHOYA_GX_VOLCADO=600,2400` vuelca esos cuadros a `cuadro-<n>.bin`; `tests/motor_gles/harness.cpp cuadro.bin salida.ppm` los vuelve a dibujar en la PC con Mesa. `CHOYA_GX_COLORES=1` pinta cada dibujo de un color distinto.
- Pasos: 1 pantalla (listo) · 2a geometría (listo) · 2b-1 texturas, coordenadas de textura y TEV en flotante (listo) · 2b-2 TEV exacto en enteros · 3 copias del EFB (hechas), paletas, profundidad y mezcla · 4 quitar Dawn.
- Memoria (como el renderer GLES de Nebula, el port de Super Mario Galaxy): los datos de cada cuadro (vértices, arreglos, uniformes, índices) van en anillos de búferes persistentes (`GL_EXT_buffer_storage`, mapeados una vez; `CHOYA_GX_PERSISTENTE=0` los apaga), tres cuadros en vuelo con una espera por lugar del anillo, y las texturas en el formato más chico que las reproduce (gris R8/RG8, RGB565, RGB5A1; `CHOYA_TEX_RGBA8=1` para todo en RGBA8). El lanzador pone CPU, GPU y memoria en `performance` mientras se juega y lo restaura al salir (`CHOYA_PERF=0` lo deja como está).
- Lo que se tomó de galaxy-vita (el port de Super Mario Galaxy a la PS Vita): estado GL en sombra (no se llama al driver si el valor no cambió), cada pipeline de aurora resuelto una vez a programa y estado GL, las texturas de cada grupo resueltas una vez a nombres GL, los dibujos que no pueden pintar nada (sin escritura de color, alfa ni profundidad, o con el recorte vacío) se saltan, y un caché de programas en disco (`CHOYA_GX_CACHE`, el lanzador usa `cache-shaders/`): cada programa se compila una vez y las siguientes se carga su binario. Con menos de `CHOYA_GX_MEMORIA_MIN` MB disponibles (96) el motor suelta lo que guarda para reusar y devuelve el heap libre al sistema. `choya-gles.txt` dice qué porcentaje del estado ya estaba puesto y cuántos programas salieron del caché.
- **Medición por tramos.** En una escena lenta, sostén **MENU sola 2 segundos**: durante 20 s se mide el hilo del juego cada milisegundo y se cuentan las llamadas a cada función del juego (el aviso está en `ApplyRuntimeCallOptions` de `abi_bridge.h`; sin medir es una lectura y un salto). Deja `perfil.txt` y en `resumen.txt` el reparto del cuadro: actualizar el juego (dentro de `EGG::SceneManager::calc`), preparar el dibujo (dentro de `EGG::SceneManager::draw`), esperar (dormido en un futex, un sleep, un ioctl del driver o un fallo de página) y otros. Si el juego está corriendo se toma la muestra con una señal; si está dormido no se le despierta (se lee de `/proc` en qué llamada al sistema está), así medir no acorta los sleeps. En la PC, `python -m choya perfil perfil.txt` nombra todo: por tramo, lo que más acumula cada función (ms por cuadro) y cuántas veces se llama por cuadro, qué funciones de aurora pesan al preparar el dibujo y qué llamada pidió cada espera. El modo diagnóstico sigue midiendo de los 20 a los 55 s.
- **Memoria de la Mali por pase.** `choya-gles.txt` reparte lo que crece la memoria de la GPU entre subidas, empezar pases, dibujos, borrados, copias del EFB, pantalla y entre cuadros, y nombra los 4 pases donde más crece al dibujar (tamaño, qué borra, cuántos dibujos, si viene justo después de copiar la profundidad o el color del EFB). `CHOYA_GX_EN_VUELO=1` obliga a terminar cada cuadro antes del siguiente (ya no se activa solo).
- **Borrados dentro del pase como dibujo.** Los borrados de aurora (GXCopyTex con borrado) se hacen con un triángulo del color y la profundidad del borrado, no con `glClear`: en Golf, `glClear` justo después de copiar el EFB hacía que el driver de la Mali le diera memoria nueva al EFB en cada cuadro sin devolverla (0.19.1 lo midió en "borrados"). `CHOYA_GX_BORRAR_CON_GL=1` vuelve a `glClear`.
- **Sin plano zfreeze.** Con el motor de Choya encendido, aurora ya no transforma en la CPU tres vértices de cada dibujo para el plano de GX coplanar (zfreeze), que el motor no usa (~8.7 ms por cuadro del hilo del juego en Golf).
- **Núcleos.** El hilo del juego (el que corre el código de la Wii y procesa el GX) queda solo en el último núcleo y los demás hilos del proceso (motor de Choya, los de la Mali, audio, aurora) en los otros tres. `CHOYA_NUCLEOS=0` lo apaga.
- **Efectos de audio apagados por omisión.** Cuestan ~10 ms por cuadro en Golf. Un archivo vacío llamado `con-reverb` en `wiisports-choya/` los enciende. Apagados, el lanzador pone `CHOYA_SIN_REVERB=1`, que hace que no se llame a `nw4r::snd::detail::AxManager::AuxCallbackFunc` (la reverberación, `AXFXReverbHiExpCallback`, ~9 % del hilo del juego en los menús): la vuelta del bus auxiliar queda en silencio y el sonido directo no cambia.
- **Tope del procesador.** KNULLI deja el H700 en 1512 MHz aunque el núcleo anuncia 1704 MHz (`cpuinfo_max_freq`). Con `CHOYA_PERF` (por omisión) el lanzador sube `scaling_max_freq` a ese tope mientras se juega y lo restaura al salir; el hilo del juego, que es lo que limita, va ~12 % más rápido. Si la temperatura pasa de 80 °C vuelve al tope de KNULLI hasta bajar de 72 °C (queda anotado en `rendimiento.txt`). Un archivo vacío llamado `cpu-normal` en `wiisports-choya/` lo deja como lo tiene KNULLI.
- **Informe de tramos más fiel (0.20.1).** `choya perfil` nombra cada dirección de regreso de la pila en la instrucción de la llamada (dirección − 1) y cuenta también las funciones integradas en línea (llvm-symbolizer), así el trabajo de aurora ya no se reparte entre funciones vecinas. El informe dice cuándo se midió y de qué hilo, y desglosa también "Otros" (lo que corre fuera de calc y draw).
- **Medición sin ruido propio y tiempos de aurora (0.20.2).** Las dos señales del perfil (CPU y pared) ya no se interrumpen entre sí: antes el muestreo de CPU caía al principio de la de pared y la hacía parecer un tercio del hilo del juego. Lo que la medición le quita al hilo del juego se anota en `perfil.txt` y sale en el informe. El motor informa cada 10 s cuánto espera el juego al hilo de aurora (`fifo::drain` hasta que el cuadro anterior queda sellado), en qué se le va el ciclo a ese hilo (sellar, codificar, publicar, preparar el siguiente) y cuánto cuestan las listas de dibujo (`GXCallDisplayList`). El contador de dibujos de aurora ya no usa una instrucción atómica (el A53 no tiene LSE: eran ~8000 llamadas a `__aarch64_ldadd8_relax` por cuadro).
- **Dawn ya no codifica los dibujos (0.20.3).** En la consola Dawn va con el backend Null y todo lo dibuja el motor de Choya, pero aurora seguía codificando en Dawn cada dibujo del cuadro (3-4 ms por cuadro en el hilo de aurora, mientras el juego esperaba a que terminara). Con el motor encendido los pases de Dawn quedan vacíos (sus borrados, copias y resoluciones siguen igual para aurora). `CHOYA_DAWN_DIBUJA=1` lo deja como antes. Las listas de dibujo se cronometran una de cada 16 (medirlas todas le costaba al juego ~1.3 ms por cuadro).
- **Texturas sin datos con nombre (0.20.4).** Cuando un dibujo usa una textura que el motor no tiene, se anota una vez en `choya-gles.txt` con el tamaño, el formato GX y los mips que dio aurora (sección "texturas sin datos" del resumen). El contador "enlaces sin textura" cuenta cada vez que un dibujo la busca, no texturas distintas. Un archivo vacío llamado `volcados` junto al juego hace que MENU + R1 guarde el cuadro con sus texturas también al jugar (sin él, solo en el modo diagnóstico).
- **Paletas sobre copias del EFB (0.20.5).** Las texturas con paleta (C4/C8/C14X2) que vienen del disco ya llegaban al motor: aurora las convierte a RGBA8 en la CPU. Las que el juego arma copiando el EFB con un formato de un canal (I8, R8, R4...) y leyéndolo luego como C4/C8 con una paleta, aurora las convertía con un pase de Dawn (`tex_palette_conv`) que el motor no tenía. Ahora el motor las hace antes del pase que las usa: cada texel toma el índice del canal R de la copia (R·255 en C8, R·15 en C4) y lo busca en la paleta, que aurora ya entrega decodificada como textura de N×1. Un cuadro con una de estas paletas no se salta aunque el motor vaya atrasado (aurora solo la repite cuando cambia la copia). El resumen dice cuántas hay por cuadro.
- **Copias que el juego no repite (0.20.6).** Las caras de los Miis se arman una sola vez con copias del EFB (135×135 y 67×67 con el EFB de 674×556), casi siempre en una pantalla de carga, sin mostrar nada. El motor tiraba los lotes más viejos con más de 16 sin mostrarse, o los grupos más viejos con más de 6 en cola, y con ellos esas copias: las cabezas quedaban como cuadros blancos. Ahora los lotes sin mostrarse se mandan al motor como un grupo que no se muestra, y con la cola llena el juego espera al motor (hasta 1 s) en vez de tirar copias únicas. El resumen cuenta cuántas veces pasó y si se perdió alguna.
- **El motor despierta en las pantallas de carga (0.20.7).** El hilo de la pantalla solo dibujaba cuando el juego presentaba un cuadro; al armar las caras de los Miis el juego no presenta nada, así que la cola se llenaba y, pasado el tiempo de espera, se tiraban las copias (la 0.20.6 contó 191 perdidas). Ahora el motor despierta a ese hilo cuando manda lotes sin mostrarse y cuando la cola está llena (`choya_gles_wake`); esos lotes se dibujan sin presentar nada. La prueba `harness cola` captura 40 lotes sin mostrar desde otro hilo y comprueba que se hacen las 40 copias.
- **Recursos convertidos.** El port usa el `assets.wlpk` del conversor (sección 3): `build-wiisports-arm64.sh` convierte el disco una vez (`CHOYA_DISCO`, por defecto `/mnt/c/wii/RSPE01`; perfil con `PERFIL_TEXTURAS`, `rg35xxh` por defecto), lo guarda en `~/choya-paquete` y lo copia a `wiisports-choya/assets.wlpk`; solo se vuelve a convertir si cambia el conversor o el perfil. En la consola, cada textura estática que el juego carga se busca en el paquete por la misma clave del conversor (crc32 del nivel 0 GX + tamaño + formato): si está, aurora ya no la decodifica y el motor sube la versión convertida (ETC2/EAC reducida, con sus mipmaps); si no, sigue por aurora como antes. Las texturas con paleta siguen por aurora (el juego puede cambiar la paleta). Hace falta `sudo apt install -y python3-numpy python3-pil python3-scipy` en Ubuntu para convertir; `CHOYA_PAQUETE=ruta` usa uno ya hecho (por ejemplo el de Windows) y `CHOYA_SIN_PAQUETE=1` arma el port sin él. El audio del paquete todavía no se usa.
- Texturas: aurora las decodifica a RGBA8 (`gfx/texture.cpp`); Choya recibe esos datos, sabe qué texturas y muestreo tiene cada grupo de texturas (`gx/gx.cpp`) y cuándo se liberan (`~TextureRef`). Las copias del EFB a textura (GXCopyTex) las hace el motor al terminar el pase: la zona del EFB se convierte al formato de la copia como en `tex_copy_conv` de aurora (intensidad, canales sueltos, RGB565 opaco, profundidad en bytes) y queda en una textura RGBA8 que se enlaza por el mismo id que aurora; el EFB guarda la profundidad en una textura para las copias de Z. Las texturas con paleta todavía salen blancas.
- `tests/test_motor_gles.py` dibuja cuadros sintéticos con Mesa (llvmpipe) para probar el motor en la PC.


Lo que genera el traductor (`generated\`, `build-wiisports\`) contiene código del juego: no lo subas a ningún repositorio.

## Formato `.wlpk`

Little-endian, alineado a 16 bytes: cabecera de 64 bytes, tabla de entradas de 48 bytes ordenada por clave (búsqueda binaria), nombres, manifiesto JSON y datos. La especificación completa está en `choya/pack.py` y `runtime/include/wl/pack.h`.

## Pruebas

```sh
python -m unittest discover tests
```

44 pruebas con datos sintéticos (7 necesitan el runtime compilado): Yaz0/LZ77, U8, BRRES, TPL, los 11 formatos GX, ETC2/EAC (calidad y que nunca desborde el modo diferencial), DSP-ADPCM, BRSTM con bloques entrelazados, respaldo por calidad, audio ADPCM con bucle decodificado en C++, modelos MDL0, TEV con la configuración de materiales reales compilado y dibujado en OpenGL ES por software (Mesa llvmpipe) con revisión de píxeles, conversión de punta a punta, una prueba cruzada donde el runtime C++ lee el paquete de Python (claves, caché y audio idénticos), la lectura de `main.dol` y del arranque para WiiCompiled, y el cambio de direcciones del runtime de WiiCompiled.

## Pendiente

- **Matrices de textura y canales de luz del material:** se leen todavía con valores por defecto (UV sin escala, luz fija).
- **Modelos con piel (varios huesos por vértice):** se muestran en pose de reposo; las animaciones (CHR0) aún no se leen.

- **BRSAR** (efectos de sonido): se cuenta en el reporte y se deja tal cual en el disco; la capa AX lo decodifica con `wl::dsp_decode`.
- **ETC2 punchthrough (RGB8A1)** para CMPR con alfa de 1 bit: hoy usa RGBA8_EAC (8 bpp); con punchthrough bajaría a 4 bpp.
- El códec ETC2/EAC se verificó con un decodificador propio. Falta confirmarlo en la Mali-G31 con `wl_view`.
- Las texturas C4/C8 usan como clave solo los índices, no la paleta.

## Legal

No incluye ni descarga datos, claves ni binarios de Nintendo. Trabaja sobre el disco que extraes de tu propia copia, en tu equipo.
