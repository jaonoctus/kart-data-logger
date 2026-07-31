#!/usr/bin/env python3
"""
Turn raw camera GPS dumps into the logger's own CSV format, so LapManager can be
exercised without the source footage.

    python3 gps_raw_to_log.py --out session.csv ../data/gps_raw_GH0*1974.csv
    ./replay session.csv -23.605392 -46.836045 -23.605442 -46.836251

Inputs are the CSVs written by gpmf_gps_raw.py. Several can be given at once -
they are merged and sorted by epoch, which is how a GoPro recording split at the
4 GB limit becomes one continuous session again.

The logger format carries fields a camera has no equivalent for:

  * totalGForce/gForceX/gForceY  zeroed. LapManager ignores them; the calibrated
    accelerometer channels live in gpmf_to_csv.py, which needs the video.
  * steering_angle              zeroed, never available from a camera.
  * sats                        12 when fix==3, else 0. GPMF carries no satellite
    count, and LapManager treats sats==0 as unusable.

Samples implying an impossible ground speed from the previous one are dropped
here rather than in the dump: GPMF occasionally emits a corrupt GPS5 sample near
(0, 0) that still claims a 3D fix, and the segment from there back to the track
can cut the finish line and register a false lap.
"""
import argparse, csv, math

COLS = "epoch,speed,totalGForce,gForceX,gForceY,steering_angle,sats,lat,lng"
MAX_KMH = 300.0


def haversine(lat1, lon1, lat2, lon2):
    p = 0.017453292519943295
    a = (0.5 - math.cos((lat2-lat1)*p)/2 +
         math.cos(lat1*p)*math.cos(lat2*p)*(1-math.cos((lon2-lon1)*p))/2)
    return 12742000 * math.asin(math.sqrt(max(0.0, min(1.0, a))))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('raw', nargs='+', help='CSVs from gpmf_gps_raw.py')
    ap.add_argument('--out', required=True)
    args = ap.parse_args()

    rows = []
    for path in args.raw:
        with open(path) as f:
            for r in csv.DictReader(f):
                rows.append((int(r['epoch']), float(r['speed2d_ms'])*3.6,
                             12 if int(r['fix']) >= 3 else 0,
                             float(r['lat']), float(r['lng'])))
    rows.sort(key=lambda r: r[0])

    kept, dropped, prev = [], 0, None
    for r in rows:
        if prev is not None:
            dt = (r[0] - prev[0]) / 1000.0
            if dt > 0 and haversine(prev[3], prev[4], r[3], r[4])/dt*3.6 > MAX_KMH:
                dropped += 1
                continue
        kept.append(r); prev = r

    with open(args.out, 'w') as f:
        f.write(COLS + '\n')
        for t, sp, sats, lat, lng in kept:
            f.write(f"{t},{sp:.1f},0.00,0.00,0.00,0.0,{sats},{lat:.6f},{lng:.6f}\n")

    span = (kept[-1][0] - kept[0][0])/1000.0
    print(f"{len(kept)} rows, {span:.1f}s -> {args.out}")
    if dropped:
        print(f"  dropped {dropped} outlier sample(s) implying >{MAX_KMH:.0f} km/h")


if __name__ == '__main__':
    main()
