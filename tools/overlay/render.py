#!/usr/bin/env python3
"""
Render a telemetry overlay onto GoPro race footage.

Draws the track map, a g-force wheel, speed, elapsed/lap time and lap number,
styled to match the kart dashboard. Telemetry comes from the videos' own GPMF
track; lap numbers come from the firmware's real LapManager via
tools/lapreplay/replay, so the overlay and the device agree by construction.

    python3 render.py --config session.json --start 0 --duration 3 -o test.mp4

See README.md. Requires ffmpeg on PATH and Pillow (tools/overlay/.venv).
"""
import argparse, bisect, csv, json, math, os, subprocess, sys, tempfile
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'lapreplay'))

# --- dashboard palette (lib/UiHelper/uiHelper.h THEME_NIGHT) ---
BG        = (5, 6, 8)
SURFACE   = (13, 16, 20)
FG        = (246, 248, 251)
FG2       = (203, 208, 216)
MUTED     = (107, 114, 128)
ACCENT    = (255, 212, 0)
GOOD      = (46, 224, 122)
BAD       = (255, 59, 59)

FONT_PATH = "/System/Library/Fonts/Supplemental/Arial Narrow Bold.ttf"
FONT_ALT  = "/System/Library/Fonts/Supplemental/Arial Bold.ttf"


def font(size):
    for p in (FONT_PATH, FONT_ALT):
        if os.path.exists(p):
            return ImageFont.truetype(p, size)
    return ImageFont.load_default()


def haversine(lat1, lon1, lat2, lon2):
    p = 0.017453292519943295
    a = (0.5 - math.cos((lat2-lat1)*p)/2 +
         math.cos(lat1*p)*math.cos(lat2*p)*(1-math.cos((lon2-lon1)*p))/2)
    return 12742000 * math.asin(math.sqrt(max(0.0, min(1.0, a))))


