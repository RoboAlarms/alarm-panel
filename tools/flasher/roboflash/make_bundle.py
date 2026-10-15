"""Build a firmware bundle from ESP-IDF build folders (FLS-015).

    python tools/flasher/run.py bundle --build firmware/crowpanel_p4/build_v1_dev \
        --build firmware/crowpanel_p4/build_v3_dev --build firmware/crowpanel_10/build_v1_dev

Each build folder gives one image: the offsets and flash settings from `flasher_args.json`,
the flash size and profile from `config/sdkconfig.json`, and the rest from the app's own header.
Images must be signed; when the owner's key (`secrets/ota_signing_key.pem`) is here, they must
be signed with it, because a panel only accepts network updates signed with the key of the
firmware it runs. A dark build (backlight forced off, docs/features/21) is refused: to someone
flashing a panel it would look broken.

`--voice <file.robovoice>` adds a signed voice pack (tools/voice, `voice.py pack`): signed with the
images' key, no draft beside a release image, one per locale and voice (BL-204).

`--radio <h2_rcp build folder>` adds the Zigbee module's own software (firmware/radios/h2_rcp) to
the bundle's `radios`: unsigned, so it is taken only from a build of that project for the H2,
and only from clean source unless `--allow-dirty-radio` says otherwise.
"""
import datetime
import hashlib
import json
import subprocess
from pathlib import Path

from . import bundle as bundle_mod
from .bundle import Image, Part, Language, Asset, Radio, BundleError, RADIO_KINDS, ASSET_KINDS
from .boards import load_catalogue, not_offered
from .imagefile import parse_app, key_digest_from_pem
from .packstore import read_pack, read_asset, read_voice, StoreError

ROOT = Path(__file__).resolve().parents[3]
DEFAULT_KEY = ROOT / "secrets" / "ota_signing_key.pem"
DEFAULT_OUT_DIR = ROOT / "build" / "flasher"


def _profile(sdk, project):
    if sdk.get("ALARM_BUILD_RELEASE"):
        return "release"
    if sdk.get("ALARM_BUILD_DEV"):
        return "dev"
    for suffix in ("dev", "release"):
        if project.endswith("-" + suffix):
            return suffix
    return "legacy"


def image_from_build(build_dir, catalogue=None, allow_dark=False):
    build_dir = Path(build_dir)
    try:
        fa = json.loads((build_dir / "flasher_args.json").read_text())
    except FileNotFoundError:
        raise BundleError(f"{build_dir}: no flasher_args.json (build the firmware first)") from None
    sdk_path = build_dir / "config" / "sdkconfig.json"
    sdk = json.loads(sdk_path.read_text()) if sdk_path.exists() else {}
    if sdk.get("BOARD_BACKLIGHT_FORCE_OFF") and not allow_dark:
        raise BundleError(f"{build_dir}: a dark build (backlight forced off); bundle a normal build")
    parts = []
    for role in bundle_mod.ROLES:
        entry = fa.get(role)
        if not entry:
            raise BundleError(f"{build_dir}: flasher_args.json has no {role}")
        data = (build_dir / entry["file"]).read_bytes()
        parts.append(Part(role=role, offset=int(entry["offset"], 0), path="", sha256="", data=data))
    app_part = next(p for p in parts if p.role == "app")
    app = parse_app(app_part.data)
    if not app.signing_key_sha256:
        raise BundleError(f"{build_dir}: the app is not signed (use the signed .bin the build wrappers make)")
    chip = fa.get("extra_esptool_args", {}).get("chip", app.chip)
    if chip != app.chip:
        raise BundleError(f"{build_dir}: flasher_args says {chip}, the app says {app.chip}")
    if not app.project.startswith("alarm_"):
        raise BundleError(f"{build_dir}: {app.project} is not a RoboAlarms firmware")
    board = app.project[len("alarm_"):].split("-", 1)[0]
    catalogue = catalogue or load_catalogue()
    info = catalogue.get(board)
    if info is None and board in not_offered():
        raise BundleError(f"{build_dir}: the Flasher does not offer {board!r} "
                          f"({not_offered()[board].get('not_offered', 'offered: false in boards.json')})")
    if info is None:
        raise BundleError(f"{build_dir}: board {board!r} is not in tools/flasher/roboflash/boards.json")
    flash_size = sdk.get("ESPTOOLPY_FLASHSIZE") or info["flash_size"]
    settings = fa.get("flash_settings", {})
    pt = next(p for p in parts if p.role == "partition-table")
    image = Image(board=board, board_name=info["name"], variant=f"v{app.max_rev // 100}",
                  profile=_profile(sdk, app.project), project=app.project, version=app.version,
                  elf_sha256=app.elf_sha256, chip=app.chip, chip_rev_min=app.min_rev,
                  chip_rev_max=app.max_rev, flash_size=flash_size,
                  flash_mode=settings.get("flash_mode", "keep"), flash_freq=settings.get("flash_freq", "keep"),
                  partition_table_sha256=hashlib.sha256(pt.data).hexdigest(),
                  signing_key_sha256=app.signing_key_sha256, parts=parts)
    # Checked here too (FLS2-002): --build installs this Image without writing or opening a bundle.
    return bundle_mod.check_layout(image)


