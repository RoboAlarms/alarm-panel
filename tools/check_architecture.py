#!/usr/bin/env python3
"""check_architecture.py - keep the component tiers one-way (docs/features/33, ADR-034).

Reads every component under components/<tier>/<name>/ - the requirements in its
CMakeLists.txt (ESP-IDF REQUIRES / PRIV_REQUIRES, the host branch's target_link_libraries)
and the #include lines of its sources - and checks them against the tier rules:

    core      portable C11: only core components; no ESP-IDF, FreeRTOS or LVGL headers;
              no heap; has a README.md and host tests in test/
    drivers   ESP-IDF chip drivers: core components and ESP-IDF, nothing board-, service- or
              UI-specific, no LVGL
    boards    one per board: core, drivers, board_api, ESP-IDF and LVGL; no services, no UI.
              A family library (<name>_family: what the boards on one main board share) is
              in this tier too; only a board component (board_<board>) may require one, and a
              family never reaches into a board of its own
    services  ESP-IDF, board-agnostic: core, drivers, board_api and other services; no board
              component, no UI component, no LVGL
    ui        portable LVGL logic: core components and LVGL only; no ESP-IDF headers but
              esp_attr.h (memory placement attributes)
    app       the shared start-up above everything: core, drivers, services, the UI and
              board_api, ESP-IDF and LVGL; never a board component

Options follow the tiers too: only board components read CONFIG_BOARD_* options, and board
components don't read the app's and services' CONFIG_ALARM_* options.

Every component needs a README.md, and components/README.md (the index) must name every
component. Every component has the same shape: public headers in include/, sources and private
headers in src/, host tests in test/; its root holds only build files and the README (no .c or
.h there). Known violations that a planned step removes are listed
in KNOWN_EXCEPTIONS with that step; an exception that no longer matches anything is an error
too, so the list only shrinks.

    python tools/check_architecture.py            # check (exit 1 on a violation)
    python tools/check_architecture.py --list     # components, tiers and dependencies
    python tools/check_architecture.py --graph    # the dependency graph as Mermaid
"""
import argparse
import fnmatch
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TIERS = ("core", "drivers", "boards", "services", "ui", "app")

# The board contract is the one boards-tier component other tiers may name (docs/features/33).
BOARD_CONTRACT = "board_api"
# Board components are board_<board>; a family library, <name>_family, holds what the boards on
# one main board share (components/boards/crowpanel_p4_family) and serves those board components.
BOARD_PREFIX = "board_"
FAMILY_SUFFIX = "_family"
LVGL_COMPONENTS = {"lvgl__lvgl", "lvgl"}

# Which project tiers each tier may require.
ALLOWED_TIERS = {
    "core": {"core"},
    "drivers": {"core", "drivers"},
    "boards": {"core", "drivers"},        # + the board contract, checked by name
    "services": {"core", "drivers", "services"},  # + the board contract
    "ui": {"core", "ui"},
    "app": {"core", "drivers", "services", "ui", "app"},  # + the board contract
}
# Whether a tier may require components that are not in this repository (ESP-IDF, registry).
EXTERNAL_ALLOWED = {"core": False, "drivers": True, "boards": True, "services": True, "ui": False,
                    "app": True}
LVGL_ALLOWED = {"core": False, "drivers": False, "boards": True, "services": False, "ui": True,
                "app": True}
PORTABLE_TIERS = {"core", "ui"}   # sources must not include ESP-IDF / FreeRTOS headers
# ...except these, which carry compile-time attributes and no behaviour: the UI places its big
# statics with EXT_RAM_BSS_ATTR under #ifdef ESP_PLATFORM (ui_internal.h, UI_SLOW_BSS).
PORTABLE_ALLOWED_HEADERS = {"core": set(), "ui": {"esp_attr.h"}}

# Headers that only exist on ESP-IDF (or FreeRTOS).
PLATFORM_HEADER = re.compile(
    r"^(esp_|esp/|freertos/|driver/|hal/|soc/|rom/|nvs|sdkconfig\.h|lwip/|mbedtls/|psa/|"
    r"sdmmc|spi_flash|mqtt_client\.h|cJSON|bootloader_|ota_ops|mdns)")
LVGL_HEADER = re.compile(r"^(lvgl\.h|lvgl/|lv_[a-z0-9_]+\.h)")
HEAP_CALL = re.compile(r"\b(malloc|calloc|realloc|free|heap_caps_[a-z_]+)\s*\(")
BOARD_OPTION = re.compile(r"\bCONFIG_BOARD_[A-Z0-9_]+")
APP_OPTION = re.compile(r"\bCONFIG_ALARM_[A-Z0-9_]+")
INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)

