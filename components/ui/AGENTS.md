# Rules for the UI (`components/ui/alarm_ui/layouts/<WxH>/alarm_ui`, `components/ui/alarm_ui`)

Rules for the touch UI and every word a person reads on the panel or in the Panel Manager. Claude
Code loads this file by itself when it reads a file here (through `CLAUDE.md` beside it); other
agents are sent here by the root [AGENTS.md](../../AGENTS.md) "Before you edit". Older notes cite it
as CONTRIBUTING.md "Rules for the UI". Display safety (nothing flashes) is in the root AGENTS.md.

- Mockup first (UI-044): a new or changed screen, row, control, flow or wording is drawn as a
  proposal in `docs/features/07-UI-LVGL-Pro/mockup` and approved by the person you work for
  before the XML or C changes; when it lands, the mockup's screens change with it
  (its README). The Panel Manager's pages the same way, in `docs/features/31-Browser-Panel-Manager/mockup`
  (WEB-026).
- The XML in each `components/ui/alarm_ui/layouts/<WxH>/alarm_ui` is the source. Never hand-edit `*_gen.c/.h`, `file_list_gen.cmake` or
  `fonts/*_data.c`: run `python tools/lvpro_gen.py components/ui/alarm_ui/layouts/<WxH>/alarm_ui` (`--fonts` after font changes).
  CI fails when they don't match the XML. `alarm_ui.c/.h` are hand-written, one copy in every layout.
  The generator keeps an object's `name` only where a string literal in the hand-written code
  (`components/ui/alarm_ui`, `host/ui_preview`, `host/ui_sim`) holds that word: a lookup built
  from pieces (`"zone_%d"`, a name read from data) must appear as a literal somewhere, or
  `ui_find` returns NULL on the panel (`lvpro_gen.py` `code_words`, docs/features/39 F10).
  Icon fonts (`icons_*`) and the big display fonts (`font_headline`, `font_digits`) come out RLE-compressed
  (lossless, `components/ui/alarm_ui/layouts/README.md`): every firmware and PC build needs
  `LV_USE_FONT_COMPRESSED`, or they draw nothing.