def languages_from_build(build_dir, version, signing, log=print):
    """The packs in <build>/lang/*.robolang, each made for `version` and signed with `signing`."""
    files = sorted((Path(build_dir) / "lang").glob("*.robolang"))
    if not files:
        log(f"note: {Path(build_dir) / 'lang'} holds no language packs; the bundle has no languages")
        return []
    languages = []
    for f in files:
        data = f.read_bytes()
        try:
            info = read_pack(data)
        except ValueError as e:
            raise BundleError(f"{f}: {e}") from None
        if info.version != version:
            raise BundleError(f"{f}: made for version {info.version}, the images are {version}")
        if info.signing_key_sha256 != signing:
            raise BundleError(f"{f} is signed with another key than the firmware (or not signed)")
        languages.append(Language(locale=info.locale, name=info.name, version=info.version,
                                  draft=info.draft, path="", sha256="", data=data))
    return languages


def assets_from_build(build_dir, version, signing, log=print):
    """The asset packs in <build>/assets/*.roboasset (kinds 3-5), each made for `version` and signed
    with `signing` (OPT-007)."""
    files = sorted((Path(build_dir) / "assets").glob("*.roboasset"))
    if not files:
        log(f"note: {Path(build_dir) / 'assets'} holds no asset packs; the bundle has none")
        return []
    assets = []
    for f in files:
        data = f.read_bytes()
        try:
            info = read_asset(data)
        except StoreError as e:
            raise BundleError(f"{f}: {e}") from None
        if info.kind not in ASSET_KINDS:
            log(f"note: {f.name} is of a kind this Flasher does not install; left out")
            continue
        if info.version != version:
            raise BundleError(f"{f}: made for version {info.version}, the images are {version}")
        if info.signing_key_sha256 != signing:
            raise BundleError(f"{f} is signed with another key than the firmware (or not signed)")
        if any(a.kind == info.kind and a.name == info.name for a in assets):
            raise BundleError(f"{f}: the asset pack {info.name} is there twice")
        assets.append(Asset(kind=info.kind, name=info.name, version=info.version, draft=info.draft,
                            path="", sha256="", data=data))
    return assets


def _read_json(path):
    try:
        return json.loads(Path(path).read_text(encoding="utf-8"))
    except FileNotFoundError:
        return None
    except ValueError as e:
        raise BundleError(f"{path}: not valid JSON ({e})") from None


def _flash_args(build_dir):
    """{offset: file} and the flash settings from `flash_args` (when there is no flasher_args.json)."""
    path = Path(build_dir) / "flash_args"
    if not path.exists():
        raise BundleError(f"{build_dir}: neither flasher_args.json nor flash_args (build it first)")
    files, settings = {}, {}
    for line in path.read_text(encoding="utf-8").splitlines():
        words = line.split()
        if words and words[0].startswith("--"):
            for flag, value in zip(words[::2], words[1::2]):
                settings[flag[2:].replace("-", "_")] = value
        elif len(words) == 2:
            files[int(words[0], 0)] = words[1]
    return files, settings


