#!/usr/bin/env python3
"""Decode captured Lightspeed dongle HID reports into human-readable events.

Parses lines of the form produced by esp32_host.ino:

    [EP 81 RAW 13] 00 00 00 00 00 00 00 00 00 00 00 00 00
    [EP 82 RAW 17] 01 00 00 ...
    [RAW 13] ...                 (older format, endpoint unknown -> assumed mouse)

Only the MOUSE interface (endpoint 0x81, 13-byte reports, no report ID) is
decoded into (buttons, dx, dy, wheel, hwheel). Keyboard (0x82) and Logitech
HID++ vendor (0x83) reports are counted but not decoded — we don't need them
for the mouse pass-through.

Mouse report byte layout (from the IF0 HID report descriptor — see
PROGRESS.md / docs/notes.md):
    byte 0     buttons 1-8   (bit0=L, bit1=R, bit2=M, bit3=back, bit4=fwd)
    byte 1     buttons 9-16  (unused on the Superlight)
    bytes 2-3  dX            int16, little-endian, signed
    bytes 4-5  dY            int16, little-endian, signed
    byte 6     wheel         int8, signed
    byte 7     hwheel/AC Pan int8, signed
    bytes 8-12 vendor padding

Usage:
    python scripts/decode_reports.py captures/dongle-reports.log
    python scripts/decode_reports.py captures/dongle-reports.log --all   # show idle frames too
"""
import re
import sys

LINE_RE = re.compile(r"\[(?:EP\s+([0-9a-fA-F]{2})\s+)?RAW\s+(\d+)\]\s*([0-9a-fA-F ]*)")

MOUSE_EP = 0x81
BTN_NAMES = ["L", "R", "M", "back", "fwd", "b6", "b7", "b8"]


def s16(lo, hi):
    """Combine two bytes (little-endian) into a signed 16-bit int."""
    v = lo | (hi << 8)
    return v - 0x10000 if v & 0x8000 else v


def s8(b):
    return b - 0x100 if b & 0x80 else b


def decode_mouse(data):
    btn = data[0]
    buttons = [name for i, name in enumerate(BTN_NAMES) if btn & (1 << i)]
    dx = s16(data[2], data[3])
    dy = s16(data[4], data[5])
    wheel = s8(data[6]) if len(data) > 6 else 0
    hwheel = s8(data[7]) if len(data) > 7 else 0
    return buttons, dx, dy, wheel, hwheel


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    show_all = "--all" in sys.argv
    if not args:
        print(__doc__)
        sys.exit(1)

    path = args[0]
    counts = {"mouse": 0, "keyboard": 0, "vendor": 0, "other": 0}
    decoded = 0

    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = LINE_RE.search(line)
            if not m:
                continue
            ep_str, _n, hexstr = m.groups()
            data = [int(b, 16) for b in hexstr.split()]
            ep = int(ep_str, 16) if ep_str else (MOUSE_EP if len(data) == 13 else None)

            if ep == MOUSE_EP or (ep is None and len(data) == 13):
                counts["mouse"] += 1
                buttons, dx, dy, wheel, hwheel = decode_mouse(data)
                # Skip pure-idle frames unless --all
                if not show_all and not buttons and dx == 0 and dy == 0 and wheel == 0 and hwheel == 0:
                    continue
                decoded += 1
                btn_str = "+".join(buttons) if buttons else "-"
                print(f"buttons={btn_str:<14} dx={dx:>6} dy={dy:>6} wheel={wheel:>3} hwheel={hwheel:>3}")
            elif ep == 0x82:
                counts["keyboard"] += 1
            elif ep == 0x83:
                counts["vendor"] += 1
            else:
                counts["other"] += 1

    print("\n--- summary ---")
    print(f"mouse reports:    {counts['mouse']}  (non-idle shown: {decoded})")
    print(f"keyboard reports: {counts['keyboard']}")
    print(f"vendor reports:   {counts['vendor']}")
    print(f"other/unknown:    {counts['other']}")


if __name__ == "__main__":
    main()
