#!/usr/bin/env python3
"""
Extract a GoPro's GPMF telemetry track into the logger's own CSV format.

    python3 gpmf_to_csv.py video/GH011973.MP4 out.csv

Emits the exact columns LogManager::task writes to /log_N.csv:

    epoch,speed,totalGForce,gForceX,gForceY,steering_angle,sats,lat,lng

so the result is interchangeable with a real session log.

Notes on the mapping:
  * epoch      GPSU gives absolute UTC once per payload (~1Hz); the 18Hz GPS5
               samples inside each payload are interpolated between adjacent
               GPSU stamps.
  * speed      GPS5 2D speed (m/s) -> km/h.
  * gForce*    ACCL runs at ~200Hz against GPS5's ~18Hz, so the accelerometer
               samples falling inside each GPS interval are averaged. That also
               damps the chassis vibration, which peaks around 6g raw and is not
               real cornering load.
               Axis order depends on camera mounting, so it is calibrated from
               the data rather than hardcoded - see calibrate_axes(). Gravity and
               any camera tilt are removed by subtracting each axis's mean.
               totalGForce is therefore the HORIZONTAL magnitude, not the 3D one:
               including gravity would make it read ~1g at a standstill.
  * sats       GPMF carries no satellite count, only GPSF (fix type) and GPSP
               (DOP). Emitted as 12 when GPSF==3, else 0, because LogManager and
               LapManager treat sats==0 as unusable.
  * steering   Not available from a camera. Always 0.0.
"""
import struct, sys, math, datetime

TYPES = {'b':'b','B':'B','s':'h','S':'H','l':'i','L':'I','f':'f','d':'d','j':'q','J':'Q'}
G = 9.80665


def boxes(f, start, end):
    pos = start
    while pos < end - 8:
        f.seek(pos); hdr = f.read(8)
        if len(hdr) < 8: return
        size, typ = struct.unpack('>I4s', hdr); typ = typ.decode('latin1'); h = 8
        if size == 1: size = struct.unpack('>Q', f.read(8))[0]; h = 16
        elif size == 0: size = end - pos
        if size < h: return
        yield typ, pos + h, pos + size
        pos += size


def find(f, s, e, path):
    want, rest = path[0], path[1:]
    for typ, bs, be in boxes(f, s, e):
        if typ == want:
            return (bs, be) if not rest else find(f, bs, be, rest)
    return None


def sample_offsets(f, ts, te):
    """Per-sample (offset, size) from the stbl tables."""
    def tab(n):
        r = find(f, ts, te, ['mdia', 'minf', 'stbl', n])
        if not r: return None
        f.seek(r[0]); return f.read(r[1] - r[0])

    stsz, stco, co64, stsc = tab('stsz'), tab('stco'), tab('co64'), tab('stsc')
    ssz, cnt = struct.unpack('>II', stsz[4:12])
    sizes = [ssz]*cnt if ssz else [struct.unpack('>I', stsz[12+4*i:16+4*i])[0] for i in range(cnt)]
    if stco:
        n = struct.unpack('>I', stco[4:8])[0]
        offs = [struct.unpack('>I', stco[8+4*i:12+4*i])[0] for i in range(n)]
    else:
        n = struct.unpack('>I', co64[4:8])[0]
        offs = [struct.unpack('>Q', co64[8+8*i:16+8*i])[0] for i in range(n)]
    n = struct.unpack('>I', stsc[4:8])[0]
    ent = [struct.unpack('>III', stsc[8+12*i:20+12*i]) for i in range(n)]

    out, si = [], 0
    for i, (first, spc, _) in enumerate(ent):
        last = ent[i+1][0]-1 if i+1 < len(ent) else len(offs)
        for c in range(first-1, last):
            if c >= len(offs): break
            o = offs[c]
            for _ in range(spc):
                if si >= len(sizes): break
                out.append((o, sizes[si])); o += sizes[si]; si += 1
    return out


