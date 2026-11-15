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
| R2 / L2 (teclado: N) | Animación siguiente / anterior (después de la última, la pose de reposo) |
| Start | Salir |

Desde la 0.3, cada material se dibuja con su **TEV** traducido a GLSL ES 3.00: etapas, texturas por unidad, coordenadas de reflejo calculadas desde la normal, registros de color y konstantes, tablas de intercambio, prueba de alfa, mezcla y profundidad. El botón Y alterna entre TEV y el sombreado simple para comparar. Desde la 0.21, cada material lleva también sus **matrices de textura** (la SRT de `ResTexSrtData` por la matriz de efecto, como `LoadResTexSrt` de NW4R: escala, giro y traslación de UV, también sobre los mapas de entorno) y sus **canales de luz** (`ResChanData`: color de material y ambiente, del registro o del vértice, con la luz encendida o apagada y la función difusa del canal, como el XF de GX; la luz es una direccional blanca desde el visor). Los modelos con **piel** guardan hasta 4 huesos por vértice (los objetos atados a un hueso y las mezclas de `NodeMix`), y las animaciones **CHR0** del disco se hornean para cada modelo que tiene todos sus huesos (cuadros clave de Hermite FVS32/48/96 y valores por cuadro CV8/16/32, la pose como `CalcWorld` con la compensación de escala de Maya); el vértice las aplica en la GPU.

### Dibujar modelos en la PC sin la consola (WSL2)

`choya_render` usa el mismo código de render que `choya_model` y guarda PNG con Mesa (llvmpipe). En Ubuntu:

```sh
sh scripts/build-pc-render.sh
mkdir -p /mnt/c/wii/render
runtime/build-pc/choya_render /mnt/c/wii/salida/assets.wlpk /mnt/c/wii/render bwg_pin --max 5
```

Agrega `--simple` para ver el sombreado simple y `--size 1024` para imágenes más grandes. `--cuadro 20` guarda además cada modelo con huesos en el cuadro 20 de cada una de sus animaciones (`modelo@animación.png`), `--doble` dibuja también las caras traseras y `--verificar` compara cada textura ETC2/EAC como la lee la GPU con el decodificador de la CPU.

### Confirmar el códec ETC2/EAC en la Mali-G31

Arma el port de `wl_view` (sección 5) y crea un archivo vacío llamado `verificar` en la carpeta del port. Al abrirlo, `wl_view` sube cada textura ETC2/EAC del paquete, la dibuja tal cual (`texelFetch` del nivel 0), la lee de vuelta y la compara con su decodificador de la CPU; escribe en `log.txt` cuántas revisó por formato, cuántas salieron idénticas y la primera diferencia, termina con `VERIFICACION OK` o `VERIFICACION CON PROBLEMAS` y sale. En la PC, con Mesa, la misma prueba sale idéntica en todos los formatos (`tests/test_etc_gpu.py`). R11/RG11 pasan por 11 bits y pueden diferir en 1 al pasar a 8.

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
13. Hace unos pocos cambios de código revisados a mano (cada uno debe encontrar su texto exacto; desde la 0.32.0 uno obligatorio que no encaja detiene todo, ver "Compilación fiable del port"): botón **B** en el teclado (S o B), porque Wii Sports lo pide desde la pantalla de título; el teclado y los mandos hacen de Wii Remote 1 (BrawlRecompiled los dejaba solo como mandos de GameCube); los controles del mando al estilo de Nebula (ver abajo); la pantalla de inicio dice Wii Sports y conserva el crédito de BrawlRecompiled y WiiCompiled; y el mensaje de `OSSetPowerCallback`, que Wii Sports llama en cada cuadro, solo se anota cuando cambia.
14. Escribe `projects\wiisports\retarget.md` con todo lo que cambió y lo que no pudo resolver.

Ante la duda no cambia nada: una dirección sin pista segura queda igual y aparece en la lista "Sin resolver" para revisarla a mano.

