# The panel's UI mockup

A mockup of the 7" touchscreen as it is today, and the place where every change to it is drawn
and agreed **before** anyone builds it. Changes made straight in the XML and C, with nobody
looking until they were on a panel, have cost usability: a proposal here costs minutes, can be
reviewed on a phone, and turns "what do you think?" into a picture to point at.

It is HTML built from the panel's own design system, not a drawing: the colours, spacing, radii,
layout sizes and line heights come from the layout's `globals.xml` and fonts, the fonts are the
panel's Inter and Lucide files, and every part is named after its LVGL component. A screen in
the mockup and the PC preview's picture of the same state can be laid over each other, and
`compare` scores how close they are.

## The rule
Anything that changes what a person sees or does on the touchscreen - a new screen, page, row or
control, a moved one, a changed flow, new or changed words - starts as a proposal here and is
built only once the person you work for has approved it. Fixing the screen back to what the
mockup already shows (a bug) needs no proposal. This is requirement UI-044. The Panel Manager
(the panel's web pages) has its own mockup, docs/features/31-Browser-Panel-Manager/mockup (WEB-026).

## Working on a change
1. **See today's screen.** Find it in the viewer (build below) and in `screens/`. If the state
   you are changing has no screen yet (the viewer's Coverage tab), add one to `screens/` first,
   copied from a close one and checked against the preview's picture.
2. **Draw the proposal.** Copy `proposals/_template.js` to `proposals/BL-NNN-short-name.js` (one
   file per backlog row). Each screen it changes is an `M.screen()` with `proposal: {bl, status:
   'draft', replaces, why}`; wrap what changed in `M.changed(...)`. Show every state the change
   has (empty, full, an error, while armed), not only the happy one.
3. **Build and check:** `python tools/ui_mockup.py build --shots` and `python tools/ui_mockup.py
   check`.
4. **Show it to the person you work for**, with the screen's link (`#<id>`) and a sentence on
   what to look at:
   - in a browser on the PC: serve `build/mockups/panel` (`python -m http.server -d
     build/mockups/panel 8544`) or open its `index.html` from the folder;
   - or copied to any static web host (the built `index.html`, with `shots/` beside it for the
     compare modes), which they can open on a phone.
5. **Record the decision** in the proposal: `status: 'approved'` or `'rejected'`, and `decided:
   '2026-10-01: what they said'`. Change the drawing until it is approved; their words in the
   chat are the approval, the file is the record. The backlog row links the proposal file.
6. **Build it** in the layout's XML and the C code (components/ui/AGENTS.md), then lay the PC
   preview's new pictures over the proposal (Side by side, Difference).
7. **Make it today's:** move the screens into `screens/` (replacing the ones they replaced, with
   `shot:` naming the new preview pictures), delete the proposal file, and run `compare` on them.
   The mockup now shows the panel as it is again.

A UI change that lands without a proposal (a fix, a word) still updates `screens/` in the same
change, so today's mockup stays true.

## Rules for the files
- **Only the kit's parts.** Screens compose the components in `kit/kit.js`; a part the kit lacks
  is added there first, named as its XML component will be (`M.arm_tile` = `components/home/arm_tile`,
  `M.row_toggle` = `ui_row_toggle()`), with its numbers in `kit/panel.css` taken from its XML.
- **Only the design system's values.** Colours are tones (`green`, `amber`, `blue`, `violet`,
  `rose`, `orange`, `red`) or tokens (`text_2`, `var(--color_line)`); spaces and radii are tokens
  (`var(--space_md)`); text is one of the panel's fonts (`M.text(s, 'label')`). `check` refuses a
  colour of a screen's own. A new token is a change to `globals.xml`, so it is part of the
  proposal.
- **Icons are Lucide names** (`kit/lucide-codepoints.json`, the Lucide version the panel's font
  comes from). The viewer flags an icon that isn't in the panel's icon fonts yet: using it means
  adding its codepoint to the ranges in `globals.xml` when the change is built.
- **The panel's words**, in English as the screen shows them, in the wording rules of
  docs/features/38. Words the C code puts together are written as they come out.
- **Nothing moves or blinks** (AGENTS.md "Display safety"). The mockup is still pictures; a
  proposal that animates says so in words and is reviewed against UI-020.
- **Fictional data only**, as everywhere in the repository: Harbor House, Alex, Pat and Sam, RFC
  5737 addresses.
- **What the code limits** still applies: the 127-byte string subjects, labels that end in "..."
  at a width, the rail's places (four, five with Smart Home), the 1024 x 600 screen. A proposal that needs more says so.

## Files
| Path | What |
|---|---|
| `kit/panel.css` | the components' CSS, one block per XML component, numbers from the XML |
| `kit/kit.js` | the components as functions (`M.hero`, `M.code_pad`, `M.row_link` ...) and the screens' frames (`M.main`, `M.settings`, `M.wizard`, `M.saver`) |
| `kit/lucide-codepoints.json` | Lucide's names and codepoints (lucide-static 1.47.0, ISC) |
| `screens/*.js` | today's screens, by area |
| `proposals/*.js` | changes under discussion; `_template.js` starts one |
| `viewer/` | the page around the screens: list, compare modes, gallery, coverage |
| `tools/ui_mockup.py` | builds, checks and compares |

## Commands
```
python tools/ui_mockup.py build --shots    # -> build/mockups/panel/index.html (+ shots/)
python tools/ui_mockup.py check            # what CI runs after the PC preview
python tools/ui_mockup.py compare [ids]    # each screen against its preview picture
```
On Windows without `python` on PATH, the ESP-IDF venv's
(`C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe`). `--shots` and `compare` need the
PC preview's pictures (`pwsh tools/ui_preview.ps1`); `compare` needs Chrome or Edge, and
`--keep <folder>` keeps the mockup's pictures. `<page>?bare#<id>` shows one screen alone at
1024 x 600, for a screenshot.

`compare` counts the pixels where any colour channel differs by more than 48 of 255. Text drawn
by a browser and by LVGL never matches exactly, so about 3 % is the floor; 3-6 % is the same
screen, and a screen above about 7 % shows a different state (a page scrolled further, a live
value) or a real difference worth a look in the viewer's Difference mode.

## Limits
- Pictures, not behaviour: what a tap does is the `data-go` links between screens, nothing more.
  Timing, sounds and the ring's glow are words in a screen's notes.
- English only. How long words get in other languages is the PC preview's en-XA walk
  (docs/features/37).
- The 7" layout only; the 4" (480 x 320) is on hold (docs/features/19).
