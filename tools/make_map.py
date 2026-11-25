#!/usr/bin/env python3
"""Builds the outline maps for the APRS screen from Natural Earth (public domain, https://www.naturalearthdata.com),
GeoJSON from github.com/nvkelso/natural-earth-vector.

  python3 tools/make_map.py <geojson folder>           main/map_data.c, built into the firmware (1:50m):
                                                       ne_50m_admin_0_countries, ne_50m_admin_1_states_provinces_lakes,
                                                       ne_50m_lakes
  python3 tools/make_map.py --detail <geojson folder>  docs/map/rigdeck-map.rdm + docs/map/map.json, the detailed map
                                                       the web page fetches and stores on the microSD card (1:10m):
                                                       ne_10m_admin_0_countries_lakes, ne_10m_admin_1_states_provinces_lakes,
                                                       ne_10m_lakes, ne_10m_lakes_north_america, ne_10m_admin_2_counties_lakes,
                                                       ne_10m_roads

Format "RDM1" (built in): u32 polyline count, then per polyline: u8 class, u16 points, i16 bounding box (min lat,
min lon, max lat, max lon), i16 first lat, i16 first lon, then (points-1) steps as int8 dlat, int8 dlon; a step that
does not fit is -128 followed by int16 dlat, int16 dlon. Units 0.01 degree.

Format "RDM2" (detailed file): "RDM2", u32 polyline count, u16 n + n bytes of UTF-8 description, then per polyline:
u8 class, u16 points, u32 bytes of steps, i32 bounding box (4), i32 first lat, i32 first lon, steps as in RDM1.
Units 0.001 degree. Little endian throughout.

Classes: 0 country/coast, 1 state/province, 2 lake, 3 county (US), 4 major road, 5 secondary road.
"""
import datetime, json, os, struct, sys

def rdp(pts, eps):
    if len(pts) < 3: return pts
    keep = [False] * len(pts); keep[0] = keep[-1] = True
    stack = [(0, len(pts) - 1)]
    while stack:
        a, b = stack.pop()
        ax, ay = pts[a]; bx, by = pts[b]
        dx, dy = bx - ax, by - ay
        L = (dx * dx + dy * dy) ** 0.5
        best, bi = -1, -1
        for i in range(a + 1, b):
            px, py = pts[i]
            d = abs(dy * (px - ax) - dx * (py - ay)) / L if L else ((px - ax) ** 2 + (py - ay) ** 2) ** 0.5
            if d > best: best, bi = d, i
        if best > eps:
            keep[bi] = True; stack += [(a, bi), (bi, b)]
    return [p for p, k in zip(pts, keep) if k]

def lines(geom):
    t, c = geom['type'], geom['coordinates']
    if t == 'Polygon': yield from c
    elif t == 'MultiPolygon':
        for p in c: yield from p
    elif t == 'LineString': yield c
    elif t == 'MultiLineString': yield from c

