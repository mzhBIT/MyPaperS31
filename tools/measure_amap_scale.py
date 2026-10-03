"""Measure static-map pixel scale with two server-rendered markers.

Reads a Web service key from a local file; never logs the key or request URL.
Only 3 requests, spaced at least 550 ms; images/report remain under tmp/map.
"""
import argparse
import io
import json
import math
from pathlib import Path
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from PIL import Image


def marker_distance(image):
    rgb = image.convert('RGB')
    columns = []
    for x in range(rgb.width):
        count = sum(1 for y in range(rgb.height)
                    if (lambda p: p[0] > 180 and p[1] < 100 and p[2] < 100)(rgb.getpixel((x, y))))
        if count >= 3:
            columns.append(x)
    groups = []
    for x in columns:
        if not groups or x - groups[-1][-1] > 2:
            groups.append([])
        groups[-1].append(x)
    groups = [g for g in groups if len(g) >= 3]
    if len(groups) != 2:
        raise ValueError(f'Expected two red marker clusters; found {len(groups)}')
    centers = [(g[0] + g[-1]) / 2 for g in groups]
    return centers[1] - centers[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--key-file', type=Path, help='Omit to read the key from standard input')
    parser.add_argument('--output', type=Path, default=Path('tmp/map/amap-scale-check'))
    args = parser.parse_args()
    key = (args.key_file.read_text(encoding='utf-8-sig') if args.key_file else sys.stdin.readline()).strip()
    if not re.fullmatch(r'[A-Za-z0-9]{20,128}', key):
        raise SystemExit('Key file must contain only the Web service key')
    args.output.mkdir(parents=True, exist_ok=True)
    longitude, latitude, delta = 120.101488, 30.344269, 0.001
    markers = (f'small,0xFF0000,A:{longitude-delta/2:.6f},{latitude:.6f};'
               f'{longitude+delta/2:.6f},{latitude:.6f}')
    rows = []
    last_start = 0.0
    for z in (15, 16, 17):
        time.sleep(max(0, 0.55 - (time.monotonic() - last_start)))
        params = dict(location=f'{longitude:.6f},{latitude:.6f}', zoom=z,
                      size='342*608', scale=1, markers=markers, key=key)
        url = 'https://restapi.amap.com/v3/staticmap?' + urllib.parse.urlencode(params)
        last_start = time.monotonic()
        try:
            with urllib.request.urlopen(url, timeout=20) as response:
                payload = response.read(4*1024*1024+1)
        except urllib.error.HTTPError as error:
            raise SystemExit(f'HTTP {error.code}; request URL omitted') from None
        except Exception as error:
            raise SystemExit(f'Network failed ({type(error).__name__}); details omitted') from None
        if len(payload) > 4*1024*1024:
            raise SystemExit('Response exceeds 4 MiB')
        try:
            image = Image.open(io.BytesIO(payload))
            image.load()
        except Exception:
            try:
                body = json.loads(payload)
                code = str(body.get('infocode', 'unknown'))
                if not code.isdigit():
                    code = 'unknown'
            except Exception:
                code = 'unknown'
            raise SystemExit(f'Not an image; API infocode={code}; body omitted') from None
        if image.size != (342, 608):
            raise SystemExit(f'Unexpected image dimensions {image.size}')
        (args.output / f'raw-z{z}-scale1.png').write_bytes(payload)
        observed = marker_distance(image)
        expected = delta/360*256*2**z
        row = dict(request_zoom=z, width=image.width, height=image.height,
                   observed_px=observed, xyz_px=expected, ratio=observed/expected,
                   level_offset=math.log2(observed/expected))
        rows.append(row)
        print(f'Z{z}: markers={observed:.1f}px XYZ={expected:.2f}px '
              f'ratio={row["ratio"]:.3f} level_offset={row["level_offset"]:.3f}')
    report = dict(center=[longitude, latitude], longitude_delta=delta, scale=1,
                  measurements=rows,
                  corrected_transition_ratio=rows[0]['observed_px']*2/(delta/360*256*2**16),
                  transition_observed_ratio=rows[1]['observed_px']*2/(delta/360*256*2**16))
    (args.output/'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print('Offline Z16 -> online request Z16 enlarged 2x: '
          f'{report["transition_observed_ratio"]:.3f}x (desired 2x)')
    print('Corrected: offline Z16 -> online request Z15 enlarged 2x: '
          f'{report["corrected_transition_ratio"]:.3f}x (desired 2x)')


if __name__ == '__main__':
    main()
