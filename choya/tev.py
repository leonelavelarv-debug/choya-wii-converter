"""Decodificación de las listas de comandos GX de materiales y shaders (registros BP/XF de TEV).

Las secciones de material y de shader de un MDL0 contienen listas de comandos GX
(0x61 = escribir registro BP, 0x10 = escribir registros XF). Aquí se localizan sin depender
de desplazamientos exactos de cabecera: se buscan tramos de comandos válidos consecutivos.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field

COLOR_IN = ["CPREV", "APREV", "C0", "A0", "C1", "A1", "C2", "A2", "TEXC", "TEXA", "RASC", "RASA",
            "ONE", "HALF", "KONST", "ZERO"]
ALPHA_IN = ["APREV", "A0", "A1", "A2", "TEXA", "RASA", "KONST", "ZERO"]
DEST = ["PREV", "R0", "R1", "R2"]
BIAS = ["0", "+0.5", "-0.5", "comp"]
SCALE = ["x1", "x2", "x4", "x0.5"]
COMPARE = ["NEVER", "LESS", "EQUAL", "LEQUAL", "GREATER", "NEQUAL", "GEQUAL", "ALWAYS"]
ALPHA_OP = ["AND", "OR", "XOR", "XNOR"]
BLEND_FACTOR = ["ZERO", "ONE", "SRCCLR", "INVSRCCLR", "SRCALPHA", "INVSRCALPHA", "DSTALPHA", "INVDSTALPHA"]
RAS_CHAN = {0: "COLOR0A0", 1: "COLOR1A1", 5: "BUMP", 6: "BUMPN", 7: "ZERO"}

KNOWN_BP = set(range(0x28, 0x30)) | set(range(0xC0, 0xE8)) | set(range(0xF6, 0xFE)) | {
    0x00, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x25, 0x26, 0x27, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0xF3, 0xFE, 0xEE, 0xEF,
    0xF0, 0xF1, 0xF2, 0xF4, 0xF5} | set(range(0x80, 0xC0))


@dataclass
class Cmd:
    kind: str          # "BP" o "XF"
    reg: int
    value: int | list[int]


def _parse_at(d: bytes, p: int, end: int):
    """Un comando GX en p: (Cmd | None si NOP, nueva posición) o None si no es válido."""
    op = d[p]
    if op == 0x00:
        return None, p + 1
    if op == 0x61 and p + 5 <= end:
        reg = d[p + 1]
        if reg not in KNOWN_BP:
            return False
        return Cmd("BP", reg, (d[p + 2] << 16) | (d[p + 3] << 8) | d[p + 4]), p + 5
    if op == 0x10 and p + 5 <= end:
        n = (struct.unpack_from(">H", d, p + 1)[0] & 0xF) + 1
        addr = struct.unpack_from(">H", d, p + 3)[0]
        if p + 5 + 4 * n > end or not (0x1000 <= addr < 0x1080):
            return False
        vals = list(struct.unpack_from(f">{n}I", d, p + 5))
        return Cmd("XF", addr, vals), p + 5 + 4 * n
    return False


def scan_commands(d: bytes, start: int, end: int, min_run: int = 3) -> list[Cmd]:
    """Encuentra tramos de al menos `min_run` comandos BP/XF válidos y los devuelve en orden."""
    out: list[Cmd] = []
    p = start
    end = min(end, len(d))
    while p < end:
        if d[p] not in (0x61, 0x10):
            p += 1
            continue
        q, run = p, []
        while q < end:
            r = _parse_at(d, q, end)
            if r is False:
                break
            cmd, q2 = r
            if cmd is not None:
                run.append(cmd)
            q = q2
        if len(run) >= min_run:
            out.extend(run)
            p = q
        else:
            p += 1
    return out


@dataclass
class TevStage:
    index: int
    texmap: int = -1
    texcoord: int = -1
    tex_enabled: bool = False
    ras: str = "ZERO"
    color: dict = field(default_factory=dict)
    alpha: dict = field(default_factory=dict)
    kcsel: int = 0
    kasel: int = 0


@dataclass
class TevConfig:
    stages: list[TevStage]
    registers: dict          # "R0".."R2" / "K0".."K3" -> (r, g, b, a) en 0..255 (o con signo para color)
    alpha_compare: dict | None
    blend: dict | None
    zmode: dict | None
    other: list[str]


def _s11(v: int) -> int:
    return v - 2048 if v & 0x400 else v


def decode(cmds: list[Cmd]) -> TevConfig:
    stages: dict[int, TevStage] = {}

    def st(i):
        return stages.setdefault(i, TevStage(i))

    regs: dict[str, list[int]] = {}
    ac = blend = zmode = None
    other = []
    for c in cmds:
        if c.kind != "BP":
            other.append(f"XF 0x{c.reg:04X} = {' '.join(f'{v:08X}' for v in c.value)}")
            continue
        r, v = c.reg, c.value
        if 0x28 <= r <= 0x2F:
            for half, sh in ((0, 0), (1, 12)):
                s = st((r - 0x28) * 2 + half)
                s.texmap = (v >> sh) & 7
                s.texcoord = (v >> (sh + 3)) & 7
                s.tex_enabled = bool((v >> (sh + 6)) & 1)
                s.ras = RAS_CHAN.get((v >> (sh + 7)) & 7, str((v >> (sh + 7)) & 7))
        elif 0xC0 <= r <= 0xDF:
            s = st((r - 0xC0) // 2)
            if r % 2 == 0:
                s.color = dict(a=COLOR_IN[(v >> 12) & 15], b=COLOR_IN[(v >> 8) & 15], c=COLOR_IN[(v >> 4) & 15],
                               d=COLOR_IN[v & 15], bias=BIAS[(v >> 16) & 3], sub=bool((v >> 18) & 1),
                               clamp=bool((v >> 19) & 1), scale=SCALE[(v >> 20) & 3], dest=DEST[(v >> 22) & 3])
            else:
                s.alpha = dict(a=ALPHA_IN[(v >> 13) & 7], b=ALPHA_IN[(v >> 10) & 7], c=ALPHA_IN[(v >> 7) & 7],
                               d=ALPHA_IN[(v >> 4) & 7], bias=BIAS[(v >> 16) & 3], sub=bool((v >> 18) & 1),
                               clamp=bool((v >> 19) & 1), scale=SCALE[(v >> 20) & 3], dest=DEST[(v >> 22) & 3],
                               rswap=v & 3, tswap=(v >> 2) & 3)
        elif 0xE0 <= r <= 0xE7:
            i = (r - 0xE0) // 2
            konst = (v >> 23) & 1
            name = f"K{i}" if konst else ("PREV" if i == 0 else f"R{i}")
            reg = regs.setdefault(name, [0, 0, 0, 0])
            if r % 2 == 0:      # L: R y A
                reg[0], reg[3] = _s11(v & 0x7FF), _s11((v >> 12) & 0x7FF)
            else:               # H: B y G
                reg[2], reg[1] = _s11(v & 0x7FF), _s11((v >> 12) & 0x7FF)
        elif 0xF6 <= r <= 0xFD:
            i = r - 0xF6
            st(i * 2).kcsel, st(i * 2).kasel = (v >> 4) & 31, (v >> 9) & 31
            st(i * 2 + 1).kcsel, st(i * 2 + 1).kasel = (v >> 14) & 31, (v >> 19) & 31
        elif r == 0xF3:
            ac = dict(ref0=v & 0xFF, ref1=(v >> 8) & 0xFF, comp0=COMPARE[(v >> 16) & 7],
                      comp1=COMPARE[(v >> 19) & 7], op=ALPHA_OP[(v >> 22) & 3])
        elif r == 0x41:
            blend = dict(enable=bool(v & 1), logic=bool(v & 2), color_update=bool(v & 8), alpha_update=bool(v & 16),
                         src=BLEND_FACTOR[(v >> 8) & 7], dst=BLEND_FACTOR[(v >> 5) & 7], subtract=bool(v & 0x800))
        elif r == 0x40:
            zmode = dict(test=bool(v & 1), func=COMPARE[(v >> 1) & 7], write=bool(v & 16))
        else:
            other.append(f"BP 0x{r:02X} = {v:06X}")
    return TevConfig(sorted(stages.values(), key=lambda s: s.index),
                     {k: tuple(v) for k, v in regs.items()}, ac, blend, zmode, other)


def format_config(cfg: TevConfig, active: int | None = None) -> list[str]:
    lines = []
    for s in cfg.stages:
        if active is not None and s.index >= active:
            continue
        c, a = s.color, s.alpha
        tex = f"tex mapa {s.texmap} coord {s.texcoord}" if s.tex_enabled else "sin textura"
        lines.append(f"    etapa {s.index}: {tex}, ras {s.ras}, konst c{s.kcsel}/a{s.kasel}")
        if c:
            lines.append(f"      color: {c['dest']} = {'(-)' if c['sub'] else ''}lerp({c['a']}, {c['b']}, {c['c']}) + "
                         f"{c['d']} bias {c['bias']} {c['scale']}{' clamp' if c['clamp'] else ''}")
        if a:
            lines.append(f"      alfa:  {a['dest']} = {'(-)' if a['sub'] else ''}lerp({a['a']}, {a['b']}, {a['c']}) + "
                         f"{a['d']} bias {a['bias']} {a['scale']}{' clamp' if a['clamp'] else ''}")
    for k, v in sorted(cfg.registers.items()):
        lines.append(f"    registro {k} = {v}")
    if cfg.alpha_compare:
        ac = cfg.alpha_compare
        lines.append(f"    alpha test: {ac['comp0']} {ac['ref0']} {ac['op']} {ac['comp1']} {ac['ref1']}")
    if cfg.blend:
        b = cfg.blend
        lines.append(f"    mezcla: {'on' if b['enable'] else 'off'} {b['src']}/{b['dst']}"
                     f"{' resta' if b['subtract'] else ''}{' logic' if b['logic'] else ''}")
    if cfg.zmode:
        z = cfg.zmode
        lines.append(f"    z: test {'on' if z['test'] else 'off'} {z['func']} escritura {'on' if z['write'] else 'off'}")
    lines += [f"    {o}" for o in cfg.other[:12]]
    return lines
