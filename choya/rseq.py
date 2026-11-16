"""Secuencias de NW4R (RSEQ) con sus bancos de instrumentos (RBNK): intérprete y sintetizador para prerenderizar en la
PC (ruta A). No es MIDI: es el lenguaje MML de NW4R, con sus pistas, llamadas, bucles, variables y condiciones.

Fuente: ogws (CC0), revisado comando por comando:
  snd_MmlParser.cpp      lectura de notas y comandos (argumentos U8/S16/VMIDI, aleatorio, variable, ejecución
                         condicional), pistas (opentrack), saltos, llamadas, bucles, variables y aritmética/lógica;
  snd_SeqTrack.cpp       espera, notas con duración, "tie", portamento, barrido, volumen/pan/pitch de la pista;
  snd_SeqPlayer.cpp      reloj: ticks por milisegundo = tempo * timebase / 60000, cuadros de 3 ms;
  snd_Bank.cpp, snd_BankFile.cpp  instrumento por programa, tecla y velocidad (tablas de rango e índice), onda,
                         envolvente, tecla original, volumen, pan y afinación;
  snd_Channel.cpp        volumen (velocidad², instrumento, pista, envolvente), tono en semitonos (tecla - tecla
                         original + barrido + pitch bend + LFO) por la afinación, pan;
  snd_EnvGenerator.cpp   ataque, caída, sostenido (tabla en dB) y liberación, en pasos de 1 ms;
  snd_Lfo.cpp            LFO (tabla de seno de 32 pasos, retraso, profundidad, rango, velocidad);
  snd_Util.cpp           CalcRandom (el mismo generador; aquí con la semilla de NW4R al empezar cada secuencia).
Las muestras se leen con brsar.read_wave (las mismas direcciones que WaveFileReader::ReadWaveParam).

Aproximaciones, contadas en el reporte: el remuestreo es lineal (el DSP del Wii usa un filtro polifásico); el pan usa
la curva de raíz cuadrada; no hay efectos (reverb, coro, envíos FX), filtro pasabajos ni sonido envolvente; el volumen
de la secuencia es el del BRSAR. Las secuencias que leen variables o comparan valores que el juego escribe en tiempo de
ejecución se prerenderizan con los valores iniciales (-1): la música interactiva queda en su camino por defecto y el
reporte la marca. El comando "userproc" (llamada al juego) no se puede reproducir y se cuenta.

Bucles: si todas las pistas abiertas saltan hacia atrás en el mismo tick y hacia el mismo tick ya visitado, ese es el
bucle. Se renderiza la introducción y dos vueltas; el cuerpo del bucle sale de la segunda vuelta, que ya trae las colas
de las notas de la primera, así que el punto de bucle no corta notas. Sin bucle, se renderiza hasta que la secuencia
termina y las notas se apagan, con un tope de duración (nunca un render infinito).
"""
from __future__ import annotations

import math
import struct
from collections import Counter
from dataclasses import dataclass, field

import numpy as np

from .audio import DecodedAudio
from .brsar import BrsarError, read_wave

FRAME_MS = 3                     # SoundThread: cada 3 ms
MAX_VOICES = 96                  # voces de AX
MAX_SECONDS = 300.0
TAIL_SECONDS = 8.0               # colas de liberación después del final
VOLUME_MIN_DB = -90.4

# EnvGenerator (snd_EnvGenerator.cpp)
DECIBEL_SQUARE = [
    -723, -722, -721, -651, -601, -562, -530, -503, -480, -460, -442, -425, -410, -396, -383, -371, -360, -349, -339,
    -330, -321, -313, -305, -297, -289, -282, -276, -269, -263, -257, -251, -245, -239, -234, -229, -224, -219, -214,
    -210, -205, -201, -196, -192, -188, -184, -180, -176, -173, -169, -165, -162, -158, -155, -152, -149, -145, -142,
    -139, -136, -133, -130, -127, -125, -122, -119, -116, -114, -111, -109, -106, -103, -101, -99, -96, -94, -91, -89,
    -87, -85, -82, -80, -78, -76, -74, -72, -70, -68, -66, -64, -62, -60, -58, -56, -54, -52, -50, -49, -47, -45, -43,
    -42, -40, -38, -36, -35, -33, -31, -30, -28, -27, -25, -23, -22, -20, -19, -17, -16, -14, -13, -11, -10, -8, -7,
    -6, -4, -3, -1, 0]
ATTACK_TABLE = [
    0.9992175, 0.9984326, 0.9976452, 0.9968553, 0.9960629, 0.9952679, 0.9944704, 0.9936704, 0.9928677, 0.9920625,
    0.9912546, 0.9904441, 0.9896309, 0.9888151, 0.9879965, 0.9871752, 0.9863512, 0.9855244, 0.9846949, 0.9838625,
    0.9830273, 0.9821893, 0.9813483, 0.9805045, 0.9796578, 0.9788081, 0.9779555, 0.9770999, 0.9762413, 0.9753797,
    0.9745150, 0.9736472, 0.9727763, 0.9719023, 0.9710251, 0.9701448, 0.9692612, 0.9683744, 0.9674844, 0.9665910,
    0.9656944, 0.9647944, 0.9638910, 0.9629842, 0.9620740, 0.9611604, 0.9602433, 0.9593226, 0.9583984, 0.9574706,
    0.9565392, 0.9556042, 0.9546655, 0.9537231, 0.9527769, 0.9518270, 0.9508732, 0.9499157, 0.9489542, 0.9479888,
    0.9470195, 0.9460462, 0.9450689, 0.9440875, 0.9431020, 0.9421124, 0.9411186, 0.9401206, 0.9391184, 0.9381118,
    0.9371009, 0.9360856, 0.9350659, 0.9340417, 0.9330131, 0.9319798, 0.9309420, 0.9298995, 0.9288523, 0.9278004,
    0.9267436, 0.9256821, 0.9246156, 0.9235442, 0.9224678, 0.9213864, 0.9202998, 0.9192081, 0.9181112, 0.9170091,
    0.9159016, 0.9147887, 0.9136703, 0.9125465, 0.9114171, 0.9102821, 0.9091414, 0.9079949, 0.9068427, 0.9056845,
    0.9045204, 0.9033502, 0.9021740, 0.9009916, 0.8998029, 0.8986080, 0.8974066, 0.8961988, 0.8949844, 0.8900599,
    0.8824622, 0.8759247, 0.8691861, 0.8636406, 0.8535788, 0.8430189, 0.8286135, 0.8149099, 0.8002172, 0.7780663,
    0.7554750, 0.7242125, 0.6828239, 0.6329169, 0.5592135, 0.4551411, 0.3298770, 0.0000000]