def _radio_source(project_path):
    """{commit, dirty} of the radio's source folder, its own build folder left out: the last commit
    that changed it (the build records none of its own, and HEAD may be a later commit that left the
    radio alone, FLS-015), dirty when git shows any change in it. {} when git can't tell."""
    path = Path(project_path)
    try:
        commit = subprocess.run(["git", "log", "-1", "--format=%h", "--", ".", ":(exclude)build"], cwd=path,
                                capture_output=True, text=True, timeout=10)
        status = subprocess.run(["git", "status", "--porcelain", "--", ".", ":(exclude)build"], cwd=path,
                                capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.SubprocessError):
        return {}
    if commit.returncode or status.returncode or not commit.stdout.strip():
        return {}
    return {"commit": commit.stdout.strip(), "dirty": bool(status.stdout.strip())}


def radio_from_build(build_dir, allow_dirty=False, kind="zigbee", catalogue=None, log=print):
    """A Radio from an ESP-IDF build of the radio's software (firmware/radios/h2_rcp)."""
    build_dir = Path(build_dir)
    want = RADIO_KINDS[kind]
    desc = _read_json(build_dir / "project_description.json")
    if desc is None:
        raise BundleError(f"{build_dir}: no project_description.json (build the radio's software first)")
    project, target = desc.get("project_name"), desc.get("target")
    if project != want["project"]:
        raise BundleError(f"{build_dir}: {project!r} is not the {kind} radio's software ({want['project']})")
    if target != want["chip"]:
        raise BundleError(f"{build_dir}: built for {target!r}; the {kind} radio is an {want['chip']}")
    fa = _read_json(build_dir / "flasher_args.json")
    if fa is not None:
        files = {}
        for role in want["parts"]:
            entry = fa.get(role)
            if not entry:
                raise BundleError(f"{build_dir}: flasher_args.json has no {role}")
            files[role] = (int(entry["offset"], 0), entry["file"])
        settings = fa.get("flash_settings", {})
        if fa.get("extra_esptool_args", {}).get("chip", target) != target:
            raise BundleError(f"{build_dir}: flasher_args.json is for another chip than the project")
    else:
        by_offset, settings = _flash_args(build_dir)
        names = {"bootloader": "bootloader/bootloader.bin",
                 "partition-table": "partition_table/partition-table.bin",
                 "app": desc.get("app_bin", "")}
        files = {}
        for role in want["parts"]:
            hits = [(o, f) for o, f in by_offset.items() if f == names[role]]
            if len(hits) != 1:
                raise BundleError(f"{build_dir}: flash_args has no {role}")
            files[role] = hits[0]
    layout = ((catalogue or {}).get("zigbee") or {}).get("radio", {}).get("parts")
    parts = []
    for role, (offset, name) in files.items():
        expected = want["parts"][role]
        if layout and int(layout[role], 0) != expected:
            raise BundleError(f"boards.json puts the {kind} radio's {role} at {layout[role]}, "
                              f"the Flasher at {expected:#x}")
        if offset != expected:
            raise BundleError(f"{build_dir}: its {role} at {offset:#x}; the {kind} radio's goes at {expected:#x}")
        try:
            data = (build_dir / name).read_bytes()
        except FileNotFoundError:
            raise BundleError(f"{build_dir}: {name} is missing (build it again)") from None
        parts.append(Part(role=role, offset=offset, path="", sha256="", data=data))
    parts.sort(key=lambda p: p.offset)
    try:
        app = parse_app(next(p for p in parts if p.role == "app").data)
    except ValueError as e:
        raise BundleError(f"{build_dir}: the app is not usable ({e})") from None
    if (app.chip, app.project) != (want["chip"], want["project"]):
        raise BundleError(f"{build_dir}: the app says {app.project} for {app.chip}, "
                          f"not {want['project']} for {want['chip']}")
    sdk = _read_json(build_dir / "config" / "sdkconfig.json") or {}
    for name, got in (("ESP_REV_MIN_FULL", app.min_rev), ("ESP_REV_MAX_FULL", app.max_rev)):
        if name in sdk and sdk[name] != got:
            raise BundleError(f"{build_dir}: sdkconfig's {name} is {sdk[name]}, the app's {got}")
    flash = {"size": sdk.get("ESPTOOLPY_FLASHSIZE") or settings.get("flash_size"),
             "mode": sdk.get("ESPTOOLPY_FLASHMODE") or settings.get("flash_mode"),
             "freq": sdk.get("ESPTOOLPY_FLASHFREQ") or settings.get("flash_freq")}
    for what, value in flash.items():
        if not value or value == "keep":
            raise BundleError(f"{build_dir}: the build does not say its flash {what}")
    source = _radio_source(desc.get("project_path") or build_dir)
    if not source:
        msg = f"{build_dir}: git can't tell whether the {kind} radio's source is clean"
    elif source["dirty"]:
        msg = (f"{build_dir}: the {kind} radio's source ({desc.get('project_path')}) has changes "
               f"not committed")
    else:
        msg = ""
    if msg and not allow_dirty:
        raise BundleError(msg + " (--allow-dirty-radio to bundle it anyway)")
    if msg:
        log("warning: " + msg)
    rcp = build_dir / "rcp_version"   # written by ESP-IDF's openthread component
    rcp_version = rcp.read_text(encoding="utf-8", errors="replace").strip() if rcp.exists() else ""
    return Radio(kind=kind, chip=app.chip, project=app.project, version=app.version,
                 elf_sha256=app.elf_sha256, chip_rev_min=app.min_rev, chip_rev_max=app.max_rev,
                 flash_size=flash["size"], flash_mode=flash["mode"], flash_freq=flash["freq"],
                 parts=parts, rcp_version=rcp_version, source=source)


