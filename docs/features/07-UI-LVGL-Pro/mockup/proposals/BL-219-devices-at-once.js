// BL-219: a change on a Smart Home tile reads as done the moment a person makes it (docs/features/24
// HCTL-006, HCTL-012; ui_devices.c). Seen 2026-10-19 on both the 7" and the 10.1" link keypad, with a
// bench Home Assistant that answered every call at once: the brightness slider sprang back to its
// old level when the finger lifted, the tile then said "Changing…" with its chip dimmed, and only
// then jumped to the new level. The delay is the screen's: it showed the device's last report until
// the next one came. Proposed: the screen shows what the person asked for straight away and keeps it
// until the device sends a new report, as Home Assistant does; it says something only when the change
// fails or isn't confirmed.
// Locks, garage doors, covers and valves are a question for the owner (bl-219-lock-under-way: a
// lock must not say "Locked" before it is). The page is views.js's Favorites (each
// mockup file has its own scope, so its pieces are repeated here).

const DEVICE_AREAS = ['Favorites', 'Living room', 'Kitchen', 'Outside'];
const DEVICE_PORCH = {glyph: 'lightbulb', tone: 'blue', name: 'Porch Light', state: 'On · 80%', wide: true, control: M.device_level({value: 80})};
const DEVICE_FAVORITES = [
  DEVICE_PORCH,
  {glyph: 'lock', tone: 'green', name: 'Front Door', state: 'Locked'},
  {glyph: 'lock-open', tone: 'amber', name: 'Back Door', state: 'Unlocked'},
  {glyph: 'warehouse', tone: 'green', name: 'Garage Door', state: 'Closed'},
  {glyph: 'thermometer', tone: 'orange', name: 'Hallway', state: 'Heating · 71° now', wide: true, control: M.device_stepper({label: 'Heat to', value: '72°'})},
  {glyph: 'sparkles', tone: 'violet', name: 'Good Night', state: 'Ran at 8:30 PM', more: false},
  {glyph: 'lightbulb', tone: 'blue', name: 'Living Room Lamp', state: 'On · 40%', wide: true, control: M.device_level({value: 40})},
  {glyph: 'lightbulb', name: 'Kitchen Lights', state: 'Off'},
];
const withState = (tiles, name, o) => tiles.map((t) => (t.name === name ? {...t, ...o} : t));
const devicesMain = (o) => M.main({view: 'devices', tone: 'amber', rail: {zones: 1, troubles: 1}, bar: {time: '10:48', troubles: 1}, ...o});

// A tile marked as changed by this proposal: the dashed outline on the tile itself, so a wide
// tile still spans its two cells.
const at1Mark = (html) => html.replace('class="device_tile', 'class="changed device_tile');
const at1View = ({note, tiles, marked = []}) => `<div class="view column">
  <div class="row" style="gap:var(--space_md)">${M.view_header({title: 'Smart Home', note})}${M.segments(DEVICE_AREAS, 0)}
    <div class="round_button">${M.icon('pencil', 20)}</div></div>
  <div class="device_grid scroll grow">${tiles.map((t) => (marked.includes(t.name) ? at1Mark(M.device_tile(t)) : M.device_tile(t))).join('')}</div></div>`;
const at1Porch = (o) => ({...DEVICE_PORCH, ...o});
const at1Tiles = (porch, kitchen) => withState(DEVICE_FAVORITES.map((t) => (t.name === 'Porch Light' ? porch : t)), 'Kitchen Lights', kitchen);
const AT1_LIT = {tone: 'blue', state: 'On · 100%'};

const at1 = (id, title, why, notes, main) => M.screen({id, area: 'Smart Home', title, shot: null,
  proposal: {bl: 'BL-219', status: 'approved', replaces: 'devices', why,
    decided: '2026-10-19: approved as drawn; locks, garage doors, covers and valves keep their under-way words, everything else shows at once'}, notes, render: () => devicesMain(main())});
const AT1_WHY = 'A change looked undone, then under way, then done, though the device had answered at once: the screen felt slow and unsure of itself. Seen 2026-10-19 on both panels.';

// ---- Today, as the owner saw it -------------------------------------------------------------
at1('bl-219-today-1', 'Today, 1: the finger lifts and the slider springs back', AT1_WHY,
  'TODAY. Porch Light was dragged from 80 % to 40 % and the finger lifted; Kitchen Lights was tapped on. For a moment nothing seems to have happened: the slider is back at 80 % and Kitchen Lights still says Off, because the screen shows the device\'s last report until the next one comes (the call is on its way, LINK-054 on a keypad).',
  () => ({content: at1View({note: 'Back Door unlocked · 2 lights on', tiles: at1Tiles(at1Porch({}), {}), marked: ['Porch Light', 'Kitchen Lights']})}));

at1('bl-219-today-2', 'Today, 2: "Changing…" with the chip dimmed', AT1_WHY,
  'TODAY. Next poll (at most a second later): the registry has the action under way, so the tiles say "Changing…" and "Turning on…" and their chips dim to 40 %; the slider still shows 80 %.',
  () => ({content: at1View({note: 'Back Door unlocked · 2 lights on',
    tiles: at1Tiles(at1Porch({state: 'Changing…', pending: true}), {state: 'Turning on…', pending: true}), marked: ['Porch Light', 'Kitchen Lights']})}));

at1('bl-219-today-3', 'Today, 3: the jump to the new level', AT1_WHY,
  'TODAY. The device reports: the slider jumps to 40 % and Kitchen Lights comes on. Three changes for one touch.',
  () => ({content: at1View({note: 'Back Door unlocked · 3 lights on',
    tiles: at1Tiles(at1Porch({state: 'On · 40%', control: M.device_level({value: 40})}), AT1_LIT), marked: ['Porch Light', 'Kitchen Lights']})}));

