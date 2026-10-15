# ui - one LVGL Pro project per screen size

The touch UI's behaviour lives once, in `components/ui/alarm_ui` (the C logic). Each screen
size has its own LVGL Pro project here: the composition, sizes and fonts that suit that screen,
built around the same contract, so the same logic runs on every board (ADR-036).

| Layout | Board | Composition |
|---|---|---|
| [`1024x600/alarm_ui`](1024x600/alarm_ui) | CrowPanel Advanced 7" (docs/features/06) | rail with logo and six entries, status bar, Home = state card beside the arming tiles or the code pad, Settings = section list beside the page |
| [`480x320/alarm_ui`](480x320/alarm_ui) | LCDWiki E32R40T 4", resistive (docs/features/19) | 72 px rail (logo, Home, Zones, Activity, Troubles, Panic), status bar with Chime, Home = state card and arming tiles, the code pad (80 x 58 keys) filling the view with the state and seconds left in the status bar, Settings = icon tabs above the page, sheets full-screen |

A board's manifest names its layout (`CONFIG_ALARM_UI_LAYOUT` in
`firmware/<board>/sdkconfig.defaults`); its firmware and its PC preview and simulator build that
layout.

## The contract

`python tools/check_ui_contract.py` (CI) checks that every layout offers what the C code uses:

- the same subjects with the same types, and the same components with the same API props
  in the same order (the UI code calls the generated `<name>_create()` functions);
- one copy of the hand-written part: `alarm_ui.c` / `alarm_ui.h` are identical in every
  layout, and every `event_cb` callback the XML names is one they define;
- the constants, fonts and styles the UI code names (`SPACE_SM`, `font_label`,
  `style_segment_active`, the toast's `TOAST_X` / `TOAST_Y` / `TOAST_KB_Y`...);
- every object the UI code or the PC preview looks up by name (`pad_hint`, `zone_list`,
  `set_users`...; `--list` prints them all);
- a display the size its folder says.

The preview (`pwsh tools/ui_preview.ps1 -Board <board>`) is the other half: the same scripted
walk runs on every layout (the pages the board has) and fails when something can't be reached,
overlaps or is in the wrong order - it presses `panel_arm`'s children as Away, Stay, Night and
`pad_keys`' as 1-9, delete, 0, enter.

Everything else is the layout's own. When a new screen needs the code to place something, give
the layouts a constant for it (as the toast's position) rather than a number in the C code.

## Working on a layout

- The XML is the source: never hand-edit `*_gen.c/.h`, `file_list_gen.cmake` or
  `fonts/*_data.c`. After editing: `python tools/lvpro_gen.py components/ui/alarm_ui/layouts/<WxH>/alarm_ui` (`--fonts`
  after font changes; see `tools/lvpro_gen.py` for lv_font_conv), then `--check`.
- Each project is self-contained so the LVGL Pro Editor can open it: the TTFs are copied into
  every project's `fonts/` (Git stores identical files once).
- Icon fonts (`icons_*`) are generated RLE-compressed and text fonts plain (`font_compressed()`
  in `tools/lvpro_gen.py`, 2026-09-25): the icons were most of the fonts' flash (the 96 px set
  79 KB, now 23 KB) and are drawn a few glyphs at a time; the pictures are identical. The two
  big display fonts, `font_headline` and `font_digits`, are compressed too (2026-09-30,
  docs/features/39 F2: 64 KB less on the 7"), as they are drawn only when what they show
  changes. Every build needs `LV_USE_FONT_COMPRESSED` (the firmware's `sdkconfig.defaults`, and
  `host/ui_preview/lv_conf.h`), or those fonts draw nothing.
- Look at the preview's screenshots (`build/ui_preview[-<board>]/shots`) after every change,
  and keep docs/features/07 UI-020 (nothing flashes) and, on resistive boards, docs/features/19 CYD-010
  (targets of 56 px, 48 px at least; the 4" lists still scroll by dragging - paged lists are
  still to do) in mind.
