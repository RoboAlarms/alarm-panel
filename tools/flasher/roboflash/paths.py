"""Where the Flasher keeps things: backups and captures where people find them (Documents),
logs where apps keep them, and its own resources next to the code (also inside a PyInstaller
build, where the package directory is unpacked under sys._MEIPASS)."""
import os
import subprocess
import sys
from pathlib import Path

RESOURCES = Path(__file__).resolve().parent


def _windows_documents():
    """The real Documents folder (it may be redirected to OneDrive), or None."""
    try:
        import ctypes
        from ctypes import wintypes
        buf = ctypes.create_unicode_buffer(wintypes.MAX_PATH)
        # CSIDL_PERSONAL = 5, SHGFP_TYPE_CURRENT = 0
        if ctypes.windll.shell32.SHGetFolderPathW(None, 5, None, 0, buf) == 0 and buf.value:
            return Path(buf.value)
    except Exception:
        pass
    return None


def documents_dir():
    if sys.platform == "win32":
        found = _windows_documents()
        if found:
            return found
    docs = Path.home() / "Documents"
    return docs if docs.is_dir() else Path.home()


def app_data_dir():
    if sys.platform == "win32":
        base = Path(os.environ.get("LOCALAPPDATA") or Path.home() / "AppData" / "Local")
        return base / "RoboAlarms" / "Flasher"
    if sys.platform == "darwin":
        return Path.home() / "Library" / "Application Support" / "RoboAlarms Flasher"
    base = Path(os.environ.get("XDG_STATE_HOME") or Path.home() / ".local" / "state")
    return base / "roboalarms-flasher"


def backups_dir():
    return documents_dir() / "RoboAlarms" / "Backups"


def captures_dir():
    return documents_dir() / "RoboAlarms" / "Captures"


def logs_dir():
    return app_data_dir() / "logs"


def cache_dir():
    """Firmware downloaded from the firmware server, kept by SHA-256 so it is fetched once and is
    there when the server can't be reached (fwserver.Cache)."""
    return app_data_dir() / "cache"


def open_folder(path):
    path = Path(path)
    path.mkdir(parents=True, exist_ok=True)
    if sys.platform == "win32":
        os.startfile(str(path))  # noqa: S606 - a folder the Flasher made
    elif sys.platform == "darwin":
        subprocess.Popen(["open", str(path)])
    else:
        subprocess.Popen(["xdg-open", str(path)])