Desde la 0.32.0 `recomp-native` termina con error (código 3) si un parche obligatorio no se aplica o si un objeto del SDK del paso 8 no mide lo que espera el runtime, y escribe `projects\wiisports\parches.json` con el estado de cada parche.


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
- **Luz de los Miis (0.20.8).** El motor no calculaba la luz por vértice de GX: el color del material (blanco) llegaba tal cual y, con las texturas de luz encima, las piernas, los zapatos y la ropa se saturaban a blanco. Ahora calcula los canales de color con las 8 luces como aurora (ambiente, difusa con signo o recorte, atenuación de foco o distancia, máscaras de luces por canal), lee las normales del vértice y las pasa por la matriz de normales, y la generación de coordenadas `GX_TG_NRM` usa la normal (antes usaba la posición), que es como Wii Sports busca en sus texturas de luz. Los volcados (MENU + R1) ahora llevan también las copias hechas en cuadros anteriores (caras, texturas de luz), así que se vuelven a dibujar completos en la PC; el banco de pruebas suma `sonda` (qué dibujo cambia un píxel), `copia` (saca una copia del volcado a imagen) y `copia-vieja`.
- **TEV exacto y coordenadas completas (0.20.9).** El motor ahora arma el mismo programa que aurora para cada material: la combinación de colores (TEV) se cuenta en enteros de 8 bits con el redondeo y el desborde de la Wii (antes en flotante), el resultado es el de la última etapa aunque no escriba en el registro de salida, y el color de los canales sin luz pasa por 8 bits como en el XF. Lo que más se nota: faltaba la segunda transformación de las coordenadas de textura (matriz posterior y normalizado de `GXSetTexCoordGen2`), que Wii Sports usa en casi todos los materiales; sin ella el pasto de la cancha salía como una mancha ampliada y las texturas de luz de los Miis se buscaban fuera de lugar. También se agregan la niebla (con ajuste por distancia), las texturas indirectas, la textura de profundidad, el modo coplanar, las coordenadas de binormal y tangente y el centro de píxel de GX. Los volcados pasan a `CHOYAGX5` (los de antes se siguen leyendo, sin estas partes). Un archivo vacío llamado `tev-flotante` junto al juego vuelve a la cuenta en flotante; `CHOYA_GX_FUENTES=carpeta` escribe los programas generados para revisarlos. Pruebas nuevas: `harness tev 0..4` (redondeo, desborde, matriz posterior, niebla e indirecta).
- **Memoria del juego y cierre al armar un Mii (0.20.10).** Un cierre con "Missing translated function ... 0x0" y LR en `RPGrpModelRfl::Construct` es Wii Sports armando un Mii sin espacio: `nw4r::g3d::ScnRfl::Construct` pide un bloque grande al montón de modelos, recibe NULL y el juego no lo revisa (salta a 0 al llamar a `G3dProc`). Ahora el runtime avisa a Choya al entrar a `MEMAllocFromExpHeapEx`, `MEMAllocFromFrmHeapEx`, `MEMFreeToExpHeap`, `MEMResizeForMBlockExpHeap` y `EGG::Heap::alloc` (con la dirección como constante: en las llamadas directas no cuesta nada fuera de esas funciones). Si una petición de 16 KB o más no cabe en un montón expandible, se anota quién la pidió (con la cadena de llamadas del juego), lo libre del montón y el árbol de montones, y otro montón expandible con espacio de sobra le presta el bloque (al que le queden 2 MB después; nunca uno que esté dentro del que se quedó sin espacio; hasta 24 MB); al devolverlo, `MEMFreeToExpHeap` recibe el montón que lo tiene. También se anota cuando EGG no deja asignar de un montón por `disableAllocationAllBut`. Un archivo vacío `sin-prestamos` deja solo los avisos. Si el juego aun así salta a 0, se escribe el árbol de montones con lo libre de cada uno. El resumen trae la sección "memoria del juego", la pila completa del cierre y empaca la carpeta del cierre en `ultimo-cierre.tar.gz`. Los volcados de MENU + R1 llevan texturas siempre (`volcados-sin-texturas` lo apaga). Prueba nueva: `test_memoria_juego` (montones falsos de MEM en la PC: préstamo, devolución, montones hijos, montón de cuadro, EGG bloqueado y el informe del cierre).
- **Pendientes del conversor (0.21.0).** Matrices de textura y canales de luz de cada material, modelos con piel y animaciones CHR0 horneadas (paquete `CMDL` 3 con vértices de 44 bytes y huesos, entradas `CANM` 1; el runtime sigue leyendo los `CMDL` 1 y 2), efectos de sonido de los BRSAR (los sonidos de onda de cada RWSD salen como audios `archivo.brsar/NOMBRE`, en DSP-ADPCM sin recodificar si se puede; las secuencias RSEQ se cuentan y siguen en el disco), CMPR con alfa de 1 bit en ETC2 RGB8A1 (4 bpp en vez de 8; si la calidad no llega, primero RGBA8_EAC), la clave de las texturas con paleta incluye la paleta (en el conversor, en `wl::TextureCache` y en el motor del juego, que ahora toma del paquete también las texturas C4/C8/C14X2 cuando la imagen y la paleta coinciden) y la verificación del códec en la GPU (`wl_view --verificar`, archivo `verificar` en el port). Pruebas nuevas: `test_huesos_material`, `test_brsar`, `test_etc_gpu`, `test_paleta_clave`.
- **Caras del público en blanco (0.21.1).** En Tenis, las cabezas del público son iconos de Mii (`RPSysKokeshiIcon`): el juego dibuja el Mii, lo copia del EFB a una textura y enseguida hace `DCFlushRange` sobre ella. El runtime tomaba ese flush como "la CPU escribió aquí", retiraba la copia y la textura se leía de la RAM, que con Dawn en Null nunca recibe la copia: cuadros blancos. Ahora, con el motor de Choya, cada copia guarda una huella de la RAM de su destino y solo se retira si esa RAM cambió (un bloque reciclado, una lectura del disco). aurora ya no apunta las copias grandes para bajarlas a la RAM (eso además partía el cuadro); las sondas de 256 bytes o menos siguen igual. El resumen trae la sección "copias del EFB tras DCFlush/DCInvalidate" con cuántas se conservaron y cuántas se retiraron. Prueba: `test_copias_efb` En 0.22.1 la huella se abarató: las copias de hasta 64 KB se leen enteras y en orden; las más grandes, 256 palabras repartidas (antes eran hasta 4096 salteadas, un fallo de caché cada una, y GXCopyTex le costaba unos 4.8 ms por cuadro al hilo del juego).
- **Sin Dawn (0.22.0).** El motor GLES de Choya ya dibujaba todo; Dawn seguía ahí con su backend Null solo para que aurora funcionara, y costaba memoria (búferes de staging de 37 MB por triple, búferes de GPU, subidas de texturas) y trabajo en el hilo de aurora. Ahora el build de la consola compila aurora con el **WebGPU nulo de Choya** (`choya/data/aurora/sin_dawn`, proveedor `AURORA_DAWN_PROVIDER=choya`): las cabeceras son las de Dawn (generadas de `dawn.json`, licencia BSD en `LICENSE-dawn`) y las funciones no usan GPU. Solo los búferes que la CPU mapea tienen memoria (y sus páginas se ocupan al escribirse); las texturas recuerdan tamaño y formato; los callbacks terminan al momento. Con eso aurora ya no codifica pases ni presenta (el cuadro se copia al motor al sellarlo), no arma WGSL para sus pipelines ni usa hilos de compilación, y no lee ni escribe la base de datos de pipelines en la microSD. El resumen dice "aurora: sin Dawn" en la sección del hilo de aurora, con los tiempos de sellar, codificar y esperar al juego. La parte generada se rehace con `python3 scripts/generar-webgpu-nulo.py` (solo si cambian las cabeceras). Pruebas: `test_sin_dawn` (la API de C++ como la usa aurora, con ASan); con `CHOYA_DEPS` apuntando a los clones de las dependencias de aurora, además compila aurora sin Dawn y dibuja con el motor en Mesa un triángulo, una copia del EFB y una textura. El build de la PC (WSL) sigue con Dawn.
- **GX más barato por dibujo (0.22.2).** Cada cambio de material (unos 1350 por cuadro) hacía que aurora armara la configuración del pipeline desde cero: borrar 2.7 KB, llenarla, hashearla y compararla byte a byte (1 KB) con la del caché. Ahora rehúsa una configuración de trabajo (solo vuelve a poner en cero las etapas que usó la anterior) y, con el motor de Choya, el hash de 64 bits basta para reconocerla. En el banco sintético (`tests/sin_dawn/banco_gx.cpp`, 6000 dibujos por cuadro) bajó de 2.06 a 1.86 ms por cuadro en la PC, un 10 %. `CHOYA_VERIFICAR_PIPELINE=1` compara cada configuración con la armada desde cero y corta si difieren. La huella de las copias del EFB lee ahora como mucho 512 palabras (las de hasta 4 KB, enteras) y siempre la última. Prueba: `test_gx_barato`.
- **Hilo GX (0.23.0).** Hasta ahora el hilo del juego hacía todo el GX: el juego arma los comandos y aurora los decodificaba en el mismo hilo (registros, listas de dibujo, pipelines, texturas y el motor de Choya), unos 14 ms de los 76 de cada cuadro. Ahora aurora los decodifica en otro núcleo, como el modo de doble núcleo de Dolphin: el hilo del juego solo escribe el FIFO y lo entrega en lotes; el hilo GX lo procesa en el mismo orden. Lo que antes aurora procesaba al vaciar el FIFO (cada dibujo, cada lista) se entrega en ese punto; lo que antes cambiaba el estado de inmediato (`GXApplyBPReg`, `GXSetSourceVtxDesc`, `GXSetZScaleOffset`) se anota en el punto ya entregado, como antes. Las funciones que leen o cambian el procesador de forma directa (copias del EFB, `GXDrawDone`, `GXSetDrawSync`, inicio y fin de cuadro, VI) esperan a que el hilo GX termine y corren como antes en el hilo del juego. Las listas de dibujo que están en la RAM del juego se leen de ahí, como hace la GPU de la Wii; las prestadas por el runtime se copian. El juego queda solo en el último núcleo y el hilo GX solo en el penúltimo. En el banco sintético (6000 dibujos por cuadro) el hilo del juego pasa de 1.4 a 0.45 ms de CPU por cuadro en la PC. El resumen trae cada 300 cuadros una línea `aurora: hilo GX` (lotes, cuánto trabajó el hilo GX, cuánto esperó el juego y en qué funciones). Un archivo vacío llamado `sin-hilo-gx` junto al juego lo apaga (`CHOYA_GX_HILO=0`); un archivo vacío `hilo-gx-copiar` copia también las listas de la RAM del juego (`CHOYA_GX_HILO_COPIAR=1`, por si algo se dibuja mal solo con el hilo encendido). Probado con ThreadSanitizer (banco y prueba de punta a punta) y con píxeles iguales con el hilo encendido y apagado. Pruebas: `test_hilo_gx` y `test_sin_dawn` (larga). En 0.23.0 el hilo no llegaba a encenderse en la consola: el runtime deja encendido el reemplazo de texturas de aurora y el hilo se negaba a arrancar con él. En 0.23.1 registrar y cargar paletas para el reemplazo pasa al hilo GX, en orden y con la paleta copiada al momento, y el hilo arranca; el resumen busca también en `run.log` la línea de encendido (o el motivo si queda apagado). En la consola, con el hilo encendido, el juego pasó de unos 72 a 48–56 ms por cuadro (de 13.8 a 18–21 fps, en una escena con algo menos de dibujos). Quedaban 4–5 ms por cuadro de espera, casi todos en las 19 copias del EFB por cuadro: cada una hacía esperar al juego a que el hilo GX terminara. En 0.23.2 las copias y su estado van al hilo GX sin esperar: el estado en el punto ya entregado (como antes, que se cambiaba al momento) y la copia después de lo dibujado; el runtime ya no hace `GXDrawDone` antes de cada copia. En el banco con 19 copias por cuadro (`BANCO_COPIAS=19`) el juego espera una sola vez por cuadro.
- **Medir el motor de pantalla (0.23.3).** Con el hilo GX el juego arma 18–21 cuadros por segundo, pero el motor de Choya dibuja unos 12: tarda ~80 ms por cuadro en el hilo de la pantalla y salta los cuadros que no alcanza. Para saber dónde se van esos 80 ms, el resumen trae ahora (sección "motor de pantalla") el tiempo de cada cuadro dibujado por partes: texturas, subidas de datos, pases, borrados, dibujos, copias del EFB, pantalla, cierre, espera a la GPU y la lectura de la memoria de la Mali (el motor la lee de debugfs unas 80 veces por cuadro); la CPU real del hilo (si es mucho menor que el tiempo, el hilo está esperando al driver o a otro hilo); el tiempo de GPU por cuadro si el driver tiene GL_EXT_disjoint_timer_query; y los cuadros por segundo dibujados por el motor, del juego y saltados. La línea "presentación" ya cuenta bien los cuadros mostrados (antes salía con la mitad o menos). Un archivo vacío `sin-medir-mali` apaga la medición de la memoria de la Mali, para comparar. Prueba: `test_motor_gles` (el informe y que sus partes sumen el total). También (0.23.4) un perfil de hilos y núcleos, con la idea del port de Super Mario Galaxy para PS Vita: cada 10 s el resumen dice qué tan ocupado estuvo cada núcleo y cada hilo del proceso (el juego, el hilo GX, la pantalla `choya-pantalla`, los de la Mali, el audio y aurora), cuánta CPU usó en ms por cuadro del juego, en qué núcleo corrió y cuánto esperó turno (listo para correr pero sin núcleo libre). Sale de `/proc/self/task/*/stat`, `schedstat` y `/proc/stat`; cuesta casi nada. `CHOYA_HILOS=0` lo apaga y `CHOYA_HILOS_CADA` cambia los segundos. Prueba: `test_perfil_hilos`.
- **Saltar cuadros en el motor (0.24.0).** La medición de la 0.23.4 mostró que el motor dibujaba 12.5 cuadros por segundo pero mostraba solo 6: cuando iba atrasado tomaba dos cuadros de la cola, dibujaba los dos y mostraba el último. El viejo no se saltaba porque Wii Sports alterna dos juegos de copias del EFB (uno por cuadro), así que las copias del viejo nunca eran parte de las del nuevo. Ahora el motor lleva la cuenta de cada copia: una que el juego hizo 3 veces o más y en uno de los últimos 3 cuadros dibujados es "de siempre", y un cuadro viejo cuyas copias son de siempre o las rehace uno más nuevo ya no se dibuja (solo si el siguiente empieza borrando el EFB). Las copias que el juego hace una vez, como las caras de los Miis, siguen obligando a dibujar ese cuadro. Debería mostrar casi todos los cuadros que dibuja (el doble que antes) con el mismo costo por cuadro. El resumen trae la línea "motor: saltos" (cuántos se saltaron y cuáles se dibujaron por copias que no se repiten). Un archivo vacío `sin-saltar-cuadros` vuelve a dibujarlos todos. Además: la memoria de la Mali ya no se mide salvo con un archivo vacío `medir-mali` (eran ~100 lecturas de debugfs y 1.7 ms por cuadro), y el motor mide el **tiempo de GPU por pase** en 1 de cada 30 cuadros (cada pase, cada copia del EFB y la copia a la pantalla por separado; línea "motor: GPU por pase" con los 8 pases más caros), para elegir el siguiente cuello de botella de la GPU. `CHOYA_GX_GPU_PASES=0` lo apaga. Prueba: `test_motor_gles` (copias alternas: el cuadro viejo se salta; uno con una copia nueva se dibuja).
- **EFB sin recargar (0.25.0).** Con la 0.24.0 la pantalla pasó de 6 a 11 cuadros por segundo; el motor sigue en ~84 ms por cuadro y casi todo es GPU (~80 ms). La Mali-G31 dibuja por mosaicos: cada vez que el motor deja el EFB para una copia guarda el color y la profundidad en la memoria (674x556, unos 3 MB) y al volver los lee otra vez, y Wii Sports hace 19 copias por cuadro. Muchas van seguidas de un borrado de toda la pantalla (GXCopyTex con borrado), así que el driver leía el EFB entero para pintarlo encima. Ahora el motor le avisa antes con `glInvalidateFramebuffer` (no leas eso) cuando es seguro: el juego borra el EFB al empezar cada cuadro y nada de lo que queda del cuadro lee fuera de la zona borrada (Wii Sports usa la franja de abajo del EFB para los iconos de 34x34, por eso se revisa). Al terminar el cuadro tampoco se guarda el EFB. Línea "motor: EFB sin recargar" en el resumen; un archivo vacío `sin-invalidar-efb` lo apaga. Además, cada copia del EFB enlaza solo lo que lee (color o profundidad), y el tiempo de GPU por pase se parte solo donde el motor cambia de framebuffer: en la 0.24.0 partirlo en cada pase hacía que la suma diera 3 veces el cuadro. Prueba: `test_motor_gles` (un borrado se avisa; si una copia lee fuera de la zona, no).
- **Copias sin cargar su destino (0.26.0).** Con la 0.25.0 la pantalla subió a 12.3–13.3 cuadros por segundo (el motor de ~84 a ~75 ms por cuadro, la GPU de ~80 a ~68 ms) y la memoria de la Mali no creció (111–112 MB). La GPU por pase dijo que las copias del EFB se llevan unos 2/3 del tiempo de GPU del cuadro. Cada copia pinta su textura entera, pero la Mali no lo sabía: cargaba lo que tenía la textura antes de pintar y, si un cuadro en vuelo todavía la leía, la conservaba. Ahora el motor le avisa (`glInvalidateFramebuffer`) antes de cada copia, de cada paleta sobre una copia y de la copia a la pantalla. La línea "motor: EFB sin recargar" cuenta también las copias. Va con el mismo interruptor: `sin-invalidar-efb`.
- **TEV mixto (0.27.0).** La corrida con `tev-flotante` mostró que el TEV exacto en enteros costaba ~25 ms de GPU por cuadro en la Mali-G31 (51–53 ms en flotante contra 76–84 exacto; en pantalla 15–16 cuadros por segundo contra 12). Pero todo en flotante se aleja de la Wii: en un cuadro volcado del juego, 5 % de los colores cambian hasta 48 niveles de 255. Ahora el modo por defecto es **mixto**: cada material se cuenta en flotante, con lo exacto que sí se ve (el desborde de 8 bits al leer los registros, el resultado de la última etapa, los colores del XF por 8 bits), y los materiales donde el redondeo de cada etapa puede cambiar el resultado (una etapa sin límite, una comparación del TEV, la prueba de alfa igual/distinto) siguen exactos. En tres cuadros volcados (juego, Miis, menú) 75 de 78, 56 de 59 y 68 de 71 programas van en flotante, y la imagen difiere de la exacta en 4 niveles de 255 como mucho. Línea "motor: TEV mixto" en el resumen. Un archivo vacío `tev-exacto` cuenta todo exacto (como antes); `tev-flotante`, todo en flotante. Prueba: `harness tev 0..4` en los tres modos.
- **TEV mixto más fino (0.27.1).** Con la 0.27.0 la GPU bajó a 61–70 ms por cuadro (de 76–84 exacto), pero no a los 51–53 del flotante: 10 programas del juego seguían exactos por tener una comparación del TEV, y son los de los pases de pantalla completa (las sombras comparan la profundidad copiada). Una comparación solo necesita lo exacto si compara un valor que calculó una etapa anterior del mismo programa; si compara texturas, el rasterizador, constantes o registros que puso el juego (valores de 8 bits), en flotante da lo mismo. Ahora solo esas van exactas. En cuatro cuadros volcados quedan 1–3 programas exactos (por una etapa sin límite) y la imagen difiere de la exacta en 4 niveles de 255 como mucho.
- **Huella de las copias más barata (0.27.2).** Con la 0.27.1 la GPU quedó en 55–60 ms por cuadro (casi lo del flotante, 2 programas exactos de 258) y en la escena pesada el motor ya dibuja casi todo lo que el juego le da (13.4–13.9 de 15.1 cuadros por segundo): ahora limita también el hilo del juego (63–65 ms por cuadro). Primer recorte ahí: la huella de la RAM de cada copia del EFB (para saber si el juego escribió encima) leía 512 palabras salteadas de cada copia, ~1.4 ms por cuadro en fallos de caché; las copias de más de 32 KB (las de pantalla) ahora leen 64. Las de hasta 32 KB (iconos de los Miis) siguen igual.
- **Un programa de copia por formato (0.28.0).** Con la 0.27.2 el juego subió a ~17.5 cuadros por segundo en la escena pesada y la pantalla quedó otra vez un poco atrás (14.2): el motor tarda ~70 ms por cuadro, la GPU ~55, y las copias del EFB siguen siendo ~60 % del tiempo de GPU por pase. Cada copia corría un solo programa con todos los formatos en una cadena de ifs y en precisión alta. Ahora cada formato tiene su programa, con el formato fijo al compilar, y los que solo eligen canales (RGBA8, A8, R8, ...) van en precisión media, que en la Mali-G31 hace el doble de cuentas por ciclo (un valor de 8 bits cabe exacto). En la PC la imagen de los cuadros volcados sale idéntica. Línea "motor: copias del EFB" en el resumen; `CHOYA_GX_COPIA_GENERICA=1` vuelve al programa único.
- **Optimización del código traducido (0.29.0).** El hilo del juego pasa ~38 ms por cuadro en el código traducido; WiiCompiled lo compila con `-O2`. `build-wiisports-arm64.sh` acepta `CHOYA_OPT` (cada modo en su propia carpeta de compilación, así que la primera vez de cada uno compila todo):
  - `CHOYA_OPT=O3`: `-O3` en el código traducido (las opciones de coma flotante del PowerPC se quedan: `-fno-fast-math -ffp-contract=off`, así que el resultado del juego no cambia). Una sola compilación.
  - Compilación guiada por un perfil tomado en la consola (PGO), en tres pasos:
    1. `CHOYA_OPT=pgo-generar`: la primera vez baja `compiler-rt` de la misma versión de Clang y compila su runtime de perfiles para aarch64 (`scripts/pgo-runtime-aarch64.sh`, queda en `<WiiCompiled>/choya-pgo/runtime`). El port va más lento y cada 20 s guarda el perfil en `wiisports-choya/pgo/*.profraw` (el lanzador pone `LLVM_PROFILE_FILE`; da igual cómo se salga del juego).
    2. Jugar unos minutos en varios deportes y menús, y copiar `wiisports-choya\pgo\*.profraw` a `C:\Users\Public\choya-pgo` (`CHOYA_PGO_DIR=otra carpeta`).
    3. `CHOYA_OPT=pgo` (o `pgo-O3`): junta los perfiles con `llvm-profdata-21 merge` y compila con `-fprofile-use`. Si el perfil cambia, recompila el código traducido. Si después se vuelve a traducir el juego, conviene tomar un perfil nuevo (las funciones que cambiaron se compilan sin perfil).
  - `wiisports-choya/compilado.txt` dice cómo se compiló, y el resumen lo muestra en "código traducido compilado con".
