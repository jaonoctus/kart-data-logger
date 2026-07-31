# Telemetry overlay

Renders a dashboard-style overlay onto GoPro race footage: track map, g-force
wheel, speed, race/lap time and lap number. Telemetry comes from the videos' own
GPMF track, so nothing has to be recorded on the device.

Lap numbers come from the firmware's **real** `LapManager`, via
`tools/lapreplay/replay --csv`. The overlay and the kart dashboard therefore
agree by construction rather than by a reimplementation that can drift.

## Requirements

* `ffmpeg` on PATH — `brew install ffmpeg`
* Pillow — already installed in `.venv/` here (`python3 -m venv .venv && .venv/bin/pip install Pillow`)
* `tools/lapreplay/replay` built — `cd ../lapreplay && make`

## Usage

```sh
# 1. order the clips, merge telemetry, trim to race start, detect laps
python3 prepare.py \
    --videos ../../video/*.MP4 \
    --race-start GH011974.MP4:186 \
    --finish-line -23.60488969 -46.83622658 -23.60493793 -46.83641597 \
    --outdir build

# 2. render (start/duration are seconds after the race start)
.venv/bin/python render.py --config build/session.json \
    --start 210 --duration 3 -o test.mp4
```

`--frames-only DIR` writes PNG frames instead of encoding, which needs no
ffmpeg and is the quick way to check layout changes.

Output defaults to 1080p. The source is 4K, so ffmpeg downscales it — 4K output
is enormous and very slow to encode for no real benefit when watching.

## Telemetry lag

GPMF telemetry runs behind the picture: the overlay reacts about a second after
the corner is already on screen. `--lag` advances the telemetry against the
video and defaults to **1.2 s**, chosen by rendering the same braking-and-corner
section at 1.2 / 1.4 / 1.5 / 2.0 / 2.5 and watching which one lands.

GPS and accelerometer lag together — cross-correlating `ACCL` against the
GPS-derived lateral acceleration puts them within 0.17 s of each other — so
`--lag` moves both. `--lag-g` can shift only the g-wheel if you ever want them
apart; it defaults to whatever `--lag` is.

```sh
.venv/bin/python render.py --config build/session.json \
    --start 210 --duration 12 --lag 1.4 -o lagtest_1.4.mp4
```

## Clip ordering

`prepare.py` orders clips by the UTC in their GPMF track, **not** by filename.
This matters: GoPro splits a single recording at the 4 GB FAT32 limit into
`GH01xxxx`, `GH02xxxx`, … so chapter 2 of one recording sorts before chapter 1
of the next, and unrelated recordings can interleave.

Clips separated by more than `--max-gap` seconds (default 5) from the previous
one are treated as a different session and dropped, with a note. For this
footage that correctly kept `GH011974` + `GH021974` (consecutive chapters,
seamless) and dropped `GH011975`/`GH011976`, which were recorded 14 hours later.

## Accelerometer axes

GPMF's accelerometer axis order depends on how the camera was mounted, so
`gpmf_to_csv.calibrate_axes()` derives it from the data:

* **vertical** — the axis carrying gravity (largest absolute mean)
* **lateral** — the horizontal axis correlating with `v × yaw_rate`, because
  cornering acceleration is exactly that
* **longitudinal** — whichever horizontal axis remains

Yaw is rotation about the vertical axis, so the gyro axis used is the same index
as the gravity axis; if those disagree the calibration is suspect. On this
footage both chapters independently resolved to `vertical=0, lateral=1,
longitudinal=2` with r ≈ +0.55.

Each axis's mean is subtracted, which removes gravity and any camera tilt
leaking into the horizontal axes. `totalGForce` is therefore the **horizontal**
magnitude — including gravity would make it read ~1 g at a standstill and would
not match the position of the dot on the wheel.

Residual peaks around 3–4 g are kart chassis vibration, not real cornering load;
the accelerometer samples inside each GPS interval are averaged to damp it.

## Layout

| element | position | source |
|---|---|---|
| lap number, current lap, last, best | top left | `laps.csv` from `LapManager` |
| race clock | top right | elapsed since race start |
| speed | bottom centre | `GPS5` 2D speed |
| g-force wheel, 1 g / 2 g rings | bottom right | calibrated `ACCL` |
| track map, finish line, position | bottom left | `GPS5` lat/lng |

Colours are the dashboard's `THEME_NIGHT` palette from `lib/UiHelper/uiHelper.h`.