// ---- Proposed (revised 2026-10-19 after looking at Home Assistant's own screens) ------------
// Home Assistant's toggle shows the new state the moment it is flipped and goes back to the
// device's state if no new report has come 2 s after the call (frontend ha-entity-toggle.ts);
// its brightness slider stays where it was let go until the next report; neither says anything
// while the call is on its way. Words like "Locking…" come only from a device that reports them.
at1('bl-219-done-at-once', 'Proposed: done the moment the finger lifts', AT1_WHY,
  'PROPOSED. The same touches. The slider stays at 40 % and the words say "On · 40%" at once; Kitchen Lights says "On" and lights up as the tap lifts (as built: its brightness, "On · 100%", joins when the light reports it, since an off light doesn\'t report the brightness it comes back at); the chips stay at full strength and the page\'s note counts the light as on. No "Changing…", "Turning on…" or "Switching…" any more. Nothing else happens when the device confirms. This covers a light, a switch, a fan and a thermostat: on and off, brightness, a fan\'s speed, a set point (the tile\'s and the sheet\'s - and +) and a mode. The rule, as Home Assistant\'s: the screen shows what was asked until the device sends a new report; from then on it shows the report, whatever it says. A touch while the last change is still on its way is not refused: the newest touch wins (it is sent as soon as the last one is answered, never repeated by itself). The same on the 10.1" link keypad (the same screen code).',
  () => ({content: at1View({note: 'Back Door unlocked · 3 lights on',
    tiles: at1Tiles(at1Porch({state: 'On · 40%', control: M.device_level({value: 40})}), {tone: 'blue', state: 'On'}), marked: ['Porch Light', 'Kitchen Lights']})}));

at1('bl-219-sheet-at-once', 'Proposed: the sheet\'s slider stays where it was let go', AT1_WHY,
  'PROPOSED. In a light\'s sheet the same: the brightness stays at 40 %, the note says "On · 40%" at once. The switch beside the name flips and stays.',
  () => ({content: at1View({note: 'Back Door unlocked · 2 lights on', tiles: at1Tiles(at1Porch({state: 'On · 40%', control: M.device_level({value: 40})}), {})}),
    over: M.device_sheet({glyph: 'lightbulb', tone: 'blue', title: 'Porch Light', note: 'On · 40%', on: true,
      body: M.changed(M.row_level({glyph: 'sun', title: 'Brightness', value: 40}))})}));

at1('bl-219-report-wins', 'Proposed: the device says something else, so the tile goes back', AT1_WHY,
  'PROPOSED. Porch Light was set to 40 % and the call was accepted, but the light\'s next report says 80 % (it ignored the change, or someone changed it again at the wall): the slider and the words go back to 80 % in one step, without words, as in Home Assistant. If no new report comes at all, the tile goes back to the last report 2 s after the call was answered (Home Assistant\'s rule), so it never shows a value the device didn\'t take for longer than that.',
  () => ({content: at1View({note: 'Back Door unlocked · 2 lights on', tiles: at1Tiles(at1Porch({}), {}), marked: ['Porch Light']})}));

at1('bl-219-lock-under-way', 'Question: a lock, a garage door or a cover', AT1_WHY,
  'QUESTION FOR THE OWNER. Back Door was tapped to lock. Today a lock, a garage door, a cover and a valve say "Locking…", "Closing…", "Opening…" with the chip dimmed until the device reports, and a scene "Running…". Home Assistant shows "Locking" or "Closing" only where the device itself reports it (many locks and garage doors do), and a lock\'s bolt or a garage door takes seconds. Recommended: keep these words for locks, garage doors, covers and valves (drawn here), because a security panel must not say "Locked" or "Closed" before it is; scenes become instant ("Ran at 10:48 PM" at once). The other choice: treat them as the lights (show "Locked" at once and go back if the report differs). Unlocking and opening still ask for the code first (HCTL-005) either way.',
  () => ({content: at1View({note: 'Front Door locked · 2 lights on',
    tiles: withState(DEVICE_FAVORITES, 'Back Door', {tone: 'amber', state: 'Locking…', pending: true}), marked: ['Back Door']})}));

at1('bl-219-failed', 'Proposed: when the change is refused', AT1_WHY,
  'PROPOSED. Home Assistant refuses the brightness change: the slider goes back to what the device reports (80 %), the words turn amber "Didn\'t change" and the note says "Porch Light didn\'t change", as today. One step, nothing blinks. A tap opens the sheet to set it again; nothing retries by itself (HCTL-012). This is the one case where the screen says more than Home Assistant\'s own (which shows a passing error): a panel on the wall is looked at later, so the tile keeps it until the next touch.',
  () => ({content: at1View({note: 'Porch Light didn\'t change · 2 lights on',
    tiles: at1Tiles(at1Porch({state: 'Didn\'t change', stateTone: 'amber'}), {}), marked: ['Porch Light']})}));

at1('bl-219-unconfirmed', 'Proposed: when no answer comes', AT1_WHY,
  'PROPOSED. No answer within the registry\'s 30 s (or the connection drops with it on its way): until then the tile shows what was asked; then it shows what the device last reported, with "Not confirmed" in amber and "Couldn\'t confirm Kitchen Lights turned on" on the note, as today. Never "didn\'t" for an outcome that isn\'t known (HCTL-012).',
  () => ({content: at1View({note: 'Couldn\'t confirm Kitchen Lights turned on',
    tiles: at1Tiles(at1Porch({}), {state: 'Not confirmed', stateTone: 'amber'}), marked: ['Kitchen Lights']})}));