def _git_source():
    try:
        commit = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT, capture_output=True,
                                text=True, timeout=10).stdout.strip()
        dirty = bool(subprocess.run(["git", "status", "--porcelain", "--untracked-files=no"], cwd=ROOT,
                                    capture_output=True, text=True, timeout=10).stdout.strip())
        return {"commit": commit, "dirty": dirty} if commit else {}
    except (OSError, subprocess.SubprocessError):
        return {}


def voices_from_files(paths, signing, images):
    """bundle.Voice for each .robovoice file given, checked as the bundle will be."""
    voices = []
    for f in paths or ():
        data = Path(f).read_bytes()
        try:
            info = read_voice(data)
        except StoreError as e:
            raise BundleError(f"{f}: {e}") from None
        if info.signing_key_sha256 != signing:
            raise BundleError(f"{f} is signed with another key than the firmware (or not signed)")
        if info.draft and any(i.profile == "release" for i in images):
            raise BundleError(f"{f} is a draft voice, which a release firmware refuses")
        if any((v.locale, v.voice_id) == (info.locale, info.voice_id) for v in voices):
            raise BundleError(f"{f}: the voice {info.locale} {info.voice_id} is given twice")
        voices.append(bundle_mod.Voice(locale=info.locale, voice_id=info.voice_id, revision=info.revision,
                                       draft=info.draft, path="", sha256="", data=data))
    return voices


