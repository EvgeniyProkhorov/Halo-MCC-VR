#!/usr/bin/env python3
"""Minimal reader for legacy DirectX binary ``.x`` mesh exports.

The MCC editing kits' ``tool.exe export-render-model-mesh`` command writes the
standard ``xof 0303bin 0032`` form.  Current Blender releases no longer ship a
DirectX importer, so this module reads the documented token stream and exposes
the Frame/Mesh hierarchy needed by authoring-kit generators.

This is intentionally not a general-purpose X-file implementation.  It keeps
all data objects and transforms, but mesh extraction supports the standard
``Mesh`` template emitted by the editing-kit tools: positions and polygon
indices.  Normals, UVs, materials, skin weights, and animation children remain
available in ``XObject.children`` but are not interpreted here.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
import argparse
import json
import math
import struct
import uuid


TOKEN_NAME = 1
TOKEN_STRING = 2
TOKEN_INTEGER = 3
TOKEN_GUID = 5
TOKEN_INTEGER_LIST = 6
TOKEN_FLOAT_LIST = 7

TOKEN_OBRACE = 10
TOKEN_CBRACE = 11
TOKEN_OPAREN = 12
TOKEN_CPAREN = 13
TOKEN_OBRACKET = 14
TOKEN_CBRACKET = 15
TOKEN_OANGLE = 16
TOKEN_CANGLE = 17
TOKEN_DOT = 18
TOKEN_COMMA = 19
TOKEN_SEMICOLON = 20
TOKEN_TEMPLATE = 31
TOKEN_WORD = 40
TOKEN_DWORD = 41
TOKEN_FLOAT = 42
TOKEN_DOUBLE = 43
TOKEN_CHAR = 44
TOKEN_UCHAR = 45
TOKEN_SWORD = 46
TOKEN_SDWORD = 47
TOKEN_VOID = 48
TOKEN_LPSTR = 49
TOKEN_UNICODE = 50
TOKEN_CSTRING = 51
TOKEN_ARRAY = 52

_PUNCTUATION = {
    TOKEN_OBRACE: "OBRACE",
    TOKEN_CBRACE: "CBRACE",
    TOKEN_OPAREN: "OPAREN",
    TOKEN_CPAREN: "CPAREN",
    TOKEN_OBRACKET: "OBRACKET",
    TOKEN_CBRACKET: "CBRACKET",
    TOKEN_OANGLE: "OANGLE",
    TOKEN_CANGLE: "CANGLE",
    TOKEN_DOT: "DOT",
    TOKEN_COMMA: "COMMA",
    TOKEN_SEMICOLON: "SEMICOLON",
    TOKEN_TEMPLATE: "TEMPLATE",
    TOKEN_WORD: "WORD",
    TOKEN_DWORD: "DWORD",
    TOKEN_FLOAT: "FLOAT",
    TOKEN_DOUBLE: "DOUBLE",
    TOKEN_CHAR: "CHAR",
    TOKEN_UCHAR: "UCHAR",
    TOKEN_SWORD: "SWORD",
    TOKEN_SDWORD: "SDWORD",
    TOKEN_VOID: "VOID",
    TOKEN_LPSTR: "LPSTR",
    TOKEN_UNICODE: "UNICODE",
    TOKEN_CSTRING: "CSTRING",
    TOKEN_ARRAY: "ARRAY",
}


@dataclass(frozen=True)
class Token:
    kind: str
    value: object = None
    offset: int = 0


@dataclass
class XObject:
    """One template-backed data object from an X file."""

    type_name: str
    name: str = ""
    records: list[Token] = field(default_factory=list)
    children: list["XObject"] = field(default_factory=list)

    def walk(self):
        yield self
        for child in self.children:
            yield from child.walk()


@dataclass(frozen=True)
class XMesh:
    name: str
    vertices: tuple[tuple[float, float, float], ...]
    faces: tuple[tuple[int, ...], ...]
    transform: tuple[float, ...]


class XFormatError(ValueError):
    pass


def _read_u16(data: bytes, offset: int) -> tuple[int, int]:
    if offset + 2 > len(data):
        raise XFormatError(f"truncated WORD at 0x{offset:X}")
    return struct.unpack_from("<H", data, offset)[0], offset + 2


def _read_u32(data: bytes, offset: int) -> tuple[int, int]:
    if offset + 4 > len(data):
        raise XFormatError(f"truncated DWORD at 0x{offset:X}")
    return struct.unpack_from("<I", data, offset)[0], offset + 4


def read_tokens(path: str | Path) -> list[Token]:
    """Read the standard binary token stream from ``path``."""

    source = Path(path)
    data = source.read_bytes()
    if len(data) < 16 or data[:4] != b"xof ":
        raise XFormatError(f"{source}: not a DirectX X file")
    version = data[4:8]
    encoding = data[8:12]
    float_bits = data[12:16]
    if version != b"0303" or encoding != b"bin ":
        raise XFormatError(
            f"{source}: unsupported header {data[:16]!r}; expected xof 0303bin")
    if float_bits not in (b"0032", b"0064"):
        raise XFormatError(f"{source}: unsupported float size {float_bits!r}")
    float_size = 4 if float_bits == b"0032" else 8
    float_code = "f" if float_size == 4 else "d"

    result: list[Token] = []
    offset = 16
    while offset < len(data):
        token_offset = offset
        token_id, offset = _read_u16(data, offset)
        if token_id in (TOKEN_NAME, TOKEN_STRING):
            count, offset = _read_u32(data, offset)
            end = offset + count
            if end > len(data):
                raise XFormatError(f"truncated string at 0x{token_offset:X}")
            value = data[offset:end].decode("utf-8", errors="replace")
            offset = end
            if token_id == TOKEN_STRING:
                terminator, offset = _read_u32(data, offset)
                if terminator not in (TOKEN_COMMA, TOKEN_SEMICOLON):
                    raise XFormatError(
                        f"invalid string terminator {terminator} at 0x{offset - 4:X}")
                result.append(Token("STRING", (value, terminator), token_offset))
            else:
                result.append(Token("NAME", value, token_offset))
        elif token_id == TOKEN_INTEGER:
            value, offset = _read_u32(data, offset)
            result.append(Token("INTEGER", value, token_offset))
        elif token_id == TOKEN_GUID:
            end = offset + 16
            if end > len(data):
                raise XFormatError(f"truncated GUID at 0x{token_offset:X}")
            result.append(Token("GUID", str(uuid.UUID(bytes_le=data[offset:end])), token_offset))
            offset = end
        elif token_id == TOKEN_INTEGER_LIST:
            count, offset = _read_u32(data, offset)
            end = offset + count * 4
            if end > len(data):
                raise XFormatError(f"truncated integer list at 0x{token_offset:X}")
            values = struct.unpack_from(f"<{count}I", data, offset) if count else ()
            result.append(Token("INTEGER_LIST", values, token_offset))
            offset = end
        elif token_id == TOKEN_FLOAT_LIST:
            count, offset = _read_u32(data, offset)
            end = offset + count * float_size
            if end > len(data):
                raise XFormatError(f"truncated float list at 0x{token_offset:X}")
            values = struct.unpack_from(f"<{count}{float_code}", data, offset) if count else ()
            result.append(Token("FLOAT_LIST", values, token_offset))
            offset = end
        elif token_id in _PUNCTUATION:
            result.append(Token(_PUNCTUATION[token_id], None, token_offset))
        else:
            raise XFormatError(f"unknown token {token_id} at 0x{token_offset:X}")
    return result


def _skip_template(tokens: list[Token], index: int) -> int:
    if tokens[index].kind != "TEMPLATE":
        raise AssertionError("template skip started on a non-template token")
    index += 1
    while index < len(tokens) and tokens[index].kind != "OBRACE":
        index += 1
    if index == len(tokens):
        raise XFormatError("template has no opening brace")
    depth = 1
    index += 1
    while index < len(tokens) and depth:
        if tokens[index].kind == "OBRACE":
            depth += 1
        elif tokens[index].kind == "CBRACE":
            depth -= 1
        index += 1
    if depth:
        raise XFormatError("unterminated template")
    return index


def _looks_like_object(tokens: list[Token], index: int) -> bool:
    if index >= len(tokens) or tokens[index].kind != "NAME":
        return False
    index += 1
    if index < len(tokens) and tokens[index].kind == "NAME":
        index += 1
    return index < len(tokens) and tokens[index].kind == "OBRACE"


def _parse_object(tokens: list[Token], index: int) -> tuple[XObject, int]:
    if not _looks_like_object(tokens, index):
        raise XFormatError(f"expected data object near token {index}")
    type_name = str(tokens[index].value)
    index += 1
    name = ""
    if tokens[index].kind == "NAME":
        name = str(tokens[index].value)
        index += 1
    if tokens[index].kind != "OBRACE":
        raise XFormatError(f"{type_name}: missing opening brace")
    index += 1
    obj = XObject(type_name=type_name, name=name)
    if index < len(tokens) and tokens[index].kind == "GUID":
        obj.records.append(tokens[index])
        index += 1

    while index < len(tokens) and tokens[index].kind != "CBRACE":
        if tokens[index].kind == "TEMPLATE":
            index = _skip_template(tokens, index)
        elif _looks_like_object(tokens, index):
            child, index = _parse_object(tokens, index)
            obj.children.append(child)
        elif tokens[index].kind == "OBRACE":
            # Optional data reference: { name [guid] }.
            start = index
            depth = 0
            while index < len(tokens):
                token = tokens[index]
                obj.records.append(token)
                index += 1
                if token.kind == "OBRACE":
                    depth += 1
                elif token.kind == "CBRACE":
                    depth -= 1
                    if depth == 0:
                        break
            if depth:
                raise XFormatError(f"unterminated data reference at token {start}")
        else:
            obj.records.append(tokens[index])
            index += 1
    if index >= len(tokens):
        raise XFormatError(f"unterminated {type_name} object")
    return obj, index + 1


def read_objects(path: str | Path) -> list[XObject]:
    """Parse top-level data objects, retaining their child hierarchy."""

    tokens = read_tokens(path)
    result: list[XObject] = []
    index = 0
    while index < len(tokens):
        if tokens[index].kind == "TEMPLATE":
            index = _skip_template(tokens, index)
        elif _looks_like_object(tokens, index):
            obj, index = _parse_object(tokens, index)
            result.append(obj)
        else:
            index += 1
    return result


IDENTITY_MATRIX = (
    1.0, 0.0, 0.0, 0.0,
    0.0, 1.0, 0.0, 0.0,
    0.0, 0.0, 1.0, 0.0,
    0.0, 0.0, 0.0, 1.0,
)


def _matrix_multiply(a: tuple[float, ...], b: tuple[float, ...]) -> tuple[float, ...]:
    return tuple(
        sum(a[row * 4 + k] * b[k * 4 + col] for k in range(4))
        for row in range(4) for col in range(4))


def _first_float_list(obj: XObject) -> tuple[float, ...] | None:
    for record in obj.records:
        if record.kind == "FLOAT_LIST":
            return tuple(float(value) for value in record.value)
    return None


def _mesh_geometry(obj: XObject) -> tuple[tuple[tuple[float, float, float], ...], tuple[tuple[int, ...], ...]]:
    integer_lists = [tuple(int(value) for value in record.value)
                     for record in obj.records if record.kind == "INTEGER_LIST"]
    float_lists = [tuple(float(value) for value in record.value)
                   for record in obj.records if record.kind == "FLOAT_LIST"]
    integer_scalars = [int(record.value) for record in obj.records
                       if record.kind == "INTEGER"]
    if not float_lists:
        raise XFormatError(f"Mesh {obj.name!r} has no position list")

    if integer_lists and len(integer_lists[0]) == 1:
        vertex_count = integer_lists[0][0]
        face_streams = integer_lists[1:]
    elif integer_scalars:
        vertex_count = integer_scalars[0]
        face_streams = integer_lists
    else:
        raise XFormatError(f"Mesh {obj.name!r} has no vertex count")
    positions = float_lists[0]
    if len(positions) != vertex_count * 3:
        raise XFormatError(
            f"Mesh {obj.name!r}: {vertex_count} vertices but {len(positions)} coordinates")
    vertices = tuple(tuple(positions[i:i + 3])
                     for i in range(0, len(positions), 3))

    face_data = tuple(value for stream in face_streams for value in stream)
    if not face_data:
        raise XFormatError(f"Mesh {obj.name!r} has no face list")
    face_count = face_data[0]
    cursor = 1
    faces: list[tuple[int, ...]] = []
    for _ in range(face_count):
        if cursor >= len(face_data):
            raise XFormatError(f"Mesh {obj.name!r}: truncated face stream")
        count = face_data[cursor]
        cursor += 1
        end = cursor + count
        if end > len(face_data):
            raise XFormatError(f"Mesh {obj.name!r}: truncated polygon")
        face = tuple(face_data[cursor:end])
        if any(index >= vertex_count for index in face):
            raise XFormatError(f"Mesh {obj.name!r}: vertex index out of range")
        faces.append(face)
        cursor = end
    if cursor != len(face_data):
        raise XFormatError(
            f"Mesh {obj.name!r}: {len(face_data) - cursor} unused face integers")
    return vertices, tuple(faces)


def extract_meshes(objects: list[XObject]) -> list[XMesh]:
    """Return all standard Mesh objects with accumulated Frame transforms."""

    result: list[XMesh] = []

    def visit(obj: XObject, parent_transform: tuple[float, ...], path: tuple[str, ...]):
        local = IDENTITY_MATRIX
        for child in obj.children:
            if child.type_name == "FrameTransformMatrix":
                values = _first_float_list(child)
                if values is None or len(values) != 16 or not all(math.isfinite(v) for v in values):
                    raise XFormatError(f"invalid transform on Frame {obj.name!r}")
                local = values
                break
        transform = _matrix_multiply(parent_transform, local)
        child_path = path + ((obj.name or obj.type_name),)
        if obj.type_name == "Mesh":
            vertices, faces = _mesh_geometry(obj)
            result.append(XMesh(
                name="/".join(child_path),
                vertices=vertices,
                faces=faces,
                transform=transform,
            ))
        for child in obj.children:
            if child.type_name != "FrameTransformMatrix":
                visit(child, transform, child_path)

    for root in objects:
        visit(root, IDENTITY_MATRIX, ())
    return result


def summarize(path: str | Path) -> dict[str, object]:
    objects = read_objects(path)
    meshes = extract_meshes(objects)
    all_vertices = [vertex for mesh in meshes for vertex in mesh.vertices]
    bounds = None
    if all_vertices:
        bounds = {
            "min": [min(vertex[axis] for vertex in all_vertices) for axis in range(3)],
            "max": [max(vertex[axis] for vertex in all_vertices) for axis in range(3)],
        }
    return {
        "top_level": [{"type": obj.type_name, "name": obj.name} for obj in objects],
        "objects": sum(1 for root in objects for _ in root.walk()),
        "meshes": len(meshes),
        "vertices": sum(len(mesh.vertices) for mesh in meshes),
        "faces": sum(len(mesh.faces) for mesh in meshes),
        "bounds_raw": bounds,
        "mesh_names": [mesh.name for mesh in meshes],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("x_file", type=Path)
    parser.add_argument("--summary", action="store_true",
                        help="print parsed object/mesh metadata as JSON")
    args = parser.parse_args()
    if args.summary:
        print(json.dumps(summarize(args.x_file), indent=2))
    else:
        objects = read_objects(args.x_file)
        for root in objects:
            for obj in root.walk():
                print(f"{obj.type_name}\t{obj.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
