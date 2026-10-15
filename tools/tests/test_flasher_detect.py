"""FLS-001, FLS-005, FLS-006: ports are classified, the board and image are chosen from the
strongest evidence, the USB bridge never chooses, and manual choices are checked."""
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "flasher"))

from roboflash import testkit  # noqa: E402
from roboflash.boards import ChipInfo, detect, verdict, image_options, load_catalogue, not_offered  # noqa: E402
from roboflash.ports import classify, list_ports  # noqa: E402
from roboflash.probe import ProbeResult  # noqa: E402

MB = 1024 * 1024


def raw(device, vid=None, pid=None, description=""):
    return SimpleNamespace(device=device, vid=vid, pid=pid, description=description, manufacturer="",
                           serial_number="", location="")


class PortTests(unittest.TestCase):
    def test_classify(self):
        self.assertEqual(classify(raw("COM3", 0x1A86, 0x7523, "USB-SERIAL CH340 (COM3)")).kind, "esp")
        self.assertEqual(classify(raw("COM3", 0x1A86, 0x7523)).bridge, "CH340")
        self.assertEqual(classify(raw("COM9", 0x046D, 0xC52B, "a mouse receiver")).kind, "usb")
        self.assertEqual(classify(raw("COM1", description="Communications Port (COM1)")).kind, "other")
        self.assertEqual(classify(raw("COM4", description="Standard Serial over Bluetooth link (COM4)")).kind, "other")
        self.assertEqual(classify(raw("COM3", 0x1A86, 0x7523)).usb_id, "1A86:7523")

    def test_order_and_mac_duplicates(self):
        ports = list_ports(lambda: [raw("COM10", 0x1A86, 0x7523), raw("COM1"), raw("COM2", 0x1A86, 0x7523),
                                    raw("/dev/cu.usbserial-1", 0x1A86, 0x7523),
                                    raw("/dev/tty.usbserial-1", 0x1A86, 0x7523)])
        self.assertEqual([p.device for p in ports], ["/dev/cu.usbserial-1", "COM2", "COM10", "COM1"])


class DetectTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cat = load_catalogue()
        cls.bundle = testkit.standard_bundle()

    def p4(self, rev=103, flash=16 * MB):
        return ChipInfo(chip="esp32p4", revision=rev, flash_size=flash, mac="30:ed:a0:00:00:01")

    def test_nothing_known_selects_nothing(self):
        d = detect(self.cat, self.bundle)
        self.assertEqual((d.confidence, d.image_key), ("unknown", ""))

    def test_firmware_names_its_board(self):
        probe = ProbeResult(port="COM4", answered=True, board="crowpanel_p4", chip_revision=103,
                            version="0.5.9", profile="dev", idle=True)
        d = detect(self.cat, self.bundle, probe)
        self.assertEqual((d.confidence, d.board, d.image_key), ("sure", "crowpanel_p4", "crowpanel_p4-v1-dev"))
        probe.chip_revision = 301
        self.assertEqual(detect(self.cat, self.bundle, probe).image_key, "crowpanel_p4-v3-dev")
        ten = ProbeResult(port="COM12", answered=True, board="crowpanel_10", chip_revision=103,
                          version="0.7.0", profile="dev", idle=True)
        d = detect(self.cat, self.bundle, ten)
        self.assertEqual((d.confidence, d.board, d.image_key), ("sure", "crowpanel_10", "crowpanel_10-v1-dev"))
        self.assertIn("10.1", d.reason)

    def test_chip_picks_board_and_silicon_build(self):
        seven = {"crowpanel_p4": self.cat["crowpanel_p4"]}     # one board fits the chip
        d = detect(seven, self.bundle, chip=self.p4(103))
        self.assertEqual((d.confidence, d.image_key), ("likely", "crowpanel_p4-v1-dev"))
        self.assertEqual(detect(seven, self.bundle, chip=self.p4(301)).image_key, "crowpanel_p4-v3-dev")

    def test_the_7_and_10_inch_are_chosen_by_the_person(self):
        """FLS-005: the CrowPanel 7" and 10.1" have the same chip, flash and USB bridge, so a chip
        alone selects neither; both are offered and named, and the person says which it is."""
        d = detect(self.cat, self.bundle, chip=self.p4(103))
        self.assertEqual((d.confidence, d.board, d.image_key), ("ambiguous", "", ""))
        self.assertEqual(sorted(d.candidates), ["crowpanel_10", "crowpanel_p4"])
        self.assertIn("7", d.reason)
        self.assertIn("10.1", d.reason)
        self.assertIn("Choose", d.reason)

    def test_the_unfinished_4_inch_is_named_but_never_chosen(self):
        """The E32R40T is not offered (docs/features/19 on hold): a chip that looks like it, or its
        firmware naming it, selects nothing and says why; its images are refused."""
        esp32 = ChipInfo(chip="esp32", revision=301, flash_size=4 * MB, mac="8c:94:df:00:00:01")
        d = detect(self.cat, self.bundle, chip=esp32)
        self.assertEqual((d.confidence, d.image_key), ("none", ""))
        self.assertIn("E32R40T", d.reason)
        self.assertIn("not finished", d.reason)
        probe = ProbeResult(port="COM3", answered=True, board="e32r40t", version="0.6.0", idle=True)
        d = detect(self.cat, self.bundle, probe)
        self.assertEqual((d.confidence, d.image_key), ("none", ""))
        self.assertIn("not finished", d.reason)
        v = verdict(self.bundle.image("e32r40t-v3-legacy"), self.cat, chip=esp32)
        self.assertFalse(v.allowed)
        self.assertIn("not finished", v.blocked)

    def test_no_match_and_ambiguous(self):
        s3 = ChipInfo(chip="esp32s3", revision=2, flash_size=8 * MB, mac="")
        self.assertEqual(detect(self.cat, self.bundle, chip=s3).confidence, "none")
        big = ChipInfo(chip="esp32", revision=301, flash_size=8 * MB, mac="")
        self.assertEqual(detect(self.cat, self.bundle, chip=big).confidence, "none")
        cyd = dict(not_offered()["e32r40t"], offered=True)   # two offered boards alike
        cat = dict(self.cat, cyd_a=dict(cyd, name="A 4\""), cyd_b=dict(cyd, name="Another 2.8\""))
        esp32 = ChipInfo(chip="esp32", revision=301, flash_size=4 * MB, mac="")
        d = detect(cat, self.bundle, chip=esp32)
        self.assertEqual((d.confidence, d.image_key), ("ambiguous", ""))
        self.assertEqual(sorted(d.candidates), ["cyd_a", "cyd_b"])

    def test_verdicts(self):
        v1 = self.bundle.image("crowpanel_p4-v1-dev")
        v3 = self.bundle.image("crowpanel_p4-v3-dev")
        self.assertTrue(verdict(v1, self.cat, chip=self.p4(103)).allowed)
        self.assertEqual(verdict(v1, self.cat, chip=self.p4(103)).confirm, "")
        esp32 = ChipInfo(chip="esp32", revision=301, flash_size=4 * MB, mac="")
        wrong_chip = verdict(v1, self.cat, chip=esp32)
        self.assertFalse(wrong_chip.allowed)
        self.assertIn("ESP32-P4", wrong_chip.blocked)
        self.assertFalse(verdict(v1, self.cat, chip=self.p4(103, flash=8 * MB)).allowed)
        rev = verdict(v3, self.cat, chip=self.p4(103))
        self.assertTrue(rev.allowed)
        self.assertIn("black screen", rev.confirm)
        cyd = dict(not_offered()["e32r40t"], offered=True, name="E32R40T 4\"")
        other = verdict(self.bundle.image("e32r40t-v3-legacy"), dict(self.cat, e32r40t=cyd),
                        probe=ProbeResult(port="x", answered=True, board="crowpanel_p4"))
        self.assertIn("says it is", other.confirm)

    def test_every_image_is_listed_with_its_reason(self):
        opts = image_options(self.bundle, self.cat, chip=self.p4(103))
        self.assertEqual([(o["key"], o["allowed"]) for o in opts],
                         [("crowpanel_p4-v1-dev", True), ("crowpanel_p4-v3-dev", True),
                          ("e32r40t-v3-legacy", False), ("crowpanel_10-v1-dev", True)])

    def test_the_person_names_the_board(self):
        """FLS-005: choosing the board picks its image for the chip's silicon; the panel's details
        then describe that board, and a board the Flasher does not install is refused."""
        import tempfile
        from roboflash.controller import Controller, Device
        from roboflash.jobs import JobFailed
        with tempfile.TemporaryDirectory() as tmp:
            c = Controller(lister=lambda: [], backups_dir=tmp, captures_dir=tmp, logs_dir=tmp)
            try:
                c.bundle = self.bundle
                c.devices["COM5"] = Device("COM5", chip=self.p4(103))
                self.assertIsNone(c.device_dict("COM5")["board_info"])
                c.select_board("COM5", "crowpanel_10")
                d = c.device_dict("COM5")
                self.assertEqual((d["image_key"], d["manual"]), ("crowpanel_10-v1-dev", True))
                self.assertEqual(d["board_info"]["revision"], "V1.2")
                c.select_board("COM5", "crowpanel_p4")
                self.assertEqual(c.device_dict("COM5")["image_key"], "crowpanel_p4-v1-dev")
                c.devices["COM5"].chip = self.p4(301)
                c.select_board("COM5", "crowpanel_p4")
                self.assertEqual(c.device_dict("COM5")["image_key"], "crowpanel_p4-v3-dev")
                with self.assertRaisesRegex(JobFailed, "no build for the CrowPanel 10.1"):
                    c.select_board("COM5", "crowpanel_10")      # no v3 build of the 10.1" here
                with self.assertRaisesRegex(JobFailed, "does not install"):
                    c.select_board("COM5", "e32r40t")
                # A panel whose firmware names the other board (the 10.1"'s first image said
                # crowpanel_p4): the build chosen by hand is what the panel's details describe.
                c.devices["COM8"] = Device("COM8", probe=ProbeResult(port="COM8", answered=True, board="crowpanel_p4",
                                                                     chip_revision=103, profile="dev", idle=True))
                self.assertEqual(c.device_dict("COM8")["board"], "crowpanel_p4")
                c.select_image("COM8", "crowpanel_10-v1-dev")
                d = c.device_dict("COM8")
                self.assertEqual((d["board"], d["board_info"]["revision"]), ("crowpanel_10", "V1.2"))
                self.assertIn("says it is a CrowPanel 7", next(o for o in d["options"] if o["key"] == "crowpanel_10-v1-dev")["confirm"])
            finally:
                c.stop()


if __name__ == "__main__":
    unittest.main()