LFO_SIN = [0, 6, 12, 19, 25, 31, 37, 43, 49, 54, 60, 65, 71, 76, 81, 85, 90, 94, 98, 102, 106, 109, 112, 115, 117, 120,
           122, 123, 125, 126, 126, 127, 127]
LFO_PITCH, LFO_VOLUME, LFO_PAN = 0, 1, 2

# Comandos (snd_MmlParser.h)
NAMES = {0x80: "wait", 0x81: "prg", 0x88: "opentrack", 0x89: "jump", 0x8A: "call", 0xA0: "rnd", 0xA1: "ind",
         0xA2: "execif", 0xB0: "timebase", 0xC0: "pan", 0xC1: "volume", 0xC2: "main_volume", 0xC3: "transpose",
         0xC4: "pitchbend", 0xC5: "bendrange", 0xC6: "prio", 0xC7: "notewait", 0xC8: "tie", 0xC9: "porta",
         0xCA: "mod_depth", 0xCB: "mod_speed", 0xCC: "mod_type", 0xCD: "mod_range", 0xCE: "porta_sw",
         0xCF: "porta_time", 0xD0: "attack", 0xD1: "decay", 0xD2: "sustain", 0xD3: "release", 0xD4: "loop_start",
         0xD5: "volume2", 0xD6: "printvar", 0xD7: "span", 0xD8: "lpf_cutoff", 0xD9: "fxsend_a", 0xDA: "fxsend_b",
         0xDB: "mainsend", 0xDC: "init_pan", 0xDD: "mute", 0xDE: "fxsend_c", 0xDF: "damper", 0xE0: "mod_delay",
         0xE1: "tempo", 0xE2: "cmd_e2", 0xE3: "sweep_pitch", 0xFC: "loop_end", 0xFD: "ret", 0xFE: "alloctrack",
         0xFF: "fin"}
IGNORED = {0xD7: "pan envolvente", 0xD8: "filtro pasabajos", 0xD9: "envío FX A", 0xDA: "envío FX B",
           0xDB: "envío principal", 0xDE: "envío FX C", 0xD6: "printvar", 0xE2: "comando E2 (sin uso conocido)"}


class SeqError(ValueError):
    pass


def _s8(v):
    return v - 256 if v >= 128 else v


def _s16(v):
    return v - 65536 if v >= 32768 else v


def pitch_ratio(semitones: float) -> float:
    """Util::CalcPitchRatio(MICROTONE_MAX * cent): la tabla es 2^(n/12) por 1/256 de semitono."""
    micro = int(256 * semitones)
    return 2.0 ** (micro / (256.0 * 12.0))


def volume_ratio(db: float) -> float:
    db = max(VOLUME_MIN_DB, min(6.0, db))
    return 0.0 if db <= VOLUME_MIN_DB else 10.0 ** (int(10 * db) / 200.0)


def calc_release(r: int) -> float:
    if r == 127:
        return 65535.0
    if r == 126:
        return 24.0
    if r < 50:
        return (r * 2 + 1) / 128.0 / 5.0
    return 60.0 / (126 - r) / 5.0


class Envelope:
    ATTACK, DECAY, SUSTAIN, RELEASE = range(4)

    def __init__(self):
        self.set(127, 127, 127, 127)
        self.reset()

    def set(self, a=None, d=None, s=None, r=None):
        if a is not None:
            self.attack = ATTACK_TABLE[a & 127]
        if d is not None:
            self.decay = calc_release(d & 127)
        if s is not None:
            self.sustain = s & 127
        if r is not None:
            self.release = calc_release(r & 127)

    def reset(self):
        self.value = VOLUME_MIN_DB * 10.0
        self.status = self.ATTACK

    def get(self) -> float:
        if self.status == self.ATTACK and self.attack == 0.0:
            return 0.0
        return self.value / 10.0

    def update(self, msec: int):
        if self.status == self.ATTACK:
            while msec > 0:
                self.value *= self.attack
                msec -= 1
                if self.value > -(1.0 / 32.0):
                    self.value = 0.0
                    self.status = self.DECAY
            if self.status != self.DECAY:
                return
        if self.status == self.DECAY:
            target = DECIBEL_SQUARE[self.sustain]
            self.value -= self.decay * msec
            if self.value <= target:
                self.value = target
                self.status = self.SUSTAIN
        elif self.status == self.RELEASE:
            self.value -= self.release * msec