- **Copias del EFB rotadas y su tiempo por dentro (0.30.0).** Con la 0.29.0 la parte "copias del EFB" del hilo de la pantalla tardaba ~30 ms por cuadro con ~20 copias, aunque toda la CPU del hilo eran ~25 ms: casi todo era esperar dentro del driver. En béisbol la GPU solo trabajaba ~37 ms por cuadro y aun así la pantalla iba a ~16 cuadros por segundo. Una causa probable: cada copia vuelve a pintar la misma textura que los cuadros anteriores todavía están leyendo en la GPU. Ahora cada copia que se repite escribe en otra de 3 texturas y los dibujos leen la última; las copias que el juego hace una sola vez, como las caras de los Miis, siguen ocupando una. La imagen sale idéntica. La línea "motor: copias por dentro" separa el tiempo de cada copia en cambiar a su textura, dibujarla y volver al EFB, con la CPU aparte, para saber dónde está la espera. El archivo vacío `sin-rotar-copias` (o `CHOYA_GX_ROTAR_COPIAS=0`) vuelve a una sola textura. El binario también guarda cómo se compiló el código traducido (`CHOYA_OPT`) y lo escribe en el log (línea "compilado:" del resumen), aunque falte `compilado.txt`.
- **0.30.1.** Los zips de la 0.29.0 y la 0.30.0 no traían `scripts/build-wiisports-arm64.sh` (el filtro que deja fuera las carpetas `build-wiisports*` también lo tapaba), así que `CHOYA_OPT` no hacía nada y se compilaba siempre con `-O2`. Ahora va incluido.
- **Copias del EFB directas (0.31.0).** La 0.30.x mostró dónde se iba el tiempo de las copias: ~20 copias por cuadro tardaban ~27 ms de pared en el hilo de la pantalla, casi todo esperando dentro del driver al pasar del EFB a la textura de la copia y al volver. La CPU del hilo (~25 ms) más la GPU (~35–47 ms) sumaban el tiempo del cuadro (~61 ms): no trabajaban a la vez. Rotar las copias en 3 texturas no cambió nada. Ahora la zona se copia con `glCopyTexSubImage2D` sin salir del EFB, a una textura intermedia del tamaño de la zona; la conversión al formato de la copia (I8, A8, escala a la mitad...) queda pendiente y se hace después, todas juntas: cuando un dibujo va a leer una de esas copias, antes de una copia de profundidad o de una paleta, y al terminar el cuadro. Si el juego copia dos veces al mismo destino sin leerlo en medio, la primera ni se convierte. La imagen sale igual (en los cuadros volcados de béisbol cambian unos cientos de píxeles en 3 de 255 como mucho, en los bordes de las copias a la mitad). La línea "motor: copias directas" dice cuántas se copiaron así, en cuántas tandas se convirtieron y cuánto tardó cada parte. El archivo vacío `copias-dibujadas` (o `CHOYA_GX_COPIA_DIRECTA=0`) vuelve a dibujar cada copia leyendo el EFB.
- **PGO arreglado (0.31.0).** El runtime de perfiles de Clang agrega al final del archivo en cada escritura, así que la versión que toma el perfil dejaba un archivo de cientos de MB con una copia acumulada cada 20 s. Ahora el lanzador usa `wiisports-%m.profraw` (el runtime suma lo nuevo dentro del mismo archivo, también entre partidas) y los contadores vuelven a cero después de cada escritura: un solo perfil de ~14 MB. `scripts/pgo-preparar.py` salta los archivos vacíos y de los viejos toma solo el último perfil (el total), así que los que ya se tomaron con la 0.29.0/0.30.x sirven.
- **0.31.1: copias rotadas y directas apagadas por omisión.** Medidas en la consola, ninguna de las dos ayudó en la Mali-G31: con las directas el hilo de la pantalla pasó de ~61 a ~100 ms por cuadro (el driver también espera en `glCopyTexSubImage2D`, y cada tanda de conversiones vuelve a cambiar de framebuffer), la memoria de la Mali subió de ~150 a ~250–290 MB (hasta 700 intermedias; ahora con tope) y apareció un parpadeo en la pantalla de "VS." de béisbol (la mitad derecha se oscurece un cuadro). El código queda para probar en otros drivers (archivos `rotar-copias` y `copias-directas` en la consola). Lo que sí ayudó: PGO, con el código traducido de ~36 a ~22–25 ms por cuadro en béisbol (el juego a ~28 cuadros por segundo).
- **0.31.2: saber qué versión hay en la consola.** El port lleva `wiisports-choya/version.txt` (versión de Choya y hora en que se armó), el lanzador su propia versión y el programa la versión en su línea "compilado:"; la primera línea del resumen dice `choya: lanzador X; port Y`. `scripts/copiar-a-consola.ps1` (PowerShell) busca la tarjeta (una unidad con `roms\ports`), copia `C:\Users\Public\choya-port` reescribiendo todo y compara `version.txt`, el lanzador y `WiiCompiled` de los dos lados; si no quedaron iguales lo dice.
- **0.32.0: compilación fiable.** Parches obligatorios que detienen la compilación, revisión de WiiCompiled fijada, perfil PGO validado sin reutilizar uno viejo, `assets.wlpk` con caché por contenido, port armado aparte con manifiesto (SHA-256 de cada archivo) y copia a la tarjeta comprobada archivo por archivo: ver "Compilación fiable del port" más abajo. La primera compilación con la 0.32.0 vuelve a convertir los recursos una vez (el paquete anterior no tiene clave) y fija la revisión de WiiCompiled que tengas. `version.txt` ya no lleva la hora: lleva la identidad del port.
- **0.33.0: resolución interna elegible.** Medido en la consola con la 0.32.0 y PGO: el juego va a 22–25 cuadros por segundo pero la pantalla a ~17, porque la GPU tarda 48–66 ms por cuadro: ~25 ms en dibujos y ~27 ms en las 10 copias del EFB (en cada una la Mali guarda el EFB entero de 674x556 y lo vuelve a leer). El lanzador ahora elige la resolución interna de WiiCompiled (`resolution_multiplier`, 480 líneas por 1): archivo vacío `resolucion-075` (0.75, EFB de ~506x417, 44 % menos píxeles en los dibujos y en cada copia) o `resolucion-05` (0.5); `CHOYA_RESOLUCION=1|0.75|0.5` manda sobre los archivos. Sin archivo sigue en 1. El resumen dice cuál se usó y el tamaño del EFB. Además, el contador "líneas/puntos" del motor separa tres causas: líneas o puntos de verdad, dibujos con "cull todo" (en GX no pintan nada: saltarlos es lo correcto) e instanciados.
- **0.34.0: vuelta al EFB perezosa.** Medido con la 0.33.0 a resolución 0.75 en tenis: la GPU ya solo tarda ~30 ms por cuadro, pero el hilo de la pantalla tarda ~50 porque en cada una de las 19 copias del EFB espera ~1.3 ms dentro del driver (rotar las copias o no invalidar el EFB no lo cambian: 23–27 ms de copias por cuadro en las tres pruebas). Ahora una copia deja enlazada su textura y el EFB se vuelve a enlazar solo cuando algo lo usa (dibujo, borrado, pase, paleta, pantalla): las copias seguidas, sin dibujos en medio, ya no van y vuelven. El informe separa cuántas copias fueron seguidas y, del cambio a la textura, cuánto es enlazarla y cuánto invalidarla, para saber dónde espera el driver. El archivo vacío `sin-vuelta-perezosa` (`CHOYA_GX_VUELTA_PEREZOSA=0`) vuelve a lo de antes.
- **Recursos convertidos.** El port usa el `assets.wlpk` del conversor (sección 3): `build-wiisports-arm64.sh` convierte el disco una vez (`CHOYA_DISCO`, por defecto `/mnt/c/wii/RSPE01`; perfil con `PERFIL_TEXTURAS`, `rg35xxh` por defecto), lo guarda en `~/choya-paquete` y lo copia a `wiisports-choya/assets.wlpk`; desde la 0.32.0 solo se reutiliza si coinciden el conversor, las opciones, el perfil y el contenido del disco (ver "Compilación fiable del port"). En la consola, cada textura estática que el juego carga se busca en el paquete por la misma clave del conversor (crc32 del nivel 0 GX + tamaño + formato): si está, aurora ya no la decodifica y el motor sube la versión convertida (ETC2/EAC reducida, con sus mipmaps); si no, sigue por aurora como antes. Las texturas con paleta siguen por aurora (el juego puede cambiar la paleta). Hace falta `sudo apt install -y python3-numpy python3-pil python3-scipy` en Ubuntu para convertir; `CHOYA_PAQUETE=ruta` usa uno ya hecho (por ejemplo el de Windows) y `CHOYA_SIN_PAQUETE=1` arma el port sin él. El audio del paquete todavía no se usa.
- Texturas: aurora las decodifica a RGBA8 (`gfx/texture.cpp`); Choya recibe esos datos, sabe qué texturas y muestreo tiene cada grupo de texturas (`gx/gx.cpp`) y cuándo se liberan (`~TextureRef`). Las copias del EFB a textura (GXCopyTex) las hace el motor al terminar el pase: la zona del EFB se convierte al formato de la copia como en `tex_copy_conv` de aurora (intensidad, canales sueltos, RGB565 opaco, profundidad en bytes) y queda en una textura RGBA8 que se enlaza por el mismo id que aurora; el EFB guarda la profundidad en una textura para las copias de Z. Las texturas con paleta todavía salen blancas.
- `tests/test_motor_gles.py` dibuja cuadros sintéticos con Mesa (llvmpipe) para probar el motor en la PC.