SOURCE_SUFFIXES = {".c", ".h"}
IDF_KEYWORDS = {"SRCS", "SRC_DIRS", "EXCLUDE_SRCS", "INCLUDE_DIRS", "PRIV_INCLUDE_DIRS",
                "REQUIRES", "PRIV_REQUIRES", "LDFRAGMENTS", "REQUIRED_IDF_TARGETS",
                "EMBED_FILES", "EMBED_TXTFILES", "KCONFIG", "KCONFIG_PROJBUILD",
                "WHOLE_ARCHIVE"}
LINK_KEYWORDS = {"PUBLIC", "PRIVATE", "INTERFACE"}

# (component, kind, pattern) -> why it is allowed for now and which step removes it.
# kind: "requires" (a CMake requirement), "include" (a header), "heap", "option".
# pattern: matched against the target (the requirement or header), or, written "file:<glob>",
# against the source file the violation is in.
KNOWN_EXCEPTIONS = {
}


@dataclass
class Component:
    name: str
    tier: str
    path: Path
    requires: set = field(default_factory=set)      # CMake requirements, all kinds
    includes: list = field(default_factory=list)    # (file, line, header)
    heap_calls: list = field(default_factory=list)  # (file, line, call)
    options: list = field(default_factory=list)     # (file, line, CONFIG_ name)
    public_headers: set = field(default_factory=set)
    has_readme: bool = False
    has_tests: bool = False
    root_sources: list = field(default_factory=list)  # .c/.h beside CMakeLists.txt


@dataclass(frozen=True)
class Violation:
    component: str
    kind: str
    target: str
    message: str
    where: str = ""     # the source file, for include and heap violations

    def __str__(self):
        return "%s: %s" % (self.component, self.message)


# ---------------------------------------------------------------------------------------------
# Parsing


def strip_cmake_comments(text):
    """Drop '#' comments (outside quotes) from CMake text."""
    out = []
    for line in text.splitlines():
        quoted = False
        cut = len(line)
        for i, ch in enumerate(line):
            if ch == '"':
                quoted = not quoted
            elif ch == "#" and not quoted:
                cut = i
                break
        out.append(line[:cut])
    return "\n".join(out)


def strip_c_comments(text):
    """Drop /* */ and // comments and string literals' contents from C text, keeping lines."""
    pattern = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', re.S)

    def repl(m):
        s = m.group(0)
        if s.startswith("/"):
            return "\n" * s.count("\n")
        return s[0] + s[-1]  # keep the quotes, drop the contents
    return pattern.sub(repl, text)


def cmake_calls(text, command):
    """Yield the argument text of every call to <command>(...) with nested parentheses."""
    for m in re.finditer(r"\b%s\s*\(" % re.escape(command), text, re.I):
        depth, i = 1, m.end()
        while i < len(text) and depth:
            if text[i] == "(":
                depth += 1
            elif text[i] == ")":
                depth -= 1
            i += 1
        yield text[m.end():i - 1]


def cmake_variables(text):
    """Values given to set(<var> ...) and list(APPEND <var> ...), unioned (every branch)."""
    variables = {}
    for args in cmake_calls(text, "set"):
        tokens = args.split()
        if tokens:
            variables.setdefault(tokens[0], []).extend(t.strip('"') for t in tokens[1:])
    for args in cmake_calls(text, "list"):
        tokens = args.split()
        if len(tokens) >= 2 and tokens[0].upper() == "APPEND":
            variables.setdefault(tokens[1], []).extend(t.strip('"') for t in tokens[2:])
    return variables


def expand(tokens, variables, depth=0):
    out = []
    for t in tokens:
        m = re.fullmatch(r"\$\{(\w+)\}", t)
        if m and depth < 5:
            out.extend(expand(variables.get(m.group(1), []), variables, depth + 1))
        else:
            out.append(t)
    return out


def cmake_requirements(text):
    """Project/ESP-IDF components a component's CMakeLists.txt asks for."""
    text = strip_cmake_comments(text)
    variables = cmake_variables(text)
    found = set()
    for args in cmake_calls(text, "idf_component_register"):
        tokens = expand(args.split(), variables)
        current = None
        for t in tokens:
            if t in IDF_KEYWORDS:
                current = t
            elif current in ("REQUIRES", "PRIV_REQUIRES"):
                found.add(t.strip('"'))
    for args in cmake_calls(text, "target_link_libraries"):
        tokens = expand(args.split(), variables)[1:]  # skip the target
        for t in tokens:
            t = t.strip('"')
            if t in LINK_KEYWORDS or t.startswith("$") or t.startswith("-"):
                continue
            found.add(t[len("idf::"):] if t.startswith("idf::") else t)
    return found


