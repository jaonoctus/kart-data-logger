#!/usr/bin/env python3
"""
Talk to the GoPro over BLE from the Mac and enumerate its presets.

This exists because the ESP32's USB-CDC log shreds anything longer than a line
or two, which made the protobuf preset reply unreadable on-device. The Mac has
no such limit, needs no flash cycle to iterate, and can decode the reply
properly -- so protocol discovery happens here, and only the answer (a preset
ID) gets baked into firmware.

Usage:  python gopro_presets.py [name-substring]
"""
import asyncio
import sys
from bleak import BleakClient, BleakScanner

CQ_SERVICE = "0000fea6-0000-1000-8000-00805f9b34fb"
CMD_REQ    = "b5f90072-aa8d-11e3-9046-0002a5d5c51b"
CMD_RSP    = "b5f90073-aa8d-11e3-9046-0002a5d5c51b"
SET_REQ    = "b5f90074-aa8d-11e3-9046-0002a5d5c51b"
SET_RSP    = "b5f90075-aa8d-11e3-9046-0002a5d5c51b"
QRY_REQ    = "b5f90076-aa8d-11e3-9046-0002a5d5c51b"
QRY_RSP    = "b5f90077-aa8d-11e3-9046-0002a5d5c51b"

NAME = sys.argv[1] if len(sys.argv) > 1 else "GoPro"


# --------------------------------------------------------------------------- #
# GoPro splits anything larger than one ATT payload. Same header rules the
# firmware implements, kept deliberately identical so findings transfer.
# --------------------------------------------------------------------------- #
class Reassembler:
    def __init__(self, label):
        self.label = label
        self.buf = bytearray()
        self.expected = 0
        self.active = False

    def feed(self, data: bytes):
        """Return a complete payload, or None while still accumulating."""
        off = 0
        b0 = data[0]
        if b0 & 0x80:                       # continuation
            if not self.active:
                return None
            off = 1
        else:
            self.buf.clear()
            self.active = True
            kind = b0 & 0x60
            if kind == 0x00:
                self.expected = b0 & 0x1F
                off = 1
            elif kind == 0x20:
                self.expected = ((b0 & 0x1F) << 8) | data[1]
                off = 2
            elif kind == 0x40:
                self.expected = (data[1] << 8) | data[2]
                off = 3
            else:
                self.active = False
                return None

        self.buf.extend(data[off:])
        if len(self.buf) >= self.expected:
            out = bytes(self.buf[: self.expected])
            self.active = False
            self.buf.clear()
            return out
        return None


# --------------------------------------------------------------------------- #
# Minimal protobuf wire-format walker.
#
# Decoding generically rather than compiling GoPro's .proto files: we only need
# to find preset IDs and any human-readable name, and a generic walk cannot be
# wrong about the schema because it does not assume one.
# --------------------------------------------------------------------------- #
def read_varint(b, i):
    shift = 0
    val = 0
    while i < len(b):
        c = b[i]
        val |= (c & 0x7F) << shift
        i += 1
        if not (c & 0x80):
            return val, i
        shift += 7
    raise ValueError("truncated varint")


def looks_like_text(raw: bytes) -> bool:
    if not raw:
        return False
    try:
        s = raw.decode("utf-8")
    except UnicodeDecodeError:
        return False
    return all(ch.isprintable() for ch in s)


# EnumPresetTitle, from Open GoPro's preset_status.proto. Custom presets on a
# HERO8 are NOT free text: you pick a name from GoPro's fixed list, so the name
# arrives as one of these enum values and never as a string field.
PRESET_TITLES = {
    0: "ACTIVITY", 1: "STANDARD", 2: "CINEMATIC", 3: "PHOTO", 4: "LIVE_BURST",
    5: "BURST", 6: "NIGHT", 7: "TIME_WARP", 8: "TIME_LAPSE", 9: "NIGHT_LAPSE",
    10: "VIDEO", 11: "SLOMO", 12: "360_VIDEO", 13: "PHOTO_2", 14: "PANORAMA",
    15: "360_PHOTO", 16: "TIME_WARP_2", 17: "360_TIME_WARP", 18: "CUSTOM",
    19: "AIR", 20: "BIKE", 21: "EPIC", 22: "INDOOR", 23: "MOTOR",
    24: "MOUNTED", 25: "OUTDOOR", 26: "POV", 27: "SELFIE", 28: "SKATE",
    29: "SNOW", 30: "TRAIL", 31: "TRAVEL", 32: "WATER", 33: "LOOPING",
}