#### Compilación fiable del port (0.32.0)

`build-wiisports-arm64.sh` ya no puede dejar un port que parece correcto pero salió de parches incompletos, de un perfil PGO viejo, de otra revisión de WiiCompiled o de recursos de otro disco. Cada paso deja constancia y el port lleva un manifiesto que se comprueba al copiarlo:

| Qué | Cómo | Si falla |
|---|---|---|
| Parches del runtime y de aurora (`choya/retarget.py`) | Cada uno tiene uno de cuatro estados: **aplicado**, **ya aplicado** (compatible, no se toca), **opcional no aplicable** y **obligatorio fallido**. Son obligatorios todos menos los de `PARCHES_OPCIONALES` (textos de la pantalla de inicio y el log de `OSSetPowerCallback`). También se comprueba que los objetos del SDK (`OSThread`, `OSContext`, `GXFifoObj`, `GXData`...) midan lo esperado. Queda en `projects/wiisports/parches.json` y `retarget.md`. | Se detiene antes de compilar; por cada parche dice archivo, parche, motivo y qué hacer. |
| Revisión de WiiCompiled (y su `aurora-main`) | La primera compilación fija el commit en `<WiiCompiled>/choya-dependencias.lock`. `dependencias.json` (en Choya) dice con cuál se probaron los parches. Los cambios locales se separan en los de Choya (`runtime-wiisports`, `projects/wiisports`, `generated`, las carpetas de compilación y los parches de aurora, comprobados deshaciéndolos y comparando con el commit) y los ajenos; con cambios ajenos el manifiesto dice que el commit solo no basta para repetir la compilación. Choya nunca hace checkout ni restaura nada. | Si el commit cambió (por ejemplo un `git pull` en WiiCompiled), se detiene y dice cómo volver; `CHOYA_DEPS_ACTUALIZAR=1` acepta el nuevo. |
| Perfil PGO (`CHOYA_OPT=pgo` / `pgo-O3`) | `scripts/pgo-perfil.py` prepara los `.profraw`, hace `llvm-profdata merge` en un archivo temporal, lo valida con `llvm-profdata show` (que se lea y traiga funciones) y solo entonces reemplaza el anterior. `wiisports.profdata.json` guarda su SHA-256, la versión de Clang, los `.profraw` usados y la identidad del código traducido. Desde la 0.32.0 los perfiles de `pgo-generar` llevan esa identidad en el nombre (`pgo-id.txt`), así se avisa si el código cambió desde que se tomaron o si hay perfiles de otra compilación (Clang usa lo que sigue igual, por eso es aviso y no error). | Se detiene y el perfil anterior queda intacto pero no se usa. `CHOYA_PGO_REUSAR=1` lo usa a propósito (si su SHA-256 coincide y es del mismo Clang) y el manifiesto lo anota. |
| Recursos (`assets.wlpk`) | `python3 -m choya recursos` calcula una clave con la huella del código del conversor, el perfil, las opciones, las versiones de numpy/Pillow/SciPy y el **contenido** de `files/` y `sys/` del disco (SHA-256 de cada archivo; no fechas, rutas ni nombres). Si coincide y el paquete guardado es el mismo, se reutiliza; si no, convierte en una carpeta temporal, lee el paquete entero para validarlo y solo entonces reemplaza el anterior. La huella del disco se calcula al preparar el port, nunca al abrir el juego. | Se detiene (el paquete anterior no se presenta como nuevo). `CHOYA_SIN_PAQUETE=1` arma sin él a propósito y el manifiesto dice "SIN assets.wlpk". |
| Carpetas de compilación | Cada modo (`O2`, `O3`, `pgo-generar`, `pgo`, `pgo-O3`) tiene su carpeta; un cambio de opciones lo ve CMake/Ninja y un perfil distinto limpia el código traducido. | — |
| Armado del port | Se arma en `~/choya-port.nuevo`, se escriben `wiisports-choya/manifiesto.json` y `manifiesto.txt`, se verifica y solo entonces reemplaza a `~/choya-port`. La copia en `C:\Users\Public\choya-port` se hace igual (`.nuevo`, verificar, reemplazar). | El port anterior queda como estaba. |
| Copia a la tarjeta | `scripts/copiar-a-consola.ps1` comprueba primero el port de la PC contra su manifiesto, copia con robocopy (sin borrar nada: partidas, ajustes, perfiles, archivos de opciones y `game/DATA` quedan) y después comprueba **cada** archivo del manifiesto en la tarjeta (que exista y tenga el mismo SHA-256). `-SoloVerificar` solo compara. | Dice en rojo qué falta o es distinto y termina con error. |

