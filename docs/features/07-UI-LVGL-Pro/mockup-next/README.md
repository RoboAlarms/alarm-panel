# mockup-next: the usability redesign (BL-131)

A copy of [the panel's UI mockup](../mockup/README.md), made to redraw what a review of every
screen the PC preview renders (165 pictures) found awkward: Settings out of order, the date and
time entered with steppers, About mixing the SD card with the panel's own memory, states said
twice, destructive buttons mid-page, words that say what a page is for instead of how things
are. Today's mockup in `../mockup` is unchanged and stays the record of the panel as it is. Here
`screens/` is today's panel (with the states the original lacked, so every proposal has a "Now"
beside it) and the redesign is in `proposals/BL-131-*.js`, shown in the viewer as
**Now | Proposed**. The Smart Home screen the new rail leads to is BL-045's, in
`proposals/BL-045-devices.js`, and the light and high-contrast appearances are BL-132's, in
the layout's `themes.xml` (`components/ui/alarm_ui/layouts/1024x600/alarm_ui/themes.xml`)
and `proposals/BL-132-appearance.js` (both below). The viewer's **Appearance**
switch, in its top bar, shows any screen in any of them.

```
python tools/ui_mockup.py --mock docs/features/07-UI-LVGL-Pro/mockup-next build --shots   # -> build/mockups/panel-next/
python tools/ui_mockup.py --mock docs/features/07-UI-LVGL-Pro/mockup-next check
```
To look at it, serve `build/mockups/panel-next` (`python -m http.server -d build/mockups/panel-next`)
or open its `index.html` from the folder.

Once the person you work for has approved a proposal, it is built and moved into `../mockup` as
that mockup's README says; when nothing is left here, this folder is deleted.

**Approved as drawn on 2026-10-01** (BL-131, BL-132 and BL-045's screen). Built so far, so today's
screens here and in `../mockup` draw them: finding 26's rail and status bar (Smart Home comes with
BL-045's screen), finding 27's password eye (`../mockup` `wifi-password`, `wifi-password-shown`),
finding 1's one-line Settings list (with Event log in it until finding 7 makes Activity the log, so
nine sections) and finding 2's Time and language (`settings-time`, `settings-time-locked`); findings
8 and 13-16's Integrations (`settings-integrations`, `integration-ha`, `ha-pairing-code`,
`ha-paired`, `ntfy-page`); finding 3's calendar and rollers (`time-set`, `setup-clock`); findings
9-12's About this panel, Software update, SD card and System details; findings 5 and 6's Wi-Fi and
network pages; findings 17 and 20's user page and pickers; findings 18, 19 and 23-25's installer
menu, zone page and sensor settings; finding 4's Screen and sound and Night hours; findings 21 and
22's setup; the small fixes listed below; BL-132's four appearances; BL-045's Smart Home screen
with its detail sheets and Arrange (`../mockup` `screens/views.js`). Left: finding 7 (Activity as
the log), waiting on the maintainers; the rest is in docs/features/07's Tasks.

## Rules the redesign follows
They come out of the findings below, and are the test for any screen not drawn yet.
1. **A row says how things are**, not what its page is for: "Harbor House · good signal", "Up to
   date", "44.7 GB free", not "Network and signal", "Check, install and manage updates".
2. **One fact, one place.** A setting has one home (the 12/24-hour clock, the chime, the event
   log); a state is said once (a switch carries its state in its detail, no card above it).
3. **What people do most comes first, what destroys comes last**, apart, under a gap: Forget,
   Unpair, Delete, Format, Factory reset.
4. **Words a person knows.** No hardware behind the feature (internal flash, partitions), no
   library names (aioroboalarms), no codes (it-IT), no raw values ("off", "clear"), GB not GiB.
5. **Pickers for choices, steppers for numbers.** A date, a time or a list is a calendar, rollers
   or a picker; a stepper only steps a number (seconds, minutes, a count).
