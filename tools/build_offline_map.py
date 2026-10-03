"""Pack QGIS XYZ PNG tiles into a read-only MPM v1/v2 grayscale map.

Requires Pillow. Coordinates in --bbox are WGS84 lon/lat, NOT AMap GCJ-02.
No contrast threshold is applied: retain all 16 gray levels for initial tests.
"""
import argparse
import math
import os
from pathlib import Path
import struct
import zlib
from concurrent.futures import ThreadPoolExecutor

from PIL import Image
try:
    import numpy as np
except ImportError:
    np = None

MAGIC = b"MPMAP001"
HEADER_SIZE = 96
HEADER = struct.Struct("<8sHHIIIII4dQQII")
ENTRY = struct.Struct("<BBHIIQIII")
TILE_SIZE = 256
TILE_BYTES = TILE_SIZE * TILE_SIZE // 2
MAX_LAT = 85.0511287798066


def tile_bounds(z, x, y):
    n = 1 << z
    lat = lambda row: math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * row / n))))
    return x / n * 360 - 180, lat(y + 1), (x + 1) / n * 360 - 180, lat(y)


def expected_tiles(bbox, z):
    west, south, east, north = bbox
    n = 1 << z
    px = lambda lon: (lon + 180) / 360 * n
    py = lambda lat: (1 - math.asinh(math.tan(math.radians(lat))) / math.pi) / 2 * n
    x0 = max(0, math.floor(px(west)))
    x1 = min(n - 1, math.ceil(px(east) - 1e-10) - 1)
    y0 = max(0, math.floor(py(north)))
    y1 = min(n - 1, math.ceil(py(south) - 1e-10) - 1)
    return {(z, x, y) for x in range(x0, x1 + 1) for y in range(y0, y1 + 1)}


def find_tiles(root, zooms, bbox=None, allow_missing=False):
    root = Path(root)
    found = {}
    wanted = set().union(*(expected_tiles(bbox, z) for z in zooms)) if bbox else None
    for z in zooms:
        for path in (root / str(z)).glob("*/*.png"):
            try:
                x, y = int(path.parent.name), int(path.stem)
            except ValueError:
                continue
            if not (0 <= x < (1 << z) and 0 <= y < (1 << z)):
                raise ValueError(f"Invalid XYZ coordinate: {path}")
            key = z, x, y
            if wanted is None or key in wanted:
                if key in found:
                    raise ValueError(f"Duplicate tile: {key}")
                found[key] = path
    if wanted is not None:
        missing = wanted - found.keys()
        if missing and not allow_missing:
            raise ValueError(f"{len(missing)} tiles still missing; first: {sorted(missing)[:5]}. "
                             "Wait for QGIS to finish, reduce bbox, or explicitly use --allow-missing.")
        if missing:
            print(f"WARNING: allowing {len(missing)} missing tiles")
    if not found:
        raise ValueError("No selected tiles")
    absent = set(zooms) - {key[0] for key in found}
    if absent and not allow_missing:
        raise ValueError(f"No tiles for zoom levels {sorted(absent)}")
    return sorted(found.items())