@dataclass
class LfoParam:
    depth: float = 0.0
    range: int = 1
    speed: float = 6.25
    delay: int = 0


class Lfo:
    def __init__(self):
        self.p = LfoParam()
        self.counter = 0.0
        self.delay_counter = 0

    def update(self, msec: int):
        if self.delay_counter < self.p.delay:
            if self.delay_counter + msec <= self.p.delay:
                self.delay_counter += msec
                return
            msec -= self.p.delay - self.delay_counter
            self.delay_counter = self.p.delay
        self.counter += self.p.speed * msec / 1000.0
        self.counter -= int(self.counter)

    def value(self) -> float:
        if self.p.depth == 0.0 or self.delay_counter < self.p.delay:
            return 0.0
        idx = int(4 * (32 * self.counter))
        if idx < 32:
            s = LFO_SIN[idx]
        elif idx < 64:
            s = LFO_SIN[32 - (idx - 32)]
        elif idx < 96:
            s = -LFO_SIN[idx - 64]
        else:
            s = -LFO_SIN[32 - (idx - 96)]
        return s / 127.0 * self.p.depth * self.p.range


# ------------------------------------------------------------------------------------------------ banco (RBNK)

@dataclass
class Inst:
    wave: int
    attack: int
    decay: int
    sustain: int
    release: int
    key: int
    volume: int
    pan: int
    tune: float


class Bank:
    """BankFileReader de ogws: instrumentos por programa, tecla y velocidad; ondas con brsar.read_wave."""

    def __init__(self, d: bytes, off: int, size: int, wave_addr: int):
        if d[off:off + 4] != b"RBNK":
            raise SeqError("no es RBNK")
        self.d, self.off, self.wave_addr = d, off, wave_addr
        self.end = min(len(d), off + (size or len(d)))
        self.version, = struct.unpack_from(">H", d, off + 6)
        if self.version < 0x0100 or self.version > 0x0101:
            raise SeqError(f"RBNK versión {self.version:#x} no soportada")
        data_off, _ds, wave_off, _ws = struct.unpack_from(">IIII", d, off + 0x10)
        self.inst_table = off + data_off + 8
        self.wave_table = off + wave_off + 8
        self.inst_count = self._u32(self.inst_table)
        self.wave_count = self._u32(self.wave_table)
        if self.inst_count > 4096 or self.wave_count > 65536:
            raise SeqError("RBNK dañado")
        self.waves: dict[int, DecodedAudio] = {}

    def _u32(self, o):
        if o < self.off or o + 4 > self.end:
            raise SeqError("lectura fuera del RBNK")
        return struct.unpack_from(">I", self.d, o)[0]

    def _ref(self, o, base):
        if o < self.off or o + 8 > self.end:
            raise SeqError("referencia fuera del RBNK")
        rtype, dtype, _r, value = struct.unpack_from(">BBHI", self.d, o)
        return dtype, (base + value if rtype == 1 else None)

    def _sub(self, dtype, addr, split):
        if addr is None:
            return None
        if dtype == 2:                            # tabla de rangos
            n = self.d[addr]
            i = 0
            while split > self.d[addr + 1 + i]:
                i += 1
                if i >= n:
                    return None
            start = (n + 1 + 3) & ~3
            return addr + start + 8 * i
        if dtype == 3:                            # tabla de índices
            lo, hi = self.d[addr], self.d[addr + 1]
            if split < lo or split > hi:
                return None
            return addr + 4 + 8 * (split - lo)
        return None

    def inst(self, prg: int, key: int, vel: int) -> Inst | None:
        if not 0 <= prg < self.inst_count:
            return None
        ref = self.inst_table + 4 + 8 * prg
        dtype, addr = self._ref(ref, self.inst_table)
        for split in (key, vel):
            if dtype == 1:
                break
            if dtype not in (2, 3):
                return None
            sub = self._sub(dtype, addr, split)
            if sub is None:
                return None
            dtype, addr = self._ref(sub, self.inst_table)
        if dtype != 1 or addr is None or addr + 0x14 > self.end:
            return None
        wave, a, d_, s, r = struct.unpack_from(">iBBBB", self.d, addr)
        okey, vol, pan = self.d[addr + 0xC], self.d[addr + 0xD], self.d[addr + 0xE]
        tune, = struct.unpack_from(">f", self.d, addr + 0x10)
        if wave < 0:
            return None
        if self.version < 0x0101:
            vol, tune = 127, 1.0
        return Inst(wave, a, d_, s, r, okey, vol, pan, tune)

    def wave(self, idx: int) -> DecodedAudio:
        if idx not in self.waves:
            if not 0 <= idx < self.wave_count:
                raise SeqError(f"onda {idx} fuera del banco")
            _t, info = self._ref(self.wave_table + 4 + 8 * idx, self.wave_table)
            if info is None:
                raise SeqError("onda sin datos")
            self.waves[idx] = read_wave(self.d, info, self.wave_addr)
        return self.waves[idx]


# ------------------------------------------------------------------------------------------------ canal (voz)

