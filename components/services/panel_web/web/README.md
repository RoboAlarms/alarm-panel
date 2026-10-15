# Panel Manager

The production browser interface is hosted entirely by the alarm panel. These files are
embedded by `components/services/panel_web` and served over HTTPS from the signed image (flash, not
RAM); there is no Node/Python server, CDN, external font or cloud dependency in the product.
They go into the image gzipped at build time (`gzip_asset.py` there: 185 KB to 59 KB on
2026-09-25) and a browser gets them as they are (`Content-Encoding: gzip`); a client that
doesn't list gzip in `Accept-Encoding` gets them plain, inflated on the way out. Edit the files
here as before: the build does the rest.

| File | What |
|---|---|
| `panel.html` | the page shell: sidebar, top bar, dialog, toast |
| `panel.css` | the look, light and dark (follows the browser). Nothing animates or flashes |
| `panel-words.js` | the words (docs/features/37): `t()`, `tc()`, `tn()`, `ht()` (escaped for HTML), `hx()` (around values already HTML), `tx()` (marks a table of the page's own words), `nt()` (fixed text); this browser's language, else the panel's (`/i18n/panel`), loaded first from `/i18n/<locale>.json`, keyed by the hash `i18n_hash` uses. Every word a page shows goes through them: `tools/i18n.py check` reads these files and `panel.html` (`data-t`, `data-t-label`, `translate="no"`) |
| `panel.js` | core: API calls, navigation (`#/page/arg`), form builder, review dialog, sign-in, network-trial and firmware-update flows |
| `panel-pages.js` | every page, and the words for the panel's numbers (zone types, roles, troubles, time zones, the same words as the touchscreen). `PAGES` at the end is the navigation |
| `panel-keypad.js` | the remote keypad: one WebSocket, the H.264 (WebCodecs) or JPEG picture on a canvas, taps / holds / drags, the computer's keyboard (tested by `components/services/panel_web/test/panel_frames.test.cjs`) |
| `mock-panel.cjs` | **PC only**: a pretend panel for trying UI changes (below) |
| `mock-screen.cjs` | **PC only**: the mock's remote keypad WebSocket, streaming real H.264 (I_PCM) or PNG of a pretend screen |

Pages are grouped: **Overview / Remote keypad / Activity**, **Household** (people and codes,
integrations, automations), **Panel settings** (display and sound, network, date and time, storage),
**Installer** (sensor sources, partitions and devices, Z-Wave, system options, access and
maintenance) and **System** (health, walk test, updates, developer). Each user sees only what the panel's
own table lets them open (docs/features/14 "Who can change what"; the panel checks every request
itself): a standard user gets the overview, the remote keypad, activity, the zone list in the
panel's words, display and sound, date and time, storage, health and update status (a walk test
running shows on their Overview, with End); a partition
master adds people, network and the Walk test page; the system master integrations and automations (on a build
with them, docs/features/41); the installer everything.
Every change opens a review that lists each setting as before → after; alarm programming,
sensor links, users and remote access also ask for your keypad code. Nothing changes while the
panel is armed or an alarm sounds.

How forms work: the firmware sends each settings domain with a schema (field names, limits);
`panel-pages.js` only says how each field is shown (label, help, unit, a select instead of a
number, check boxes instead of a bit mask). A field the page doesn't know yet still appears
with a generic label, so new firmware fields are never hidden.

## Try a change on the PC first

```powershell
& "$env:LOCALAPPDATA\Programs\node-v24.21.0-win-x64\node.exe" components/services/panel_web/web/mock-panel.cjs
```

