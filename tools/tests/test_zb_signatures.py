"""Zigbee device signatures: JSON rows -> C table (tools/zb_signatures.py; docs/features/20
testing.md section 2). ZB-007, ZB-012: the rows are reviewed data, the generated header is kept in
step by --check, and a row that isn't what the table promises is refused."""
import copy
import importlib.util
import io
import json
import re
import shutil
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
ROOT = TOOLS.parent
SPEC = importlib.util.spec_from_file_location("zb_signatures", TOOLS / "zb_signatures.py")
zs = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(zs)

ZSRC_H = ROOT / "components/core/zone_hub/include/zone_hub/zsrc_zigbee.h"
REGISTRY_H = ROOT / "components/core/zigbee_zones/include/zigbee_zones/zbz_registry.h"


def run(*argv):
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        code = zs.main(list(argv))
    return code, out.getvalue(), err.getvalue()


class TheRepositoryTable(unittest.TestCase):
    def test_rows_load_and_the_header_is_current(self):
        rows, problems = zs.load_rows(zs.SIG_DIR)
        self.assertEqual([], problems)
        self.assertGreaterEqual(len(rows), 15)  # testing.md section 2's seed rows
        code, out, _ = run("--check")
        self.assertEqual(0, code, "run: python tools/zb_signatures.py")
        self.assertIn("matches", out)

    def test_rendering_is_deterministic(self):
        rows, _ = zs.load_rows(zs.SIG_DIR)
        self.assertEqual(zs.render(rows), zs.render(copy.deepcopy(rows)))

    def test_every_row_answers_what_testing_md_asks(self):
        rows, _ = zs.load_rows(zs.SIG_DIR)
        roles = {r["role"] for r in rows}
        # a sensor, a siren, a keypad, a fob and an unsupported device are all seeded
        self.assertTrue({"sensor", "siren", "keypad", "keyfob"} <= roles)
        self.assertIn("unsupported", {r["enroll"] for r in rows})
        for r in rows:
            self.assertEqual(r["supervision_s"], 1800 if r["router"] else 7200, r["file"])

    def test_the_cluster_names_mirror_the_c_header(self):
        text = ZSRC_H.read_text(encoding="utf-8")
        c_bits = set(re.findall(r"#define ZSRC_ZB_CL_([A-Z_]+)\s", text))
        self.assertEqual(c_bits, zs.CLUSTER_NAMES)
        c_ids = {name: int(v, 16) for name, v in
                 re.findall(r"#define ZSRC_ZB_ZCL_([A-Z_]+)\s+(0x[0-9A-Fa-f]+)u", text)}
        for cid in zs.CLUSTER_BITS:
            self.assertIn(cid, c_ids.values(), hex(cid))

    def test_the_roles_mirror_zbz_role_t(self):
        text = REGISTRY_H.read_text(encoding="utf-8")
        body = re.search(r"typedef enum \{(.*?)\} zbz_role_t;", text, re.S).group(1)
        names = [m.lower() for m in re.findall(r"ZBZ_ROLE_([A-Z_]+)", body)]
        self.assertEqual(names, zs.ROLES)


