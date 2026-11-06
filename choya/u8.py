"""Archivos U8 (.arc, .szs descomprimido, .carc descomprimido)."""
from __future__ import annotations

import struct

U8_MAGIC = b"\x55\xAA\x38\x2D"


def is_u8(data: bytes) -> bool:
    return data[:4] == U8_MAGIC


def read_u8(data: bytes) -> list[tuple[str, bytes]]:
    """Devuelve la lista de (ruta, datos) de los archivos dentro del U8."""
    if not is_u8(data):
        raise ValueError("no es un archivo U8")
    root_off, header_size, data_off = struct.unpack_from(">III", data, 4)
    # Nodo raíz: tipo(1) nombre(3) offset(4) tamaño(4) — tamaño = número total de nodos.
    total = struct.unpack_from(">I", data, root_off + 8)[0]
    strings = root_off + total * 12
    nodes = [struct.unpack_from(">III", data, root_off + i * 12) for i in range(total)]

    def name_at(off: int) -> str:
        end = data.index(b"\x00", strings + off)
        return data[strings + off:end].decode("shift_jis", errors="replace")

    out: list[tuple[str, bytes]] = []
    # Pila de (índice_final_exclusivo, ruta) para directorios abiertos.
    stack: list[tuple[int, str]] = [(total, "")]
    for i in range(1, total):
        while stack and i >= stack[-1][0]:
            stack.pop()
        type_name, off, size = nodes[i]
        is_dir = (type_name >> 24) == 1
        name = name_at(type_name & 0x00FFFFFF)
        parent = stack[-1][1] if stack else ""
        path = f"{parent}/{name}" if parent else name
        if is_dir:
            stack.append((size, path))
        else:
            out.append((path, bytes(data[off:off + size])))
    return out


def build_u8(files: list[tuple[str, bytes]]) -> bytes:
    """Construye un U8 simple (se usa en pruebas). Rutas con '/' generan directorios."""
    tree: dict = {}
    for path, blob in files:
        parts = path.split("/")
        node = tree
        for p in parts[:-1]:
            node = node.setdefault(p, {})
        node[parts[-1]] = blob

    nodes: list[list] = []
    names = bytearray(b"\x00")
    blobs: list[tuple[int, bytes]] = []

    def add_name(n: str) -> int:
        off = len(names)
        names.extend(n.encode() + b"\x00")
        return off

    nodes.append([1, 0, 0, 0])  # raíz

    def walk(d: dict, parent_index: int):
        for name in sorted(d):
            val = d[name]
            if isinstance(val, dict):
                idx = len(nodes)
                nodes.append([1, add_name(name), parent_index, 0])
                walk(val, idx)
                nodes[idx][3] = len(nodes)
            else:
                idx = len(nodes)
                nodes.append([0, add_name(name), 0, len(val)])
                blobs.append((idx, val))

    walk(tree, 0)
    nodes[0][3] = len(nodes)
    root_off = 0x20
    header_size = len(nodes) * 12 + len(names)
    data_off = (root_off + header_size + 0x1F) & ~0x1F
    body = bytearray()
    for idx, blob in blobs:
        nodes[idx][2] = data_off + len(body)
        body.extend(blob)
        body.extend(b"\x00" * ((-len(body)) % 0x20))
    out = bytearray(U8_MAGIC + struct.pack(">III", root_off, header_size, data_off))
    out.extend(b"\x00" * (root_off - len(out)))
    for t, n, o, s in nodes:
        out.extend(struct.pack(">III", (t << 24) | n, o, s))
    out.extend(names)
    out.extend(b"\x00" * (data_off - len(out)))
    out.extend(body)
    return bytes(out)
