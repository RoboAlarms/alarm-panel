# Images for the front page

The pictures the repository's [README](../../README.md) shows. Refresh them when the screens
they show change, and keep the factory-code banner out of them.

| File | What it is | Where it comes from |
|---|---|---|
| `roboalarms-mark.png` | The logo | The brand master in [RoboAlarms/documentation](https://github.com/RoboAlarms/documentation) ([BRAND.md](../BRAND.md)) |
| `hero.svg` | The home screen in a panel, and the Panel Manager on a phone | The two pictures below, on a dark teal card, embedded as data URIs (GitHub shows an SVG as an image and loads nothing from outside it); no text in the picture |
| `panel-zones.png`, `panel-arming.png`, `panel-smart-home.png` | Screens of the 7" panel | The PC preview |

How the current set was made (2026-10-14):
- **The panel's screens:** the PC preview's walk (`tools/ui_preview.ps1`), with both factory codes
  changed through the engine before the first shot and a battery at 82 %, so the status bar shows
  no factory-code banner: `01_home_ready` (in the hero), `14_zones`, `06_exit_delay` and
  `17_devices`.
- **The phone:** the Panel Manager mockup (`node docs/features/31-Browser-Panel-Manager/mockup/build.cjs`),
  `app.html?role=master&theme=dark`, signed in as the system master so the Overview shows Away,
  Stay and Night; captured in headless Chrome with a phone's emulation at 390 × 717 (it ends
  under "Plugged in"), twice the pixels, and its clock set to the panel's (Saturday, September 19,
  10:41 PM). The page's "Overview" heading is hidden in the picture, ahead of its planned removal,
  so the first card sits under the menu bar.