- One UI logic, one layout per screen size (ADR-036): every layout keeps the same subjects,
  component APIs and the object names the code looks up (`python tools/check_ui_contract.py`,
  CI; `components/ui/alarm_ui/layouts/README.md`). A change to a subject, a component's props or a looked-up name goes into
  every layout at once (while the 4" is on hold, CI checks only the 7"; see CONTRIBUTING.md "Build and test").
  Geometry the code needs comes from the layout's constants (the toast's
  `toast_x`/`toast_y`/`toast_kb_y`), never a number in C. Check each layout's preview
  (`pwsh tools/ui_preview.ps1 -Board e32r40t` for the 4").
- Colours bound with `bind_style` must not also be set as local (`style_*` attribute) styles:
  local styles always win.
- `components/ui/alarm_ui/src` stays portable (LVGL, alarm_core, event_log): the platform is
  reached only through `ui_backend_t`. Modules: `ui_app.c` (Home, pad, lists, screen saver),
  `ui_forms.c` (rows, keyboard, sheets), `ui_settings.c`, `ui_installer.c`, `ui_sources.c`
  (where a zone's sensor is: source picker, terminals, trust facts), `ui_net.c`,
  `ui_integrations.c`, `ui_notify.c` (ntfy and Pushover pages), `ui_storage.c` (About > SD card),
  `ui_lang.c` (the language rows and Clock of Time and language, the setup's language),
  `ui_zwave.c` (Installer > Z-Wave, only with `ALARM_UI_ZWAVE`),
  `ui_devices.c` (Smart Home: the rail's item, the devices by area, their tiles and actions,
  docs/features/24; only with `ALARM_UI_DEVICES`),
  `ui_walk.c` (the walk test: Settings > Walk test, Home's card while one runs, its status-bar
  banner and summary, docs/features/40),
  `ui_voice.c` (a zone's Spoken name row and editor, the voice's words on Screen and sound >
  Announcements, docs/features/08 voice-first-build.md; hidden without a voice),
  `ui_update.c` (About > Software update), `ui_saver.c` (screen saver
  picture), `ui_theme.c` (the appearance: switches the layout's themes.xml palettes
  at run time), `ui_wizard.c` (the setup; with the panel link its role and a keypad's pairing steps),
  `ui_link.c` (the panel link, docs/features/11: Installer > Keypads and devices' keypads on Wi-Fi, a
  keypad's Home, Emergency words, About and stand-alone page; nothing without `ui_backend_t.link_*`),
  `ui_calls.c` (the calls that wait for another panel: a keypad's master, LINK-054),
  `ui_tz.c`, `ui_text.c`, `ui_status.c` (the status screen a board shows
  when the full UI doesn't fit in its RAM); shared state in `ui_internal.h`. User-facing
  strings live in these modules, **except the event sentences** ("Burglary alarm" / "Front Door",
  "Armed Away" / "By Master"), whose single copy is `components/core/notify/src/notify_words.c`: the
  Activity list and a push notification to a phone read the same words, so they can't drift
  apart (docs/features/26). `ui_text.c` keeps only what is about this screen - the tones, the icon
  table and the zone card - and wraps the rest. Change a sentence there, not here, and prove it
  with the preview.
- Every word the screen shows goes through the catalog (docs/features/37): `TR("Saved")`,
  `TR_C("zone state", "Open")` for a word with two meanings, `TRN("{0} zone", "{0} zones", n)` for
  a count, `TR_NOOP` in a static table; the rules are in `ui_internal.h` ("words on the
  screen"). Whole sentences only, never glued pieces; values go in through
  `I18N_FORMAT(buf, len, TR("Entry through {0}"), name)`, never a translated `printf` format; a
  `/* TR: ... */` note says where a text shows and, where it lands in a fixed buffer, its byte
  budget. Household names are values, never marked; text deliberately fixed is `NOTR("...")`.
  Literal text in the layouts' XML goes through `alarm_ui_tr()` by itself (`lvpro_gen.py`); an
  ambiguous one takes a context from a `<!-- tr-context: ... -->` comment before its element.
  The Panel Manager's words go the same way through `web/panel-words.js` (`t()`, `ht()` inside
  HTML, `tn()`, `tc()`, `nt()` for fixed text; a table of words that can hold a name is a function,
  read when the page is drawn; `panel.html` marks with `data-t`). After changing words run `python tools/i18n.py extract`, then `python tools/i18n.py build` (the template's fingerprint in `catalogs/i18n_template_gen.c` follows it; CI's `build --check` fails otherwise) (extract refreshes
  `components/core/i18n/po/messages.pot`) and `python tools/i18n.py check`, which fails on an
  unmarked word in any module listed in `po/MARKED` (CI runs both). The English on screen must
  not change when strings are wrapped: compare the preview's pictures before and after.
- Pictures (not icons) live in `components/ui/alarm_ui/images` as baseline JPEGs, embedded with
  `python tools/embed_image.py <jpg>` (never edit the generated `*_jpg.c`) and decoded once at
  runtime; they stay out of the LVGL Pro XML (set from C), see `images/README.md`.
- Who may open a Settings page follows docs/features/14 "Who can change what": check it with
  `ui_settings_allowed(ACCESS_...)` and show `ui_settings_locked()` otherwise. Things a homeowner
  changes with the home (Wi-Fi, time, integrations, software updates) must not need installer mode.
- String subjects hold at most 127 bytes (`UI_SUBJECT_STRING_LENGTH=128`): keep notes, dialog
  texts and setup explanations shorter. The sheets' note is the one exception, 191 bytes
  (`UI_SHEET_NOTE_LENGTH`, `ui_sheet_note_init`), so a confirmation can join two sentences of up
  to 95 bytes each.
- General screens never mention a particular integration (Home Assistant): only Settings >
  Integrations, the integration's own page, and the source picker and "Source" row of a zone
  (`ui_sources.c`) or device that uses it (docs/features/07 UI-018).
- Live words on screen (the terminal picker, "Right now") change at most once a second, and
  only when the input settled (debounced): nothing may flicker (UI-020, UI-023).
- No flow may require a physical button on the device (the boards sit behind a case).
- End-user screens (the touchscreen and the Panel Manager) show what the panel can do, never the
  hardware behind it: a clock row that opens "Set the date and time" when a clock module
  answers, not "Clock module". Module names, wiring and diagnostics belong to the console, the
  Flasher and the docs.