def collect(src, layers, scale, tol, maxpts, maxstep):
    out = []
    for path, cls, flt in layers:
        d = json.load(open(os.path.join(src, path)))
        for ft in d['features']:
            if not ft.get('geometry') or not flt(ft['properties']): continue
            for r in lines(ft['geometry']):
                q = []
                for pt in r:
                    p = (round(pt[1] * scale), round(pt[0] * scale))
                    if not q or q[-1] != p: q.append(p)
                if len(q) < 2: continue
                q = rdp(q, tol[cls])
                parts, cur = [], [q[0]]          # split at jumps across the 180th meridian, and into short pieces
                for a, c in zip(q, q[1:]):
                    if abs(c[1] - a[1]) > 90 * scale: parts.append(cur); cur = [c]; continue
                    da, do = c[0] - a[0], c[1] - a[1]
                    k = int(max(abs(da), abs(do)) // maxstep) + 1     # a step too long for int16 becomes several
                    for j in range(1, k + 1):
                        cur.append((a[0] + da * j // k, a[1] + do * j // k))
                    if len(cur) >= maxpts: parts.append(cur); cur = [cur[-1]]
                parts.append(cur)
                for part in parts:
                    if len(part) >= 2: out.append((cls, part))
    return out

def steps(q):
    b = bytearray()
    for (a0, o0), (a1, o1) in zip(q, q[1:]):
        da, do = a1 - a0, o1 - o0
        if -127 <= da <= 127 and -127 <= do <= 127: b += struct.pack('<bb', da, do)
        else: b += struct.pack('<bhh', -128, da, do)
    return b

def builtin(src):
    out = collect(src, [('ne_50m_admin_0_countries.geojson', 0, lambda p: True),
                        ('ne_50m_admin_1_states_provinces_lakes.geojson', 1, lambda p: True),
                        ('ne_50m_lakes.geojson', 2, lambda p: (p['scalerank'] if p.get('scalerank') is not None else 9) <= 2)],
                  100, {0: 3.5, 1: 3.0, 2: 3.0}, 65000, 30000)
    b = bytearray(b'RDM1') + struct.pack('<I', len(out))
    npts = 0
    for cls, q in out:
        la = [p[0] for p in q]; lo = [p[1] for p in q]
        b += struct.pack('<BHhhhh', cls, len(q), min(la), min(lo), max(la), max(lo))
        b += struct.pack('<hh', q[0][0], q[0][1]) + steps(q)
        npts += len(q)
    src_lines = ['// Generated by tools/make_map.py from Natural Earth 1:50m (public domain). Do not edit.',
                 '#include <stdint.h>', '#include <stddef.h>',
                 '#ifdef __cplusplus', 'extern "C" {', '#endif',
                 'extern const uint8_t g_mapData[];', 'extern const size_t g_mapDataLen;',
                 'const uint8_t g_mapData[] = {']
    for i in range(0, len(b), 24):
        src_lines.append('    ' + ','.join(str(x) for x in b[i:i + 24]) + ',')
    src_lines += ['};', 'const size_t g_mapDataLen = sizeof g_mapData;', '#ifdef __cplusplus', '}', '#endif', '']
    open(os.path.join(os.path.dirname(__file__), '..', 'main', 'map_data.c'), 'w').write('\n'.join(src_lines))
    print(f'{len(out)} polylines, {npts} points, {len(b)} bytes')

def detail(src):
    road = lambda kinds: (lambda p: p.get('type') in kinds)
    out = collect(src, [('ne_10m_admin_0_countries_lakes.geojson', 0, lambda p: True),
                        ('ne_10m_admin_1_states_provinces_lakes.geojson', 1, lambda p: True),
                        ('ne_10m_lakes.geojson', 2, lambda p: True),
                        ('ne_10m_lakes_north_america.geojson', 2, lambda p: True),
                        ('ne_10m_admin_2_counties_lakes.geojson', 3, lambda p: True),
                        ('ne_10m_roads.geojson', 4, road(('Major Highway', 'Beltway'))),
                        ('ne_10m_roads.geojson', 5, road(('Secondary Highway',)))],
                  1000, {0: 6, 1: 6, 2: 6, 3: 8, 4: 8, 5: 10}, 4000, 30000)
    today = datetime.date.today().isoformat()
    desc = (f'RigDeck detailed map {today}: Natural Earth 1:10m (public domain) countries, states/provinces, '
            'lakes, US counties, highways').encode()
    b = bytearray(b'RDM2') + struct.pack('<I', len(out)) + struct.pack('<H', len(desc)) + desc
    npts, by = 0, [0] * 6
    for cls, q in out:
        la = [p[0] for p in q]; lo = [p[1] for p in q]
        s = steps(q)
        b += struct.pack('<BHIiiiiii', cls, len(q), len(s), min(la), min(lo), max(la), max(lo), q[0][0], q[0][1]) + s
        npts += len(q); by[cls] += 1
    d = os.path.join(os.path.dirname(__file__), '..', 'docs', 'map')
    os.makedirs(d, exist_ok=True)
    open(os.path.join(d, 'rigdeck-map.rdm'), 'wb').write(b)
    json.dump({'version': today, 'file': 'rigdeck-map.rdm', 'size': len(b), 'desc': desc.decode()},
              open(os.path.join(d, 'map.json'), 'w'), indent=1)
    print(f'{len(out)} polylines {by}, {npts} points, {len(b)} bytes')

if __name__ == '__main__':
    if sys.argv[1] == '--detail': detail(sys.argv[2])
    else: builtin(sys.argv[1])
