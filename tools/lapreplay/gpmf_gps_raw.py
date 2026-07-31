#!/usr/bin/env python3
"""
Dump a GoPro's GPS track exactly as the camera recorded it.

    python3 gpmf_gps_raw.py video/GH011974.MP4 gps_raw_GH011974.csv

This is the unprocessed counterpart to gpmf_to_csv.py. Nothing is converted,
fused, filtered or renamed to suit the firmware:

    epoch,utc,lat,lng,alt_m,speed2d_ms,speed3d_ms,fix,dop,payload

  * lat/lng/alt/speed  straight out of GPS5 after its own SCAL divisor, in the
                       camera's units (degrees, metres, metres per second).
  * epoch              GPSU stamps the payload (~1Hz) in absolute UTC; the ~18Hz
                       GPS5 samples inside it are spread evenly between adjacent
                       stamps. This is the only derived column, and it is the
                       one thing the raw track cannot provide per-sample.
  * fix                GPSF: 0 none, 2 = 2D, 3 = 3D.
  * dop                GPSP, dilution of precision (hundredths - 500 is 5.0).
  * payload            index of the source payload, so samples sharing a GPSU
                       stamp can be identified.

Outliers are counted but NOT removed: gpmf_to_csv.py drops samples implying an
impossible ground speed, which is a decision that belongs to whatever consumes
the data, not to the dump.

The video timeline is deliberately absent. GPMF telemetry does not line up with
the picture at a fixed offset - measured against this footage it needed ~1.2s on
one chapter and ~2s on the next - so any video-relative column would be a guess.
The GPS timeline itself is sound, which is what makes this usable for exercising
LapManager.
"""
import csv, datetime, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gpmf_to_csv as G   # noqa: E402

COLS = ['epoch', 'utc', 'lat', 'lng', 'alt_m', 'speed2d_ms', 'speed3d_ms',
        'fix', 'dop', 'payload']
MAX_KMH = 300.0


def extract(src):
    payloads = G.read_payloads(src)
    stamps, step = G.payload_epochs(payloads)

    rows = []
    for i, p in enumerate(payloads):
        gps = p.get('GPS5', [])
        if not gps:
            continue
        t0 = stamps[i]
        t1 = stamps[i+1] if i+1 < len(stamps) else t0 + int(step)
        span = (t1 - t0) / len(gps)
        fix = int(p.get('GPSF', 0))
        dop = int(p.get('GPSP', 0))
        for j, (lat, lng, alt, sp2d, sp3d) in enumerate(gps):
            rows.append({
                'epoch': int(t0 + j*span),
                'utc': datetime.datetime.fromtimestamp(
                    (t0 + j*span)/1000, datetime.timezone.utc
                ).strftime('%Y-%m-%dT%H:%M:%S.%f')[:-3] + 'Z',
                'lat': f"{lat:.7f}", 'lng': f"{lng:.7f}", 'alt_m': f"{alt:.3f}",
                'speed2d_ms': f"{sp2d:.3f}", 'speed3d_ms': f"{sp3d:.3f}",
                'fix': fix, 'dop': dop, 'payload': i,
            })
    rows.sort(key=lambda r: r['epoch'])
    return rows


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    rows = extract(src)
    if not rows:
        sys.exit(f"{src}: gpmd track carries no GPS5 samples")

    with open(dst, 'w', newline='') as f:
        w = csv.DictWriter(f, COLS, lineterminator='\n')   # not excel's CRLF
        w.writeheader()
        w.writerows(rows)

    dur = (rows[-1]['epoch'] - rows[0]['epoch'])/1000.0
    nofix = sum(1 for r in rows if r['fix'] < 3)
    jumps, prev = 0, None
    for r in rows:
        if prev:
            dt = (r['epoch'] - prev['epoch'])/1000.0
            if dt > 0 and G.haversine(float(prev['lat']), float(prev['lng']),
                                      float(r['lat']), float(r['lng']))/dt*3.6 > MAX_KMH:
                jumps += 1
        prev = r
    print(f"{len(rows)} samples -> {dst}")
    print(f"  span   {dur:.1f}s  ({len(rows)/dur:.1f} Hz)")
    print(f"  start  {rows[0]['utc']}")
    print(f"  speed  {min(float(r['speed2d_ms']) for r in rows)*3.6:.1f} .. "
          f"{max(float(r['speed2d_ms']) for r in rows)*3.6:.1f} km/h (2D)")
    lats = [float(r['lat']) for r in rows]
    lngs = [float(r['lng']) for r in rows]
    print(f"  lat    {min(lats):.7f} .. {max(lats):.7f}")
    print(f"  lng    {min(lngs):.7f} .. {max(lngs):.7f}")
    print(f"  fix    {len(rows)-nofix} of {len(rows)} samples 3D"
          + (f", {nofix} without" if nofix else ""))
    print(f"  dop    {min(r['dop'] for r in rows)/100:.1f} .. "
          f"{max(r['dop'] for r in rows)/100:.1f}")
    if jumps:
        print(f"  note   {jumps} sample(s) imply >{MAX_KMH:.0f} km/h from the "
              f"previous one; kept, unlike gpmf_to_csv.py")


if __name__ == '__main__':
    main()
