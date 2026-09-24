#!/usr/bin/env python3
"""Detect the boards that are plugged in, build the matching firmware, flash it.

    python scripts/flash.py                 flash whatever is plugged in
    python scripts/flash.py --check         only report what is detected
    python scripts/flash.py --only pico     restrict to one board (repeatable)
    python scripts/flash.py --build-only    compile both sketches, touch no hardware
    python scripts/flash.py --json          machine-readable events (for the GUI)

Boards (detected through `arduino-cli board list`, no extra Python packages):
    esp32  ESP32-S3 DevKitC-1 host   CP210x UART port (10C4:EA60)
    pico   Raspberry Pi Pico device  running this firmware (046D:C547 / 239A:CAFE),
                                     running other RP2040 firmware (2E8A:*), or
                                     sitting in BOOTSEL (UF2 drive)

A Pico that is already running this firmware is rebooted into its bootloader
automatically (1200-baud touch), so nothing has to be held down. Only a blank or
hung Pico needs BOOTSEL held while plugging it in.

Needs arduino-cli (PATH, $ARDUINO_CLI, or ~/arduino-cli*/) with the esp32 and
rp2040 cores installed. Close the Control app / any serial monitor first: an open
COM port cannot be flashed.

--json emits one JSON object per line:
    {"event":"detect","found":[{board,port,protocol,state,ident}],"skipped":[[board,reason]],"notes":[...]}
    {"event":"step","board":"esp32","step":"build|flash|verify","status":"start|ok|fail","detail":"...","seconds":1.2}
    {"event":"log","board":"esp32","line":"..."}
    {"event":"summary","results":{"esp32":"ok","pico":"fail"},"ok":false}
Exit codes: 0 ok, 1 a build/flash failed, 2 nothing detected, 3 arduino-cli missing.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from dataclasses import asdict, dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# ---- what we know how to flash ----------------------------------------------
# FQBNs match the "Build" lines in the sketch headers.
CORE_HINTS = {
    "esp32": "arduino-cli core install esp32:esp32@3.0.0",
    "pico": "arduino-cli core install rp2040:rp2040@5.6.0 --additional-urls "
            "https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json",
}


@dataclass(frozen=True)
class Target:
    key: str
    name: str
    sketch: Path
    fqbn: str


TARGETS = {
    "esp32": Target(
        "esp32", "ESP32-S3 host", ROOT / "firmware" / "esp32_host",
        "esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default,FlashMode=opi,FlashSize=16M,PSRAM=disabled",
    ),
    "pico": Target(
        "pico", "Pico device", ROOT / "firmware" / "pico_device",
        "rp2040:rp2040:rpipico:flash=2097152_65536,usbstack=tinyusb",
    ),
}
ORDER = ("esp32", "pico")

ESP_UART = (0x10C4, 0xEA60)          # CP210x bridge on the DevKitC "UART" port
ESP_NATIVE = (0x303A, 0x1001)        # ESP32-S3 native USB: not the flashing path here
PICO_OURS = {                        # a serial port with these ids is our Pico
    (0x046D, 0xC547): "running this project's firmware",   # real Logitech receivers expose no COM port
    (0x239A, 0xCAFE): "running an earlier build of this firmware",
}
RPI_VID = 0x2E8A


@dataclass(frozen=True)
class Found:
    board: str       # key in TARGETS
    port: str        # value for `arduino-cli upload -p`
    protocol: str    # value for `--protocol` ("serial" or "uf2conv")
    state: str       # human-readable
    ident: str       # e.g. "10C4:EA60 sn 2409D2BD"


# ---- detection (pure: takes the parsed `board list` JSON) ---------------------
def _hex(props, key):
    try:
        return int(props.get(key), 16)
    except (TypeError, ValueError):
        return None


def classify(detected):
    """detected_ports list -> ({board: [Found, ...]}, [note, ...])."""
    found = {k: [] for k in ORDER}
    notes = []
    for entry in detected or []:
        port = entry.get("port") or {}
        addr = port.get("address") or ""
        proto = port.get("protocol") or ""
        props = port.get("properties") or {}
        vid, pid = _hex(props, "vid"), _hex(props, "pid")
        ident = f"{vid:04X}:{pid:04X}" if vid is not None and pid is not None else "-"
        sn = props.get("serialNumber") or ""
        if sn:
            ident += f" sn {sn[:8]}"

        if proto == "uf2conv":
            found["pico"].append(Found("pico", addr, "uf2conv", "in BOOTSEL mode (UF2 drive)", "RPI-RP2"))
        elif proto == "serial":
            ids = (vid, pid)
            if ids == ESP_UART:
                found["esp32"].append(Found("esp32", addr, "serial", "CP210x UART bridge", ident))
            elif ids == ESP_NATIVE:
                notes.append(f"{addr}: ESP32-S3 native USB port ({ident}) is not used for flashing here; "
                             "plug the board's UART port instead")
            elif ids in PICO_OURS:
                found["pico"].append(Found("pico", addr, "serial", PICO_OURS[ids], ident))
            elif vid == RPI_VID:
                found["pico"].append(Found("pico", addr, "serial", "RP2040 running other firmware", ident))
    return found, notes


def plan(found, only=None, esp_port=None, pico_port=None):
    """Pick one Found per board. -> ([Found jobs in ORDER], [(board, reason) skipped])."""
    overrides = {"esp32": esp_port, "pico": pico_port}
    flag = {"esp32": "--esp-port", "pico": "--pico-port"}
    jobs, skipped = [], []
    for key in ORDER:
        if only and key not in only:
            continue
        cands = found.get(key, [])
        if overrides[key]:
            match = [c for c in cands if c.port.upper() == overrides[key].upper()]
            jobs.append(match[0] if match else
                        Found(key, overrides[key], "serial", "port given on the command line", "-"))
        elif len(cands) == 1:
            jobs.append(cands[0])
        elif not cands:
            skipped.append((key, "not detected"))
        else:
            skipped.append((key, f"{len(cands)} candidates ({', '.join(c.port for c in cands)}); pass {flag[key]}"))
    return jobs, skipped


# ---- failure hints ----------------------------------------------------------------
def hint_for(key, text):
    rules = [
        (r"Access is denied|PermissionError|could not open port|cannot open|being used by another",
         "The port is busy. Close the Control app / any serial monitor holding it, then retry."),
        (r"Failed to connect|No serial data received|Wrong boot mode|Timed out waiting for packet",
         "The ESP32 did not enter download mode. Hold BOOT, tap RESET, release BOOT, then retry."),
        (r"No drive to deploy|no.{0,20}UF2|Timeout waiting",
         "The Pico did not show up as a UF2 drive. Unplug it, hold BOOTSEL while plugging it back in, then retry."),
        (r"platform not installed|Platform .* not found|Unknown FQBN|is not installed|core .* not found",
         f"A board core is missing. Try: {CORE_HINTS[key]}"),
    ]
    for pattern, hint in rules:
        if re.search(pattern, text, re.IGNORECASE):
            return hint
    return ""


# ---- arduino-cli -------------------------------------------------------------------
def find_cli():
    env = os.environ.get("ARDUINO_CLI")
    if env and Path(env).exists():
        return env
    which = shutil.which("arduino-cli")
    if which:
        return which
    for pattern in ("arduino-cli*/arduino-cli.exe", "arduino-cli*/arduino-cli"):
        hits = sorted(Path.home().glob(pattern))
        if hits:
            return str(hits[-1])
    return None


def run(cmd, on_line):
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace")
    lines = []
    for raw in proc.stdout:
        line = raw.rstrip("\r\n")
        lines.append(line)
        on_line(line)
    return proc.wait(), lines


def detect(cli):
    r = subprocess.run([cli, "board", "list", "--format", "json"], capture_output=True,
                       text=True, encoding="utf-8", errors="replace", timeout=90)
    if r.returncode != 0:
        raise RuntimeError("arduino-cli board list failed: " + (r.stderr or r.stdout).strip()[:300])
    data = json.loads(r.stdout or "{}")
    return classify(data.get("detected_ports", []) if isinstance(data, dict) else data)


# ---- output ------------------------------------------------------------------------
class Reporter:
    def __init__(self, json_mode, verbose):
        self.json_mode, self.verbose = json_mode, verbose

    def event(self, **kw):
        if self.json_mode:
            print(json.dumps(kw), flush=True)

    def say(self, text=""):
        if not self.json_mode:
            print(text, flush=True)

    def log(self, board, line):
        if self.json_mode:
            self.event(event="log", board=board, line=line)
        elif self.verbose:
            print("      " + line, flush=True)

    def step(self, board, step, status, detail="", seconds=None):
        self.event(event="step", board=board, step=step, status=status, detail=detail,
                   seconds=None if seconds is None else round(seconds, 1))
        if status == "start":
            self.say(f"[{board}] {step} ...")
        else:
            took = f"  ({seconds:.1f}s)" if seconds is not None else ""
            tag = "ok" if status == "ok" else "FAILED"
            self.say(f"[{board}] {step} {tag}{(': ' + detail) if detail else ''}{took}")


# ---- steps --------------------------------------------------------------------------
def build(cli, target, rep):
    rep.step(target.key, "build", "start")
    t0 = time.time()
    rc, lines = run([cli, "compile", "--fqbn", target.fqbn, str(target.sketch)],
                    lambda l: rep.log(target.key, l))
    dt = time.time() - t0
    if rc != 0:
        tail = "\n".join(lines[-25:])
        rep.step(target.key, "build", "fail", hint_for(target.key, tail) or "see output below", dt)
        rep.say("\n".join("      " + l for l in lines[-25:]))
        return False
    size = next((l for l in lines if l.startswith("Sketch uses")), "")
    rep.step(target.key, "build", "ok", size.split(". ")[0], dt)
    return True


def flash(cli, target, found, rep):
    where = found.port + ("" if found.protocol == "serial" else " (UF2 drive)")
    rep.step(target.key, "flash", "start", where)
    cmd = [cli, "upload", "-p", found.port, "--fqbn", target.fqbn]
    if found.protocol != "serial":
        cmd += ["--protocol", found.protocol]
    cmd.append(str(target.sketch))
    t0 = time.time()
    rc, lines = run(cmd, lambda l: rep.log(target.key, l))
    dt = time.time() - t0
    if rc != 0:
        tail = "\n".join(lines[-25:])
        rep.step(target.key, "flash", "fail", hint_for(target.key, tail) or "see output below", dt)
        rep.say("\n".join("      " + l for l in lines[-25:]))
        return False
    rep.step(target.key, "flash", "ok", found.port, dt)
    return True


def verify_pico(cli, rep, timeout=30):
    """After flashing, the Pico must come back as a serial port with our identity."""
    rep.step("pico", "verify", "start", "waiting for it to re-enumerate")
    t0 = time.time()
    while time.time() - t0 < timeout:
        found, _ = detect(cli)
        for f in found["pico"]:
            if f.protocol == "serial" and f.ident.startswith("046D:C547"):
                rep.step("pico", "verify", "ok", f"{f.port} {f.ident}", time.time() - t0)
                return True
        time.sleep(1)
    rep.step("pico", "verify", "fail", "did not come back as 046D:C547 within %ds" % timeout, time.time() - t0)
    return False


# ---- main ---------------------------------------------------------------------------
def main(argv=None):
    ap = argparse.ArgumentParser(description="Detect, build and flash the mouse-passthrough boards.")
    ap.add_argument("--check", action="store_true", help="only report what is detected")
    ap.add_argument("--build-only", action="store_true", help="compile, do not touch any hardware")
    ap.add_argument("--only", choices=ORDER, action="append", help="restrict to one board (repeatable)")
    ap.add_argument("--esp-port", metavar="COMx", help="force the ESP32 UART port")
    ap.add_argument("--pico-port", metavar="COMx", help="force the Pico port (serial or UF2)")
    ap.add_argument("--json", action="store_true", help="emit JSON events instead of text")
    ap.add_argument("-v", "--verbose", action="store_true", help="show tool output as it happens")
    args = ap.parse_args(argv)

    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass
    rep = Reporter(args.json, args.verbose)
    started = time.time()

    cli = find_cli()
    if not cli:
        rep.event(event="error", message="arduino-cli not found")
        print("arduino-cli not found. Install it, or set ARDUINO_CLI to its path.", file=sys.stderr)
        return 3

    if args.build_only:
        results = {}
        for key in ORDER:
            if args.only and key not in args.only:
                continue
            results[key] = "ok" if build(cli, TARGETS[key], rep) else "fail"
        ok = all(v == "ok" for v in results.values())
        rep.event(event="summary", results=results, ok=ok)
        return 0 if ok else 1

    rep.say("Detecting boards ...")
    found, notes = detect(cli)
    jobs, skipped = plan(found, args.only, args.esp_port, args.pico_port)
    rep.event(event="detect", found=[asdict(j) for j in jobs], skipped=[list(s) for s in skipped], notes=notes)
    for j in jobs:
        rep.say(f"  {j.board:<6}{TARGETS[j.board].name:<16}{j.port:<11}{j.state}  [{j.ident}]")
    for key, reason in skipped:
        rep.say(f"  {key:<6}{TARGETS[key].name:<16}skipped: {reason}")
    for n in notes:
        rep.say("  note: " + n)

    if not jobs:
        rep.say("\nNo boards to flash. Plug in the ESP32's UART port and/or the Pico "
                "(hold BOOTSEL if the Pico has no firmware yet).")
        rep.event(event="summary", results={}, ok=False)
        return 2
    if args.check:
        return 0

    results = {}
    for job in jobs:
        target = TARGETS[job.board]
        rep.say(f"\n== {target.name} ==")
        ok = build(cli, target, rep) and flash(cli, target, job, rep)
        if ok and job.board == "pico":
            ok = verify_pico(cli, rep)
        results[job.board] = "ok" if ok else "fail"

    good = all(v == "ok" for v in results.values())
    rep.event(event="summary", results=results, ok=good)
    rep.say("\n" + ", ".join(f"{k}: {v}" for k, v in results.items()) + f"   ({time.time() - started:.0f}s total)")
    return 0 if good else 1


if __name__ == "__main__":
    sys.exit(main())