def parse_payload(buf):
    """Return {'GPS5'|'GPS9': [...], 'ACCL': [...], 'GPSU': str, 'GPSF': v} for one payload.

    Older cameras (e.g. HERO8) emit GPS5 (lat,lon,alt,2D,3D) plus a separate
    ~1Hz GPSU timestamp. Newer ones (HERO11+) emit GPS9 instead: each sample
    already carries its own (lat,lon,alt,2D,3D,days_since_2000,secs_since_midnight,
    dop,fix), a '?' (complex) GPMF type whose layout comes from the preceding
    TYPE key rather than a single type char.
    """
    res = {}

    def walk(b, scal):
        pos = 0
        cur = scal
        typestr = None
        while pos + 8 <= len(b):
            key = b[pos:pos+4].decode('latin1', 'replace')
            typ = chr(b[pos+4]); s = b[pos+5]
            rpt = struct.unpack('>H', b[pos+6:pos+8])[0]
            pos += 8; dlen = s*rpt; data = b[pos:pos+dlen]; pos += (dlen+3) & ~3

            if typ == '\0':
                walk(data, None); cur = scal; continue
            if key == 'GPSU':
                res['GPSU'] = data.decode('latin1', 'replace').strip('\0'); continue
            if key == 'TYPE':
                typestr = data.decode('latin1', 'replace'); continue
            if key == 'GPS9':
                if not typestr: continue
                try: fmt = ''.join(TYPES[c] for c in typestr)
                except KeyError: continue
                if struct.calcsize('>'+fmt) != s: continue
                sc = cur or (1.0,)*len(fmt)
                recs = []
                for i in range(rpt):
                    try: vals = struct.unpack('>'+fmt, data[i*s:(i+1)*s])
                    except struct.error: continue
                    recs.append(tuple(vals[k]/sc[k] for k in range(len(vals))))
                res.setdefault('GPS9', []).extend(recs)
                continue
            if typ not in TYPES: continue
            w = struct.calcsize('>'+TYPES[typ])
            if s % w: continue
            per = s // w
            try: v = struct.unpack('>'+TYPES[typ]*per*rpt, data)
            except struct.error: continue
            if key == 'SCAL':
                cur = v; continue
            sc = cur or (1,)*per
            if len(sc) < per: sc = (sc[0],)*per
            rows = [tuple(v[i*per+j]/sc[j] for j in range(per)) for i in range(rpt)]
            if key in ('GPS5', 'ACCL', 'GYRO'):
                res.setdefault(key, []).extend(rows)
            elif key in ('GPSF', 'GPSP'):
                res[key] = rows[0][0]

    walk(buf, None)
    return res


def read_payloads(path):
    """Every GPMF payload in a GoPro file, in recording order."""
    f = open(path, 'rb'); f.seek(0, 2); fsize = f.tell()
    moov = find(f, 0, fsize, ['moov'])
    if not moov: sys.exit("no moov box")

    trak = None
    for typ, ts, te in boxes(f, *moov):
        if typ != 'trak': continue
        stsd = find(f, ts, te, ['mdia', 'minf', 'stbl', 'stsd'])
        if not stsd: continue
        f.seek(stsd[0]); d = f.read(min(64, stsd[1]-stsd[0]))
        if len(d) >= 16 and d[12:16] == b'gpmd':
            trak = (ts, te)
    if not trak: sys.exit("no gpmd track - this file has no GPMF telemetry")

    out = []
    for off, sz in sample_offsets(f, *trak):
        f.seek(off)
        out.append(parse_payload(f.read(sz)))
    f.close()
    return out


def payload_epochs(payloads):
    """Absolute start time per payload, interpolating any missing GPSU stamps."""
    stamps = [gpsu_to_epoch_ms(p['GPSU']) if 'GPSU' in p else None for p in payloads]
    known = [i for i, s in enumerate(stamps) if s is not None]
    if not known: sys.exit("no GPSU stamps - cannot build an absolute timebase")
    step = (stamps[known[-1]] - stamps[known[0]]) / max(1, known[-1]-known[0])
    return [s if s is not None else stamps[known[0]] + int(i*step)
            for i, s in enumerate(stamps)], step


def _corr(x, y):
    n = min(len(x), len(y))
    if n < 8: return 0.0
    x, y = x[:n], y[:n]
    mx, my = sum(x)/n, sum(y)/n
    sx = math.sqrt(sum((v-mx)**2 for v in x)); sy = math.sqrt(sum((v-my)**2 for v in y))
    return sum((a-mx)*(b-my) for a, b in zip(x, y)) / (sx*sy + 1e-12)


