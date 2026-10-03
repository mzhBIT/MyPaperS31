"""Select existing MPM regional packs by a WGS84 SHP administrative polygon.

Copies unchanged files to a NEW output directory; never re-encodes map pixels.
No third-party geometry dependency. Boundary-touching core regions are retained.
"""
import argparse
import json
import math
from pathlib import Path
import shutil
import struct

import build_offline_map as m
import split_offline_map as s


def administrative_polygon(shp, name):
    shp = Path(shp)
    projection = shp.with_suffix('.prj').read_text(encoding='utf-8')
    if 'PROJCS' in projection or 'WGS_1984' not in projection:
        raise ValueError('Administrative SHP must use WGS84 longitude/latitude')
    encoding = shp.with_suffix('.cpg').read_text(encoding='ascii').strip()
    with shp.with_suffix('.dbf').open('rb') as stream:
        header = stream.read(32)
        count = struct.unpack_from('<I', header, 4)[0]
        hs, rs = struct.unpack_from('<HH', header, 8)
        descriptors = stream.read(hs-32)
        fields, offset = {}, 1
        for i in range(0, len(descriptors)-1, 32):
            d = descriptors[i:i+32]
            if len(d) != 32 or d[0] == 13:
                break
            field = d[:11].split(b'\0')[0].decode('ascii')
            fields[field] = (offset, d[16])
            offset += d[16]
        if 'name' not in fields or offset > rs:
            raise ValueError('Invalid administrative DBF fields')
        matches = []
        for record in range(count):
            row = stream.read(rs)
            if len(row) != rs:
                raise ValueError('Truncated administrative DBF')
            if row[:1] == b'*':
                continue
            values = {key: row[start:start+size].decode(encoding).strip()
                      for key, (start, size) in fields.items()}
            if values['name'] == name:
                matches.append((record, values))
    if len(matches) != 1:
        raise ValueError(f'Expected one administrative boundary for {name}, found {len(matches)}')
    index, attributes = matches[0]
    with shp.with_suffix('.shx').open('rb') as stream:
        stream.seek(100+index*8)
        offset, length = struct.unpack('>II', stream.read(8))
    with shp.open('rb') as stream:
        stream.seek(offset*2)
        record, actual_length = struct.unpack('>II', stream.read(8))
        if actual_length != length or record != index+1:
            raise ValueError('SHP/SHX record mismatch')
        data = stream.read(length*2)
    if len(data) != length*2 or len(data) < 44 or struct.unpack_from('<I', data)[0] not in (5,15,25):
        raise ValueError('Expected a polygon SHP record')
    bounds = struct.unpack_from('<4d', data, 4)
    parts, points = struct.unpack_from('<2I', data, 36)
    if not parts or parts > points or 44+parts*4+points*16 > len(data):
        raise ValueError('Invalid SHP polygon layout')
    starts = list(struct.unpack_from(f'<{parts}I', data, 44)) + [points]
    if starts[0] != 0 or any(a >= b for a,b in zip(starts,starts[1:])):
        raise ValueError('Invalid SHP polygon rings')
    xy = list(struct.iter_unpack('<2d', data[44+parts*4:44+parts*4+points*16]))
    if not all(math.isfinite(x) and math.isfinite(y) and -180<=x<=180 and -90<=y<=90 for x,y in xy):
        raise ValueError('Invalid WGS84 coordinates')
    rings = [xy[a:b] for a,b in zip(starts, starts[1:])]
    if any(len(r)<4 or r[0]!=r[-1] for r in rings):
        raise ValueError('Unclosed polygon ring')
    return rings, bounds, attributes


def point_inside(x, y, rings):
    inside = False
    for ring in rings:
        for (ax,ay),(bx,by) in zip(ring,ring[1:]):
            cross = (x-ax)*(by-ay)-(y-ay)*(bx-ax)
            if abs(cross) < 1e-12 and min(ax,bx)<=x<=max(ax,bx) and min(ay,by)<=y<=max(ay,by):
                return True
            if (ay>y)!=(by>y) and x < ax+(y-ay)*(bx-ax)/(by-ay):
                inside = not inside
    return inside  # Even/odd parity supports holes and multiple islands.


