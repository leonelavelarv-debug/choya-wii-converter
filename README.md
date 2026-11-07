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
13. Escribe `projects\wiisports\retarget.md` con todo lo que cambió y lo que no pudo resolver.

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


Para arrancarlo y ver dónde se detiene:

```sh
bash scripts/run-wiisports-wsl.sh /mnt/c/wii/RSPE01/DATA
python3 -m choya donde 0x80009D90      # qué función de Wii Sports hay en una dirección
```

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
