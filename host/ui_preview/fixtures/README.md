# Preview fixtures

- `font_greek_body.bin`: a language pack's font for the body text (docs/features/37 I18N-049), as
  `i18n.py pack --font body=<file>` puts it in a pack. Inter Regular at the body font's size and
  depth (19 px, 4 bpp, `components/ui/alarm_ui/layouts/1024x600/alarm_ui/globals.xml`), the Greek
  letters U+0391-U+03A9 and U+03AC-U+03CE (lower case, with the accented ones). Made in
  `components/ui/alarm_ui/layouts/1024x600/alarm_ui` with
  `lv_font_conv --font fonts/Inter-Regular.ttf --size 19 --bpp 4 --format bin --no-compress --range 0x391-0x3A9,0x3AC-0x3CE -o <this file>`.
  The preview's pretend `el-GR` language hands it over (`host_panel_lang_world()`).