def fmt_time(ms, with_ms=True):
    if ms is None or ms <= 0:
        return "--:--" if not with_ms else "--:--.---"
    m, s, f = int(ms//60000), int((ms % 60000)//1000), int(ms % 1000)
    return f"{m}:{s:02d}.{f:03d}" if with_ms else f"{m}:{s:02d}"


class Telemetry:
    """Merged, time-sorted samples with binary-search lookup by epoch."""

    def __init__(self, csv_path):
        self.t, self.rows = [], []
        with open(csv_path) as f:
            for r in csv.DictReader(f):
                self.t.append(int(r['epoch']))
                self.rows.append((
                    float(r['speed']), float(r['totalGForce']),
                    float(r['gForceX']), float(r['gForceY']),
                    float(r['lat']), float(r['lng'])))
        if not self.rows:
            raise SystemExit("no telemetry rows")

    def at(self, epoch_ms):
        i = bisect.bisect_left(self.t, epoch_ms)
        if i <= 0: return self.rows[0]
        if i >= len(self.t): return self.rows[-1]
        # nearest neighbour is plenty at ~18Hz against a 30fps render
        return self.rows[i] if (self.t[i]-epoch_ms) < (epoch_ms-self.t[i-1]) else self.rows[i-1]


class TrackMap:
    """Projects lat/lng onto a fixed-size box, preserving aspect ratio."""

    def __init__(self, telem, size, pad=14):
        lats = [r[4] for r in telem.rows]
        lngs = [r[5] for r in telem.rows]
        self.lat0, self.lat1 = min(lats), max(lats)
        self.lng0, self.lng1 = min(lngs), max(lngs)
        self.size, self.pad = size, pad
        # metres per degree at this latitude, so the map isn't stretched
        mlat = (self.lat1-self.lat0) * 111320
        mlng = (self.lng1-self.lng0) * 111320 * math.cos(math.radians(self.lat0))
        self.scale = (size - 2*pad) / max(mlat, mlng, 1e-6)
        self.mlat, self.mlng = mlat, mlng
        self.ox = pad + ((size-2*pad) - mlng*self.scale)/2
        self.oy = pad + ((size-2*pad) - mlat*self.scale)/2
        self.pts = [self.project(r[4], r[5]) for r in telem.rows]

    def project(self, lat, lng):
        x = (lng - self.lng0) * 111320 * math.cos(math.radians(self.lat0)) * self.scale
        y = (lat - self.lat0) * 111320 * self.scale
        return (self.ox + x, self.oy + (self.mlat*self.scale - y))   # north up


def panel(d, box, radius=10, fill=SURFACE, alpha=205, outline=(28, 32, 38)):
    d.rounded_rectangle(box, radius=radius, fill=fill+(alpha,), outline=outline+(255,), width=2)


def draw_map(d, tmap, pos, gate, origin):
    ox, oy = origin
    s = tmap.size
    panel(d, (ox, oy, ox+s, oy+s))
    pts = [(ox+x, oy+y) for x, y in tmap.pts]
    if len(pts) > 1:
        d.line(pts, fill=MUTED+(190,), width=3, joint="curve")
    if gate:
        a = tmap.project(gate[0], gate[1]); b = tmap.project(gate[2], gate[3])
        d.line([(ox+a[0], oy+a[1]), (ox+b[0], oy+b[1])], fill=ACCENT+(255,), width=4)
    if pos:
        draw_map_dot(d, tmap, pos, origin)


def draw_map_dot(d, tmap, pos, origin):
    ox, oy = origin
    x, y = tmap.project(*pos)
    x, y = ox+x, oy+y
    d.ellipse((x-11, y-11, x+11, y+11), fill=ACCENT+(60,))
    d.ellipse((x-6, y-6, x+6, y+6), fill=ACCENT+(255,), outline=BG+(255,), width=2)


def draw_gwheel_static(d, centre, radius, f_small):
    cx, cy = centre
    panel(d, (cx-radius-26, cy-radius-26, cx+radius+26, cy+radius+52))
    for ring, lbl in ((1.0, "1g"), (2.0, "2g")):
        rr = radius * ring/2.0
        d.ellipse((cx-rr, cy-rr, cx+rr, cy+rr), outline=MUTED+(120,), width=2)
        d.text((cx+rr-16, cy-13), lbl, font=f_small, fill=MUTED+(190,))
    d.line((cx-radius, cy, cx+radius, cy), fill=MUTED+(90,), width=1)
    d.line((cx, cy-radius, cx, cy+radius), fill=MUTED+(90,), width=1)


def draw_gwheel_dot(d, centre, radius, gx, gy, total, f_val):
    cx, cy = centre
    # The CSV stores the physical acceleration vector: +gForceX really is a
    # right-hand turn (verified against GPS heading over the full race,
    # r = +0.976, 99% sign agreement). The dial, though, uses the usual g-ball
    # convention showing what the DRIVER feels - a right-hand corner throws you
    # left - so the lateral axis is negated here, at the display only.
    # gForceY is left as-is: its polarity could not be established from this
    # footage (r = -0.11 against dv/dt, 59% agreement - kart vibration drowns it).
    # clamp at the outer 2g ring so the dot never leaves the dial
    px = cx - max(-2.0, min(2.0, gx)) * radius/2.0
    py = cy - max(-2.0, min(2.0, gy)) * radius/2.0
    d.line((cx, cy, px, py), fill=ACCENT+(150,), width=3)
    d.ellipse((px-13, py-13, px+13, py+13), fill=ACCENT+(70,))
    d.ellipse((px-8, py-8, px+8, py+8), fill=ACCENT+(255,), outline=BG+(255,), width=2)
    t = f"{total:.2f} g"
    d.text((cx, cy+radius+16), t, font=f_val, fill=FG+(255,), anchor="ma")


def draw_readouts(d, w, speed, elapsed, lap_ms, lap_no, laps_total, last_ms, best_ms, F):
    # speed block, bottom centre
    bw, bh = 430, 190
    x0, y0 = (w-bw)//2, 1080-bh-28
    d.text((x0+bw//2, y0+8), f"{int(round(speed))}", font=F['speed'], fill=FG+(255,), anchor="ma")
    d.text((x0+bw//2, y0+bh-46), "KM/H", font=F['label'], fill=MUTED+(255,), anchor="ma")

    # lap + timing block, top left
    pw, ph = 480, 176
    px, py = 28, 28
    d.text((px+20, py+10), f"LAP {lap_no}", font=F['lap'], fill=ACCENT+(255,))
    if laps_total:
        d.text((px+pw-20, py+22), f"/ {laps_total}", font=F['label'], fill=MUTED+(255,), anchor="ra")
    d.text((px+20, py+74), fmt_time(lap_ms), font=F['time'], fill=FG+(255,))
    d.text((px+20, py+128), "LAST", font=F['tag'], fill=MUTED+(255,))
    d.text((px+108, py+126), fmt_time(last_ms), font=F['small'], fill=FG2+(255,))
    d.text((px+250, py+128), "BEST", font=F['tag'], fill=MUTED+(255,))
    d.text((px+338, py+126), fmt_time(best_ms), font=F['small'],
           fill=(GOOD if best_ms and last_ms == best_ms else FG2)+(255,))

    # race clock, top right
    cw, ch = 250, 92
    cx0, cy0 = w-cw-28, 28
    d.text((cx0+cw//2, cy0+6), fmt_time(elapsed, False), font=F['clock'], fill=FG+(255,), anchor="ma")
    d.text((cx0+cw//2, cy0+ch-26), "RACE TIME", font=F['tag'], fill=MUTED+(255,), anchor="ma")


def build_static(tmap, gate, W, H, F):
    """Everything that never changes: panels, track outline, g-wheel rings.

    Redrawing the 13k-point track polyline on all ~23,000 frames dominated the
    render, so it is baked once and copied per frame instead.
    """
    img = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(img, 'RGBA')
    draw_map(d, tmap, None, gate, (28, H-tmap.size-28))
    draw_gwheel_static(d, (W-210, H-230), 150, F['gsmall'])
    bw, bh = 430, 190
    panel(d, ((W-bw)//2, 1080-bh-28, (W-bw)//2+bw, 1080-28))
    panel(d, (28, 28, 28+480, 28+176))
    panel(d, (W-250-28, 28, W-28, 28+92))
    return img


def build_frames(args, telem, laps, gate, W, H, fps, out_pipe, t0=None, n=None):
    tmap = TrackMap(telem, 340)
    F = {
        'speed': font(150), 'label': font(38), 'lap': font(58), 'time': font(52),
        'small': font(32), 'tag': font(26), 'clock': font(58), 'gsmall': font(24),
        'gval': font(34),
    }
    if t0 is None: t0 = telem.t[0] + int(args.start * 1000)
    if n is None:  n = int(round(args.duration * fps))
    crossings = [c[0] for c in laps]
    base = build_static(tmap, gate, W, H, F)

    # Two independent offsets. --lag moves the GPS-derived channels (speed, map
    # position, and therefore lap timing); --lag-g moves only the accelerometer.
    # Cross-correlating ACCL against GPS-derived lateral puts them within 0.17s
    # of each other in the data, so a non-zero --lag-g is a presentation choice
    # rather than a correction of a measurable offset.
    lag_ms = int(getattr(args, 'lag', 0.0) * 1000)
    lag_g_ms = int(getattr(args, 'lag_g', 0.0) * 1000)
    for i in range(n):
        vt = t0 + int(i * 1000 / fps)
        epoch = vt + lag_ms
        speed, _, _, _, lat, lng = telem.at(epoch)
        _, totalg, gx, gy, _, _ = telem.at(vt + lag_g_ms)

        k = bisect.bisect_right(crossings, epoch)
        lap_no = k                                   # laps completed so far
        lap_ms = (epoch - crossings[k-1]) if k > 0 else None
        last_ms = laps[k-1][1] if k > 0 and laps[k-1][1] else None
        best_ms = min((c[1] for c in laps[:k] if c[1]), default=None)

        img = base.copy()
        d = ImageDraw.Draw(img, 'RGBA')
        draw_map_dot(d, tmap, (lat, lng), (28, H-tmap.size-28))
        draw_gwheel_dot(d, (W-210, H-230), 150, gx, gy, totalg, F['gval'])
        draw_readouts(d, W, speed, epoch - telem.t[0], lap_ms,
                      max(1, lap_no), len([c for c in laps if c[1]]),
                      last_ms, best_ms, F)
        out_pipe.write(img.tobytes())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--config', required=True, help='session JSON')
    ap.add_argument('--start', type=float, default=0.0, help='seconds after race start')
    ap.add_argument('--duration', type=float, default=3.0)
    ap.add_argument('-o', '--out', default='overlay.mp4')
    ap.add_argument('--width', type=int, default=1920)
    ap.add_argument('--height', type=int, default=1080)
    ap.add_argument('--fps', type=float, default=29.97)
    ap.add_argument('--preset', default='veryfast')
    ap.add_argument('--crf', type=int, default=20)
    ap.add_argument('--telemetry', help='reuse a prepared race CSV')
    ap.add_argument('--laps', help='reuse a prepared laps CSV')
    ap.add_argument('--frames-only', metavar='DIR',
                    help='write PNG frames instead of encoding (no ffmpeg needed)')
    ap.add_argument('--lag', type=float, default=1.2,
                    help='seconds to advance the telemetry relative to the video. '
                         'Default 1.2, settled by rendering a sweep of test clips '
                         '(1.2 / 1.4 / 1.5 / 2.0 / 2.5) over a braking-and-corner '
                         'section and picking the one where both the map dot and '
                         'the g-wheel line up with the footage.')
    ap.add_argument('--lag-g', type=float, default=None, dest='lag_g',
                    help='override the g-force offset separately from --lag. '
                         'Defaults to --lag, since ACCL and GPS sit within 0.17s '
                         'of each other in the data and shift together.')
    ap.add_argument('--per-segment', action='store_true',
                    help='write one file per source clip instead of concatenating; '
                         'output is named <out-stem>_<clip>.mp4')
    args = ap.parse_args()

    cfg = json.load(open(args.config))
    gate = (cfg['finish_line']['left_lat'], cfg['finish_line']['left_lng'],
            cfg['finish_line']['right_lat'], cfg['finish_line']['right_lng'])

    # default to the artefacts prepare.py wrote next to the config
    d = os.path.dirname(os.path.abspath(args.config))
    args.telemetry = args.telemetry or os.path.join(d, 'race.csv')
    args.laps = args.laps or os.path.join(d, 'laps.csv')

    if args.lag_g is None:
        args.lag_g = args.lag        # both channels shift together by default

    telem = Telemetry(args.telemetry)
    laps = []
    with open(args.laps) as f:
        for r in csv.DictReader(f):
            laps.append((int(r['crossing_epoch']), int(r['lap_time_ms']) or None))

    W, H = args.width, args.height

    if args.frames_only:
        os.makedirs(args.frames_only, exist_ok=True)

        class PngSink:
            def __init__(self): self.i = 0
            def write(self, raw):
                Image.frombytes('RGBA', (W, H), raw).save(
                    os.path.join(args.frames_only, f"f{self.i:05d}.png"))
                self.i += 1
        sink = PngSink()
        build_frames(args, telem, laps, gate, W, H, args.fps, sink)
        print(f"wrote {sink.i} PNG frames to {args.frames_only}")
        return

    # A GoPro recording split at the 4GB limit gives chapters whose video
    # durations do not exactly match the gap between their GPSU stamps (here
    # 531.5s of video vs 532.9s of GPS time). Concatenating the sources and
    # treating it as one timeline drifts the overlay out of sync after the join,
    # so each chapter is rendered against its OWN epoch mapping and the finished
    # pieces are concatenated instead.
    req_start = telem.t[0] + int(args.start * 1000)
    req_end = req_start + int(args.duration * 1000)

    jobs = []
    for sg in cfg['segments']:
        s0 = sg['start_epoch']
        s1 = s0 + int(sg['duration'] * 1000)
        a, b = max(s0, req_start), min(s1, req_end)
        if b - a < 100:            # ignore slivers
            continue
        jobs.append({'path': sg['path'], 'name': os.path.basename(sg['path']),
                     'seek': (a - s0) / 1000.0, 'len': (b - a) / 1000.0, 't0': a})
    if not jobs:
        raise SystemExit("requested range falls outside the available footage")

    def encode(job, dest):
        n = int(round(job['len'] * args.fps))
        cmd = [
            'ffmpeg', '-y', '-hide_banner', '-loglevel', 'error',
            '-ss', f"{job['seek']:.3f}", '-t', f"{job['len']:.3f}", '-i', job['path'],
            '-f', 'rawvideo', '-pix_fmt', 'rgba', '-s', f"{W}x{H}",
            '-r', str(args.fps), '-i', 'pipe:0',
            '-filter_complex',
            f"[0:v]scale={W}:{H},fps={args.fps}[bg];[bg][1:v]overlay=0:0[v]",
            '-map', '[v]', '-map', '0:a?',
            '-c:v', 'libx264', '-preset', args.preset, '-crf', str(args.crf),
            '-pix_fmt', 'yuv420p', '-c:a', 'aac', '-b:a', '160k',
            '-movflags', '+faststart', dest,
        ]
        p = subprocess.Popen(cmd, stdin=subprocess.PIPE)
        try:
            build_frames(args, telem, laps, gate, W, H, args.fps, p.stdin,
                         t0=job['t0'], n=n)
        finally:
            p.stdin.close()
        if p.wait():
            raise SystemExit("ffmpeg failed")

    total = sum(j['len'] for j in jobs)
    print(f"{len(jobs)} segment(s), {total:.1f}s, ~{int(total*args.fps):,} frames")
    for j in jobs:
        print(f"  {j['name']} @ {j['seek']:.3f}s for {j['len']:.3f}s")

    if args.per_segment:
        stem, ext = os.path.splitext(args.out)
        for j in jobs:
            dest = f"{stem}_{os.path.splitext(j['name'])[0]}{ext}"
            print(f"  -> {j['name']} -> {dest}", flush=True)
            encode(j, dest)
            print(f"     done: {dest}", flush=True)
        return
    if len(jobs) == 1:
        encode(jobs[0], args.out)
    else:
        tmpdir = tempfile.mkdtemp(prefix='overlay_')
        parts = []
        for i, j in enumerate(jobs):
            dest = os.path.join(tmpdir, f"part{i:02d}.mp4")
            print(f"  -> rendering {j['name']} ...", flush=True)
            encode(j, dest)
            parts.append(dest)
        listing = os.path.join(tmpdir, 'concat.txt')
        with open(listing, 'w') as f:
            for pth in parts:
                f.write(f"file '{pth}'\n")
        print("  -> concatenating (stream copy) ...", flush=True)
        subprocess.run(['ffmpeg', '-y', '-hide_banner', '-loglevel', 'error',
                        '-f', 'concat', '-safe', '0', '-i', listing,
                        '-c', 'copy', '-movflags', '+faststart', args.out], check=True)
        for pth in parts: os.remove(pth)
        os.remove(listing); os.rmdir(tmpdir)

    print(f"wrote {args.out}")


if __name__ == '__main__':
    main()
