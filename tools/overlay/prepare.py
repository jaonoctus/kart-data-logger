#!/usr/bin/env python3
"""
Build a session for the overlay renderer from a set of GoPro clips.

Orders clips by the UTC embedded in their GPMF track (NOT by filename - GoPro
chapter numbering puts GH02xxxx after GH01xxxx for the same recording, and
unrelated recordings can sort between them), merges their telemetry, trims to
the race start, and runs the firmware's LapManager to get lap boundaries.

    python3 prepare.py --videos ../../video/GH011974.MP4 ../../video/GH021974.MP4 \
        --race-start GH011974.MP4:186 \
        --finish-line -23.60488969 -46.83622658 -23.60493793 -46.83641597 \
        --outdir build

Writes build/{session.json,race.csv,laps.csv}.
"""
import argparse, csv, json, os, subprocess, sys, datetime

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
LAPREPLAY = os.path.join(ROOT, 'tools', 'lapreplay')
sys.path.insert(0, LAPREPLAY)

import gpmf_to_csv as G   # noqa: E402


def probe(path):
    """(start_epoch_ms, video_duration_s) from the container + GPMF."""
    f = open(path, 'rb'); f.seek(0, 2); n = f.tell()
    moov = G.find(f, 0, n, ['moov'])
    if not moov: raise SystemExit(f"{path}: no moov")
    vdur, start = None, None
    for typ, ts, te in G.boxes(f, *moov):
        if typ != 'trak': continue
        stsd = G.find(f, ts, te, ['mdia', 'minf', 'stbl', 'stsd'])
        if not stsd: continue
        f.seek(stsd[0]); d = f.read(min(64, stsd[1]-stsd[0]))
        fmt = d[12:16]
        mdhd = G.find(f, ts, te, ['mdia', 'mdhd'])
        f.seek(mdhd[0]); m = f.read(mdhd[1]-mdhd[0])
        import struct
        if m[0] == 1: tscale, dur = struct.unpack('>IQ', m[20:32])
        else:         tscale, dur = struct.unpack('>II', m[12:20])
        if fmt in (b'avc1', b'hvc1', b'hev1'):
            vdur = dur/tscale
        elif fmt == b'gpmd':
            off = G.sample_offsets(f, ts, te)
            f.seek(off[0][0])
            p = G.parse_payload(f.read(off[0][1]))
            if 'GPSU' in p:
                start = G.gpsu_to_epoch_ms(p['GPSU'])
            elif p.get('GPS9'):
                days, secs = p['GPS9'][0][5], p['GPS9'][0][6]
                start = G.gps9_epoch_ms(days, secs)
    f.close()
    if start is None or vdur is None:
        raise SystemExit(f"{path}: missing video or GPMF track")
    return start, vdur


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--videos', nargs='+', required=True)
    ap.add_argument('--race-start', required=True,
                    help='FILENAME:SECONDS, e.g. GH011974.MP4:186')
    ap.add_argument('--finish-line', nargs=4, type=float, required=True,
                    metavar=('LLAT', 'LLNG', 'RLAT', 'RLNG'))
    ap.add_argument('--outdir', default='build')
    ap.add_argument('--max-gap', type=float, default=5.0,
                    help='seconds; clips further apart than this are treated as '
                         'separate sessions and the later ones dropped')
    args = ap.parse_args()

    os.makedirs(args.outdir, exist_ok=True)

    segs = []
    for v in args.videos:
        start, dur = probe(v)
        segs.append({'path': os.path.abspath(v), 'name': os.path.basename(v),
                     'start_epoch': start, 'duration': dur})
    segs.sort(key=lambda s: s['start_epoch'])

    print("clips in true (UTC) order:")
    for s in segs:
        t = datetime.datetime.fromtimestamp(s['start_epoch']/1000, datetime.timezone.utc)
        print(f"  {s['name']:16s} {t:%H:%M:%S}  {s['duration']:7.1f}s")

    # split off any clip that doesn't continue the previous one
    kept = [segs[0]]
    for prev, s in zip(segs, segs[1:]):
        gap = (s['start_epoch'] - (prev['start_epoch'] + int(prev['duration']*1000)))/1000.0
        if gap > args.max_gap:
            print(f"\n  ! {s['name']} starts {gap/60:.0f} min after {prev['name']} ends "
                  f"- treating as a separate session and dropping it")
            break
        kept.append(s)
    segs = kept

    fname, _, off = args.race_start.partition(':')
    base = next((s for s in segs if s['name'] == fname), None)
    if base is None: raise SystemExit(f"--race-start names {fname}, not in the kept clips")
    race_epoch = base['start_epoch'] + int(float(off)*1000)

    # merge telemetry
    rows = []
    for s in segs:
        tmp = os.path.join(args.outdir, s['name'] + '.csv')
        subprocess.run([sys.executable, os.path.join(LAPREPLAY, 'gpmf_to_csv.py'),
                        s['path'], tmp], check=True)
        with open(tmp) as f:
            rows += list(csv.DictReader(f))
        os.remove(tmp)
    rows.sort(key=lambda r: int(r['epoch']))
    race = [r for r in rows if int(r['epoch']) >= race_epoch]

    cols = ("epoch,speed,totalGForce,gForceX,gForceY,steering_angle,sats,lat,lng").split(',')
    race_csv = os.path.join(args.outdir, 'race.csv')
    with open(race_csv, 'w') as f:
        f.write(','.join(cols)+'\n')
        for r in race:
            f.write(','.join(r[c] for c in cols)+'\n')

    dur = (int(race[-1]['epoch']) - int(race[0]['epoch']))/1000.0
    print(f"\nrace telemetry: {len(race)} rows, {dur:.1f}s -> {race_csv}")

    # laps, straight from the firmware's LapManager
    replay = os.path.join(LAPREPLAY, 'replay')
    if not os.path.exists(replay):
        raise SystemExit(f"{replay} not built - run 'make' in tools/lapreplay first")
    laps_csv = os.path.join(args.outdir, 'laps.csv')
    out = subprocess.run([replay, race_csv, *[str(x) for x in args.finish_line], '--csv'],
                         capture_output=True, text=True, check=True).stdout
    open(laps_csv, 'w').write(out)
    n = max(0, len(out.strip().splitlines()) - 1)
    print(f"lap crossings: {n} -> {laps_csv}")

    cfg = {
        'race_start_epoch': race_epoch,
        'finish_line': dict(zip(('left_lat', 'left_lng', 'right_lat', 'right_lng'),
                                args.finish_line)),
        'segments': segs,
    }
    cfg_path = os.path.join(args.outdir, 'session.json')
    json.dump(cfg, open(cfg_path, 'w'), indent=2)
    print(f"session -> {cfg_path}")


if __name__ == '__main__':
    main()
