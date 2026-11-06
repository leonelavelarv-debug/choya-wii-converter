"""Perfiles de optimización para la RG35XX H (Mali-G31 MP2, 1 GB, pantalla 640x480)."""
from __future__ import annotations

from dataclasses import dataclass, asdict


@dataclass(frozen=True)
class Profile:
    name: str
    description: str
    max_size_3d: int            # lado máximo de texturas de modelos 3D
    max_size_ui: int            # lado máximo de texturas de interfaz (layouts, timg/)
    gles3: bool                 # ETC2/EAC (GLES 3.x); False = formatos sin comprimir de GLES 2
    keep_mips: bool
    audio_rate: int
    audio_mono_sfx: bool        # convertir a mono los sonidos cortos (no la música)
    music_min_seconds: float    # duración a partir de la cual un audio se trata como música

    def to_dict(self):
        return asdict(self)


PROFILES = {
    "rg35xxh": Profile(
        name="rg35xxh",
        description="Equilibrado para la RG35XX H: 3D a 256 px, interfaz a 512 px, ETC2/EAC.",
        max_size_3d=256, max_size_ui=512, gles3=True, keep_mips=True,
        audio_rate=32000, audio_mono_sfx=True, music_min_seconds=20.0,
    ),
    "ligero": Profile(
        name="ligero",
        description="Máximo rendimiento: 3D a 128 px, interfaz a 256 px, audio a 22 kHz.",
        max_size_3d=128, max_size_ui=256, gles3=True, keep_mips=True,
        audio_rate=22050, audio_mono_sfx=True, music_min_seconds=20.0,
    ),
    "calidad": Profile(
        name="calidad",
        description="Más nitidez: 3D a 512 px y la interfaz sin reducir (puede bajar los FPS).",
        max_size_3d=512, max_size_ui=1024, gles3=True, keep_mips=True,
        audio_rate=32000, audio_mono_sfx=False, music_min_seconds=20.0,
    ),
    "gles2": Profile(
        name="gles2",
        description="Compatibilidad sin ETC2: RGB565 / RGBA4444 / LA8 para controladores GLES 2.",
        max_size_3d=256, max_size_ui=512, gles3=False, keep_mips=True,
        audio_rate=32000, audio_mono_sfx=True, music_min_seconds=20.0,
    ),
}


def get(name: str) -> Profile:
    try:
        return PROFILES[name]
    except KeyError:
        raise SystemExit(f"perfil desconocido '{name}'. Opciones: {', '.join(PROFILES)}")
