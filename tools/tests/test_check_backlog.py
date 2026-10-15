"""docs/BACKLOG.md and the feature documents' open boxes agree (tools/check_backlog.py)."""
import importlib.util
import tempfile
import textwrap
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("check_backlog", TOOLS / "check_backlog.py")
cb = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(cb)

BACKLOG = """\
# Backlog

| ID | Work | Needs | Details |
|---|---|---|---|
| BL-001 | Something | PC | [01](features/01-One/README.md) |
| BL-002 | Waits on a decision | PC; BL-003 | [01](features/01-One/README.md) |

| ID | Question | Holds up | Details |
|---|---|---|---|
| BL-003 | Which one? | BL-002 | [01](features/01-One/README.md) |
"""


class CheckBacklog(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        (self.root / "docs" / "features" / "01-One").mkdir(parents=True)
        self.write("docs/BACKLOG.md", BACKLOG)
        self.write("docs/features/01-One/README.md", "# 01\n\n## Tasks\n- [x] Done\n")

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, rel, text):
        (self.root / rel).write_text(textwrap.dedent(text), encoding="utf-8")

    def test_clean_tree_passes(self):
        self.assertEqual(cb.check(self.root), [])

    def test_open_box_needs_a_tag(self):
        self.write("docs/features/01-One/README.md", "## Tasks\n- [ ] Untracked work\n")
        problems = cb.check(self.root)
        self.assertEqual(len(problems), 1)
        self.assertIn("no backlog row", problems[0])

    def test_tag_on_the_box_or_its_continuation(self):
        self.write("docs/features/01-One/README.md",
                   "## Tasks\n- [ ] Work (BL-001)\n- [ ] Longer work that wraps\n"
                   "  onto a second line (BL-002)\n")
        self.assertEqual(cb.check(self.root), [])

    def test_tag_on_a_heading_covers_its_boxes(self):
        self.write("docs/features/01-One/README.md",
                   "## Tasks\n### Phase 1 (BL-001)\n- [ ] One\n- [ ] Two\n#### Detail\n- [ ] Three\n"
                   "### Phase 2\n- [ ] Four\n")
        problems = cb.check(self.root)
        self.assertEqual(len(problems), 1, problems)
        self.assertIn(":8:", problems[0])

    def test_unknown_row_is_reported(self):
        self.write("docs/features/01-One/README.md", "## Tasks\n- [ ] Work (BL-099)\n")
        self.assertIn("BL-099 is not in docs/BACKLOG.md", cb.check(self.root)[0])

    def test_placeholder_is_reported(self):
        self.write("docs/features/01-One/README.md", "## Tasks\n- [ ] Work (BL-NEW: thing)\n")
        self.assertIn("BL-NEW", cb.check(self.root)[0])

    def test_boxes_in_code_fences_and_the_template_are_ignored(self):
        self.write("docs/features/01-One/README.md", "```\n- [ ] an example\n```\n")
        self.write("docs/features/_TEMPLATE.md", "## Tasks\n- [ ] Step-by-step work items\n")
        self.assertEqual(cb.check(self.root), [])

    def test_duplicate_numbers_and_dangling_mentions(self):
        self.write("docs/BACKLOG.md", BACKLOG.replace("BL-003 | Which", "BL-001 | Which"))
        problems = "\n".join(cb.check(self.root))
        self.assertIn("BL-001 is used by more than one row", problems)
        self.assertIn("mentions BL-003, which is not a row", problems)

    def test_broken_link_and_table_shape(self):
        self.write("docs/BACKLOG.md", BACKLOG.replace("features/01-One", "features/02-Two", 1)
                   + "| BL-004 | Short row |\n")
        problems = "\n".join(cb.check(self.root))
        self.assertIn("leads nowhere", problems)
        self.assertIn("cells in a table", problems)

    def test_listing_collects_boxes_per_row(self):
        self.write("docs/features/01-One/README.md", "## Tasks\n- [ ] Work (BL-001)\n")
        listing = {}
        self.assertEqual(cb.check(self.root, listing), [])
        self.assertEqual(listing, {"BL-001": ["docs/features/01-One/README.md:2"]})


if __name__ == "__main__":
    unittest.main()