def segment_hits(a, b, rect):
    w,south,e,n = rect
    ax,ay = a; bx,by = b
    if max(ax,bx)<w or min(ax,bx)>e or max(ay,by)<south or min(ay,by)>n:
        return False
    low, high = 0., 1.
    for origin,delta,start,end in ((ax,bx-ax,w,e),(ay,by-ay,south,n)):
        if delta == 0:
            if not start<=origin<=end:
                return False
        else:
            t0,t1 = sorted(((start-origin)/delta,(end-origin)/delta))
            low,high = max(low,t0),min(high,t1)
            if low > high:
                return False
    return True


def intersects(rings, rect):
    w,south,e,n = rect
    for ring in rings:
        for a,b in zip(ring,ring[1:]):
            if segment_hits(a,b,rect):
                return True
    return any(point_inside(x,y,rings) for x,y in ((w,south),(e,south),(e,n),(w,n)))


def select_packs(packs, rings, bounds):
    w,south,e,n = bounds
    selected = []
    for info in packs:
        if info['file'] == 'overview.mpm':
            selected.append(info)
            continue
        a,b,c,d = info['core_bounds']
        if a<=e and c>=w and b<=n and d>=south and intersects(rings,(a,b,c,d)):
            selected.append(info)
    if not selected or selected[0]['file'] != 'overview.mpm' or len(selected) == 1:
        raise ValueError('No overview or no intersecting detail packs')
    return selected


def set_region_start(output):
    """Only catalog metadata changes: start at the selected region's bounds center."""
    manifest = json.loads((output/'manifest.json').read_text(encoding='utf-8'))
    manifest['packs'][0] = dict(manifest['packs'][0], core_bounds=manifest['boundary_bounds'])
    s.write_json(output/'manifest.json',manifest)
    s.write_catalog(output,manifest['packs'])


def run(args):
    source, output = args.source.resolve(), args.output.resolve()
    if output == source or source in output.parents or output in source.parents:
        raise ValueError('Output must be independent of source')
    if output.exists():
        raise ValueError('Output already exists; use a NEW directory')
    manifest = json.loads((source/'manifest.json').read_text(encoding='utf-8'))
    if manifest.get('snapshot_only') or manifest.get('format') not in ('MPM-v1-packset','MPM-v2-packset'):
        raise ValueError('Expected a completed map set')
    rings,bounds,attributes = administrative_polygon(args.admin_shp,args.name)
    selected = select_packs(manifest['packs'],rings,bounds)
    total = sum(p['actual_bytes'] for p in selected)
    print(f'Selected {len(selected)} packs, {total/2**30:.2f} GiB; polygon points={sum(map(len,rings))}',flush=True)
    if args.plan_only:
        return
    # Validate names and source sizes before creating any output.
    for info in selected:
        filename=info['file']
        if Path(filename).name!=filename or '/' in filename or '\\' in filename or '..' in filename:
            raise ValueError('Unsafe pack filename')
        if (source/filename).stat().st_size != info['actual_bytes']:
            raise ValueError(f'Source pack size changed: {filename}')
    output.mkdir(parents=True,exist_ok=False)
    for number,info in enumerate(selected,1):
        original=source/info['file']
        before=original.stat()
        temporary=output/(info['file']+'.tmp')
        shutil.copyfile(original,temporary)
        fields=m.verify_pack(temporary)
        after=original.stat()
        if (before.st_size,before.st_mtime_ns)!=(after.st_size,after.st_mtime_ns):
            raise ValueError('Source changed during copy')
        if fields[4]!=info['tiles'] or list(fields[8:12])!=info['bounds']:
            raise ValueError('Copied pack does not match source manifest')
        temporary.rename(output/info['file'])
        print(f'Copied and verified {number}/{len(selected)}: {info["file"]}',flush=True)
    result=dict(format=manifest['format'],snapshot_only=False,packs=selected,
                region=args.name,selection='administrative-polygon intersects pack core',
                boundary_attributes=attributes,boundary_bounds=list(bounds),
                boundary_source=str(args.admin_shp.resolve()),source_packset=str(source),
                attribution=manifest['attribution'],actual_bytes=total,
                overview_scope='Original Zhejiang overview retained unchanged')
    s.write_json(output/'manifest.json',result)
    set_region_start(output)
    print(f'Complete: {output}; {total/2**30:.2f} GiB',flush=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--admin-shp',type=Path,required=True)
    parser.add_argument('--name',required=True)
    parser.add_argument('--plan-only',action='store_true')
    try:
        run(parser.parse_args())
    except (ValueError,OSError,struct.error) as error:
        parser.exit(1,f'Error: {error}\n')


if __name__=='__main__':
    main()
