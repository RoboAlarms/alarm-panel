"""backlog.py list: each row's Needs cell becomes the flag `list --pc` filters on."""
import importlib.util
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("backlog", TOOLS / "backlog.py")
bl = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bl)


def row(needs, rid="BL-001"):
    return {"id": rid, "cells": {"Needs": needs}}


class NeedsFlag(unittest.TestCase):
    def test_pc_alone_and_with_a_note(self):
        self.assertEqual(bl.needs_flag(row("PC"), set()), "PC")
        self.assertEqual(bl.needs_flag(row("PC (good first)"), set()), "PC")

    def test_pc_then_something_else(self):
        self.assertEqual(bl.needs_flag(row("PC, then Panel"), set()), "PC+")

    def test_hardware_and_services(self):
        self.assertEqual(bl.needs_flag(row("Panel"), set()), "panel")
        self.assertEqual(bl.needs_flag(row("Part: a ZAC93 Z-Wave radio"), set()), "part")
        self.assertEqual(bl.needs_flag(row("Service: an ntfy server"), set()), "service")

    def test_maintainers(self):
        self.assertEqual(bl.needs_flag(row("Maintainers"), set()), "maintainers")
        self.assertEqual(bl.needs_flag(row("Maintainers, then PC"), set()), "maintainers")

    def test_an_open_row_blocks(self):
        self.assertEqual(bl.needs_flag(row("PC; BL-002"), {"BL-001", "BL-002"}), "blocked:BL-002")
        self.assertEqual(bl.needs_flag(row("PC; BL-002"), {"BL-001"}), "PC+")

    def test_no_needs_column(self):
        self.assertEqual(bl.needs_flag({"id": "BL-003", "cells": {}}, set()), "-")


if __name__ == "__main__":
    unittest.main()
