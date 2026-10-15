# B008 - Installer mode showed an empty state card on Home

**Status: fixed in code (2026-09-23) and checked in the preview (2026-10-05); found by reading the
code, not seen on a panel, where only another keypad can show it.** Found while building the 4" layout (docs/features/19 Phase 3), whose icon fonts are cut
down to exactly the glyphs the code draws large.

## Symptom (expected, not observed)
While the installer is in installer mode, Home's state card (the hero) shows an empty tinted
disk where the state's icon belongs; the words ("Installer mode") are there.

## Cause
The hero draws `subject_hero_icon` with the 96 px icon font (`icons_96` in
`components/ui/alarm_ui/layouts/1024x600/alarm_ui/globals.xml`), which holds only the glyphs listed in its `range`. Two state
icons the code sets (`components/ui/alarm_ui/src/ui_app.c`, the hero's switch) were missing:

- U+E158 `UI_ICON_SHIELD`, installer mode (`MODE_PROGRAM`): drawn, so the disk was empty;
- U+E3D6 `UI_ICON_DOOR_OPEN`, entry time: the hero shows the seconds left instead, so it never
  showed there (the screen saver draws it at 40 px, which had it).

The preview never photographed Home in installer mode, so no screenshot caught it. Home is in
installer mode only while another keypad holds it: this keypad ends it when Settings is left, and
the Panel Manager enters and leaves it inside one change.

## Fix
Both glyphs added to `icons_96`'s range and the font regenerated (`lvpro_gen.py --fonts`;
only `icons_96_data.c` changed). The 4" layout's large icon sets are built from the full list
of state and setup icons (docs/features/19). The 7" preview's 118 screenshots are unchanged.

## Verify
On the PC (2026-10-05): the preview's walk enters installer mode at a second keypad
(`host_panel_keypad2_program`) and photographs Home (`home_installer_mode_elsewhere`, appended so
no other picture is renumbered); its glyph check (OPT-005) fails the walk if the shield is missing
from the 96 px font. It also checks that the banner's tap says the installer is at another keypad
and leaves their mode alone. On a panel this needs a second keypad (M7): enter installer mode
there and look at this one's Home, where the blue disk shows a shield.