def scan_component(path, tier):
    comp = Component(name=path.name, tier=tier, path=path)
    comp.has_readme = (path / "README.md").is_file()
    comp.has_tests = (path / "test" / "CMakeLists.txt").is_file()
    comp.root_sources = sorted(f.name for f in path.iterdir()
                               if f.is_file() and f.suffix in SOURCE_SUFFIXES)
    cmake = path / "CMakeLists.txt"
    if cmake.is_file():
        comp.requires = cmake_requirements(cmake.read_text(encoding="utf-8"))
        comp.requires.discard(comp.name)
    for f in sorted(path.rglob("*")):
        if f.suffix not in SOURCE_SUFFIXES or not f.is_file():
            continue
        rel = f.relative_to(path)
        if rel.parts[0] == "test":
            continue
        if rel.parts[0] == "include":
            comp.public_headers.add("/".join(rel.parts[1:]))
        text = f.read_text(encoding="utf-8", errors="replace")
        for m in INCLUDE.finditer(text):
            line = text.count("\n", 0, m.start()) + 1
            comp.includes.append((rel.as_posix(), line, m.group(1)))
        code = strip_c_comments(text)
        for m in HEAP_CALL.finditer(code):
            line = code.count("\n", 0, m.start()) + 1
            comp.heap_calls.append((rel.as_posix(), line, m.group(1)))
        for pattern in (BOARD_OPTION, APP_OPTION):
            for m in pattern.finditer(code):
                line = code.count("\n", 0, m.start()) + 1
                comp.options.append((rel.as_posix(), line, m.group(0)))
    return comp


def discover(root):
    components = {}
    for tier in TIERS:
        tier_dir = root / "components" / tier
        if not tier_dir.is_dir():
            continue
        for path in sorted(tier_dir.iterdir()):
            if path.is_dir() and (path / "CMakeLists.txt").is_file():
                comp = scan_component(path, tier)
                if comp.name in components:
                    raise SystemExit("component name used twice: %s" % comp.name)
                components[comp.name] = comp
    return components


# ---------------------------------------------------------------------------------------------
# Rules


def header_owners(components):
    owners = {}
    for comp in components.values():
        for h in comp.public_headers:
            owners.setdefault(h, set()).add(comp.name)
    return owners


def is_family(comp):
    return comp.tier == "boards" and comp.name.endswith(FAMILY_SUFFIX)


def tier_allows(comp, other):
    """May component <comp> depend on project component <other>?"""
    if other.name == BOARD_CONTRACT:
        return comp.tier in ("boards", "services", "app")
    if is_family(other):
        return comp.tier == "boards" and comp.name.startswith(BOARD_PREFIX)
    return other.tier in ALLOWED_TIERS[comp.tier]


def check(components):
    violations = []
    owners = header_owners(components)
    for comp in components.values():
        tier = comp.tier
        if not comp.has_readme:
            violations.append(Violation(comp.name, "readme", "README.md",
                                        "no README.md (docs/features/33 principle 2)"))
        for name in comp.root_sources:
            violations.append(Violation(comp.name, "layout", name,
                                        "%s is at the component's root: sources and private headers "
                                        "go in src/, public headers in include/" % name, name))
        if tier == "core" and not comp.has_tests:
            violations.append(Violation(comp.name, "tests", "test",
                                        "a core component needs host tests in test/"))
        for req in sorted(comp.requires):
            other = components.get(req)
            if other is not None:
                if not tier_allows(comp, other):
                    violations.append(Violation(
                        comp.name, "requires", req,
                        "%s component requires %s (%s)" % (tier, req, other.tier)))
            elif req in LVGL_COMPONENTS:
                if not LVGL_ALLOWED[tier]:
                    violations.append(Violation(comp.name, "requires", req,
                                                "%s component requires LVGL" % tier))
            elif not EXTERNAL_ALLOWED[tier] and req not in ("m",):
                violations.append(Violation(
                    comp.name, "requires", req,
                    "%s component requires %s, which is not a portable project component"
                    % (tier, req)))
        for rel, line, header in comp.includes:
            where = "%s:%d" % (rel, line)
            own = owners.get(header, set()) - {comp.name}
            if own and header not in comp.public_headers:
                bad = sorted(o for o in own if not tier_allows(comp, components[o]))
                if bad and len(bad) == len(own):
                    violations.append(Violation(
                        comp.name, "include", header,
                        "%s includes %s from %s (%s)"
                        % (where, header, ", ".join(bad), components[bad[0]].tier), rel))
                continue
            if (tier in PORTABLE_TIERS and PLATFORM_HEADER.match(header)
                    and header not in PORTABLE_ALLOWED_HEADERS[tier]):
                violations.append(Violation(comp.name, "include", header,
                                            "%s includes ESP-IDF/FreeRTOS header %s"
                                            % (where, header), rel))
            elif not LVGL_ALLOWED[tier] and LVGL_HEADER.match(header):
                violations.append(Violation(comp.name, "include", header,
                                            "%s includes LVGL header %s" % (where, header), rel))
        for rel, line, option in comp.options:
            if option.startswith("CONFIG_BOARD_") and tier != "boards":
                violations.append(Violation(comp.name, "option", option,
                                            "%s:%d reads the board option %s (only boards may)"
                                            % (rel, line, option), rel))
            elif option.startswith("CONFIG_ALARM_") and tier == "boards":
                violations.append(Violation(comp.name, "option", option,
                                            "%s:%d reads the app option %s (boards may not)"
                                            % (rel, line, option), rel))
        if tier == "core":
            for rel, line, call in comp.heap_calls:
                violations.append(Violation(comp.name, "heap", call,
                                            "%s:%d calls %s() (core components don't allocate)"
                                            % (rel, line, call), rel))
    return violations


