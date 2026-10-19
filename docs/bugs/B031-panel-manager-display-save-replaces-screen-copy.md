# B031: the Panel Manager's display save replaces the screen's copy

**Status: open. Found by reading the code on 2026-10-16 (BL-216's design, docs/features/11
"Smart Home and Home Assistant on keypads"); not yet tried on a panel.** Backlog: BL-218.

## Symptom
Expected from the code, not seen yet. After the Panel Manager saves the display settings, the
touchscreen can lose a change or undo the Panel Manager's:
- a change made on the screen in the last 1.5 s, not saved yet, is dropped;
- when the screen's task is busy for more than 100 ms, the screen keeps its old settings, and its
  next save writes them back over the Panel Manager's;
- a screen that shows a partition other than 1 goes back to partition 1.

The developer link's Voice volume (development builds) takes the same path.

## Evidence
Found by reading; nothing was captured from a panel.
- `preferences_changed()` (`firmware/crowpanel_p4/main/dev_adapter.c:349-357`) builds a whole
  `ui_options_t` with `ui_port_esp_options()`, then, only if `board_ui_lock(100)` succeeds, hands
  it to `ui_app_apply_options()`. Nothing retries when the lock times out.
- It runs after the Panel Manager's `config/display` save (`panel_web.c:2485`) and after the
  developer link's `POST /dev/v1/voice` (`dev_remote.c:824`).
- `ui_port_esp_options()` (`ui_port_esp.c:1703-1730`) starts from `ui_options_defaults()`, whose
  partition is 1 (`ui_backend.h:2025`), and fills in what `panel_prefs` stores.
- `ui_app_apply_options()` (`ui_app.c:742-757`) replaces the screen's whole copy (`S.opt`). It
  keeps the old partition only when the new one is out of range, and 1 is in range.
- The screen saves its own changes 1.5 s after the last one (`SAVE_AFTER_MS`, `s_opts_dirty`,
  `ui_settings.c:17-19`, `:244`), and its save writes every field (`save_options`,
  `ui_port_esp.c:509-555`).

## Cause
`preferences_changed` replaces the screen's copy with what is stored, instead of changing only
the fields that were saved elsewhere:
1. **An unsaved change.** `S.opt` is replaced while a change made on the screen waits for its
   save, so the change is gone and the save that follows writes the stored values.
2. **A busy screen.** When `board_ui_lock(100)` fails, the screen keeps its old copy. Its next
   save, after any change on the screen, writes the whole old copy back, the Panel Manager's
   change included.
3. **The partition.** The new copy's partition is the default 1. On a master the screen shows
   partition 1 today; a link keypad's screen shows its home partition (`ui_link.c:1362`,
   `panel_app.c:1592`). Whether this callback can run on a keypad was not checked.

## Fix
Not started. Decided 2026-10-16 (the owner's decision 12 in docs/features/11 Decisions): once
BL-216 builds the settings mailbox (`panel_prefs_remote_submit`, docs/features/07 UI-046),
`preferences_changed` submits the saved fields as a patch to it. The screen's task then
merges only those fields into its own copy and keeps its partition and language; UI-046's rules
decide what happens while a change on the screen is unsaved.

## Verify
On a panel, disarmed: change Brightness on the screen and, within 1.5 s, save the display
settings in the Panel Manager. Afterwards the screen must show what is stored, and the screen's
save must not undo the Panel Manager's change. A busy screen is hard to make by hand: a host test
with the UI lock held past 100 ms, then a change on the screen, must keep the Panel Manager's
change.