El manifiesto (`manifiesto.json` para máquinas, `manifiesto.txt` para leer) tiene: versión y commit de Choya (y si había cambios sin commit), commit de WiiCompiled y sus cambios locales, Clang y la imagen de Docker, arquitectura, CPU y glibc que pide el ejecutable, modo y opciones de compilación (y las versiones fijadas de aurora leídas de `CMakeCache.txt`), SHA-256 del `main.dol` y huella del código traducido (`generated/`), el perfil PGO cuando hay, el resultado de los parches, el estado del `assets.wlpk` (o que no lleva) y el SHA-256 y tamaño de cada archivo del port. La **identidad del port** es la huella de todos sus archivos menos `manifiesto.json`, `manifiesto.txt` y `version.txt` (que la lleva escrita): no depende de la fecha y nada se calcula sobre sí mismo. `version.txt` dice `Choya X (commit), modo, port <identidad>` y es la primera línea del resumen de la consola.

Comprobar a mano una copia (por ejemplo la tarjeta montada en WSL): `python3 -m choya verificar-port /mnt/k/roms/ports`.

Lo que no comprueba: que el juego corra en la consola (eso solo se ve en la RG35XX H) ni la versión exacta de Clang 21 de apt.llvm.org (se anota, no se fija: la imagen de Docker la conserva mientras no se reconstruya).

