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

Usa Debian 11 (glibc 2.31) para que el binario funcione en KNULLI. Salen `wl_view`, `wl_info` y `wl_selftest` en `runtime/build-aarch64/`.

Para probar en la PC sin GPU: `cmake -S runtime -B runtime/build -DWL_WITH_GL=OFF && cmake --build runtime/build`.

## 5. Probar en la consola (puerta 2 del plan)

```powershell
python -m choya package C:\wii\ports --pack-dir C:\wii\salida --files C:\wii\RSPE01\DATA\files ^
    --runtime runtime\build-aarch64\wl_view --binary wl_view
```

Copia `Wii Sports.sh` y la carpeta `wiisports/` a `ports/` en la microSD. Al abrirlo, `wl_view` sube todas las texturas, imprime en `log.txt` el tiempo y la memoria, y deja recorrerlas con la cruceta (L1/R1 saltan 10, A cambia el zoom, Start sale). Si alguna textura sale negra o rota, el registro dice cuál.

`wl_info assets.wlpk -l` revisa el paquete por SSH sin abrir la pantalla.

## 6. Conectar con la capa GX del juego

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

## Formato `.wlpk`

Little-endian, alineado a 16 bytes: cabecera de 64 bytes, tabla de entradas de 48 bytes ordenada por clave (búsqueda binaria), nombres, manifiesto JSON y datos. La especificación completa está en `choya/pack.py` y `runtime/include/wl/pack.h`.

## Pruebas

```sh
python -m unittest discover tests
```

24 pruebas con datos sintéticos: Yaz0/LZ77, U8, BRRES, TPL, los 11 formatos GX, ETC2/EAC (calidad y que nunca desborde el modo diferencial), DSP-ADPCM, BRSTM con bloques entrelazados, respaldo por calidad, audio ADPCM con bucle decodificado en C++, conversión de punta a punta, y una prueba cruzada donde el runtime C++ lee el paquete de Python (claves, caché y audio idénticos).

## Pendiente

- **BRSAR** (efectos de sonido): se cuenta en el reporte y se deja tal cual en el disco; la capa AX lo decodifica con `wl::dsp_decode`.
- **Modelos (MDL0) y shaders TEV**: se cuentan pero no se convierten offline; van en la capa GX del runtime (puertas 1 y 3 del plan).
- **ETC2 punchthrough (RGB8A1)** para CMPR con alfa de 1 bit: hoy usa RGBA8_EAC (8 bpp); con punchthrough bajaría a 4 bpp.
- El códec ETC2/EAC se verificó con un decodificador propio. Falta confirmarlo en la Mali-G31 con `wl_view`.
- Las texturas C4/C8 usan como clave solo los índices, no la paleta.

## Legal

No incluye ni descarga datos, claves ni binarios de Nintendo. Trabaja sobre el disco que extraes de tu propia copia, en tu equipo.