class Channel:
    def __init__(self, track, inst: Inst, wave: DecodedAudio, key, velocity, length, init_pan, start_block, prio):
        self.track = track
        self.key = key
        self.original_key = inst.key
        self.init_volume = (velocity / 127.0) ** 2 * (inst.volume / 127.0)
        self.tune = inst.tune
        self.env = Envelope()
        self.env.set(inst.attack, inst.decay, inst.sustain, inst.release)
        self.env.reset()
        self.lfo = Lfo()
        self.lfo_target = LFO_PITCH
        self.init_pan = (inst.pan - 64) / 63.0 + init_pan / 63.0
        self.length = length
        self.sweep_pitch, self.sweep_len, self.sweep_counter, self.auto_sweep = 0.0, 0, 0, False
        self.user_volume, self.user_pitch, self.user_ratio, self.user_pan = 1.0, 0.0, 1.0, 0.0
        self.wave = wave
        self.pos = 0.0
        self.active = True
        self.start_block = start_block
        self.priority = prio
        self.blocks: list = []           # (paso de lectura, ganancia inicial, ganancia final, pan) por cuadro

    def release(self):
        if self.env.status != Envelope.RELEASE:
            self.env.status = Envelope.RELEASE

    def sweep_value(self) -> float:
        if self.sweep_pitch == 0.0 or self.sweep_counter >= self.sweep_len:
            return 0.0
        return self.sweep_pitch * (self.sweep_len - self.sweep_counter) / self.sweep_len

    def update(self, out_rate: int, block: int):
        """Channel::Update (periódico): parámetros de este cuadro de 3 ms; avanza envolvente, LFO y barrido."""
        lfo = self.lfo.value()
        volume = self.init_volume * self.user_volume
        ve_init = volume_ratio(self.env.get())
        if self.lfo_target == LFO_VOLUME:
            ve_init *= volume_ratio(6.0 * lfo)
        if self.env.status == Envelope.RELEASE and ve_init == 0.0:
            self.active = False
            return
        cent = self.key - self.original_key + self.sweep_value() + self.user_pitch
        if self.lfo_target == LFO_PITCH:
            cent += lfo
        pitch = pitch_ratio(cent) * self.tune * self.user_ratio
        pan = self.init_pan + self.user_pan + (lfo if self.lfo_target == LFO_PAN else 0.0)
        if self.auto_sweep:
            self.sweep_counter = min(self.sweep_counter + FRAME_MS, self.sweep_len)
        self.lfo.update(FRAME_MS)
        self.env.update(FRAME_MS)
        ve_target = volume_ratio(self.env.get())
        if self.lfo_target == LFO_VOLUME:
            ve_target *= volume_ratio(6.0 * self.lfo.value())
        step = self.wave.rate / out_rate * pitch
        self.blocks.append((step, volume * ve_init, volume * ve_target, max(-1.0, min(1.0, pan))))
        # fin de la onda sin bucle (la voz avisa al canal)
        n = len(self.wave.samples)
        self.pos += step * (out_rate * FRAME_MS // 1000)
        if self.wave.loop_start is None and self.pos >= n:
            self.active = False


# ------------------------------------------------------------------------------------------------ pistas y reproductor

class Track:
    def __init__(self, player, no):
        self.player, self.no = player, no
        self.opened = False
        self.channels: list[Channel] = []
        self.prg, self.priority, self.wait = 0, 64, 0
        self.mute = self.note_finish_wait = self.porta = self.damper = False
        self.volume = self.volume2 = 127
        self.pan = self.init_pan = self.pitch_bend = self.transpose = 0
        self.attack = self.decay = self.sustain = self.release = 0xFF
        self.bend_range, self.porta_key, self.porta_time = 2, 60, 0
        self.sweep_pitch = 0.0
        self.lfo = LfoParam()
        self.lfo_target = LFO_PITCH
        self.vars = [-1] * 16
        self.note_wait, self.tie, self.cmp = True, False, True
        self.stack: list = []           # [(dirección, contador de bucle o None)]
        self.addr = 0
        self.first_tick: dict[int, int] = {}
        self.loop_jump: tuple | None = None   # (tick del salto, tick del destino)

    # lectura con límites
    def byte(self) -> int:
        d = self.player.data
        if not self.player.base <= self.addr < self.player.end:
            raise SeqError(f"pista {self.no}: lectura fuera de la secuencia (0x{self.addr:X})")
        v = d[self.addr]
        self.addr += 1
        return v

    def u16(self):
        return (self.byte() << 8) | self.byte()

    def u24(self):
        return (self.byte() << 16) | (self.byte() << 8) | self.byte()

    def vlq(self):
        v = 0
        for _ in range(5):
            b = self.byte()
            v = (v << 7) | (b & 0x7F)
            if not b & 0x80:
                return v
        raise SeqError("número de longitud variable demasiado largo")

    def var_ptr(self, idx):
        p = self.player
        if idx < 16:
            return p.local_vars, idx
        if idx < 32:
            return p.global_vars, idx - 16
        if idx < 48:
            return self.vars, idx - 32
        return None, 0

    def arg(self, kind):
        p = self.player
        if kind == "u8":
            return self.byte()
        if kind == "s16":
            return self.u16()
        if kind == "vmidi":
            return self.vlq()
        if kind == "var":
            arr, i = self.var_ptr(self.byte())
            p.stats["variables leídas"] += 1
            p.interactive = True
            return arr[i] if arr is not None else 0
        lo, hi = _s16(self.u16()), _s16(self.u16())       # aleatorio
        p.stats["valores aleatorios"] += 1
        return ((p.random() * (hi - lo + 1)) >> 16) + lo

    def update_channel_length(self):
        for ch in self.channels:
            if ch.length > 0:
                ch.length -= 1
            if ch.length == 0 and ch.env.status != Envelope.RELEASE and not self.damper:
                ch.release()
            if not ch.auto_sweep:
                ch.sweep_counter = min(ch.sweep_counter + 1, ch.sweep_len)

    def parse_next_tick(self, tick) -> int:
        if not self.opened:
            return 0
        if self.note_finish_wait:
            if self.channels:
                return 1
            self.note_finish_wait = False
        if self.wait > 0:
            self.wait -= 1
            if self.wait > 0:
                return 1
        guard = 0
        while self.wait == 0 and not self.note_finish_wait:
            guard += 1
            if guard > 100000:
                raise SeqError(f"pista {self.no}: bucle sin espera")
            if self.parse(tick):
                return -1
        return 1

    def parse(self, tick) -> bool:
        """MmlParser::Parse; True = fin de pista."""
        p = self.player
        start = self.addr
        self.first_tick.setdefault(start, tick)
        cmd = self.byte()
        exec_ = True
        kind = None
        if cmd == 0xA2:
            cmd = self.byte()
            exec_ = self.cmp
            p.stats["ejecución condicional"] += 1
        if cmd == 0xA0:
            cmd, kind = self.byte(), "rnd"
        if cmd == 0xA1:
            cmd, kind = self.byte(), "var"
        if not cmd & 0x80:                                  # nota
            vel = self.byte()
            length = self.arg(kind or "vmidi")
            key = cmd + self.transpose
            p.ops["nota"] += 1
            if not exec_:
                return False
            key = max(0, min(127, key))
            if not self.mute:
                self.note_on(key, vel, length if length > 0 else -1)
            if self.note_wait:
                self.wait = length
                if length == 0:
                    self.note_finish_wait = True
            return False
        p.ops[NAMES.get(cmd, f"0x{cmd:02X}")] += 1
        if cmd in IGNORED:
            p.ignored[IGNORED[cmd]] += 1
        hi = cmd & 0xF0
        a1 = a2 = 0
        if hi == 0x80:
            if cmd == 0x80:
                t = self.arg(kind or "vmidi")
                if exec_:
                    self.wait = t
            elif cmd == 0x81:
                a1 = self.arg(kind or "vmidi")
                if exec_:
                    self.prg = a1 & 0xFFFF
            elif cmd == 0x88:
                no, off = self.byte(), self.u24()
                if exec_:
                    p.open_track(no, off, self)
            elif cmd in (0x89, 0x8A):
                off = self.u24()
                if exec_:
                    if cmd == 0x8A:
                        if len(self.stack) < 3:
                            self.stack.append((self.addr, None))
                            self.addr = p.base + off
                    else:
                        target = p.base + off
                        if target <= start and self.loop_jump is None and target in self.first_tick:
                            self.loop_jump = (tick, self.first_tick[target])
                            p.jumps.append(tick)
                        self.addr = target
            else:
                raise SeqError(f"comando 0x{cmd:02X} desconocido")
        elif hi in (0xB0, 0xC0, 0xD0):
            v = self.arg(kind or "u8") & 0xFF          # el argumento se guarda en un u8 (también rnd / var)
            if exec_:
                self.command(cmd, _s8(v) if cmd in (0xC3, 0xC4) else v, start, tick)
        elif hi == 0x90:
            if exec_:
                self.command(cmd, 0, start, tick)
        elif hi == 0xE0:
            a1 = _s16(self.arg(kind or "s16") & 0xFFFF)
            if exec_:
                self.command(cmd, a1, start, tick)
        elif hi == 0xF0:
            if cmd == 0xFE:
                self.u16()
            elif cmd == 0xFF:
                return True
            elif cmd == 0xF0:
                ex = self.byte()
                p.ops[f"ex_{ex:02X}"] += 1
                if (ex & 0xF0) == 0xE0:
                    self.arg(kind or "s16")
                    p.ignored["userproc (llamada al juego)"] += 1
                elif (ex & 0xF0) in (0x80, 0x90):
                    a1 = self.byte()
                    a2 = _s16(self.arg(kind or "s16") & 0xFFFF)
                    if exec_:
                        self.ex_command(ex, a1, a2)
                else:
                    raise SeqError(f"comando extendido 0x{ex:02X} desconocido")
            elif exec_:
                self.command(cmd, 0, start, tick)
        else:
            raise SeqError(f"comando 0x{cmd:02X} desconocido")
        return False

    def command(self, cmd, a, start=0, tick=0):
        p = self.player
        if cmd == 0xE1:
            p.tempo = a
        elif cmd == 0xB0:
            p.timebase = a
        elif cmd == 0xDD:
            self.mute = a != 0
            if a in (1, 2):
                for ch in self.channels:
                    ch.release() if a == 2 else setattr(ch, "active", False)
        elif cmd == 0xC1:
            self.volume = a
        elif cmd == 0xD5:
            self.volume2 = a
        elif cmd == 0xC2:
            p.volume = a
        elif cmd == 0xC3:
            self.transpose = a
        elif cmd == 0xC4:
            self.pitch_bend = a
        elif cmd == 0xC5:
            self.bend_range = a
        elif cmd == 0xC0:
            self.pan = a - 64
        elif cmd == 0xDC:
            self.init_pan = a - 64
        elif cmd == 0xC6:
            self.priority = a
        elif cmd == 0xC7:
            self.note_wait = bool(a)
        elif cmd == 0xCF:
            self.porta_time = a
        elif cmd == 0xCA:
            self.lfo.depth = (a & 0xFF) / 128.0
        elif cmd == 0xCB:
            self.lfo.speed = (a & 0xFF) * (100.0 / 256.0)
        elif cmd == 0xCC:
            self.lfo_target = a
        elif cmd == 0xCD:
            self.lfo.range = a
        elif cmd == 0xE0:
            self.lfo.delay = a * 5
        elif cmd == 0xE3:
            self.sweep_pitch = a / 32.0
        elif cmd == 0xD0:
            self.attack = a
        elif cmd == 0xD1:
            self.decay = a
        elif cmd == 0xD2:
            self.sustain = a
        elif cmd == 0xD3:
            self.release = a
        elif cmd == 0xDF:
            self.damper = (a & 0xFF) >= 64
        elif cmd == 0xC8:
            self.tie = bool(a)
            for ch in self.channels:
                ch.release()
            self.channels = []          # FreeAllChannel: siguen sonando, pero ya no son de la pista
        elif cmd == 0xC9:
            self.porta_key = a + self.transpose
            self.porta = True
        elif cmd == 0xCE:
            self.porta = a != 0
        elif cmd == 0xFD:
            if self.stack:
                self.addr = self.stack.pop()[0]
        elif cmd == 0xD4:
            if len(self.stack) < 3:
                self.stack.append((self.addr, a))
        elif cmd == 0xFC:
            if self.stack:
                addr, count = self.stack[-1]
                if count is None:
                    count = 0
                if count != 0:
                    count -= 1
                    if count == 0:
                        self.stack.pop()
                        return
                self.stack[-1] = (addr, count)
                self.addr = addr
                if count == 0 and self.loop_jump is None and addr in self.first_tick and addr <= start:
                    self.loop_jump = (tick, self.first_tick[addr])   # loop_start 0: bucle infinito
                    p.stats["bucles infinitos (loop_start 0)"] += 1

    def ex_command(self, ex, idx, a2):
        arr, i = self.var_ptr(idx)
        if arr is None:
            return
        p = self.player
        if idx < 32:
            p.interactive = True
        v = arr[i]
        clamp = lambda x: _s16(x & 0xFFFF)
        if ex == 0x80:
            arr[i] = clamp(a2)
        elif ex == 0x81:
            arr[i] = clamp(v + a2)
        elif ex == 0x82:
            arr[i] = clamp(v - a2)
        elif ex == 0x83:
            arr[i] = clamp(v * a2)
        elif ex == 0x84:
            if a2:
                arr[i] = clamp(int(v / a2))
        elif ex == 0x85:
            arr[i] = clamp(v << a2) if a2 >= 0 else clamp(v >> -a2)
        elif ex == 0x86:
            neg = a2 < 0
            r = (p.random() * (abs(a2) + 1)) >> 16
            arr[i] = clamp(-r if neg else r)
            p.stats["valores aleatorios"] += 1
        elif ex == 0x87:
            arr[i] = clamp(v & a2)
        elif ex == 0x88:
            arr[i] = clamp(v | a2)
        elif ex == 0x89:
            arr[i] = clamp(v ^ a2)
        elif ex == 0x8A:
            arr[i] = clamp(~(a2 & 0xFFFF))
        elif ex == 0x8B:
            if a2:
                arr[i] = clamp(int(math.fmod(v, a2)))
        elif ex == 0x90:
            self.cmp = v == a2
        elif ex == 0x91:
            self.cmp = v >= a2
        elif ex == 0x92:
            self.cmp = v > a2
        elif ex == 0x93:
            self.cmp = v <= a2
        elif ex == 0x94:
            self.cmp = v < a2
        elif ex == 0x95:
            self.cmp = v != a2

    def note_on(self, key, vel, length):
        p = self.player
        ch = None
        if self.tie and self.channels:
            ch = self.channels[0]
            ch.key = key
            ch.init_volume = (vel / 127.0) ** 2
        if ch is None:
            inst = p.bank.inst(self.prg, key, vel) if p.bank else None
            if inst is None:
                p.stats["notas sin instrumento"] += 1
                return
            try:
                wave = p.bank.wave(inst.wave)
            except (SeqError, BrsarError, struct.error, ValueError):
                p.stats["notas con onda dañada"] += 1
                return
            ch = p.alloc(self, inst, wave, key, vel, -1 if self.tie else length, self.init_pan,
                         p.priority + self.priority)
            if ch is None:
                return
            self.channels.insert(0, ch)
        if self.attack != 0xFF:
            ch.env.set(a=self.attack)
        if self.decay != 0xFF:
            ch.env.set(d=self.decay)
        if self.sustain != 0xFF:
            ch.env.set(s=self.sustain)
        if self.release != 0xFF:
            ch.env.set(r=self.release)
        sweep = self.sweep_pitch
        if self.porta:
            sweep += self.porta_key - key
        if self.porta_time == 0:
            ch.sweep_pitch, ch.sweep_len, ch.auto_sweep = sweep, length, False
        else:
            t = self.porta_time * self.porta_time
            t = int(t * abs(sweep))
            t = ((t >> 5) << 2) + (t >> 5)
            ch.sweep_pitch, ch.sweep_len, ch.auto_sweep = sweep, t, True
        ch.sweep_counter = 0
        self.porta_key = key

    def update_channel_param(self):
        p = self.player
        vol = (self.volume / 127.0) ** 2 * (self.volume2 / 127.0) ** 2 * (p.volume / 127.0) ** 2 * p.sound_volume
        pitch = self.pitch_bend / 128.0 * self.bend_range
        pan = max(-1.0, min(1.0, self.pan / 63.0))
        for ch in self.channels:
            ch.user_volume, ch.user_pitch, ch.user_pan = vol, pitch, pan
            ch.lfo.p = LfoParam(self.lfo.depth, self.lfo.range, self.lfo.speed, self.lfo.delay)
            ch.lfo_target = self.lfo_target


@dataclass
class RenderResult:
    samples: np.ndarray          # (n, 2) int16
    rate: int
    loop_start: int | None
    ended: bool
    interactive: bool
    ops: Counter = field(default_factory=Counter)
    ignored: Counter = field(default_factory=Counter)
    stats: Counter = field(default_factory=Counter)
    notes: list = field(default_factory=list)

    def to_audio(self) -> DecodedAudio:
        return DecodedAudio(self.samples, self.rate, self.loop_start, "secuencia")


class Player:
    def __init__(self, data: bytes, base: int, end: int, bank: Bank | None, rate: int, sound_volume: float = 1.0):
        self.data, self.base, self.end = data, base, end
        self.bank = bank
        self.rate = rate
        self.sound_volume = sound_volume
        self.tempo, self.timebase, self.volume, self.priority = 120, 48, 127, 64
        self.local_vars = [-1] * 16
        self.global_vars = [-1] * 16
        self.tracks: list[Track | None] = [None] * 16
        self.voices: list[Channel] = []
        self.done: list[Channel] = []
        self.tick = 0
        self.tick_fraction = 0.0
        self.block = 0
        self.seed = 0x12345678
        self.ops: Counter = Counter()
        self.ignored: Counter = Counter()
        self.stats: Counter = Counter()
        self.interactive = False
        self.jumps: list = []

    def random(self) -> int:
        self.seed = (self.seed * 0x19660D + 0x3C6EF35F) & 0xFFFFFFFF
        return self.seed >> 16

    def open_track(self, no, off, opener):
        if not 0 <= no < 16 or no == opener.no:
            return
        t = Track(self, no)
        t.addr = self.base + off
        t.opened = True
        if self.tracks[no] is not None:
            for ch in self.tracks[no].channels:
                ch.release()
        self.tracks[no] = t

    def alloc(self, track, inst, wave, key, vel, length, init_pan, prio) -> Channel | None:
        active = [v for v in self.voices if v.active]
        if len(active) >= MAX_VOICES:   # como AX: se queda sin la voz de menor prioridad (la más vieja)
            victim = min(active, key=lambda v: (v.priority, v.start_block))
            if victim.priority > prio:
                self.stats["notas sin voz libre"] += 1
                return None
            victim.active = False
            self.stats["voces robadas"] += 1
        ch = Channel(track, inst, wave, key, vel, length, init_pan, self.block, prio)
        self.voices.append(ch)
        return ch

    def parse_next_tick(self) -> bool:
        """True si ya no queda ninguna pista abierta."""
        active = False
        for t in self.tracks:
            if t is None:
                continue
            t.update_channel_length()
            if t.parse_next_tick(self.tick) < 0:
                t.opened = False
                for ch in t.channels:
                    ch.release()
            if t.opened:
                active = True
        return not active

    def update(self) -> bool:
        """SeqPlayer::UpdateTick(3): True si la secuencia terminó."""
        per_ms = self.timebase * self.tempo / 60000.0
        if per_ms == 0.0:
            return False
        rest = float(FRAME_MS)
        nxt = self.tick_fraction / per_ms
        while nxt < rest:
            rest -= nxt
            if self.parse_next_tick():
                return True
            self.tick += 1
            per_ms = self.timebase * self.tempo / 60000.0
            if per_ms == 0.0:
                return False
            nxt = 1.0 / per_ms
        nxt -= rest
        self.tick_fraction = nxt * per_ms
        return False

    def step_channels(self):
        for t in self.tracks:
            if t is not None:
                t.update_channel_param()
        for v in self.voices:
            if v.active:
                v.update(self.rate, self.block)
        # canales que terminaron: fuera de su pista (noteFinishWait los espera)
        for t in self.tracks:
            if t is not None and t.channels:
                t.channels = [c for c in t.channels if c.active]
        if any(not v.active for v in self.voices):
            self.done.extend(v for v in self.voices if not v.active)
            self.voices = [v for v in self.voices if v.active]
        self.block += 1


def _mix(channels: list[Channel], total_blocks: int, rate: int) -> np.ndarray:
    """Cada voz se renderiza de una vez (posiciones acumuladas, interpolación lineal, rampa de ganancia por cuadro)."""
    blen = rate * FRAME_MS // 1000
    out = np.zeros((total_blocks * blen, 2), np.float32)
    for ch in channels:
        if not ch.blocks:
            continue
        nb = len(ch.blocks)
        arr = np.array(ch.blocks, np.float64)
        steps = np.repeat(arr[:, 0], blen)
        pos = np.concatenate(([0.0], np.cumsum(steps)[:-1]))
        w = ch.wave
        n = len(w.samples)
        if w.loop_start is not None and n > w.loop_start:
            ls = float(w.loop_start)
            span = n - ls
            pos = np.where(pos >= n, ls + np.mod(pos - ls, span), pos)
            valid = np.ones(len(pos), bool)
        else:
            valid = pos < n - 1
        i0 = np.minimum(pos.astype(np.int64), n - 1)
        frac = (pos - i0).astype(np.float32)
        i1 = np.where(i0 + 1 >= n, (w.loop_start if w.loop_start is not None else n - 1), i0 + 1)
        i1 = np.minimum(i1, n - 1)
        src = w.samples.astype(np.float32) / 32768.0
        ramp = np.tile(np.arange(blen, dtype=np.float32) / blen, nb)
        gain = (np.repeat(arr[:, 1], blen) * (1 - ramp) + np.repeat(arr[:, 2], blen) * ramp).astype(np.float32)
        gain *= valid
        pan = np.repeat(arr[:, 3], blen).astype(np.float32)
        gl, gr = np.sqrt((1 - pan) / 2), np.sqrt((1 + pan) / 2)   # curva de raíz cuadrada
        start = ch.start_block * blen
        end = min(start + nb * blen, len(out))
        k = end - start
        if src.shape[1] >= 2:            # onda estéreo: cada canal a su lado
            left = src[i0[:k], 0] * (1 - frac[:k]) + src[i1[:k], 0] * frac[:k]
            right = src[i0[:k], 1] * (1 - frac[:k]) + src[i1[:k], 1] * frac[:k]
            out[start:end, 0] += left * gain[:k] * gl[:k] * math.sqrt(2)
            out[start:end, 1] += right * gain[:k] * gr[:k] * math.sqrt(2)
        else:
            mono = src[i0[:k], 0] * (1 - frac[:k]) + src[i1[:k], 0] * frac[:k]
            out[start:end, 0] += mono * gain[:k] * gl[:k]
            out[start:end, 1] += mono * gain[:k] * gr[:k]
    return out


def render(data: bytes, base: int, end: int, start_offset: int, bank: Bank | None, rate: int = 32000,
           max_seconds: float = MAX_SECONDS, sound_volume: float = 1.0) -> RenderResult:
    """Prerenderiza una secuencia (pista 0 en base + start_offset)."""
    if not base <= base + start_offset < end:
        raise SeqError("inicio de la secuencia fuera de los datos")
    p = Player(data, base, end, bank, rate, sound_volume)
    t0 = Track(p, 0)
    t0.addr = base + start_offset
    t0.opened = True
    p.tracks[0] = t0
    max_blocks = int(max_seconds * 1000 / FRAME_MS)
    tail_blocks = int(TAIL_SECONDS * 1000 / FRAME_MS)
    ended = False
    loop = None            # (tick inicio, tick fin)
    block_of_tick: dict[int, int] = {}
    second_end_block = None
    loop_blocks = None
    finished_block = None
    while p.block < max_blocks + tail_blocks:
        if finished_block is None and p.block < max_blocks:
            before = p.tick
            if p.update():
                ended = True
                finished_block = p.block
            for t in range(before, p.tick):            # los ticks que se procesaron en este cuadro
                block_of_tick.setdefault(t, p.block)
            if finished_block is not None:
                block_of_tick.setdefault(p.tick, p.block)
        p.step_channels()
        if loop is None and finished_block is None:
            open_tracks = [t for t in p.tracks if t is not None and t.opened]
            if open_tracks and all(t.loop_jump is not None for t in open_tracks):
                ends = {t.loop_jump[0] for t in open_tracks}
                starts = {t.loop_jump[1] for t in open_tracks}
                if len(ends) == 1 and len(starts) == 1:
                    loop = (starts.pop(), ends.pop())
                    for t in open_tracks:      # se espera la segunda vuelta
                        t.loop_jump = None
                    p.jumps = []
                else:
                    p.stats["pistas con bucles distintos"] += 1
                    finished_block = p.block    # no hay un bucle común: se corta con colas
        elif loop is not None and second_end_block is None:
            open_tracks = [t for t in p.tracks if t is not None and t.opened]
            if open_tracks and all(t.loop_jump is not None for t in open_tracks):
                second_end_block = p.block
                break
        if finished_block is not None:
            if not p.voices or p.block >= finished_block + tail_blocks:
                break
    if loop is None and not ended:
        p.stats["sin final ni bucle: cortada al tope"] += 1
    channels = p.done + p.voices
    total = p.block
    mixed = _mix(channels, total, rate)
    blen = rate * FRAME_MS // 1000
    loop_start = None
    if loop is not None and second_end_block is not None:
        ls = block_of_tick.get(loop[0])
        le = block_of_tick.get(loop[1])
        if ls is not None and le is not None and le > ls:
            ls_s, le_s, le2_s = ls * blen, le * blen, second_end_block * blen
            mixed = np.concatenate([mixed[:ls_s], mixed[le_s:le2_s]])
            loop_start = ls_s
            loop_blocks = second_end_block - le
    elif loop is not None:
        p.stats["bucle sin segunda vuelta completa (tope)"] += 1
    peak = float(np.abs(mixed).max()) if len(mixed) else 0.0
    if peak > 1.0:
        p.stats["muestras saturadas"] += int((np.abs(mixed) > 1.0).sum())
    pcm = np.clip(np.rint(mixed * 32767.0), -32768, 32767).astype(np.int16)
    res = RenderResult(pcm, rate, loop_start, ended, p.interactive, p.ops, p.ignored, p.stats)
    if loop_blocks:
        res.notes.append(f"bucle de {loop_blocks * FRAME_MS / 1000:.2f} s")
    return res


def seq_base(d: bytes, off: int, size: int) -> tuple[int, int]:
    """(base de los datos MML, fin) de un RSEQ (SeqFileReader::GetBaseAddress)."""
    if d[off:off + 4] != b"RSEQ":
        raise SeqError("no es RSEQ")
    ver, = struct.unpack_from(">H", d, off + 6)
    if ver > 0x0100 or ver < 0x0100:
        raise SeqError(f"RSEQ versión {ver:#x} no soportada")
    data_off, data_size = struct.unpack_from(">II", d, off + 0x10)
    blk = off + data_off
    if d[blk:blk + 4] != b"DATA":
        raise SeqError("RSEQ sin bloque DATA")
    base = blk + struct.unpack_from(">I", d, blk + 8)[0]
    end = min(len(d), blk + data_size, off + (size or len(d)))
    if not blk < base <= end:
        raise SeqError("RSEQ con la base fuera del bloque")
    return base, end
