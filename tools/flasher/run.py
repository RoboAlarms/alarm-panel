#!/usr/bin/env python3
"""RoboAlarms Flasher (docs/features/34). `python tools/flasher/run.py --help`."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from roboflash.app import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