PRESET_GROUPS = {1000: "VIDEO", 1001: "PHOTO", 1002: "TIMELAPSE"}

# The handful of setting IDs worth naming in the summary.
RESOLUTION = {1: "4K", 4: "2.7K", 6: "2.7K 4:3", 7: "1440", 9: "1080",
              12: "720", 18: "4K 4:3", 24: "5K"}
FPS  = {0: "240", 1: "120", 2: "100", 5: "60", 6: "50", 8: "30", 9: "25", 10: "24"}
LENS = {0: "Wide", 2: "Narrow", 3: "SuperView", 4: "Linear"}


def summarize_presets(payload: bytes):
    """Print the one table this whole exercise exists to produce."""
    groups = parse_groups(payload)
    print("\n" + "=" * 62)
    print(f"{'ID':>7}  {'group':<10} {'title':<12} {'custom':<7} profile")
    print("-" * 62)
    for gid, presets in groups:
        for p in presets:
            title = PRESET_TITLES.get(p.get("title_id"), f"?{p.get('title_id')}")
            s = p.get("settings", {})
            prof = " / ".join(x for x in (
                RESOLUTION.get(s.get(2)), FPS.get(s.get(3)) and
                FPS[s[3]] + "fps", LENS.get(s.get(121))) if x)
            print(f"{p.get('id'):>7}  {PRESET_GROUPS.get(gid, gid):<10} "
                  f"{title:<12} {'yes' if p.get('user_defined') else '':<7} {prof}")
    print("=" * 62)
    print("Pin the one you want with  -D GOPRO_PRESET_ID=<ID>  in platformio.ini")


def parse_groups(b):
    """Walk NotifyPresetStatus into [(group_id, [preset dicts])]."""
    groups = []
    for field, raw in iter_fields(b):
        if field != 1:
            continue
        gid, presets = None, []
        for f2, r2 in iter_fields(raw):
            if f2 == 1:
                gid = r2
            elif f2 == 2:
                p, settings = {}, {}
                for f3, r3 in iter_fields(r2):
                    if f3 == 1:
                        p["id"] = r3
                    elif f3 == 3:
                        p["title_id"] = r3
                    elif f3 == 5:
                        p["user_defined"] = bool(r3)
                    elif f3 == 7:
                        sid = sval = None
                        for f4, r4 in iter_fields(r3):
                            if f4 == 1:
                                sid = r4
                            elif f4 == 2:
                                sval = r4
                        if sid is not None:
                            settings[sid] = sval
                p["settings"] = settings
                presets.append(p)
        groups.append((gid, presets))
    return groups


def iter_fields(b):
    """Yield (field_no, value) - int for varints, bytes for length-delimited."""
    i = 0
    while i < len(b):
        try:
            key, i = read_varint(b, i)
        except ValueError:
            return
        field, wt = key >> 3, key & 7
        if wt == 0:
            val, i = read_varint(b, i)
            yield field, val
        elif wt == 2:
            ln, i = read_varint(b, i)
            yield field, b[i:i + ln]
            i += ln
        elif wt == 5:
            i += 4
        elif wt == 1:
            i += 8
        else:
            return


def walk(b, depth=0, out=None, path=()):
    """Yield (path, field_no, wire_type, value) and pretty-print the tree."""
    if out is None:
        out = []
    i = 0
    pad = "  " * depth
    while i < len(b):
        try:
            key, i = read_varint(b, i)
        except ValueError:
            break
        field, wt = key >> 3, key & 7
        if wt == 0:
            val, i = read_varint(b, i)
            print(f"{pad}{field}: {val}")
            out.append((path + (field,), val))
        elif wt == 2:
            ln, i = read_varint(b, i)
            raw = b[i : i + ln]
            i += ln
            if looks_like_text(raw) and len(raw) > 0:
                print(f'{pad}{field}: "{raw.decode()}"')
                out.append((path + (field,), raw.decode()))
            else:
                print(f"{pad}{field} {{")
                try:
                    walk(raw, depth + 1, out, path + (field,))
                except Exception:
                    print(f"{pad}  <{raw.hex()}>")
                print(f"{pad}}}")
        elif wt == 5:
            val = int.from_bytes(b[i : i + 4], "little")
            i += 4
            print(f"{pad}{field}: 0x{val:08X}")
        elif wt == 1:
            val = int.from_bytes(b[i : i + 8], "little")
            i += 8
            print(f"{pad}{field}: 0x{val:016X}")
        else:
            break
    return out


