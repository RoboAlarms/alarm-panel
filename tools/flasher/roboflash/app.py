"""Start the Flasher: its window by default, or a command.

    run.py                         the window (a browser tab if no web view is available); the
                                   firmware comes from the firmware server (FLS-032)
    run.py --server URL            another firmware server (or ROBOALARMS_FIRMWARE_SERVER), such as
                                   a test server on this computer (http://localhost:8092)
    run.py --mock                  a pretend desk of boards and a pretend firmware server, to try
                                   the screens without hardware (with --server: that server)
    run.py --build DIR [--build DIR]   use images straight from build folders (a developer's PC)
    run.py --bundle FILE           use this bundle
    run.py bundle --build DIR ...  make a bundle (make_bundle.py)
    run.py ports                   list the serial ports
    run.py probe PORT              look at a panel without restarting it
    run.py identify PORT           read the chip through its loader (restarts the board)
"""
import argparse
import sys
import threading
import time
import webbrowser

from . import __version__


def _window(url, title, log):
    """Show the page in a native window; False when no web view is available."""
    try:
        import webview
    except Exception as e:
        log(f"no window toolkit ({e!r}): using a browser tab")
        return False
    try:
        webview.create_window(title, url, width=1200, height=800, min_size=(960, 640),
                              background_color="#f6f7f6")
        log("window: pywebview")
        webview.start()
        return True
    except Exception as e:
        log(f"the window would not open ({e!r}): using a browser tab")
        return False


def _source(args, mock_dir=None):
    """The firmware server: --server, ROBOALARMS_FIRMWARE_SERVER or the project's. --mock without
    --server has a pretend one in memory, its test key trusted."""
    from . import fwserver
    if args.mock and not args.server:
        from .imagefile import signature_key_digest
        from .testkit import TEST_KEY_DIGEST, mock_server
        return fwserver.ServerSource(mock_server(), trusted={TEST_KEY_DIGEST: "Test key"},
                                     verify=signature_key_digest)
    try:
        url = fwserver.server_url(args.server)
    except ValueError as e:
        raise SystemExit(f"error: --server: {e}")
    cache = fwserver.Cache(f"{mock_dir}/cache") if mock_dir else None
    return fwserver.ServerSource(fwserver.Client(url, cache))


def run_gui(args):
    from .controller import Controller, bundle_from_builds
    from .server import FlasherServer
    if args.mock:
        from .mock import MockWorld
        from .testkit import standard_bundle
        import tempfile
        bundle = standard_bundle(app_size=640 * 1024)
        world = MockWorld(bundle, no_driver=args.mock_driver, radio=args.mock_radio)
        tmp = tempfile.mkdtemp(prefix="roboflash-mock-")
        c = Controller(lister=world.lister, opener=world.opener, connect=world.connect,
                       driver_probe=world.driver_probe,
                       backups_dir=f"{tmp}/Backups", captures_dir=f"{tmp}/Captures", logs_dir=f"{tmp}/logs",
                       boot_timeout=10, source=_source(args, tmp))
    else:
        c = Controller(source=_source(args))
    if args.build:
        try:
            c.set_bundle(bundle_from_builds(args.build))
        except Exception as e:
            c.set_bundle(None, f"build folders: {e}")
    elif args.bundle:
        try:
            c.load_bundle_path(args.bundle)
        except Exception as e:
            c.set_bundle(None, str(e))
    c.start()
    server = FlasherServer(c, args.http_port)
    server.serve_in_thread()
    title = "RoboAlarms Flasher" + (" (mock boards)" if args.mock else "")
    print(f"RoboAlarms Flasher {__version__}: {server.url}", flush=True)
    if args.url_file:
        from pathlib import Path
        Path(args.url_file).write_text(server.url, encoding="utf-8")
    if not args.browser and not args.no_open and _window(server.url, title, c.log):
        c.stop()
        return 0
    if not args.no_open:
        webbrowser.open(server.url)
    # In a browser tab: run until the page has been gone for a while (or Ctrl+C).
    try:
        seen = False
        while True:
            time.sleep(1)
            if c.events.listeners:
                seen = True
            elif seen and time.monotonic() - c.events.last_seen > 20 and not args.no_open:
                break
    except KeyboardInterrupt:
        pass
    c.stop()
    return 0


def run_bundle(args):
    from .make_bundle import make, DEFAULT_KEY
    from .bundle import BundleError
    try:
        make(args.build, out=args.out, key=args.key or DEFAULT_KEY, allow_foreign_key=args.allow_foreign_key,
             allow_dark=args.allow_dark, radio_dirs=args.radio or (), allow_dirty_radio=args.allow_dirty_radio,
             voice_files=args.voice or ())
    except BundleError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


