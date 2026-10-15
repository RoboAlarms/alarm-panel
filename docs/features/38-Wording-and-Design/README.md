# 38 · Wording and design

**Status:** Future, **not well defined yet** - the scope, the order and how the rules are checked
are still to be decided; the Panel Manager's Overview is built to the mockup below (BL-113); Left:
the rest (BL-114) · **Milestone:** none
yet · **Depends on:** 07, 31, 34, 37

## Goal
Every screen says what it needs to in as few words as work, and says it the same way everywhere,
the touchscreen, the Panel Manager and the Flasher, with one set of colours that mean the same
thing on each. People glance at an alarm panel, often in a hurry; a line nobody reads protects
nobody. Today many screens explain in sentences what a label, an icon or the button already says.
The screens are too wordy, and the fixes should reach all three; the Overview came first
(BL-113, docs/features/31).

## Reference behaviour
A keypad alarm panel uses a small, fixed vocabulary: Away, Stay (Home on newer models), Night,
Instant or No entry delay, Silent exit, Bypass, Chime, Disarm. Summaries of keypad user guides (not
the printed guides themselves; to be checked before any text is copied) report short trouble texts
(Check, Low bat, Comm fail), and ALARM CANCELED shown only when a disarm stopped the report, the
alarm otherwise staying on display (alarm memory) until a second disarm clears it. We keep the
keypad's words where they explain themselves, and share one catalog of words across the
touchscreen and the Panel Manager (docs/features/37).

## Research (2026-09-29)
Two rounds of web research for the Overview, summarised in our own words. Its limits:
- ISA-101 and SIA CP-01 are paywalled and come from summaries.
- "Alarm occurred" is the research's own wording.

