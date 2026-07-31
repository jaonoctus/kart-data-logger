# Recorded GPS tracks

Raw GPS as it came off the GoPro's GPMF track, one file per chapter of a single
12-lap session (Kartódromo Granja Viana, 2026-07-23). Committed because the
source footage is ~1.2 GB and lives outside the repo: with these, lap detection
can be exercised on real track data without it.

| file | samples | span | notes |
|---|---|---|---|
| `gps_raw_GH011974.csv` | 9,548 | 532.8 s | starts before the race; 2 corrupt near-(0,0) samples left in |
| `gps_raw_GH021974.csv` | 7,508 | 423.2 s | continues the same recording past the 4 GB split |

Both are ~18 Hz, 3D fix throughout, DOP 1.1–1.9. Columns and their exact
provenance are documented at the top of `../lapreplay/gpmf_gps_raw.py`; nothing
is filtered or converted, including the corrupt samples.

## Replaying them

`gps_raw_to_log.py` merges the chapters back into one session in the logger's
own CSV format, which `replay` feeds through the firmware's real `LapManager`:

```sh
cd ../lapreplay && make
python3 gps_raw_to_log.py --out /tmp/session.csv ../data/gps_raw_GH0*1974.csv
./replay /tmp/session.csv -23.605392 -46.836045 -23.605442 -46.836251
```

That yields 12 crossings, best lap 1:02.628 — identical to what the full
video-derived pipeline (`tools/overlay/prepare.py`) produces, so this is a
faithful stand-in for it and a usable regression fixture.

## What is *not* here

No video timeline. GPMF telemetry does not sit at a fixed offset from the
picture — the overlay needed ~1.2 s of correction on chapter 1 and closer to
2 s on chapter 2 — so the two cannot be related without re-measuring per clip.
The GPS timeline against itself is sound, which is the part that matters for
lap logic.

No accelerometer. The g-force channels need axis calibration against gyro and
speed, which `gpmf_to_csv.py` does from the video.