def _smooth(x, w=9):
    return [sum(x[max(0, i-w//2):i+w//2+1]) / len(x[max(0, i-w//2):i+w//2+1])
            for i in range(len(x))]


def calibrate_axes(payloads):
    """
    Work out which accelerometer axis is vertical / lateral / longitudinal.

    GPMF's axis order depends on how the camera is mounted, so this is derived
    from the data rather than hardcoded:
      * vertical      = the axis carrying gravity (largest |mean|)
      * lateral       = the horizontal axis correlating with v x yaw_rate, since
                        cornering acceleration is exactly that. Yaw is rotation
                        about the vertical axis, so the gyro axis used is the
                        same index as the gravity axis - which doubles as a
                        consistency check.
      * longitudinal  = whichever horizontal axis is left
    Returns (vert, lat, lon, bias, r) where bias is the per-axis mean to
    subtract (removes gravity and any camera tilt leaking into the horizontals).
    """
    A, Y, S = [], [], []
    for p in payloads:
        A += p.get('ACCL', []); Y += p.get('GYRO', [])
        S += [r[3] for r in (p.get('GPS9') or p.get('GPS5', []))]
    if not A or not S:
        return 0, 1, 2, [0.0, 0.0, 0.0], 0.0

    bias = [sum(r[i] for r in A)/len(A) for i in range(3)]
    vert = max(range(3), key=lambda i: abs(bias[i]))
    horiz = [i for i in range(3) if i != vert]
    if not Y:
        return vert, horiz[0], horiz[1], bias, 0.0

    pa, py = len(A)/len(S), len(Y)/len(S)

    def block(src, idx, k, per):
        lo = min(int(k*per), len(src)-1)
        hi = min(max(int(k*per)+1, int((k+1)*per)), len(src))
        w = src[lo:hi] or [src[lo]]
        return sum(r[idx] for r in w)/len(w)

    yaw = _smooth([S[k] * block(Y, vert, k, py) for k in range(len(S))])
    scored = []
    for h in horiz:
        ah = _smooth([block(A, h, k, pa) - bias[h] for k in range(len(S))])
        scored.append((abs(_corr(ah, yaw)), _corr(ah, yaw), h))
    scored.sort(reverse=True)
    _, r, lat = scored[0]
    lon = [i for i in horiz if i != lat][0]
    return vert, lat, lon, bias, r


def haversine(lat1, lon1, lat2, lon2):
    p = 0.017453292519943295
    a = (0.5 - math.cos((lat2-lat1)*p)/2 +
         math.cos(lat1*p)*math.cos(lat2*p)*(1-math.cos((lon2-lon1)*p))/2)
    return 12742000 * math.asin(math.sqrt(max(0.0, min(1.0, a))))


def gpsu_to_epoch_ms(s):
    """'260723014637.305' -> epoch ms (YYMMDDHHMMSS.sss, UTC)."""
    whole, _, frac = s.partition('.')
    dt = datetime.datetime(2000 + int(whole[0:2]), int(whole[2:4]), int(whole[4:6]),
                           int(whole[6:8]), int(whole[8:10]), int(whole[10:12]),
                           tzinfo=datetime.timezone.utc)
    ms = int(dt.timestamp() * 1000)
    if frac: ms += int(frac[:3].ljust(3, '0'))
    return ms


_EPOCH_2000 = datetime.datetime(2000, 1, 1, tzinfo=datetime.timezone.utc)


def gps9_epoch_ms(days_since_2000, secs_since_midnight):
    """GPS9's own per-sample timestamp -> epoch ms (UTC)."""
    dt = _EPOCH_2000 + datetime.timedelta(days=days_since_2000, seconds=secs_since_midnight)
    return int(dt.timestamp() * 1000)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]

    # --repeat N stitches the track into N synthetic laps. Only meaningful when
    # the clip is a near-closed loop; useful for exercising lap *timing*, which
    # a single crossing cannot do.
    repeat = 1
    if '--repeat' in sys.argv:
        repeat = max(1, int(sys.argv[sys.argv.index('--repeat') + 1]))

    payloads = read_payloads(src)

    vert, lat_ax, lon_ax, bias, r = calibrate_axes(payloads)
    print(f"  accel axes: vertical={vert} lateral={lat_ax} longitudinal={lon_ax} "
          f"(lateral r={r:+.2f} vs v*yaw)")

    use_gps9 = any('GPS9' in p for p in payloads)
    if use_gps9:
        print("  GPS9 stream (per-sample timestamp/fix/DOP) - no GPSU interpolation needed")
        stamps = step = None
    else:
        stamps, step = payload_epochs(payloads)

    rows = []
    for i, p in enumerate(payloads):
        gps = p.get('GPS9', []) if use_gps9 else p.get('GPS5', [])
        if not gps: continue

        accl = p.get('ACCL', [])
        per_gps = len(accl) / len(gps) if accl else 0

        if not use_gps9:
            t0 = stamps[i]
            t1 = stamps[i+1] if i+1 < len(stamps) else t0 + int(step)
            span = (t1 - t0) / len(gps)
            sats = 12 if p.get('GPSF', 0) >= 3 else 0

        for j, rec in enumerate(gps):
            if use_gps9:
                lat, lon, alt, sp2d, sp3d, days, secs, dop, fix = rec
                t = gps9_epoch_ms(days, secs)
                sats = 12 if fix >= 3 else 0
            else:
                lat, lon, alt, sp2d, sp3d = rec
                t = int(t0 + j*span)
            if accl:
                a0, a1 = int(j*per_gps), max(int(j*per_gps)+1, int((j+1)*per_gps))
                win = accl[a0:a1] or [accl[min(a0, len(accl)-1)]]
                comp = [sum(w[k] for w in win)/len(win) - bias[k] for k in range(3)]
                gx = comp[lat_ax] / G          # lateral, + = one side consistently
                gy = comp[lon_ax] / G          # longitudinal, + = one direction
            else:
                gx = gy = 0.0
            # Horizontal magnitude, not the 3D one: including gravity would make
            # this read ~1g at a standstill and would not match the g-wheel dot.
            rows.append((t, sp2d*3.6, math.sqrt(gx*gx + gy*gy),
                         gx, gy, 0.0, sats, lat, lon))

    rows.sort(key=lambda r: r[0])

    # GPMF occasionally emits a corrupt GPS sample that still claims a fix -
    # typically coordinates near (0, 0), or stale/last-known positions during
    # GPS acquisition. Left in, the segment from such a point back to the
    # track spans thousands of km and can intersect the finish line,
    # registering a false lap. Reject any sample implying an impossible ground
    # speed from the last accepted one - but only anchor on sats>0 samples,
    # since sats==0 rows are already unusable and chaining stale no-fix points
    # together can make an implausible jump look like a plausible speed.
    MAX_KMH = 300.0
    kept, dropped, prev = [], 0, None
    for r in rows:
        if r[6] == 0:
            kept.append(r); continue
        if prev is not None:
            dt = (r[0] - prev[0]) / 1000.0
            if dt > 0:
                d = haversine(prev[7], prev[8], r[7], r[8])
                if (d / dt) * 3.6 > MAX_KMH:
                    dropped += 1
                    continue
        kept.append(r); prev = r
    if dropped:
        print(f"  dropped {dropped} outlier GPS sample(s) implying >{MAX_KMH:.0f} km/h")
    rows = kept

    span = rows[-1][0] - rows[0][0] + int((rows[-1][0]-rows[0][0]) / max(1, len(rows)-1))
    with open(dst, 'w') as out:
        out.write("epoch,speed,totalGForce,gForceX,gForceY,steering_angle,sats,lat,lng\n")
        for lap in range(repeat):
            for t, sp, tg, gx, gy, st, sats, lat, lon in rows:
                out.write(f"{t + lap*span},{sp:.1f},{tg:.2f},{gx:.2f},{gy:.2f},"
                          f"{st:.1f},{sats},{lat:.6f},{lon:.6f}\n")
    if repeat > 1:
        print(f"stitched {repeat} synthetic laps, span {span/1000:.3f}s each")

    dur = (rows[-1][0]-rows[0][0])/1000.0
    print(f"{len(rows)} rows -> {dst}")
    print(f"  span   {dur:.1f}s  ({len(rows)/dur:.1f} Hz)")
    print(f"  start  {datetime.datetime.fromtimestamp(rows[0][0]/1000, datetime.timezone.utc)}")
    print(f"  speed  {min(r[1] for r in rows):.1f} .. {max(r[1] for r in rows):.1f} km/h")
    print(f"  lat    {min(r[7] for r in rows):.6f} .. {max(r[7] for r in rows):.6f}")
    print(f"  lng    {min(r[8] for r in rows):.6f} .. {max(r[8] for r in rows):.6f}")


if __name__ == '__main__':
    main()
