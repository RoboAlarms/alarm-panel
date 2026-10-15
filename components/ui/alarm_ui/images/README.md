# UI images

## RoboAlarms rail mark

`roboalarms_mark.pgm` is the approved RoboAlarms logo's 44x44 alpha mask (P5 grayscale:
0 transparent, 255 opaque). It derives from the initial logo in `src/assets/brand/` (RoboAlarms/documentation)
(Feature 32): crop bounds x=200..1053, y=163..1110, fit inside 44x44 with transparent
padding and Lanczos3 resampling. The document master remains unchanged. This small mask
keeps the original silhouette and smooth edges; its RGB colour comes from the UI state.

Run `python tools/embed_logo.py` to generate `roboalarms_mark_a8.c`; `--check` verifies
it in CI using only Python's standard library. The LVGL A8 resource uses 1,936 bytes
of constant pixel data, with no PNG decoder or runtime conversion. The rail's XML binds
`image_recolor` to `subject_tone_color`; `ui_app.c` sets the embedded source at startup.
The existing 56x56 rail container and its spacing stay unchanged.

## Screen saver pictures

Pictures the touch UI draws that are not icons or text. They are **not** part of the LVGL Pro
project (`components/ui/alarm_ui/layouts/1024x600/alarm_ui`): the Editor only knows bitmap
formats, while these stay JPEGs and are decoded once at runtime with LVGL's TJPGD
(`src/ui_saver.c`). On the panel the saver's JPEG is not in the firmware image: the build makes the picture pack
(`saver.roboasset`) from it and the saver asks the backend for the part `photo`; the PC
preview and simulator embed the `_jpg.c` in `host/ui_preview/host_panel.c`.

| File | Use | Source |
|---|---|---|
| `saver_forest.jpg` | Screen saver background (1024x600) | Generated with Google's AI image generator (2026-09-20) for this project; converted from WebP to a baseline JPEG, quality 85 |

To change or add one: save it as a **baseline** JPEG at the screen's size, then run

```
python tools/embed_image.py components/ui/alarm_ui/images/<name>.jpg
```

which writes `<name>_jpg.c` (never edit it by hand; CI runs the tool with `--check`). Keep it
dark and calm: the screen saver runs at low backlight, and nothing on screen may flash or move
quickly (docs/features/07 UI-020).
