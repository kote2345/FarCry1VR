"""Inspect the bounded Vulkan LM input snapshot and its original level atlas.

Usage: python buildscripts/analyze-vulkan-lightmap-probe.py snapshot.bin --pak levellm.pak
Uses only the Python standard library. No game launch is performed.
"""
import argparse
import collections
import hashlib
import math
import struct
import zipfile
from pathlib import Path


def uv_sets(archive):
    data = archive.read("dot3lm.dat")
    _, version, pairs, count = struct.unpack_from("<4I", data)
    offset = 32
    atlases = {}
    for atlas in range(pairs):
        _, _, objects = struct.unpack_from("<3I", data, offset)
        offset += 12
        for object_id in struct.unpack_from(f"<{objects}I", data, offset):
            atlases[object_id] = atlas
        offset += objects * 4
    matches = collections.defaultdict(list)
    for _ in range(count):
        object_id, _, vertices = struct.unpack_from("<3I", data, offset)
        # The stock PC disk format has 16-bit EntityId and four-byte alignment.
        offset += 32 if version >= 3 else 12
        coords = data[offset:offset + vertices * 8]
        offset += vertices * 8
        matches[hashlib.sha256(coords).digest()].append((object_id, atlases[object_id]))
    return matches


def sample_dds(data, u, v):
    height, width = struct.unpack_from("<2I", data, 12)
    x = min(width - 1, max(0, int(u * width)))
    y = min(height - 1, max(0, int(v * height)))
    fourcc = data[84:88]
    if fourcc in (b"DXT1", b"DXT3"):
        block_size = 8 if fourcc == b"DXT1" else 16
        offset = 128 + ((y // 4) * ((width + 3) // 4) + x // 4) * block_size
        pixel = (y % 4) * 4 + x % 4
        alpha = 255
        if fourcc == b"DXT3":
            alpha = ((data[offset + pixel // 2] >> ((pixel % 2) * 4)) & 15) * 17
            offset += 8
        a, b, selectors = struct.unpack_from("<HHI", data, offset)
        colors = [((c >> 11) * 255 // 31, ((c >> 5) & 63) * 255 // 63,
                   (c & 31) * 255 // 31) for c in (a, b)]
        if fourcc == b"DXT1" and a <= b:
            colors.extend([tuple((i + j) // 2 for i, j in zip(*colors)), (0, 0, 0)])
        else:
            colors.extend([tuple((2 * i + j) // 3 for i, j in zip(*colors)),
                           tuple((i + 2 * j) // 3 for i, j in zip(*colors[:2]))])
        return (*colors[(selectors >> (2 * pixel)) & 3], alpha)
    bits = struct.unpack_from("<I", data, 88)[0]
    if bits != 32:
        raise ValueError(f"Unsupported atlas DDS format: {fourcc!r}, {bits} bits")
    b, g, r, a = struct.unpack_from("<4B", data, 128 + (y * width + x) * 4)
    return r, g, b, a


def inspect(snapshot, archive):
    source_sets = uv_sets(archive)
    offset = 0
    while offset < len(snapshot):
        header = struct.unpack_from("<24I", snapshot, offset)
        offset += 96
        if header[:2] != (0x4C4D5652, 1):
            raise ValueError("Unknown snapshot record")
        frame, record, vertex_format, stride, vertices, indices, topology, state = header[2:10]
        names = [snapshot[offset + i * 256:offset + (i + 1) * 256]
                 .split(b"\0", 1)[0].decode("utf-8", "replace") for i in range(5)]
        offset += 1280
        constants = struct.unpack_from("<85f", snapshot, offset)
        offset += 340
        offset += vertices * stride
        raw_uvs = snapshot[offset:offset + vertices * 8]
        offset += vertices * 8
        uvs = list(struct.iter_unpack("<2f", raw_uvs))
        triangles = struct.unpack_from(f"<{indices}H", snapshot, offset)
        offset += indices * 2
        matches = source_sets.get(hashlib.sha256(raw_uvs).digest(), [])
        invalid = sum(not all(math.isfinite(v) for v in uv) for uv in uvs)
        out_of_range = sum(index >= vertices for index in triangles)
        print(f"Draw {record}, frame {frame}: {names[0]}, VF {vertex_format}, stride {stride}, "
              f"{vertices} vertices, {indices} indices, state 0x{state:x}, shadows 0x{header[14]:x}")
        print(f"  Textures: {names[1:]}")
        print(f"  Exact disk UV match (object, atlas): {matches}; "
              f"invalid UVs {invalid}, invalid indices {out_of_range}")
        print(f"  Ambient RGB: {constants[24:27]}; UV selectors: {header[18:21]}")
        lm_name = names[3].replace("\\", "/").rsplit("/", 1)[-1] + ".dds"
        if lm_name not in archive.namelist() or topology != 0 or invalid or out_of_range:
            continue
        image = archive.read(lm_name)
        _, width = struct.unpack_from("<2I", image, 12)
        zero_centers = 0
        spans = []
        for i in range(0, indices - 2, 3):
            coords = [uvs[k] for k in triangles[i:i + 3]]
            center = tuple(sum(uv[axis] for uv in coords) / 3 for axis in range(2))
            zero_centers += max(sample_dds(image, *center)[:3]) == 0
            spans.append(max(max(uv[axis] for uv in coords) - min(uv[axis] for uv in coords)
                             for axis in range(2)) * width)
        spans.sort()
        if spans:
            print(f"  Atlas {lm_name}: black triangle centers {zero_centers}/{len(spans)}, "
                  f"UV span in texels median {spans[len(spans)//2]:.2f}, max {spans[-1]:.2f}")
        if matches and not any(lm_name.lower() == f"c{atlas}.dds" for _, atlas in matches):
            print("  MISMATCH: selected color atlas differs from the atlas owning this UV set")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("snapshot", type=Path)
    parser.add_argument("--pak", type=Path, required=True)
    args = parser.parse_args()
    with zipfile.ZipFile(args.pak) as archive:
        inspect(args.snapshot.read_bytes(), archive)
