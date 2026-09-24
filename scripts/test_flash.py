"""Offline tests for scripts/flash.py detection and selection (no hardware needed).

    python scripts/test_flash.py
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import flash  # noqa: E402


def serial(addr, vid=None, pid=None, sn=None):
    props = {}
    if vid is not None:
        props["vid"], props["pid"] = f"0x{vid:04X}", f"0x{pid:04X}"
    if sn:
        props["serialNumber"] = sn
    return {"port": {"address": addr, "protocol": "serial", "properties": props}}


def uf2(addr="UF2_Board"):
    return {"port": {"address": addr, "protocol": "uf2conv", "properties": {}}}


COM1 = serial("COM1")                                        # motherboard port: no ids
ESP = serial("COM9", 0x10C4, 0xEA60, "2409D2BDC699EF11")
PICO = serial("COM7", 0x046D, 0xC547, "E66540F0A3147A2D")


class Classify(unittest.TestCase):
    def test_both_boards(self):
        found, notes = flash.classify([COM1, ESP, PICO])
        self.assertEqual([f.port for f in found["esp32"]], ["COM9"])
        self.assertEqual([f.port for f in found["pico"]], ["COM7"])
        self.assertEqual(notes, [])
        self.assertIn("2409D2BD", found["esp32"][0].ident)

    def test_pico_in_bootsel(self):
        found, _ = flash.classify([COM1, uf2()])
        self.assertEqual(found["esp32"], [])
        f = found["pico"][0]
        self.assertEqual((f.port, f.protocol), ("UF2_Board", "uf2conv"))

    def test_nothing_plugged_in(self):
        found, notes = flash.classify([COM1])
        self.assertEqual(found, {"esp32": [], "pico": []})
        self.assertEqual(notes, [])

    def test_empty_and_missing(self):
        self.assertEqual(flash.classify([])[0], {"esp32": [], "pico": []})
        self.assertEqual(flash.classify(None)[0], {"esp32": [], "pico": []})

    def test_esp_native_port_is_only_a_note(self):
        found, notes = flash.classify([serial("COM12", 0x303A, 0x1001)])
        self.assertEqual(found["esp32"], [])
        self.assertEqual(len(notes), 1)
        self.assertIn("COM12", notes[0])

    def test_old_and_stock_pico_identities(self):
        found, _ = flash.classify([serial("COM4", 0x239A, 0xCAFE), serial("COM5", 0x2E8A, 0x000A)])
        self.assertEqual([f.port for f in found["pico"]], ["COM4", "COM5"])
        self.assertIn("earlier build", found["pico"][0].state)
        self.assertIn("other firmware", found["pico"][1].state)

    def test_unrelated_serial_devices_ignored(self):
        found, _ = flash.classify([serial("COM3", 0x0403, 0x6001), serial("COM6", 0x1A86, 0x7523)])
        self.assertEqual(found, {"esp32": [], "pico": []})

    def test_bad_ids_do_not_crash(self):
        weird = {"port": {"address": "COM8", "protocol": "serial", "properties": {"vid": "zz", "pid": None}}}
        self.assertEqual(flash.classify([weird])[0], {"esp32": [], "pico": []})


class Plan(unittest.TestCase):
    def test_flashes_what_is_present_in_order(self):
        found, _ = flash.classify([PICO, ESP])
        jobs, skipped = flash.plan(found)
        self.assertEqual([j.board for j in jobs], ["esp32", "pico"])
        self.assertEqual(skipped, [])

    def test_missing_board_is_skipped_not_fatal(self):
        found, _ = flash.classify([ESP])
        jobs, skipped = flash.plan(found)
        self.assertEqual([j.board for j in jobs], ["esp32"])
        self.assertEqual(skipped, [("pico", "not detected")])

    def test_ambiguous_needs_explicit_port(self):
        found, _ = flash.classify([ESP, serial("COM11", 0x10C4, 0xEA60, "OTHER")])
        jobs, skipped = flash.plan(found)
        self.assertEqual(jobs, [])
        self.assertEqual(skipped[0][0], "esp32")
        self.assertIn("--esp-port", skipped[0][1])
        jobs, _ = flash.plan(found, esp_port="com11")           # case-insensitive match
        self.assertEqual([(j.board, j.port) for j in jobs], [("esp32", "COM11")])

    def test_only_filter(self):
        found, _ = flash.classify([ESP, PICO])
        jobs, skipped = flash.plan(found, only=["pico"])
        self.assertEqual([j.board for j in jobs], ["pico"])
        self.assertEqual(skipped, [])

    def test_forced_port_works_even_if_undetected(self):
        jobs, _ = flash.plan({"esp32": [], "pico": []}, only=["esp32"], esp_port="COM42")
        self.assertEqual([(j.board, j.port, j.protocol) for j in jobs], [("esp32", "COM42", "serial")])


class Hints(unittest.TestCase):
    def test_known_failures_get_actionable_hints(self):
        self.assertIn("busy", flash.hint_for("esp32", "could not open port 'COM9': PermissionError(13, 'Access is denied.')"))
        self.assertIn("BOOT", flash.hint_for("esp32", "A fatal error occurred: Failed to connect to ESP32-S3"))
        self.assertIn("BOOTSEL", flash.hint_for("pico", "No drive to deploy."))
        self.assertIn("core install", flash.hint_for("pico", "Error: platform not installed"))

    def test_unknown_failure_has_no_hint(self):
        self.assertEqual(flash.hint_for("pico", "something else entirely"), "")


if __name__ == "__main__":
    unittest.main(verbosity=2)