class Refusals(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        for f in zs.SIG_DIR.glob("*.json"):
            shutil.copy(f, self.dir / f.name)
        self.assertEqual(0, run("--dir", str(self.dir))[0])  # writes the header

    def tearDown(self):
        self.tmp.cleanup()

    def edit(self, name, change):
        p = self.dir / name
        data = json.loads(p.read_text(encoding="utf-8"))
        change(data)
        p.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")

    def refused(self, name, change, words):
        self.edit(name, change)
        code, _, err = run("--dir", str(self.dir), "--check")
        self.assertEqual(2, code, err)
        self.assertIn(name, err)
        self.assertIn(words, err)

    def test_check_finds_a_stale_or_missing_header(self):
        self.assertEqual(0, run("--dir", str(self.dir), "--check")[0])
        self.edit("sonoff_snzb_04pr2.json", lambda d: d.__setitem__("nwk", "0x1B01"))
        code, _, err = run("--dir", str(self.dir), "--check")
        self.assertEqual(1, code)
        self.assertIn("stale", err)
        self.assertEqual(0, run("--dir", str(self.dir))[0])
        self.assertEqual(0, run("--dir", str(self.dir), "--check")[0])
        (self.dir / zs.HEADER_NAME).unlink()
        self.assertEqual(1, run("--dir", str(self.dir), "--check")[0])

    def test_a_real_looking_address(self):
        self.refused("sonoff_snzb_04pr2.json",
                     lambda d: d.__setitem__("ieee", "0x00158D0001A2B3C4"), "locally administered")

    def test_an_address_twice(self):
        other = json.loads((self.dir / "sonoff_snzb_02d.json").read_text(encoding="utf-8"))
        self.edit("sonoff_snzb_04pr2.json", lambda d: d.__setitem__("ieee", other["ieee"]))
        code, _, err = run("--dir", str(self.dir), "--check")
        self.assertEqual(2, code)
        self.assertIn("ieee", err)

    def test_a_short_address_that_is_not_a_device(self):
        self.refused("sonoff_snzb_04pr2.json", lambda d: d.__setitem__("nwk", "0x0000"),
                     "short address")

    def test_router_against_the_node_descriptor(self):
        def change(d):
            d["expect"]["router"] = True
            d["expect"]["sleepy"] = False
        self.refused("sonoff_snzb_04pr2.json", change, "node descriptor")

    def test_sleepy_is_the_opposite_of_router(self):
        self.refused("sonoff_snzb_04pr2.json",
                     lambda d: d["expect"].__setitem__("sleepy", False), "opposite")

    def test_input_clusters_must_be_the_endpoints(self):
        self.refused("sonoff_snzb_04pr2.json",
                     lambda d: d["expect"]["inputs"][0]["clusters"].append("TEMP"),
                     "input clusters give")

    def test_an_input_on_a_vendor_profile(self):
        def change(d):
            d["expect"]["inputs"].append({"endpoint": 2, "clusters": ["BASIC", "IDENTIFY", "DIAGNOSTICS"],
                                          "zone_type": None, "class": "generic"})
        self.refused("centralite_3326l.json", change, "Home Automation")

    def test_a_siren_has_no_inputs_and_a_sensor_some(self):
        siren_input = {"endpoint": 1, "clusters": [], "zone_type": "0x0225", "class": "generic"}
        self.refused("tuya_ts0216.json",
                     lambda d: d["expect"]["inputs"].append(siren_input), "own page")

    def test_a_sensor_without_inputs(self):
        self.refused("sonoff_snzb_04pr2.json",
                     lambda d: d["expect"].__setitem__("inputs", []), "at least one")

    def test_too_many_clusters(self):
        self.refused("sonoff_snzb_04pr2.json",
                     lambda d: d["endpoints"][0].__setitem__("in", ["0x%04X" % (0xFC00 + i)
                                                                   for i in range(17)]),
                     "more than 16")

    def test_an_endpoint_out_of_range(self):
        self.refused("sonoff_snzb_04pr2.json",
                     lambda d: d["endpoints"][0].__setitem__("endpoint", 241), "1..240 or 242")

    def test_unknown_names(self):
        self.refused("sonoff_snzb_04pr2.json",
                     lambda d: d["expect"]["inputs"][0].__setitem__("class", "doorbell"), "class")
        self.refused("sonoff_snzb_02d.json",
                     lambda d: d["expect"].__setitem__("role", "thermostat"), "role")

    def test_text_the_c_buffers_cannot_hold(self):
        self.refused("sonoff_snzb_04pr2.json",
                     lambda d: d["basic"].__setitem__("model", "M" * 32), "31 characters")
        self.refused("sonoff_snzb_02d.json",
                     lambda d: d["basic"].__setitem__("manufacturer", "Café"), "ASCII")

    def test_damaged_json(self):
        (self.dir / "sonoff_snzb_04pr2.json").write_text("{\"device\": ", encoding="utf-8")
        code, _, err = run("--dir", str(self.dir), "--check")
        self.assertEqual(2, code)
        self.assertIn("JSON", err)

    def test_an_empty_directory(self):
        with tempfile.TemporaryDirectory() as empty:
            self.assertEqual(2, run("--dir", empty, "--check")[0])


if __name__ == "__main__":
    unittest.main()