def gray4_tile(path):
    with Image.open(path) as source:
        source.load()  # Reject truncated/still-being-written PNGs.
        if source.size != (TILE_SIZE, TILE_SIZE):
            raise ValueError(f"Expected 256x256 tile: {path}, got {source.size}")
        rgba = source.convert("RGBA")
    white = Image.new("RGBA", rgba.size, (255, 255, 255, 255))
    white.alpha_composite(rgba)
    pixels = white.convert("L").tobytes()
    if np is not None:
        values = (np.frombuffer(pixels, dtype=np.uint8).astype(np.uint16) * 15 + 127) // 255
        return ((values[::2] << 4) | values[1::2]).astype(np.uint8).tobytes()
    packed = bytearray(TILE_BYTES)
    for i in range(0, len(pixels), 2):
        a = (pixels[i] * 15 + 127) // 255
        b = (pixels[i + 1] * 15 + 127) // 255
        packed[i // 2] = (a << 4) | b
    return bytes(packed)


def unpack_tile(data):
    pixels = bytearray(TILE_SIZE * TILE_SIZE)
    for i, value in enumerate(data):
        pixels[2 * i] = (value >> 4) * 17
        pixels[2 * i + 1] = (value & 15) * 17
    return Image.frombytes("L", (TILE_SIZE, TILE_SIZE), bytes(pixels))


def build_pack(tiles, output, force=False, version=1, workers=1):
    if version not in (1, 2) or not 1 <= workers <= 32:
        raise ValueError("Invalid format version/workers")
    output = Path(output)
    if output.exists() and not force:
        raise ValueError(f"Output already exists: {output}; use --force to replace")
    output.parent.mkdir(parents=True, exist_ok=True)
    # Exclusive temporary creation protects another concurrent packer.
    temporary = output.with_name(output.name + ".tmp")
    records = bytearray()
    bounds = [180.0, 90.0, -180.0, -90.0]
    data_offset = HEADER_SIZE + len(tiles) * ENTRY.size
    created = False
    try:
        with temporary.open("xb+") as stream:
            created = True
            stream.seek(data_offset)
            # Bounded batches keep at most 64 decoded tiles in memory.
            with ThreadPoolExecutor(max_workers=workers) as pool:
                for start in range(0, len(tiles), 64):
                    batch = tiles[start:start+64]
                    for number, (((z, x, y), path), payload) in enumerate(
                            zip(batch, pool.map(gray4_tile, [p for _, p in batch])), start+1):
                        uniform = (version == 2 and payload[0] >> 4 == payload[0] & 15
                                   and payload.count(payload[:1]) == TILE_BYTES)
                        offset = 0 if uniform else stream.tell()
                        if not uniform:
                            stream.write(payload)
                        records.extend(ENTRY.pack(z, int(uniform), payload[0] & 15 if uniform else 0,
                                                  x, y, offset, 0 if uniform else len(payload),
                                                  zlib.crc32(payload), 0))
                        west, south, east, north = tile_bounds(z, x, y)
                        bounds = [min(bounds[0], west), min(bounds[1], south),
                                  max(bounds[2], east), max(bounds[3], north)]
                    if (start//64) % 16 == 0 or number == len(tiles):
                        print(f"Packed {number}/{len(tiles)}", flush=True)
            # All-uniform files still need the complete index area.
            stream.truncate(stream.tell())
            zooms = [key[0] for key, _ in tiles]
            header = HEADER.pack(MAGIC, version, HEADER_SIZE, TILE_SIZE, len(tiles),
                                 min(zooms), max(zooms), 1, *bounds,
                                 HEADER_SIZE, data_offset, ENTRY.size, zlib.crc32(records))
            stream.seek(0)
            stream.write(header.ljust(HEADER_SIZE, b"\0"))
            stream.write(records)
            stream.flush()
            os.fsync(stream.fileno())
        verify_pack(temporary)
        os.replace(temporary, output)
        print(f"Ready: {output} ({output.stat().st_size / 1048576:.2f} MiB)")
    finally:
        if created and temporary.exists():
            temporary.unlink()


def verify_pack(path):
    with Path(path).open("rb") as stream:
        raw = stream.read(HEADER_SIZE)
        if len(raw) != HEADER_SIZE:
            raise ValueError("Truncated header")
        fields = HEADER.unpack_from(raw)
        magic, version, hs, ts, count, minz, maxz, flags = fields[:8]
        index_offset, data_offset, entry_size, index_crc = fields[12:]
        if version not in (1, 2) or (magic, hs, ts, flags, entry_size) != (MAGIC, HEADER_SIZE, 256, 1, ENTRY.size) or any(raw[88:]):
            raise ValueError("Unsupported map header")
        if not count or not 0 <= minz <= maxz <= 22:
            raise ValueError("Invalid zoom/count")
        if index_offset != HEADER_SIZE or data_offset != HEADER_SIZE + count * ENTRY.size:
            raise ValueError("Invalid index layout")
        file_size = os.fstat(stream.fileno()).st_size
        if data_offset > file_size:
            raise ValueError("Invalid/truncated payload size")
        index = stream.read(count * ENTRY.size)
        if zlib.crc32(index) != index_crc:
            raise ValueError("Index CRC mismatch")
        previous = None
        cursor = data_offset
        for i in range(count):
            z, encoding, reserved, x, y, offset, length, crc, reserved2 = ENTRY.unpack_from(index, i * ENTRY.size)
            key = z, x, y
            if previous is not None and key <= previous:
                raise ValueError("Index not uniquely sorted")
            previous = key
            if not minz <= z <= maxz or not 0 <= x < (1 << z) or not 0 <= y < (1 << z):
                raise ValueError(f"Invalid tile coordinate {key}")
            uniform = version == 2 and encoding == 1 and 0 <= reserved <= 15 and offset == length == 0
            normal = encoding == reserved == 0 and length == TILE_BYTES and offset == cursor
            if reserved2 or not (uniform or normal):
                raise ValueError(f"Invalid tile entry {key}")
            if uniform:
                payload = bytes([reserved * 17]) * TILE_BYTES
            else:
                stream.seek(offset)
                payload = stream.read(length)
                cursor += length
            if len(payload) != TILE_BYTES or zlib.crc32(payload) != crc:
                raise ValueError(f"Tile CRC mismatch: {key}")
        if cursor != file_size:
            raise ValueError("Invalid/truncated payload size")
    print(f"Verified {count} tiles, zoom {minz}..{maxz}")
    return fields


def preview_pack(path, output, zoom, lon, lat, width=684, height=1216):
    verify_pack(path)
    n = 1 << zoom
    cx = (lon + 180) / 360 * n * TILE_SIZE
    cy = (1 - math.asinh(math.tan(math.radians(lat))) / math.pi) / 2 * n * TILE_SIZE
    left, top = math.floor(cx - width / 2), math.floor(cy - height / 2)
    image = Image.new("L", (width, height), 255)
    needed = {(zoom, x, y) for x in range(left // 256, (left + width - 1) // 256 + 1)
              for y in range(top // 256, (top + height - 1) // 256 + 1)}
    with Path(path).open("rb") as stream:
        fields = HEADER.unpack_from(stream.read(HEADER_SIZE))
        count, index_offset = fields[4], fields[12]
        stream.seek(index_offset)
        index = stream.read(count * ENTRY.size)
        for i in range(count):
            z, encoding, gray, x, y, offset, length, _, _ = ENTRY.unpack_from(index, i * ENTRY.size)
            if (z, x, y) not in needed:
                continue
            stream.seek(offset)
            payload = bytes([gray * 17]) * TILE_BYTES if encoding == 1 else stream.read(length)
            image.paste(unpack_tile(payload), (x * 256 - left, y * 256 - top))
            needed.remove((z, x, y))
    if needed:
        raise ValueError(f"Preview contains {len(needed)} missing tiles")
    Path(output).parent.mkdir(parents=True, exist_ok=True)
    image.save(output)
    print(f"Preview: {output}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    build = sub.add_parser("build")
    build.add_argument("--tiles", required=True, type=Path)
    build.add_argument("--output", required=True, type=Path)
    build.add_argument("--zooms", default="14,15,16")
    build.add_argument("--bbox", type=float, nargs=4, metavar=("WEST", "SOUTH", "EAST", "NORTH"))
    build.add_argument("--allow-missing", action="store_true")
    build.add_argument("--force", action="store_true")
    build.add_argument("--version", type=int, choices=(1, 2), default=1)
    build.add_argument("--workers", type=int, default=1)
    verify = sub.add_parser("verify")
    verify.add_argument("pack", type=Path)
    preview = sub.add_parser("preview")
    preview.add_argument("pack", type=Path)
    preview.add_argument("--output", required=True, type=Path)
    preview.add_argument("--zoom", required=True, type=int)
    preview.add_argument("--center", required=True, type=float, nargs=2, metavar=("LON", "LAT"))
    args = parser.parse_args()
    try:
        if args.command == "build":
            zooms = sorted(set(int(z) for z in args.zooms.split(",")))
            if not zooms or any(z < 0 or z > 22 for z in zooms):
                raise ValueError("Zoom must be 0..22")
            if args.bbox:
                w, s, e, n = args.bbox
                if not all(math.isfinite(v) for v in args.bbox) or not (-180 <= w < e <= 180 and -MAX_LAT <= s < n <= MAX_LAT):
                    raise ValueError("Invalid WGS84 bbox; crossing the antimeridian is not supported")
            build_pack(find_tiles(args.tiles, zooms, args.bbox, args.allow_missing), args.output,
                       args.force, args.version, args.workers)
        elif args.command == "verify":
            verify_pack(args.pack)
        else:
            lon, lat = args.center
            if not 0 <= args.zoom <= 22 or not -180 <= lon < 180 or not -MAX_LAT < lat < MAX_LAT:
                raise ValueError("Invalid preview zoom/center")
            preview_pack(args.pack, args.output, args.zoom, lon, lat)
    except (ValueError, OSError) as error:
        parser.exit(1, f"Error: {error}\n")


if __name__ == "__main__":
    main()