### Round 1: layout and wording
1. **How arming is worded.** The common practice, which we follow:
   - A mode's button is its name, with a few words under it or none.
   - Options are two- or three-word switches ("No entry delay", "Silent exit", "Bypass open
     zones"), shown only when they apply.
   - Bypass is offered only when something is open.
   - Countdowns are a big number.
   - Longer explanations live in help pages, not on the screen.
2. **UX-writing guidance** (Nielsen Norman Group, Microsoft, GOV.UK, W3C COGA):
   - People scan and skip text, so show two levels only: a glance level, and one tap deeper.
   - Never repeat the headline in the line under it.
   - Buttons are verbs that say what happens; NN/g suggests commands of two to four words.
   - No tooltips for anything needed to decide.

   From that guidance the research proposed word budgets: headline 1-4, a line of about 8,
   buttons 1-3. It also proposed numbers as numerals and one time format.
3. **Alarm-specific presentation** (a CS-V-01 verification overview, alarm-fatigue design notes):
   - An alarm card answers what, where and when, then offers one action, then lists what tripped
     in order.
   - Don't call an alarm "verified" or "confirmed": the industry's definitions vary.
   - A trouble is a name, one line and one fix.
   - Status tiles show a value and a word, and change words (not only colour) when something is
     wrong.

### Round 2: colour, a confirmation during an alarm, two terms
1. **Colour.** The arming step looked off because too many accents competed in one place: a green
   headline, rose chips, an amber notice and the Panel Manager's teal switches.
   - High-performance HMI practice (ISA-101) keeps a neutral base and saves colour for meaning,
     with red only for alarms.
   - Button-hierarchy guidance gives the one primary action the only solid fill; secondary actions
     are outlined or quieter.
   - Alarm products disagree on which colour means which state (Home Assistant's historical
     defaults differ again), so there is no convention to copy; our own documented palette is the
     better guide.
   - WCAG 1.4.1: colour is never the only cue. WCAG 1.4.11: 3:1 for control edges and meaningful
     icons.
   - Recommended: the step neutral with one accent, the level's tone. The card's headline keeps
     the true state's colour. One solid button in the level's tone, as the touchscreen's confirm
     key. Disarm green. Red only for alarms and destructive actions.
   - Runner-up: one brand colour for every primary button. It lost because it breaks the match
     with the touchscreen and adds one more accent.
2. **A confirmation when disarming during an alarm.**
   - No mainstream alarm app was found adding a dialog to a remote disarm. Where a hold-to-confirm
     exists, it is for the choices that change what the monitoring station does, not for
     disarming.
   - SIA CP-01 rewards speed: a disarm within the abort window stops a burglary report (typically
     15-45 s, set per panel, from panel manuals). After it, a cancel report goes out. A dialog
     spends those seconds, and delays the person who most needs the siren to stop.
   - NN/g: confirmations used on routine, reversible actions stop being read.
   - Recommended: no dialog in an alarm or entry delay.
   - For a normal disarm the two rounds differ. Round 1 kept one confirmation; round 2 would drop
     it too and offer a steady "Re-arm" afterwards. We keep the confirmation, as decided.
   - A browser has no duress code, and a dialog doesn't change that. Round 2 also advised that a
     browser disarm be logged as coming from the Panel Manager (not built yet; BL-113).
3. **Alarm memory's name.** "Canceled" is the report's word: per summaries of keypad user guides, a
   keypad shows ALARM CANCELED when a disarm stopped the report, and SIA CP-01's cancel window is
   about retracting one.
   - The state after an alarm is the panel's memory of an alarm that happened. Recommended:
     "Alarm occurred", with "Clear alarm".
   - Use "Alarm canceled" only when the disarm fell inside the abort window.
   - "Alarm stopped" lost because it hides that an alarm happened.
4. **"Instant" or "No entry delay".**
   - Alarm apps mostly say "No entry delay", or show an "Entry delay" switch that is turned off;
     some keypad guides still say Instant.
   - "Instant" can be read as no *exit* delay, and Instant vs Maximum needs installer knowledge.
   - Recommended: "No entry delay", with a few words under it. Keep Instant and Maximum as the
     installer's synonyms in the documents.

### Sources
Round 1:
[NN/g, how users read on the web](https://www.nngroup.com/articles/how-users-read-on-the-web/),
[NN/g, UI copy](https://www.nngroup.com/articles/ui-copy/),
[NN/g, confirmation dialogs](https://www.nngroup.com/articles/confirmation-dialog/),
[NN/g, progressive disclosure](https://www.nngroup.com/articles/progressive-disclosure/),
[NN/g, tooltips](https://www.nngroup.com/articles/tooltip-guidelines/),
[Microsoft, simple words](https://learn.microsoft.com/en-us/style-guide/word-choice/use-simple-words-concise-sentences),
[Microsoft, confirmations](https://learn.microsoft.com/en-us/windows/win32/uxguide/mess-confirm),
[GOV.UK style guide](https://guidance.publishing.service.gov.uk/writing-to-gov-uk-standards/style-guides/a-to-z-style-guide/),
[W3C COGA](https://www.w3.org/TR/coga-usable/),
[SP&T News, CS-V-01 and enhanced call verification](https://www.sptnews.ca/3777-understanding-enhanced-call-verification/),
[Security Systems News, verified alarm definitions vary](https://www.securitysystemsnews.com/article/verified-alarm-definitions-vary),
[Invene, alert fatigue](https://www.invene.com/blog/designing-experiences-to-counter-alert-fatigue).

Round 2:
[ISA-101 colour strategy (summary)](https://industrialmonitordirect.com/blogs/knowledgebase/isa-101-high-performance-hmi-design-principles-color-strategy),
[Understanding ISA-101](https://michellealzoladesign.com/understanding-isa-101/),
[Rockwell process HMI style guide](https://literature.rockwellautomation.com/idc/groups/literature/documents/wp/proces-wp023_-en-p.pdf),
[UX Movement, primary and secondary buttons](https://uxmovement.com/buttons/visual-weight-of-primary-and-secondary-action-buttons/),
[Carbon, button usage](https://v10.carbondesignsystem.com/components/button/usage/),
[Queensland Government Design System, button](https://www.designsystem.qld.gov.au/components/button),
[Home Assistant, alarm panel state colours](https://github.com/home-assistant/core/issues/83975),
[WCAG 1.4.11 non-text contrast](https://www.w3.org/WAI/WCAG21/Understanding/non-text-contrast.html),
[WCAG 1.4.1 use of colour](https://www.w3.org/WAI/WCAG21/Understanding/use-of-color.html),
[Baymard, accidental taps](https://baymard.com/blog/handling-accidental-taps-on-touch-devices),
[SIA, CP-01-2019](https://www.securityindustry.org/industry-standards/cp-01-2019/).

## The Overview mockup (2026-09-29)
A sketch of the Panel Manager's new Overview, made before any code so it could be steered:
[`mockup/overview.template.html`](mockup/overview.template.html), built into one file that opens
on its own by `node docs/features/38-Wording-and-Design/mockup/build.cjs` (to
`build/mockups/overview/overview-mockup.html`, with the real stylesheet and icons). It runs a
pretend panel: the striped bar at its top switches the alarm state, the power, the connection and
the theme; the buttons work. Nothing in it talks to a panel.

### Decisions from the reviews
| Review | Decision |
|---|---|
| The Overview doesn't read as an alarm system's (a Clock tile always saying Set) | One card per partition with the state and Away / Stay / Night; tiles for zones, troubles, power and connection; recent activity; the clock only as a trouble; no Open remote keypad button; arming and disarming without a code once signed in |
| A single small button while arming or armed looks off | A full-width action on the card |
| Zones and Troubles should lead to pages that explain more | A Zones page (needs attention, all zones with filters) and a new Troubles page (what the panel watches) |
| Connection should open Network; Power's icon and words | Connection opens Network; Power shows the status bar's battery pictures or a plug, in plain words |
| The Away / Stay / Night icons were hard to see | The touchscreen's icon chips and colours |
| During an alarm, more than one sensor must be clear at once | A numbered "What happened" list in trip order |
| Each level has different options, as on the panel | A step per level with only its options |
| No redundant notes; confirm before leaving the house unprotected | Arming's own two steps are its confirmation; a dialog before a normal disarm; none for Cancel arming |
| Too wordy | Round 1: the word budgets |
| The colours still seemed off; the open questions | Round 2: the colours, the confirmations and the terms |
| A better name for the page? | It stays **Overview**: most people name their alarm (the partition) Home, so a page called Home would read twice |
| One partition should look like two | The page's title is always Overview, and every card carries its partition's name (Home), with one partition as with several |

### What it settled
- **Layout:** the page's title is always Overview. Each partition's card shows:
  - the partition's name (Home), small and above the state, with one partition as with several;
  - the state in 1-4 words, with one line under it, and a big countdown during exit and entry time;
  - when disarmed, the three level tiles (with the touchscreen's own words: Everyone is leaving,
    Someone is home, Going to bed) and Chime;
  - otherwise, one full-width action.

  Below the cards, four tiles (Zones, Troubles, Power, Connection) open their pages, then Recent
  activity.
- **Arming step:** a tile opens the level's step in the card ("Arm Away", with Back). It shows:
  - what that level watches and how many zones;
  - only its options: No entry delay for all, Silent exit for Away only, Bypass <zone> only when
    something blocks that level;
  - one summary line (time to leave, time to get in, zones watched);
  - the Arm button.

  Readiness is per level: an active motion sensor holds up Away only ("Ready for Stay and Night").
- **Confirmations:**
  - A dialog only before a normal disarm: "Disarm Home? / Stops watching 8 zones. Smoke, CO and
    water stay on. / [Keep armed] [Disarm]", counting the zones watched now and focusing the safe
    button.
  - None for arming (its step is the confirmation), Cancel arming, an alarm or entry delay.
- **During an alarm:**
  - "Burglary alarm / Back door, then 2 more · 10:14 PM · Siren on".
  - One green "Disarm and silence".
  - "What happened": each sensor numbered in the order it tripped, the first marked, the later
    ones as seconds after it, five rows and then Show all. It grows while the page is open.
  - After a disarm: "Alarm occurred", the same list, "Clear alarm".
- **Troubles:** every trouble the touchscreen lists (system, keypads, zones, restored ones) in its words,
  grouped Affects protection, Fix soon, Restored, each with since when and opening to what it means and
  how to fix it (a button for the page that does, or who fixes it); Acknowledge as at the touchscreen;
  "What the panel watches" as a checklist, folded while there are troubles (BL-137).
- **Colours:**
  - The step is neutral, with one accent: the level's tone.
  - One solid button per card, as the touchscreen's confirm key:
    | Button | Dark theme | Light theme |
    |---|---|---|
    | Away | #FB7185 | #E11D48 |
    | Stay | #60A5FA | #2563EB |
    | Night | #A78BFA | #7C3AED |
    | Disarm, Clear alarm | #34D399 | #047857 |
    | Words on the button | #0B0F14 (dark ink), 7.1-10:1 | white, 4.7-5.7:1 |
  - The step's switches take the level's tone when on.
  - Cancel arming is outlined. Red is the alarm card's alone.
  - Icon chips are the tone at 16 % (12 % on white).
  - Nothing moves except the countdown and its bar (once a second) and the alarm list (as sensors
    trip). Nothing flashes (CONTRIBUTING.md "Display safety").

## Inventory (2026-10-01)
`python tools/i18n.py inventory` writes `build/i18n/inventory.md` (never committed) from the
template: every English text, by surface and source file, with its context, byte budget and first
source reference. The Flasher's words are not in the catalog (BL-047), so that section is a best
effort: the visible text of `tools/flasher/roboflash/web/` found by the unmarked-text scanners
(`index.html`, `app.js`); strings the Flasher's Python sends are not listed.

Entries: Touchscreen 1567, Panel Manager 1457, Notifications 200, other 191 (3415 in the catalog;
"other" is the date and region tables, the Z-Wave quirks and the web server's own words), Flasher 646.

Findings against the draft rules (candidates; nothing is changed):
- WORD-001/002/003: the catalog doesn't say whether a text is a headline, a line, a button or a
  helper, so the word counts can't be checked from it (the open question about a check in CI);
  1661 entries have no translator note and 2390 no byte budget.
- WORD-002: buttons over 3 words or carrying a live value: "Remove the sensor, keep the zone"
  (touchscreen, `ui_sources.c`), "Stop and finish later" and "Install a signed file" (Panel
  Manager), "Forgot the code? Open without it ({0} s left)" (touchscreen, `ui_installer.c`).
- WORD-006: one act, four verbs: "Delete this user" (touchscreen, `ui_settings.c`), "Remove this
  zone" (`ui_installer.c`), "Forget this network" (`ui_net.c`), "Erase everything" (`ui_installer.c`);
  the Panel Manager has "Remove zone {0}?" and "Forget" as well.
- WORD-006: the touchscreen says "Add a user", "User deleted", "Users"; the Panel Manager says "Add
  a person", "Add this person", "People and codes".
- WORD-006: delay or time: "Entry delay" and "No entry delay" beside "Entry time 1" and "No exit
  time" (Panel Manager, `panel-pages.js`); the touchscreen has "Exit time restarts".
- WORD-006: the arming mode is "Stay", but "Away, Home and Disarm with a user's code"
  (touchscreen, `ui_zwave.c`) says Home; "Sensor lost" and "Sensor missing" sit side by side in
  `ui_sources.c`.
- WORD-006: the same sentence has two catalog keys, so two translations: "Z-Wave sensors aren't
  heard for up to 15 minutes, and arming waits until the radio is back. Keep the panel powered."
  (touchscreen, straight apostrophe) and the Panel Manager's with a curly one. Touchscreen entries
  use the straight apostrophe (182) and the Panel Manager's the curly (78); one rule would end it.
- WORD-007: units are long on one screen and short on another: "Keep holding for 2 seconds",
  "After 30 seconds", "After 1 hour" (touchscreen) beside "{0} s", "{0} min", "{0} h {1} min"
  (both); a number is spelled out in "Unplug the panel for ten seconds" (touchscreen,
  `ui_installer.c`).
- WORD-007: time formats: "14:30 instead of 2:30 PM" (touchscreen, `ui_net.c`) and the Panel
  Manager's "The region's, or 14:30 or 2:30 PM whatever the region." (awkward), "24-hour clock"
  against "24-hour" in `web_config.c`.
- WORD-006 (Flasher): its words are outside the catalog, so none can follow the keypad's; its
  "Cancel", "Continue" and "Guided setup" have no counterpart to compare until BL-047.

## Requirements (draft, not agreed)
The rules the Overview mockup follows; to be agreed before they apply anywhere else.

| ID | Requirement | Verified by |
|---|---|---|
| WORD-001 | A state's headline is 1-4 words; the line under it about 8 at most, and never repeats the headline | not defined yet |
| WORD-002 | A button is 1-3 words, a verb that says what happens, with no line under it | not defined yet |
| WORD-003 | An option is a 1-3 word label with a helper of a few words, shown only where it applies | not defined yet |
| WORD-004 | Two levels at most: what is needed now, and one tap deeper for options and details; no tooltip carries anything needed to decide | not defined yet |
| WORD-005 | A confirmation only before a real consequence that a person could regret and that nothing else confirms: a title naming the action, one line, buttons named after the action, the safe one focused; never where it would delay stopping an alarm | not defined yet |
| WORD-006 | The keypad's vocabulary everywhere, from one catalog: where the touchscreen has words for something, the Panel Manager and the Flasher use the same words | not defined yet |
| WORD-007 | Numbers as numerals, one time format, units attached (30 s, 1:00, 10:14 PM) | not defined yet |
| WORD-008 | A problem is a name, one line and one fix; words change with the state, never colour alone, and nothing blinks (CONTRIBUTING.md "Display safety") | not defined yet |
| WORD-009 | One solid button per card, in the tone of what it does (the arming level's, green to disarm); everything else on the card neutral; red only for an alarm and destructive actions | not defined yet |
| WORD-010 | Words on a coloured fill reach 4.5:1 and a control's edges and icons 3:1, in both themes (WCAG 1.4.3, 1.4.11) | not defined yet |

## Design
Not designed beyond the Overview. What is known:
- The touchscreen and the Panel Manager share one catalog (docs/features/37). Changing an English
  source string changes its catalog key, so its translations in the drafts (`es-US`, `en-GB`)
  have to be redone. Wording changes cost the translations, and are best done before the native
  speakers' reviews (BL-005, BL-110), or together with them.
- The touchscreen already has the palette above (layouts' `globals.xml`: color_rose, color_blue,
  color_violet, color_green, color_amber, color_red) and the icon chips. The touchscreen has the
  Overview's words since 2026-10-01: "No entry delay" (not "Instant"), "Alarm occurred" and,
  when the disarm stopped the report, "Alarm canceled" with "No alert was sent".
- The Flasher (docs/features/34) has its own words, not in the catalog yet (BL-047, G7).
- The manuals' pictures ([UPDATING.md](https://github.com/RoboAlarms/documentation/blob/main/UPDATING.md) in RoboAlarms/documentation) change when the screens' words do.

## Test and acceptance
Not defined yet. For the Overview: the mockup reviewed in both themes and at phone
width, then the built page in the mock (`mock-panel.cjs`) and the simulator, then on a panel
(BL-113). For the rules: see the open question about a check in CI.

## Open questions
- **Scope and order** after the Overview: the touchscreen, the rest of the Panel Manager, or the
  Flasher first? Page by page, or one sweep?
- **The touchscreen's own words:** bring them to the Overview's ("No entry delay", "Alarm
  occurred"), with their translations redone? The research recommends it for both.
- **A confirmation before a normal disarm:** kept as decided, as round 1 advised; round 2
  would drop it and offer a steady "Re-arm" after a disarm instead.
- **"Alarm canceled"** when a disarm fell inside the abort window (with "No alert was sent"): worth
  the extra state?
- **A check in CI:** the `/* TR: ... */` notes already give byte budgets; should `tools/i18n.py
  check` also warn on word counts per kind of text (headline, line, button, helper)?
- **The Panel Manager's teal** (its accent for switches and primary buttons elsewhere): keep it as
  the settings pages' colour, or move every page to the state palette?
- **Who reviews wording:** the maintainers alone, or a small test with people who don't know the panel
  (as BL-078 plans for the Flasher)?

## Tasks
- [ ] BL-114 Decide the scope, the order and how the rules are checked (the maintainers), then write this document's design
- [x] BL-114 List every screen's words: the touchscreen, the Panel Manager, the Flasher, the notifications (inventory: `python tools/i18n.py inventory`; 5153415)
- [ ] BL-114 Bring the touchscreen to the Overview's words where they differ ("No entry delay", "Alarm occurred"), in every layout, with the translations redone and the preview's pictures refreshed
- [ ] BL-114 Apply the agreed rules screen by screen, with the translations redone and the manuals' pictures refreshed