Open `http://127.0.0.1:8531` and sign in as `Alex` with `correct horse battery` (system master),
`Installer` with `installer password` (installer), `Pat` with `partition master` or `Sam`
with `standard user`; pages that confirm a change ask for that person's keypad code: `1234`
(Alex), `4321` (installer), `5555` (Pat), `1111` (Sam).
The mock answers `/api/v1` with the firmware's shapes and schemas, applies the firmware's access
table, and keeps changes in memory until it stops. Its remote keypad (`mock-screen.cjs`) streams
real H.264 that the browser decodes (or PNG), with pretend keys that light when clicked and
blocks for typed letters. `MOCK_AUTOLOGIN=installer` (or `master`, `partition`, `standard`)
skips sign-in, for headless screenshots; `MOCK_LOCKED=1` pretends the panel is armed;
`MOCK_PARTITIONS=2` puts a second partition (the garage) in use; `MOCK_ZWAVE=0` a panel
without Z-Wave (by default it has a radio, three devices - one not heard, its setup stopped - and a
sensor waiting to join; **Add a device** walks through the steps on timers, the joining device's
PIN is 12345); `MOCK_CLOCK=ok` (or `lost`,
`failed`, `other`) fits a battery-backed clock module (docs/features/35), which adds its card and
the date and time set by hand to Date and time. `MOCK_I18N=build/i18n/pseudo` (after
`python tools/i18n.py pseudo --build`) serves the test languages' catalogs, in the form the panel
sends them, and offers them as the panel's languages; pick one under Display and sound > This
browser, or as the panel's own language (docs/features/37). `MOCK_I18N=build/i18n/web` (after
`python tools/i18n.py web`) does the same with the real languages (Spanish, `es-US`). The mock's
own answers (events, zone states, the region and clock names) stay English. Afterwards, choose English again under This browser: the choice stays in the
browser, and a test language left there makes a later look at the mock read strangely.
The mock arms and disarms as the person signed in (`POST /api/v1/partition`, BL-113), with a small
copy of the engine's rules and timers per partition: exit and entry delays, alarm memory, bypass, the
guest's and installer's disarm rule, and each action only in the state it is for (disarm while armed,
in exit or entry time or in an alarm; acknowledge always; the rest while disarmed; otherwise 409
`bad_state`). `MOCK_SCENARIO=ready|notready|motion|exit|armed|stay|entry|alarm|memory|fire`
starts the first partition in that state for screenshots (`MOCK_LOCKED=1` alone starts it armed;
`fire` is the hallway smoke detector in alarm while disarmed, siren on, `alarm_kind` "fire":
acknowledge silences it and the alarm ends into memory; its trouble beeps are left unacknowledged, in the
garage too with `MOCK_PARTITIONS=2`);
`MOCK_WALK=1` starts a walk test at boot on the first partition, begun by the installer at the panel
(docs/features/40: the Overview's card and the nav's count for a test started elsewhere) and, once any
walk test runs there, a pretend tester trips a zone every `MOCK_WALK_STEP_MS` (4000; `0`: nobody walks):
doors and windows, the hallway twice, a keyfob, one cover and a CO detector's own test button, leaving
two zones untested;
`MOCK_AUTOMATIONS=0` is a build without automations (by default Harbor House's four, as the simulator
seeds them, and its devices; `empty`, `full`, `clock` (the clock isn't set), `readonly` and `newer` (a record made
by newer software) show those states);
`MOCK_VOICE=ready` (a draft voice pack, Announcements on; the default), `released`, `unavailable` (installed, not ready),
`none`, `error` (the voice information can't be read) or `absent` (a firmware without the voice) sets Display and sound's
Announcements card and each zone's Spoken name (BL-202); an upload is a file of 8192 to 970752 bytes in whole 4096 blocks
that starts `RVOC`;
`MOCK_FAST=1` runs the delays 20 times faster (tests); `MOCK_AUTOLOGIN=guest` signs in as the dog
walker (a guest has no browser sign-in on a real panel; this is only for trying the guest's limits).
`node --test components/services/panel_web/test/panel_pages.test.cjs` renders every page each role may open and checks
the refusals (CI runs it); keep the mock in step when the API changes.

The firmware ships these files minified, then gzipped (`../minify_asset.py`, called by `../gzip_asset.py`; Python
standard library only, no Node at build time): comments, indentation and empty lines go, nothing is renamed, line
breaks stay, and strings, templates and regular expressions are untouched. These sources stay the readable ones. To run
the tests against the shipped files, make them with `python components/services/panel_web/minify_asset.py --all
components/services/panel_web/web <folder>`, then set `PANEL_WEB_DIR=<folder>` for the same `node --test` command (CI runs both).

The same mock runs inside a page in [the Panel Manager's mockup](../../../../docs/features/31-Browser-Panel-Manager/mockup/README.md)
(`node docs/features/31-Browser-Panel-Manager/mockup/build.cjs`): every switch above in a bar, with no
Node to run, openable on a phone. A change to what the pages show starts there as a proposal (WEB-026).

## Open the real panel

1. On the physical panel, sign into **Settings > Installer** and enable **Web server**.
   The page shows its HTTPS address and certificate SHA256 fingerprint. Fresh panels default
   off; a previously enrolled Feature 30 development panel migrates enabled.
2. Open `https://<panel-IP>/` from its LAN or routed VPN. The panel has a per-device self-signed
   certificate: verify its fingerprint from local Installer before trusting it in the browser.
3. Sign in with a user name and a password (keypad codes don't sign in). The installer, system
   and partition masters and standard users can have one; whoever looks after the users turns
   it on: at the panel, **Settings > Users >** the person **> Allow browser sign-in**, or here
   on **People and codes** with their own keypad code. The user name is the person's name on
   Users, the installer's too (**Installer** from the factory). A password set for
   somebody else is temporary: they choose their own at their first sign-in. Passwords are 12
   to 64 characters, stored salted and hashed (PBKDF2-HMAC-SHA256) and never shown. After five
   wrong sign-ins, sign-in waits a minute (doubling to 15 minutes). A keypad code (anything
   under 12 characters) is refused with directions and does not count as a wrong sign-in (B010).
4. **Remote keypad** shows the panel's screen, H.264 where the browser decodes it (WebCodecs,
   on an HTTPS page), JPEG otherwise or when you choose **Still pictures (JPEG)** above the
   screen (remembered in that browser), a new picture only when the screen changes. Being on the
   page is control: click to press, hold for a long press (Panic), drag to scroll; type on the
   computer's keyboard when the panel shows a text box or a code pad. A touch at the panel's own
   screen wins for a few seconds. While you are there the panel shows a red banner; the person at
   the panel can tap it to pause you, then allow you or turn remote access off. A pause lasts
   until they answer: reconnecting doesn't undo it. A tab in the background stops the pictures
   and, after a minute, lets the panel go.
5. **Integrations** shows only modules included in this firmware. Compiled-in but disabled
   services remain configurable. Blank secret fields keep saved credentials; explicit clear
   controls remove them. Review screens never display the entered secret.
6. Network changes require **Keep the new settings** within two minutes. Reconnect at the new
   address first. An unconfirmed change, including a reboot, restores the previous profile.

## Remote development

Enable **Developer mode** in local Installer. It requires Web server and persists until
turned off. Disabling Web server closes both services, so local access is needed to re-enable.

With Developer mode on, **Software updates** gains "Install a signed file", and the
Developer page shows bounded service logs and workstation enrollment. The upload takes a signed
`.bin` for the current board/profile. Upload checks digest and
signature, stages the image, then reconnects to its expected ELF identity during probation.
Keep that browser open until health checks finish. A failed startup or missing reconnect
rolls back. It does not compile firmware: the development workstation still builds source.

Workstation tools are optional for household operation and direct browser flashing. To enroll
a new remote development workstation without USB:

```powershell
python tools/panelctl.py prepare-enrollment --name bench-2
```

Paste the printed **public certificate file's contents** into **Developer > Enroll a workstation**.
The private key stays on the workstation. Download the panel's enrollment receipt, then:

```powershell
python tools/panelctl.py --host <panel-IP> import-enrollment --name bench-2 --file <receipt.json>
python tools/panelctl.py info --device bench-2
python tools/panelctl.py flash --device bench-2 --build --allow-older
```

Enrollment currently replaces the single enrolled development workstation for that panel.
One workstation can hold many independently pinned panel identities. USB enrollment, explicit
IP/VPN selection and bounded network discovery remain in `tools/panelctl.py`. Browser session
limits are four sessions, 20 minutes idle and eight hours absolute. Named helper accounts,
passkeys and off-site hardware commissioning remain future work.

## Configuration coverage and validation

See [Feature 31](../../../../docs/features/31-Browser-Panel-Manager/README.md) for the complete configuration
inventory, authority rules, measured hardware acceptance and remaining commissioning limits.
The build requires `CONFIG_CJSON_NESTING_LIMIT=12`, `CONFIG_LWIP_MAX_SOCKETS=24` and a
2048-byte HTTP header limit. Keep the existing display safety flags enabled.

The scripts in `tools/tests/panel_web_*_hil.py` are explicit device acceptance tools and
require an out-of-band certificate fingerprint. `panel_web_browser_test.py` is a temporary
read-only loopback test transport for automated browser inspection when browser certificate
trust must be handled by the owner. It is not shipped and is never required by the panel.
