"""RoboAlarms Flasher: put the panel's firmware on a board and test what is wired to it.

docs/features/34. The engine modules (ports, probe, bundle, boards, jobs, console, rf_test) import
neither the window toolkit nor esptool, so the host tests run them with fakes; esptool is only
imported by `chip`, the window only by `app`.
"""

__version__ = "0.1.2"
