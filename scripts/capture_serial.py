#!/usr/bin/env python3
"""Capture serial output from a board to a file (and echo to the terminal).

Usage:
    python scripts/capture_serial.py <PORT> <OUTFILE> [BAUD]

Example (Windows, ESP32 CP210x bridge on COM12):
    python scripts/capture_serial.py COM12 captures/dongle-descriptors.log

Stop with Ctrl-C. The output file is flushed after every line, so it's safe
to read in another window while capture is running.
"""
import sys
import serial  # pyserial


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)

    port = sys.argv[1]
    outfile = sys.argv[2]
    baud = int(sys.argv[3]) if len(sys.argv) > 3 else 115200

    print(f"[capture] opening {port} @ {baud} -> {outfile}  (Ctrl-C to stop)")
    s = serial.Serial(port, baud, timeout=1)

    with open(outfile, "w", encoding="utf-8", newline="") as f:
        try:
            while True:
                line = s.readline().decode(errors="replace")
                if line:
                    print(line, end="")
                    f.write(line)
                    f.flush()
        except KeyboardInterrupt:
            print("\n[capture] stopped.")
        finally:
            s.close()


if __name__ == "__main__":
    main()