Lo que genera el traductor (`generated\`, `build-wiisports\`) contiene código del juego: no lo subas a ningún repositorio.

### Usar el motor de Choya en otro juego de WiiCompiled

Casi todo lo que se hizo para que Wii Sports corra fluido no depende de Wii Sports. Choya lo copia y lo parcha en cada compilación (`choya/retarget.py`), y cada parte se enciende o apaga con una variable:

| Parte | Dónde vive | Cómo se enciende |
|---|---|---|
| Motor GLES (dibuja el GX con OpenGL ES 3.2, sin Dawn) | `choya/data/aurora/choya_gx_core.cpp`, `choya_gx.cpp`, `sin_dawn/`; pantalla y presentación en `choya/data/choya_gles.cpp` | `CHOYA_GLES=1` |
| Hilo GX (aurora decodifica el GX en otro núcleo) | `choya/data/aurora/choya_hilo.*` y los cambios a aurora de `AURORA_EDITS` | `CHOYA_GX_HILO=1` (`0` lo apaga) |
| Saltar cuadros viejos sin mostrarse | motor GLES | encendido; `CHOYA_GX_SALTAR=0` lo apaga |
| EFB sin recargar (avisar a la GPU por mosaicos antes de un borrado, de cada copia y al terminar el cuadro) | motor GLES | encendido; `CHOYA_GX_INVALIDAR=0` lo apaga |
| TEV mixto (flotante salvo donde el redondeo de 8 bits cambia el resultado) | motor GLES | por defecto; `CHOYA_GX_TEV=exacto` o `flotante` |
| Un programa de copia del EFB por formato | motor GLES | encendido; `CHOYA_GX_COPIA_GENERICA=1` lo apaga |
| Copias del EFB rotadas en 3 texturas (sin esperar a que la GPU suelte la anterior) | motor GLES | apagado desde la 0.31.1; `CHOYA_GX_ROTAR_COPIAS=1` lo prende |
| Copias del EFB directas (`glCopyTexSubImage2D` sin salir del EFB) y conversión después, en tandas | motor GLES | apagado desde la 0.31.1; `CHOYA_GX_COPIA_DIRECTA=1` lo prende |
| Vuelta al EFB perezosa tras las copias (las copias seguidas no van y vuelven) | motor GLES | encendida; `CHOYA_GX_VUELTA_PEREZOSA=0` la apaga |
| Copias del EFB, paletas sobre copias, copias tras DCFlush | motor GLES y parches del runtime | siempre |
| Medir: tiempo por partes del cuadro, GPU por cuadro y por pase | motor GLES | encendido; `CHOYA_GX_TIEMPO_GPU=0`, `CHOYA_GX_GPU_PASES=0` |
| Medir la memoria de la Mali por partes | motor GLES | `CHOYA_GX_MALI=1` |
| Perfil de hilos y núcleos, y fijar el juego y el hilo GX a sus núcleos | `choya_gles.cpp`, `choya_gx_core.cpp` | encendido; `CHOYA_HILOS=0` apaga el perfil |
| Tramos del cuadro (MENU sola 2 s) | `choya/data/choya_perfil.cpp` | MENU sola 2 s en la consola |
| Compilación fiable: parches obligatorios, revisión fijada, PGO validado, caché de recursos por contenido, manifiesto y verificación | `choya/retarget.py` (`PARCHES_OPCIONALES`), `choya/dependencias.py`, `scripts/pgo-perfil.py`, `choya/recursos_cache.py`, `choya/manifiesto.py`, `scripts/copiar-a-consola.ps1` | siempre; `CHOYA_DEPS_ACTUALIZAR=1`, `CHOYA_PGO_REUSAR=1`, `CHOYA_SIN_PAQUETE=1` |
| Optimización del código traducido: `-O3` o PGO con perfil de la consola | `cmake/PublicProducts.cmake` (ediciones de `retarget.py`), `scripts/build-wiisports-arm64.sh`, `scripts/pgo-runtime-aarch64.sh`, guardado del perfil en `choya_gles.cpp` | `CHOYA_OPT=O3`, `pgo-generar`, `pgo`, `pgo-O3` al compilar |

Lo que sí es de cada juego: el mapeo de direcciones del runtime de WiiCompiled y los parches del runtime que tocan funciones por su dirección (por ejemplo `GX__CopyTex_8016fd74` o `GXSetDrawSync`, que en otro juego tienen otra dirección y hay que buscarlas en su runtime), los arreglos de memoria propios de Wii Sports (préstamos entre montones, `CHOYA_PRESTAMOS`), el paquete de recursos convertidos (`assets.wlpk`, que el conversor arma del disco de ese juego) y el lanzador de KNULLI (nombres de carpetas y archivos).

## Formato `.wlpk`

Little-endian, alineado a 16 bytes: cabecera de 64 bytes, tabla de entradas de 48 bytes ordenada por clave (búsqueda binaria), nombres, manifiesto JSON y datos. La especificación completa está en `choya/pack.py` y `runtime/include/wl/pack.h`.

## Pruebas

```sh
python -m unittest discover tests
```

133 pruebas con datos sintéticos (algunas necesitan el runtime compilado, Mesa, el WiiCompiled real o PowerShell): Yaz0/LZ77, U8, BRRES, TPL, los 11 formatos GX, ETC2/EAC (calidad y que nunca desborde el modo diferencial), DSP-ADPCM, BRSTM con bloques entrelazados, respaldo por calidad, audio ADPCM con bucle decodificado en C++, modelos MDL0, TEV con la configuración de materiales reales compilado y dibujado en OpenGL ES por software (Mesa llvmpipe) con revisión de píxeles, conversión de punta a punta, una prueba cruzada donde el runtime C++ lee el paquete de Python (claves, caché y audio idénticos), la lectura de `main.dol` y del arranque para WiiCompiled, el cambio de direcciones del runtime de WiiCompiled y la vigilancia de la memoria del juego (montones de MEM falsos). Desde la 0.32.0 también la fiabilidad de la compilación (`tests/test_fiabilidad.py`) y `build-wiisports-arm64.sh` de punta a punta con un Docker falso (`tests/test_script_compilacion.py`): parche obligatorio que detiene, parches que se vuelven a aplicar, merge PGO fallido sin usar el perfil viejo, recurso cambiado que invalida el paquete, empaquetado fallido que conserva el port anterior y verificación (Python y `copiar-a-consola.ps1`) que detecta un archivo alterado.

## Pendiente

- **Confirmar en la consola** lo que se probó en la PC con Mesa: el códec ETC2/EAC (incluido RGB8A1) en la Mali-G31 con `wl_view` y el archivo `verificar`, y las animaciones en `choya_model`.
- **Secuencias del BRSAR** (RSEQ, música y efectos hechos como MIDI con instrumentos de un RBNK): no son audio grabado; seguirían en el disco hasta tener un secuenciador.
- **Luces de la escena:** los modelos se iluminan con una luz direccional del visor; las luces de cada escena (`LightSet`, `SCN0`) no se leen todavía. Los modos de mapeo de NW4R por cámara o luz (`ref_camera`, `ref_light`) usan la cámara del visor.
- **Animaciones de material** (SRT0 de texturas, CLR0 de colores, PAT0 de cambio de textura, VIS0 de visibilidad) no se leen; solo las de huesos (CHR0).

## Legal

No incluye ni descarga datos, claves ni binarios de Nintendo. Trabaja sobre el disco que extraes de tu propia copia, en tu equipo.