def make(build_dirs, out=None, key=DEFAULT_KEY, allow_foreign_key=False, allow_dark=False, log=print,
         radio_dirs=(), allow_dirty_radio=False, voice_files=()):
    images = [image_from_build(d, allow_dark=allow_dark) for d in build_dirs]
    catalogue = load_catalogue()
    radios = []
    for d in radio_dirs or ():
        board = images[0].board if images else ""
        radio = radio_from_build(d, allow_dirty=allow_dirty_radio, catalogue=catalogue.get(board), log=log)
        if any(r.kind == radio.kind for r in radios):
            raise BundleError(f"two builds give the {radio.kind} radio's software")
        radios.append(radio)
    keys = {i.signing_key_sha256 for i in images}
    if len(keys) != 1:
        raise BundleError("the images are signed with different keys")
    signing = keys.pop()
    owner = key_digest_from_pem(key) if key and Path(key).exists() else ""
    if owner and owner != signing:
        msg = ("the images are not signed with the owner's key "
               f"({key}): a panel flashed with them would refuse the owner's network updates")
        if not allow_foreign_key:
            raise BundleError(msg + " (--allow-foreign-key to bundle them anyway)")
        log("warning: " + msg)
    elif not owner:
        log(f"note: {key} not found; the signing key was not compared with the owner's")
    seen = set()
    for image in images:
        if image.key in seen:
            raise BundleError(f"two builds give the same image {image.key}")
        seen.add(image.key)
    versions_set = {i.version for i in images}
    languages = []
    assets = []
    if len(versions_set) == 1:
        languages = languages_from_build(build_dirs[0], next(iter(versions_set)), signing, log)
        assets = assets_from_build(build_dirs[0], next(iter(versions_set)), signing, log)
        # One set serves every image (the CrowPanel 7" and 10.1" share their letters, picture and
        # certificates): another build's packs must hold the same contents, signatures aside.
        payload = lambda data: data[:read_asset(data).payload_length]  # noqa: E731
        first = {(a.kind, a.name): payload(a.data) for a in assets}
        for d in build_dirs[1:]:
            for f in sorted((Path(d) / "assets").glob("*.roboasset")):
                try:
                    info = read_asset(f.read_bytes())
                except StoreError as e:
                    raise BundleError(f"{f}: {e}") from None
                mine = first.get((info.kind, info.name))
                if mine is not None and mine != payload(f.read_bytes()):
                    raise BundleError(f"{f}: differs from {build_dirs[0]}'s {info.name} pack, and a "
                                      "bundle holds one set of asset packs for every image")
    elif (Path(build_dirs[0]) / "lang").is_dir():
        raise BundleError("the images are of different versions: the language packs fit only one")
    elif (Path(build_dirs[0]) / "assets").is_dir():
        raise BundleError("the images are of different versions: the asset packs fit only one")
    if out is None:
        versions = "-".join(sorted({i.version for i in images}))
        boards = "-".join(dict.fromkeys(f"{i.board}-{i.variant}" for i in images))
        profiles = "-".join(sorted({i.profile for i in images}))
        out = DEFAULT_OUT_DIR / f"roboalarms-{versions}-{boards}-{profiles}{bundle_mod.EXTENSION}"
    created = datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0).isoformat()
    voices = voices_from_files(voice_files, signing, images)
    for voice in voices:
        log(f"  voice {voice.label()}{' draft' if voice.draft else ''}, {voice.size} bytes")
    path = bundle_mod.write(out, images, created, _git_source(), signing, languages, radios, assets, voices)
    bundle_mod.load(path)   # read it back exactly as the Flasher will
    for image in images:
        log(f"  {image.key}: {image.project} {image.version}, {image.silicon_text()}, "
            f"{sum(len(p.data) for p in image.parts)} bytes")
    for lang in languages:
        log(f"  language {lang.locale} ({lang.name}) {lang.version}{' draft' if lang.draft else ''}, "
            f"{len(lang.data)} bytes")
    for asset in assets:
        log(f"  asset pack {asset.name} (kind {asset.kind}) {asset.version}{' draft' if asset.draft else ''}, "
            f"{len(asset.data)} bytes")
    for radio in radios:
        log(f"  {radio.kind} radio: {radio.project} {radio.version} for {radio.chip}, unsigned, "
            f"{sum(len(p.data) for p in radio.parts)} bytes"
            + (f", {radio.rcp_version}" if radio.rcp_version else ""))
    log(f"wrote {path}")
    return path