def run_check_bundle(args):
    from .bundle import load, BundleError
    try:
        b = load(args.file)
    except BundleError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    print(f"{b.name}: {b.version_text()}, created {b.created}, source {b.source}")
    for i in b.images:
        print(f"  {i.key}: {i.project} {i.version}, {i.silicon_text()}, {i.flash_size}")
    for lang in b.languages:
        print(f"  language {lang.locale} ({lang.name}) {lang.version}{' draft' if lang.draft else ''}, "
              f"{lang.size} bytes")
    for asset in b.assets:
        print(f"  asset pack {asset.name} {asset.version}{' draft' if asset.draft else ''}, "
              f"{asset.size} bytes")
    for voice in b.voices:
        print(f"  voice {voice.label()}{' draft' if voice.draft else ''}, {voice.size} bytes")
    return 0


def run_ports(args):
    from .ports import list_ports
    for p in list_ports():
        if p.kind == "other" and not args.all:
            continue
        print(f"{p.device:14} {p.kind:5} {p.usb_id:9} {p.bridge:14} {p.description}")
    return 0


def run_probe(args):
    from .probe import probe
    import json
    r = probe(args.port)
    print(json.dumps(r.to_dict(), indent=2))
    return 0 if r.error is None else 1


def run_identify(args, probe=None, connect=None):
    """Read the chip through its loader. It restarts the board, so the panel is looked at first
    and the jobs' idle gate applies (FLS-004, FLS2-001): an armed or busy panel is never touched.
    A board without a RoboAlarms console (a factory demo, a release image) is read as before.
    probe(port) and connect(port, log): the tests' stand-ins."""
    from . import chip
    from .jobs import JobFailed, PROBE_MAX_AGE, refuse_unless_idle
    from .probe import probe as probe_port
    from .serialio import PortError
    import json
    try:
        refuse_unless_idle((probe or probe_port)(args.port),
                           "Identifying restarts the board, so it waits. Nothing was changed.",
                           max_age=PROBE_MAX_AGE)
    except (JobFailed, PortError) as e:
        title = e.title if isinstance(e, JobFailed) else e.message
        print(f"error: {title} {e.advice}".rstrip(), file=sys.stderr)
        return 1
    session = (connect or chip.connect)(args.port, log=lambda t: print("  " + t) if args.verbose else None)
    try:
        print(json.dumps(session.info.to_dict(), indent=2))
    finally:
        session.reset()
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(prog="roboflash", description="RoboAlarms Flasher " + __version__)
    ap.add_argument("--mock", action="store_true", help="pretend boards, no hardware touched")
    ap.add_argument("--mock-driver", choices=["missing", "failed"],
                    help="with --mock: a CH340 Windows has no (or a failed) driver for, and no ports")
    ap.add_argument("--mock-radio", choices=["factory", "older", "s3", "all"], default="factory",
                    help="with --mock: the chips on their own USB (FLS-030): the factory Zigbee module "
                         "(COM9), ours at 0.9.0 (COM10), an ESP32-S3 (COM11), or all three")
    ap.add_argument("--browser", action="store_true", help="use a browser tab instead of a window")
    ap.add_argument("--no-open", action="store_true", help="only print the address (tests)")
    ap.add_argument("--http-port", type=int, default=0, help=argparse.SUPPRESS)
    ap.add_argument("--url-file", help=argparse.SUPPRESS)   # tests: where to write the page's address
    ap.add_argument("--server", help="the firmware server's address (default: ROBOALARMS_FIRMWARE_SERVER, "
                                     "else https://firmware.roboalarms.com)")
    ap.add_argument("--bundle", help="a .roboflash bundle")
    ap.add_argument("--build", action="append", help="an ESP-IDF build folder (repeatable)")
    sub = ap.add_subparsers(dest="cmd")
    b = sub.add_parser("bundle", help="make a bundle from build folders")
    b.add_argument("--build", action="append", required=True)
    b.add_argument("--out")
    b.add_argument("--key", help="the owner's signing key (default secrets/ota_signing_key.pem)")
    b.add_argument("--allow-foreign-key", action="store_true")
    b.add_argument("--allow-dark", action="store_true")
    b.add_argument("--radio", action="append", help="a build folder of the Zigbee module's software (h2_rcp)")
    b.add_argument("--voice", action="append", help="a signed voice pack (.robovoice) to carry (repeatable)")
    b.add_argument("--allow-dirty-radio", action="store_true",
                   help="bundle the radio's software even if its source has changes not committed")
    cb = sub.add_parser("check-bundle", help="open a bundle and list what it holds")
    cb.add_argument("file")
    p = sub.add_parser("ports", help="list serial ports")
    p.add_argument("--all", action="store_true")
    pr = sub.add_parser("probe", help="look at a panel without restarting it")
    pr.add_argument("port")
    idf = sub.add_parser("identify", help="read the chip through its loader (restarts the board)")
    idf.add_argument("port")
    idf.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)
    return {"bundle": run_bundle, "check-bundle": run_check_bundle, "ports": run_ports,
            "probe": run_probe, "identify": run_identify}.get(args.cmd, run_gui)(args)


if __name__ == "__main__":
    sys.exit(main())