6. **A sheet fits its content**; a page's note says something its title doesn't, in one line,
   or isn't there; an explanation that belongs to a row is that row's detail, not a paragraph.
7. **No toast for what the screen already shows** (the project's rule).
8. **A group lines up.** Every row of a group starts its words at the same place: a switch among
   rows with icons has an icon too; a live value is a row of its own (on top), not a flat line
   between cards; a group of switches only needs none.
9. **Navigation goes places.** The rail and the Settings list hold places, one line each; a switch
   (Chime) sits where its state shows, a count (Troubles) shows once, and a list entry speaks
   only when something needs you.

## What the review found, and the proposal for each
Drawn (`proposals/`):

| # | Today | Proposed | Screens |
|---|---|---|---|
| 1 | Settings has 10 sections; the list scrolls, so About and Installer are below the fold. Each is a coloured chip, a name and a subtitle that describes the page ("Network and signal"): two lines and a different colour per section, beside the rail and the page | 8 sections that fit, one line each: an icon in the rail's style and the name, Installer set apart under a line; a tag only when something needs you (About: "Update") | every `n-*` settings page, `n-about-update` |
| 2 | The 12/24-hour clock is set in two places: a switch under Date and time, "Clock" under Language and region | One page, **Time and language**: what the clock says, time zone, one Clock choice (Automatic, 12-hour, 24-hour), language, region; the time server under Advanced | `n-time`, `n-time-unset` |
| 3 | Setting the clock by hand: five stepper rows (year, month, day, hour, minute) that scroll | A calendar and hour/minute/AM-PM rollers on one sheet, the result in words | `n-time-set`, `n-setup-clock` |
| 4 | Display and sound: 12 rows in one scroll; the chime here and on the rail; night hours at the bottom with Starts/Ends steppers (the preview shows 10:00 AM to 12:00 PM, which looks like a wrong default) | **Screen and sound**: Screen, then Sound; the chime left to the rail; Night hours its own page, its hours one row that opens rollers | `n-display`, `n-night`, `n-night-hours` |
| 5 | Wi-Fi: a full-width red "Forget this network" at the top; the joined network listed twice; its settings behind a small gear | The joined network is a row that opens its page; Forget is at that page's bottom | `n-wifi` |
| 6 | Network: "How the panel gets its address", "Name server", "From the router" | "IP address settings", "DNS server", "Automatic" | `n-network` |
| 7 | Event log in Settings and Activity on the rail: the same log twice ("Settings has the full event log"); every time "Just now" | Activity is the log, with the filters; times say when | `n-activity` |
| 8 | Integrations: a paragraph between the sections explains the rows | The rows say it; "Notifications" reads "Alerts to your phone" | `n-integrations` |
| 9 | **About**: Firmware and Panel ID as loose lines, then Software update, **Storage** (the SD card mixed with the internal flash) and System details, each described by what the page is for. The **Internal flash** page lists partition sizes (firmware slot, update slot, outside partitions) | **About this panel**: the panel, its version and ID on top; Software update ("Up to date", or "Install" in violet when one waits), **SD card** as its own row with its free space, System details "for someone helping you". The Storage and Internal flash pages go: one line, "Firmware space", in System details; the Panel Manager keeps the full numbers | `n-about`, `n-about-update` |
| 10 | Software update: a section repeating the title, Install and Check for updates as two equal buttons, a toast while checking, the update source (a web address) among them | What is waiting, one Install button, "Check again" as a row with the time it last looked, the source under Advanced | `n-update` |
| 11 | SD card: Status, then a message, a blank gap, five value lines (GiB), a FAT12/16/32 sentence, three equal buttons below the fold; nothing says what the card is for. No card: five lines of "Unavailable" | Its state and free space with a bar, "Optional. Nothing is kept on it yet.", Safely remove, Card details (format, model), Format last and apart. No card: the state and "Check for a card", nothing else | `n-sd`, `n-sd-none` |
| 12 | System details: "Hardware and software / Network / Runtime", a stray dot before the address (bug), "3 d 4 h 12 min", RAM in KB | This panel (ID, version, running for), Network (Wi-Fi, IP, MAC), Set up (zones, users, events), Hardware (processor, Wi-Fi module, memory, firmware space); the licences stay at the bottom | `n-system-details` |
| 13 | Home Assistant, MQTT, ntfy, Pushover: a status card ("Off · Switch it on…") above a switch that says the same | One switch with the state in its detail ("Off", "On · pairing", "On · connected and paired") | `n-ha-off`, `n-ha-paired` |
| 14 | Pairing with Home Assistant: the six digits to compare are a small value line; "The codes match - allow" / "They don't match - reject" stacked | The code big, the question above it, "No, reject" and "Yes, allow" side by side | `n-ha-code` |
| 15 | "Connected · aioroboalarms 0.1.0", "Paired with aioroboalarms 0.1.0": the library inside Home Assistant | "On · connected and paired", where to find the panel in Home Assistant; the library name to System details | `n-ha-paired` |
| 16 | ntfy: server settings first, then "New topic" and "Show the QR code" buttons and a long paragraph; "What to send" two screens down | Switch, **Add a phone** (opens the QR code), test, What to send, and Where they go last; the topic's warning is its row's detail | `n-ntfy` |
| 17 | A user: "Delete this user" above the Panel Manager section; "Change the browser password · On" with a red "Turn off browser sign-in" under it | Browser sign-in a switch, the password under it, Delete last and apart | `n-user` |
| 18 | Installer menu: 11 rows in no order, Factory reset mid-list; the Panel Manager in two places ("Web server" under Remote access, the installer's sign-in under Panel Manager) | Four groups (Sensors and devices, How the alarm works, This panel, Test and start over); the Panel Manager in one place; "System options" is "Delays and arming" | `n-installer-menu` |
| 19 | Steppers holding words ("Source default", "Not supervised") wrap in their box and step through times one tap at a time | A picker row; steppers only for numbers | `n-zone-sensor` |
| 20 | Pickers are 520 px tall around one choice (Language with only English) | A sheet as tall as its choices | `n-language-pick` |
| 21 | Setup: "Leave setup" beside Back on every step, and on the last step beside "Go to Home"; the date step wears the time zone's globe | "Leave setup" a quiet link on the left, gone from the last step; a calendar-clock icon | `n-setup-wifi`, `n-setup-clock`, `n-setup-done` |
| 22 | Setup: "Installer code changed" toasts while the row says "Changed" and is still up on the last step; the Next button reads "Keep for now"; the factory-code warning on the last step is grey | No toast; "Next" and an amber line; the codes row amber with "Change" | `n-setup-codes`, `n-setup-done` |
| 23 | A zone (installer): up to twenty rows in one run starting at three edges (icon rows, switches, a flat "Right now" line) with a paragraph between them; for a wired sensor Contact and Response time on this page **and** on Sensor settings, under other names; "Alarm limit" (a stepper reading "System setting") and "Never shut down" are one setting in two rows; "Report this zone" describes Off beside a switch that is on | How the zone is right now on top (the source and what it means as its detail), then The zone, Its sensor, What it does, Advanced; Contact and Response time only on Sensor settings, its row saying what they are; Alarm limit one picker (system, 1 to 6, Never); Remove last, apart | `n-zone-edit`, `-2`, `-3` |
| 24 | Two settings, two pages, nearly the same words: Supervision on the zone page ("A wireless sensor silent this long shows Check"), Unreachable after on Sensor settings ("A sensor silent this long shows Check"); both exist on purpose (docs/features/22) | Side by side on Sensor settings under "When it goes quiet": "Shows Check when it misses a check-in", "Shows Check when its source loses it" | `n-zone-sensor`, `-2` |
| 25 | Rows of a group start at different places (a switch without an icon among rows with one), and a page note repeats the banner ("Installer mode · arming is off until you leave" under "Installer · Arming off · Tap to leave") | Rule 8; the Installer page has no note; "0 in use" reads "None" | `n-installer-menu`, `n-user`, `n-zone-sensor` |
| 26 | The rail: the logo and six entries fill it, so the Devices screen (docs/features/24 HCTL-003) has no place. Two entries aren't places: Chime is a switch (UI-025), and Troubles repeats the status bar's amber count with a badge of its own | The rail is for places: **Security** (today's Home), Zones, **Smart Home** (a house with Wi-Fi, the name on two lines; only with a device to control), Activity, and Panic at its foot (why these names: below). Chime is a round switch beside the gear (its code panel, its bell, dimmed while armed, as today); the trouble count is said once, in the status bar, as a pill that opens Troubles. The rail's buttons are 88 px wide, room for longer words in other languages; Smart Home's is a line taller (86 px). Where Smart Home leads is BL-045's proposal (below) | `n-home`, `n-armed`, `n-zones`, `n-troubles`, `n-devices` |
| 27 | A password (Wi-Fi, the setup's Wi-Fi step, the browser sign-in, MQTT, the ntfy and Pushover tokens) shows only as dots, so a slip in a long one shows up only when it is refused | The field ends in an eye, as on a phone: a tap shows what was typed, another hides it. Every password field has it (they share the text sheet); it starts hidden each time the sheet opens, and the choice isn't kept | `n-wifi-password`, `n-wifi-password-shown` |

Decided requirements that finding 26 would change once approved (docs/features/07 unless named):
"The navigation rail (Home, Zones, Activity, Troubles, Chime, Panic)"; UI-022 (the rail's Chime
dimmed while armed: the status bar's); UI-025 (the rail's Chime shows the bell: the status bar's);
UI-030 (the count agrees with the rail's badge: with the status bar's pill); docs/features/24
HCTL-003 (the rail item **Smart Home**, not Devices: third, after Zones). The rail's Home becomes
**Security**, and the setup's last button "Go to Home" becomes "Done". Each language needs words
that fit 88 px a line, two lines at most: Spanish "Seguridad" is 74 px, and "Hogar inteligente"
fits on two lines ("inteligente" is 75 px), or "Domótica" (69 px) on one.

Why Security and Smart Home (decided 2026-10-01): the home page is the security page, and a
Smart Home button leads to the devices; together they are "Security & Smart Home". The name is wider than the rail's buttons (89 px), so it takes two lines,
which was chosen over the shorter Devices. Its icon, a house with Wi-Fi, was Settings'
Integrations icon; there it becomes a puzzle piece, and the Home Assistant row, where most devices
come from, keeps the house. Home is also an arm mode (Home Assistant's Armed home),
Alarm is the event on this panel (Burglary alarm, alarm memory), and
Automation means rules and schedules in the apps people know (Home Assistant): the word
for BL-045's later built-in automations.

## The Smart Home screen (BL-045)
Where the rail's Smart Home leads (docs/features/24's device control), drawn with the redesign's
rail: `n-devices` (Favorites), `n-devices-outside` (under way, failed, offline; a cover, a fan, a
valve), the detail sheets `n-device-light` and `n-device-climate`, `n-device-unlock`, and
arranging the page: `n-devices-arrange`, `n-devices-arrange-tile`, `n-devices-arrange-add`.
- **A tile does what a tap can do** (switch, lock, run a scene). A device that needs more gets a
  tile two cells wide with its control on it: a dimmable light its brightness bar, a thermostat
  its set point between minus and plus, a cover with a position its Open, Stop and Close, a fan
  its speeds. The kind sets the width; twelve cells (four columns, three rows of 132 px) fill the
  view, so Favorites doesn't scroll.
- **The state is words and colour**, what is under way ("Closing…", its chip dimmed), what
  failed ("Didn't turn on", amber; a tap tries again, nothing retries by itself, HCTL-012) and
  what is offline (readable, "Offline since 9:12 PM", its chip dimmed and its controls at rest,
  BL-181) included. Nothing moves.
- **The note under the title says what needs you and what is on** ("Back Door unlocked · 2 lights
  on"), live words under UI-023, instead of how to use the screen.
- **A sheet applies a change at once**, so it closes with an X. Unlocking and opening ask for a
  code on the pad disarming uses (HCTL-005).
- **People lay the page out themselves** (asked for: "an easy way to lay out the screen the way
  they want"). The pencil beside the areas starts Arrange: every tile gets a dashed edge and a
  grip, and its controls rest. A tap picks a tile; the arrows move it a place (or it is dragged),
  Small and Wide set its size, Remove takes it off Favorites; Add lists the devices that aren't
  on it; Done ends. The arrows are the way without dragging (WCAG 2.5.7), and tiles that make room
  jump into place without sliding (UI-020). No code: it changes how the screen looks, as
  brightness does. It changes HCTL-008, where reordering and favourites are in Settings > Devices
  with a master or installer code: those move here, with the size, and hiding and renaming stay
  there. The layout is the panel's, the same for everyone.

Ideas taken from [Tessera](https://github.com/MaxGramser/homeassistant_espscreen), an open-source
project for ESP32 wall screens: tiles of one or two cells, the control on the tile, states under
way and faults in words, the set point between minus and plus, a cover's keys. Patterns only:
Tessera is AGPL-3.0 and this project is Apache-2.0, so none of its code or pictures. Not taken:
its two-tap Confirm circle that beats (UI-020; an unlock takes a code here), a spinner over a
waiting tile, a pastel background per tile (here the colour is the state). Left for later: a
"Lock all" key in the header, colour presets for a light (LVGL 9 has no colour wheel), round
device keys on a bedside page, small history graphs (they need a history first).

## Light and high contrast (BL-132)
A light mode beside the dark one was asked for, and a high-contrast mode for people who
have a hard time seeing. The palettes are the layout's `themes.xml`: **Dark** (today's), **Light**, **High
contrast** (white on black) and **High contrast, light** (black on white). Each names every colour
of the layout's globals.xml, plus four it can't change today: `color_alarm_card` (the literal
0x3A1216 in `style_hero_alarm`), `color_toast_line` (the toasts' literal border), `color_knob`
(a switch's and a slider's knob, `color_text` today, which would turn dark in Light) and
`color_key_special` (the keyboard's special keys, `color_line` today: high contrast makes that an
outline colour, and their symbols all but vanished).
- **Picked in Screen and sound** (`a-display`, `a-appearance`): Appearance is Dark, Light or
  Automatic, and High contrast a switch beside it that works with either. No code, like
  brightness (UI-015). Automatic follows the night hours (UI-043): dark during them, when a
  hallway panel is looked at in the dark, light by day, switching once at each end; with the
  night hours off it says so and stays light.
- **Every word readable**, held by `ui_mockup.py check`: words and tones 4.5:1 or more on the
  screen, a card and a key, a special or held key too (WCAG AA), 7:1 in high contrast (AAA), and words on a tone or an alarm
  background the same. A finding on the way: today's `color_text_3` (0x5B6778, the hints) is 2.7
  to 3.4:1, under AA; the Dark palette raises it to 0x808DA0.
- **High contrast changes colours and edges, not the layout**: every card, row and control has a
  2 px outline (3:1 or more), what is chosen is ringed in the text colour, and a switch's knob on
  its coloured track takes `color_on_tone`. The fonts stay, so nothing moves or gets cut off;
  larger text would be a feature of its own (a layout with room for it).
- **In Light** the tones are deeper (a green or amber word on white needs it), the cards white on
  a pale grey, the knob white with a thin ring. An alarm stays unmistakable: the screen washes pale
  red and the state card is red-edged on pink, with the same slow glow (UI-020).
- **The screen saver keeps the dark colours** in every appearance (its words sit on the dimmed
  photo; a theme's `saver=` names the palette it keeps).
- **A change applies at once, in one step**: nothing fades (UI-020).

Drawn: `a-display`, `a-appearance`, `a-display-contrast`, and the main screens in each appearance
beside today's: `a-home-light`, `a-armed-light`, `a-devices-light`, `a-settings-light`,
`a-alarm-light`, `a-saver-light`, `a-home-contrast`, `a-devices-contrast`, `a-alarm-contrast`,
`a-home-contrast-light`, `a-devices-contrast-light`, and the keyboard in both high contrasts:
`a-keys-contrast`, `a-keys-contrast-light`. Any other screen: the viewer's Appearance
switch (it remembers the choice in this browser; a screen drawn for one appearance keeps it).

Built (part 1 1c72299, part 2 22fa668): the palettes moved from the kit into the layout, beside
globals.xml (`themes.xml`, the one copy: this viewer and `ui_mockup.py check` read it there), and
the four new colours joined globals.xml. `lvpro_gen.py` makes them a palette table that the
`COLOR_*` constants read, and gives every style a place in a registry; `ui_theme_apply()`
(`ui_theme.c`, with the switch in the layout's `alarm_ui.c`) changes the palette, the styles, the
colour subjects and every object's own colours at once, without rebuilding: the view, the page
and an open sheet stay. Each palette value stands for one colour name (the generator nudges a
shared value in the bits a 16-bit screen drops), so a colour already on the screen is found by
its value. The XML's literal colours became the new tokens (the hero's alarm card, the toasts'
edge, the knobs, the special keys); high contrast's outlines and rings and the light themes'
knob ring are `<styles themes="...">` sections of themes.xml (an outline inside the edge, so
nothing moves). The PC preview's walk ends in each appearance (`theme_*` pictures, drawn in
`screens/appearance.js`). Part 2: the two choices stored (`ui/appearance`, `ui/contrast`),
Appearance and High contrast in Screen and sound and the Panel Manager, and Automatic following
the night hours (`ui_light_theme()`; a switch due during an alarm, entry or exit time waits). UI-010
says it.

Not drawn (a word, a bug or the same rule again; each is small), all built now (BL-131's small
fixes and the pieces before them):
- Home: "Setup finished" toasts over Home, which already shows it (rule 7).
- Home: while the keypad is locked out, its keys still look ready; they could dim until it unlocks.
- Language and region: "it-IT isn't installed" names a locale code; "Italian isn't installed" (rule 4).
- Region "United States (the language's)" and Clock "The region's" read awkwardly: their pickers start with "Automatic", and the row shows the result ("United States", "12-hour"), as `n-time` does.
- Zone pickers for Home Assistant and Zigbee show the device's raw state in lower case ("off", "clear"): "Off", "Clear" (rule 4).
- Network with a fixed address: "Network · 255.255.255.0 · Which addresses count as local" is the subnet mask; "Subnet mask" (the page is called Network too).
- Event log's note "keeps at least 960": "Keeps the last 960 or more".
- MQTT and Pushover follow `n-ntfy`'s order (rule 2 and 3); Supervision on a device follows `n-zone-sensor` (rule 5).
- A settings slider (Brightness, Volume) puts its title after a small icon, a little left of the titles of the rows with an icon chip around it (rule 8).
- Today's page notes repeat the section's subtitle ("The clock is set from the internet", "This keypad's screen and sounds, and the chime"); the redesign drops a note that says nothing new (rule 6).
- The setup's welcome lists four steps; its progress bar has five.
- Installer, before the code: three grey paragraphs under one button; the lost-code paragraph could be a row ("Lost the installer code?") that opens the explanation.
- In the setup, lists scroll under the Back and Next buttons with nothing to show there is more.
