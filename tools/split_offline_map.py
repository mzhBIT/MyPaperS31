"""Catalog XYZ tiles on disk and build resumable, geographically split MPM v2 packs.

Never modifies input tiles. Use --plan-only while QGIS is still exporting.
Requires Python 3 and Pillow; optional NumPy speeds quantization.
Firmware selects regional packs automatically through catalog.mpc.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import sqlite3
import struct
import zlib

import build_offline_map as m


def expected_rectangle(extent, zoom):
    xmin,xmax,ymin,ymax = extent
    half = math.pi*6378137
    n = 1 << zoom
    return (math.floor((xmin+half)/(2*half)*n),
            math.floor((half-ymax)/(2*half)*n),
            math.ceil((xmax+half)/(2*half)*n)-1,
            math.ceil((half-ymin)/(2*half)*n)-1)


def write_json(path, value):
    temporary = path.with_suffix(path.suffix + '.tmp')
    with temporary.open('w', encoding='utf-8') as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


def catalog(root, output, zooms):
    """Use SQLite instead of holding a million paths/tuples in Python RAM."""
    db = sqlite3.connect(output / 'tiles.sqlite')
    db.execute('DROP TABLE IF EXISTS tiles')
    db.execute('CREATE TABLE tiles (z INTEGER, x INTEGER, y INTEGER, path TEXT, '
               'size INTEGER, mtime INTEGER, PRIMARY KEY(z,x,y)) WITHOUT ROWID')
    batch = []
    for z in zooms:
        folder = root / str(z)
        if not folder.is_dir():
            continue
        with os.scandir(folder) as columns:
            for column in columns:
                if not column.is_dir() or not column.name.isdigit():
                    continue
                x = int(column.name)
                with os.scandir(column.path) as files:
                    for file in files:
                        if not file.name.lower().endswith('.png') or not file.name[:-4].isdigit():
                            continue
                        y = int(file.name[:-4])
                        if not (0 <= x < 1 << z and 0 <= y < 1 << z):
                            raise ValueError(f'Invalid XYZ coordinate: {file.path}')
                        st = file.stat()
                        batch.append((z, x, y, file.path, st.st_size, st.st_mtime_ns))
                        if len(batch) >= 5000:
                            db.executemany('INSERT INTO tiles VALUES (?,?,?,?,?,?)', batch)
                            batch.clear()
        print(f'Cataloged zoom {z}', flush=True)
    db.executemany('INSERT INTO tiles VALUES (?,?,?,?,?,?)', batch)
    db.commit()
    return db


def select(db, zooms, region, halo=0):
    base, x0, y0, x1, y1 = region
    for z in zooms:
        factor = 1 << (z-base)
        # Half-open core bounds, plus the configured base-zoom halo on each side.
        limit = 1 << z
        bounds = (max(0, (x0-halo)*factor), max(0, (y0-halo)*factor),
                  min(limit, (x1+halo)*factor), min(limit, (y1+halo)*factor))
        yield from db.execute('SELECT z,x,y,path,size,mtime FROM tiles '
                              'WHERE z=? AND x>=? AND x<? AND y>=? AND y<? ORDER BY x,y',
                              (z, bounds[0], bounds[2], bounds[1], bounds[3]))


def fingerprint(rows):
    digest = hashlib.sha256()
    for row in rows:
        digest.update(json.dumps(row, ensure_ascii=False, separators=(',', ':')).encode('utf-8'))
        digest.update(b'\n')
    return digest.hexdigest()


def region_bounds(region):
    z, x0, y0, x1, y1 = region
    west, _, _, north = m.tile_bounds(z, x0, y0)
    _, south, east, _ = m.tile_bounds(z, x1-1, y1-1)
    return [west, south, east, north]


def plan(db, zooms, split_zoom=14, block=16, halo=1, max_bytes=512*2**20):
    overview = [z for z in zooms if z < split_zoom]
    details = [z for z in zooms if z >= split_zoom]
    packs = []

    def add(name, rows, core=None):
        if not rows:
            return
        bounds = [180., 90., -180., -90.]
        for z, x, y, *_ in rows:
            w, s, e, n = m.tile_bounds(z, x, y)
            bounds = [min(bounds[0], w), min(bounds[1], s), max(bounds[2], e), max(bounds[3], n)]
        size = m.HEADER_SIZE + len(rows)*(m.ENTRY.size+m.TILE_BYTES)
        if size >= 2**31 or len(rows) > 131072:
            raise ValueError(f'{name} exceeds current firmware limits')
        packs.append(dict(file=name+'.mpm', tiles=len(rows), bytes=size,
                          zooms=sorted({row[0] for row in rows}), bounds=bounds,
                          core_bounds=region_bounds(core) if core else bounds,
                          source_fingerprint=fingerprint(rows)))

    if overview:
        rows = list(db.execute('SELECT * FROM tiles WHERE z<? ORDER BY z,x,y', (split_zoom,)))
        add('overview', rows)

    def detail_region(region):
        rows = list(select(db, details, region, halo))
        if not rows:
            return
        size = m.HEADER_SIZE + len(rows)*(m.ENTRY.size+m.TILE_BYTES)
        base, x0, y0, x1, y1 = region
        if size > max_bytes:
            if x1-x0 == 1 and y1-y0 == 1:
                raise ValueError('One block plus overlap exceeds size cap; reduce halo or maximum zoom')
            if x1-x0 >= y1-y0:
                middle = (x0+x1)//2
                detail_region((base,x0,y0,middle,y1))
                detail_region((base,middle,y0,x1,y1))
            else:
                middle = (y0+y1)//2
                detail_region((base,x0,y0,x1,middle))
                detail_region((base,x0,middle,x1,y1))
            return
        add(f'detail-z{base}-{x0}-{y0}-{x1}-{y1}', rows, region)

    if details:
        cells = set()
        for z in details:
            factor = (1 << (z-split_zoom))*block
            cells.update(db.execute('SELECT DISTINCT x / ?, y / ? FROM tiles WHERE z=?',
                                    (factor, factor, z)))
        for x, y in sorted(cells):
            detail_region((split_zoom,x*block,y*block,(x+1)*block,(y+1)*block))
    return packs


def pack_rows(db, info, split_zoom, halo):
    if info['file'] == 'overview.mpm':
        return list(db.execute('SELECT * FROM tiles WHERE z<? ORDER BY z,x,y', (split_zoom,)))
    parts = Path(info['file']).stem.split('-')
    region = (int(parts[1][1:]), *map(int, parts[2:]))
    return list(select(db, info['zooms'], region, halo))


def build_one(db, output, info, split_zoom, halo, version=1, workers=1):
    rows = pack_rows(db, info, split_zoom, halo)
    if fingerprint(rows) != info['source_fingerprint']:
        raise ValueError('Catalog changed after planning')
    target = output / info['file']
    receipt = target.with_suffix('.done.json')
    if target.exists() or receipt.exists():
        if not target.exists() or not receipt.exists():
            raise ValueError(f'Incomplete/unrecognized prior output: {target}; use a new output directory')
        old = json.loads(receipt.read_text(encoding='utf-8'))
        if old != info:
            raise ValueError(f'Source/options changed for {target}; use a new output directory')
        fields = m.verify_pack(target)
        if fields[1] != version or fields[4] != info['tiles'] or list(fields[8:12]) != info['bounds']:
            raise ValueError(f'Prior pack does not match its receipt: {target}')
        print(f'Resume: {target.name} already verified')
        return
    # Detect active QGIS rewrites, both before and after converting a pack.
    def check_source():
        for _, _, _, path, size, stamp in rows:
            st = Path(path).stat()
            if st.st_size != size or st.st_mtime_ns != stamp:
                raise ValueError(f'Source changed while packing: {path}; wait for QGIS to finish')
    check_source()
    m.build_pack([((z,x,y),Path(path)) for z,x,y,path,_,_ in rows], target,
                 version=version, workers=workers)
    check_source()
    write_json(receipt, info)


def write_catalog(output, packs):
    """Small portable device catalog, no JSON parser or full-province index needed."""
    entry = struct.Struct('<64sI4x8d')
    records = bytearray()
    for info in packs:
        name = info['file'].encode('ascii')
        if len(name) >= 64 or '/' in info['file'] or '\\' in info['file']:
            raise ValueError('Invalid catalog filename')
        mask = sum(1 << z for z in info['zooms'])
        records.extend(entry.pack(name, mask, *info['core_bounds'], *info['bounds']))
    target = output/'catalog.mpc'
    temporary = output/'catalog.mpc.tmp'
    with temporary.open('wb') as stream:
        stream.write(struct.pack('<8sHHIIIII', b'MPCAT001', 1, 32, entry.size,
                                 len(packs), zlib.crc32(records), 1, 0))
        stream.write(records)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, target)


def run(args):
    root, output = args.tiles.resolve(), args.output.resolve()
    if not root.is_dir():
        raise ValueError(f'Tile directory not found: {root}')
    if output == root or root in output.parents or output in root.parents:
        raise ValueError('Output must be separate from the input tile tree')
    if not 0 <= args.min_zoom <= args.max_zoom <= 22:
        raise ValueError('Zoom range must be 0..22')
    if not 0 <= args.split_zoom <= 22 or args.block < 1 or args.halo < 0:
        raise ValueError('Invalid grid parameters')
    if not 1 <= args.max_pack_mib <= 1900:
        raise ValueError('Detail pack cap must be 1..1900 MiB')
    if not args.plan_only and not args.extent_3857:
        raise ValueError('Build requires --extent-3857 XMIN XMAX YMIN YMAX to verify full coverage')
    if args.extent_3857:
        a,b,c,d = args.extent_3857
        if not all(math.isfinite(v) for v in (a,b,c,d)) or a >= b or c >= d:
            raise ValueError('Invalid extent')
    output.mkdir(parents=True, exist_ok=True)
    zooms = list(range(args.min_zoom,args.max_zoom+1))
    db = catalog(root, output, zooms)
    try:
        levels = []
        incomplete = False
        for z in zooms:
            count,x0,y0,x1,y1 = db.execute('SELECT COUNT(*),MIN(x),MIN(y),MAX(x),MAX(y) '
                                          'FROM tiles WHERE z=?', (z,)).fetchone()
            rectangular = bool(count) and count == (x1-x0+1)*(y1-y0+1)
            covers_extent = False
            if args.extent_3857 and count:
                a,b,c,d = expected_rectangle(args.extent_3857,z)
                covers_extent = x0 <= a and y0 <= b and x1 >= c and y1 >= d
            incomplete |= not rectangular or not covers_extent
            levels.append(dict(zoom=z,tiles=count,rectangle_complete=rectangular,
                               covers_extent=covers_extent))
        packs = plan(db,zooms,args.split_zoom,args.block,args.halo,int(args.max_pack_mib*2**20))
        version = getattr(args, 'version', 1)
        for info in packs:
            info['version'] = version
        manifest = dict(format=f'MPM-v{version}-packset', source=str(root),
                        snapshot_only=args.plan_only, levels=levels, packs=packs,
                        split_zoom=args.split_zoom, block=args.block, halo=args.halo,
                        detail_cap_mib=args.max_pack_mib,
                        extent_3857=args.extent_3857,
                        attribution='© OpenStreetMap contributors')
        write_json(output/'plan.json',manifest)
        print(f'Plan: {len(packs)} packs, {sum(p["bytes"] for p in packs)/2**30:.2f} GiB including overlap')
        if args.plan_only:
            print('Snapshot only: NOT a completed-export verification. Rescan after QGIS finishes.')
            return
        if incomplete:
            raise ValueError('Missing zooms or holes within a zoom rectangle; wait for the full export')
        for info in packs:
            build_one(db,output,info,args.split_zoom,args.halo,version,getattr(args,'workers',1))
        # Keep plan/receipts stable for resume; actual sizes only enter the final manifest.
        manifest['packs'] = [dict(info, actual_bytes=(output/info['file']).stat().st_size) for info in packs]
        # Only publish a usable set after all packs were successfully completed.
        manifest['snapshot_only'] = False
        write_json(output/'manifest.json',manifest)
        write_catalog(output,packs)
        print(f'Complete: {len(packs)} packs, {sum(p["actual_bytes"] for p in manifest["packs"])/2**30:.2f} GiB')
    finally:
        db.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tiles',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--min-zoom',type=int,default=8)
    parser.add_argument('--max-zoom',type=int,default=16)
    parser.add_argument('--split-zoom',type=int,default=14)
    parser.add_argument('--block',type=int,default=16,help='Core side in split-zoom tiles')
    parser.add_argument('--halo',type=int,default=3,help='Overlap in split-zoom tiles (3 covers a 1216px screen)')
    parser.add_argument('--version',type=int,choices=(1,2),default=2)
    parser.add_argument('--workers',type=int,default=8)
    parser.add_argument('--max-pack-mib',type=float,default=512)
    parser.add_argument('--plan-only',action='store_true')
    parser.add_argument('--extent-3857',type=float,nargs=4,metavar=('XMIN','XMAX','YMIN','YMAX'))
    args = parser.parse_args()
    try:
        run(args)
    except (ValueError,OSError,sqlite3.Error) as error:
        parser.exit(1,f'Error: {error}\n')


if __name__ == '__main__':
    main()