async def main():
    print(f"scanning for '{NAME}' ...")
    # Match on the name only: filtering on the service UUID alone also matches
    # cached/unnamed entries, and we then waste a 30s connect on the wrong one.
    dev = await BleakScanner.find_device_by_filter(
        lambda d, ad: NAME.lower() in ((d.name or ad.local_name or "").lower()),
        timeout=20.0,
    )
    if not dev:
        print("no camera found. Turn it on and make sure the ESP32 is not "
              "holding the link (stop any session on the dash).")
        return

    print(f"found {dev.name} @ {dev.address}")
    print("connecting (needs the camera in pairing mode the first time:\n"
          "  Preferences > Connections > Connect Device > GoPro App)")

    done = asyncio.Event()
    asm = {CMD_RSP: Reassembler("cmd"),
           SET_RSP: Reassembler("set"),
           QRY_RSP: Reassembler("qry")}

    def make_cb(uuid, label):
        def cb(_, data: bytearray):
            payload = asm[uuid].feed(bytes(data))
            if payload is None:
                return
            print(f"\n=== {label} reply, {len(payload)} bytes ===")
            print(payload.hex(" "))
            # Protobuf replies echo [featureId][actionId] before the message.
            if len(payload) > 2 and payload[0] == 0xF5:
                print(f"--- protobuf feature=0x{payload[0]:02X} "
                      f"action=0x{payload[1]:02X} ---")
                try:
                    walk(payload[2:])
                    summarize_presets(payload[2:])
                except Exception as e:
                    print(f"(decode failed: {e})")
                done.set()
        return cb

    # Bonding can need a couple of goes: the camera drops the first attempt
    # while it brings up its pairing screen.
    client = None
    for attempt in range(1, 4):
        try:
            client = BleakClient(dev, timeout=30.0)
            await client.connect()
            break
        except Exception as e:
            print(f"  attempt {attempt}/3 failed: {type(e).__name__}")
            client = None
            await asyncio.sleep(3.0)

    if client is None:
        print("\ncould not connect. Almost always means the camera is not in\n"
              "pairing mode. On the camera:\n"
              "  Preferences > Connections > Connect Device > GoPro App\n"
              "then run this again while it says 'Pairing'.")
        return

    try:
        print("connected")
        for uuid, label in ((CMD_RSP, "cmd"), (SET_RSP, "set"), (QRY_RSP, "qry")):
            try:
                await client.start_notify(uuid, make_cb(uuid, label))
            except Exception as e:
                print(f"subscribe {label} failed: {e}")

        # Get Preset Status: protobuf-framed query, [len][feature][action].
        print("\n>>> Get Preset Status (F5 72)")
        await client.write_gatt_char(QRY_REQ, bytes([0x02, 0xF5, 0x72]), True)

        try:
            await asyncio.wait_for(done.wait(), timeout=10.0)
        except asyncio.TimeoutError:
            print("\nno protobuf reply within 10s -- trying the register variant")
            await client.write_gatt_char(QRY_REQ, bytes([0x02, 0xF5, 0x73]), True)
            try:
                await asyncio.wait_for(done.wait(), timeout=10.0)
            except asyncio.TimeoutError:
                print("still nothing: this camera likely does not expose "
                      "preset status over BLE")

        # Settings dump too -- cheap, and it pins the profile codes we need
        # regardless of how the preset question lands.
        print("\n>>> Get All Setting Values (0x12)")
        await client.write_gatt_char(QRY_REQ, bytes([0x01, 0x12]), True)
        await asyncio.sleep(4.0)
    finally:
        try:
            await client.disconnect()
        except Exception:
            pass
        print("\ndisconnected")


asyncio.run(main())