def exception_matches(key, v):
    component, kind, pattern = key
    if component != v.component or kind != v.kind:
        return False
    if pattern.startswith("file:"):
        return bool(v.where) and fnmatch.fnmatchcase(v.where, pattern[len("file:"):])
    return fnmatch.fnmatchcase(v.target, pattern)


def check_index(root, components):
    """components/README.md must exist and name every component (as `name`)."""
    index = root / "components" / "README.md"
    if not index.is_file():
        return [Violation("components", "index", "README.md", "components/README.md is missing")]
    text = index.read_text(encoding="utf-8")
    return [Violation(c.name, "index", "components/README.md",
                      "not listed in components/README.md (add it to the index)")
            for c in components.values() if "`%s`" % c.name not in text]


def apply_exceptions(violations, exceptions):
    """Split violations into (remaining, used exception keys)."""
    remaining, used = [], set()
    for v in violations:
        key = next((k for k in exceptions if exception_matches(k, v)), None)
        if key is None:
            remaining.append(v)
        else:
            used.add(key)
    return remaining, used


def run(root, exceptions, out=sys.stdout):
    components = discover(root)
    violations = check(components) + check_index(root, components)
    remaining, used = apply_exceptions(violations, exceptions)
    stale = [k for k in exceptions if k not in used]
    for v in remaining:
        print("VIOLATION  " + str(v), file=out)
    for k in stale:
        print("STALE EXCEPTION  %s %s %s: nothing matches any more, remove it from "
              "KNOWN_EXCEPTIONS" % k, file=out)
    tiers = {}
    for c in components.values():
        tiers.setdefault(c.tier, 0)
        tiers[c.tier] += 1
    summary = ", ".join("%d %s" % (tiers[t], t) for t in TIERS if t in tiers)
    if remaining or stale:
        print("FAIL: %d violation(s), %d stale exception(s) in %d components (%s)"
              % (len(remaining), len(stale), len(components), summary), file=out)
        return 1
    print("OK: %d components (%s) follow the tier rules; %d known exception(s) pending"
          % (len(components), summary, len(used)), file=out)
    return 0


def print_list(root, out=sys.stdout):
    components = discover(root)
    for tier in TIERS:
        for c in (c for c in components.values() if c.tier == tier):
            project = sorted(r for r in c.requires if r in components)
            print("%-9s %-20s -> %s" % (tier, c.name, ", ".join(project) or "-"), file=out)


def print_graph(root, out=sys.stdout):
    components = discover(root)
    print("flowchart TB", file=out)
    for tier in TIERS:
        members = [c for c in components.values() if c.tier == tier]
        if members:
            print("  subgraph %s" % tier, file=out)
            for c in members:
                print("    %s" % c.name, file=out)
            print("  end", file=out)
    for c in components.values():
        for r in sorted(c.requires):
            if r in components:
                print("  %s --> %s" % (c.name, r), file=out)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--root", type=Path, default=ROOT, help=argparse.SUPPRESS)
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--list", action="store_true", help="print components and dependencies")
    group.add_argument("--graph", action="store_true", help="print a Mermaid dependency graph")
    args = parser.parse_args(argv)
    if args.list:
        print_list(args.root)
        return 0
    if args.graph:
        print_graph(args.root)
        return 0
    return run(args.root, KNOWN_EXCEPTIONS)


if __name__ == "__main__":
    sys.exit(main())
