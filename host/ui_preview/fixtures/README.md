# Preview fixtures

- `font_greek_body.bin`: a language pack's font for the body text (docs/features/37 I18N-049), as
  `i18n.py pack --font body=<file>` puts it in a pack. Inter Regular at the body font's size and
  depth (19 px, 4 bpp, `components/ui/alarm_ui/layouts/1024x600/alarm_ui/globals.xml`), the Greek
  letters U+0391-U+03A9 and U+03AC-U+03CE (lower case, with the accented ones). Made in
  `components/ui/alarm_ui/layouts/1024x600/alarm_ui` with
  `lv_font_conv --font fonts/Inter-Regular.ttf --size 19 --bpp 4 --format bin --no-compress --range 0x391-0x3A9,0x3AC-0x3CE -o <this file>`.
  The preview's pretend `el-GR` language hands it over (`host_panel_lang_world()`).
- `font_test_headline.bin`: the test languages' own headline font (I18N-049). The image's headline
  font holds only the letters the shipped catalogs use and the letters pack has no headline part
  (docs/features/39 asset-packs.md), so the made-up accents of `en-XA` and `ar-XB` would draw as
  boxes, wider than letters, and the walk would measure those. InterDisplay SemiBold at the
  headline's size and depth (42 px, 4 bpp), the letters of U+00A0-U+017F the headline lacks. Made
  in `components/ui/alarm_ui/layouts/1024x600/alarm_ui` with
  `lv_font_conv --font fonts/InterDisplay-SemiBold.ttf --size 42 --bpp 4 --format bin --no-compress --range 0xA0-0xAA,0xAC-0xAF,0xB1-0xB6,0xB8-0xBA,0xBC-0xBE,0xC0,0xC2-0xD9,0xDB-0xE0,0xE2,0xE4-0xE8,0xEA-0xEC,0xEE-0xF0,0xF2,0xF4-0xF9,0xFB,0xFD-0x17F -o <this file>`.
  The preview hands it over for any `-XA` or `-XB` language.
