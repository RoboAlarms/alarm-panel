// BL-216, proposal P1 (docs/features/11 "Smart Home and Home Assistant on keypads"): Smart Home
// on a panel in the link's keypad role. The owner asked on 2026-10-16 that the Smart Home controls
// work on a keypad as they do on the alarm panel. Today a keypad's rail has no Smart Home
// (ui_devices.c, ui_link.c; screens/panel-link.js kp-home-ready). With this, the keypad shows the
// alarm panel's page unchanged: the same tiles, sheets, code pad, words and states, from Harbor
// House's devices over the link (LINK-044..049). Only the rail changes: no new words, no new kit
// part.
//
// Approved 2026-10-16 (the owner: "Approve P1, recommendations OK"), with the design's decisions
// taken as recommended (docs/features/11 Decisions): 1, a keypad's codes unlock or open for its
// home partition, as for its arming; 2, each panel keeps its own Arrange; 3, device control while
// armed Away is the same on both screens; 11, the scope is the Smart Home view only; 13, this
// proposal. When BL-216 is built, the preview's host_link_demo draws these states, and
// screens/panel-link.js takes them (its kp() frame no longer hides Smart Home).
//
// Each file has its own scope: the Smart Home page and its devices are copied from
// screens/views.js (devicesView, DEVICE_FAVORITES, resting, withState), so the tiles are the
// master's own. The design's 10.1" keypad runs the 7"'s 1024 x 600 layout (docs/features/06), so
// these are its screens too.

const KP_HOME = 'bl-216-kp-home';
const KP_DEVICES = 'bl-216-kp-devices';
const CODES_KP = 'Factory codes · Change them on the alarm panel';
const ONLINE_BAR = {banner: 'codes', bannerText: CODES_KP};

// A keypad's main screen (panel-link.js kp()) with Smart Home in its rail, outlined as the change.
const kpMain = ({tone = 'green', bar = ONLINE_BAR, ...o}) => M.main({tone, rail: {devices: true}, bar: {time: '10:42', ...bar}, ...o})
  .replace(/<div class="nav_item two([^"]*)" data-go="devices">([\s\S]*?)<\/div>/,
    (_, c, inner) => M.changed(`<div class="nav_item two${c}" data-go="${KP_DEVICES}">${inner}</div>`))
  .replace('data-go="home-ready"', `data-go="${KP_HOME}"`);

// ---- copied from screens/views.js: the master's Smart Home page and its devices ----
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
const devicesView = ({note, area = 0, tiles}) => `<div class="view column">
  <div class="row" style="gap:var(--space_md)">${M.view_header({title: 'Smart Home', note})}${M.segments(DEVICE_AREAS, area)}
    <div class="round_button">${M.icon('pencil', 20)}</div></div>
  <div class="device_grid scroll grow">${tiles.map(M.device_tile).join('')}</div></div>`;
const withState = (tiles, name, o) => tiles.map((t) => (t.name === name ? {...t, ...o} : t));
const resting = (html, c) => html.replace(`class="${c}"`, `class="${c} disabled"`);
const FAVORITES_NOTE = 'Back Door unlocked · 2 lights on';

M.screen({
  id: KP_HOME, area: 'Keypad', title: 'Keypad: Home, Smart Home in the rail', shot: null,
  proposal: {bl: 'BL-216', status: 'approved', replaces: 'kp-home-ready',
    decided: '2026-10-16: "Approve P1, recommendations OK"',
    why: 'The owner asked that Smart Home work on a keypad as on the alarm panel: today a keypad\'s rail has no Smart Home, so the house\'s lights and locks are reached only at the alarm panel.'},
  notes: 'The keypad\'s Home as today (kp-home-ready), with Smart Home in the rail between Zones and Activity, as on the alarm panel: five places with Panic, the rail\'s limit. It shows under the master\'s rule, at least one listed device that isn\'t from Z-Wave, here from Harbor House\'s list (LINK-044). After a keypad restart it shows once the list is in. A keypad whose alarm panel has no Smart Home, an older one, or a keypad built without home control keeps today\'s rail. Every other keypad screen (Zones, Activity, Settings) gets the same rail.',
  render: () => kpMain({content: M.home({tone: 'green', glyph: 'shield-check', headline: 'Disarmed', detail: 'Ready to arm', note: 'All zones closed'}, M.panel_arm())}),
});

M.screen({
  id: KP_DEVICES, area: 'Keypad', title: 'Keypad: Smart Home, the alarm panel\'s devices', shot: null,
  proposal: {bl: 'BL-216', status: 'approved', replaces: null,
    decided: '2026-10-16: "Approve P1, recommendations OK"',
    why: 'Someone at a keypad controls the house\'s devices as at the alarm panel: the same page, tiles and words, so nothing new is learned.'},
  notes: 'The alarm panel\'s Smart Home page (devices), unchanged: Favorites, the areas, the tiles, the note and the sheets, with Harbor House\'s devices and states. A change shows within 1 s of Harbor House\'s own screen on a working Wi-Fi (LINK-045). Each tap goes to Harbor House, which judges it with the same function its own screen uses (LINK-048). The pencil arranges this keypad\'s page only (decision 2). Settings > Integrations and Automations stay on the alarm panel. Only the rail item is outlined: nothing on the page is new.',
  render: () => kpMain({view: 'devices', content: devicesView({note: FAVORITES_NOTE, tiles: DEVICE_FAVORITES})}),
});

M.screen({
  id: 'bl-216-kp-device-unlock', area: 'Keypad', title: 'Keypad: Smart Home, unlocking asks for a code', shot: null,
  proposal: {bl: 'BL-216', status: 'approved', replaces: null,
    decided: '2026-10-16: "Approve P1, recommendations OK"',
    why: 'Unlocking or opening from a keypad needs a code, as at the alarm panel (HCTL-005), and the code is checked where codes live.'},
  notes: 'The alarm panel\'s pad (device-unlock): "Unlock Front Door". The keypad knows an unlock needs a code, so the pad opens at once, without asking Harbor House first; Harbor House checks the code as this keypad\'s and judges the unlock again (LINK-047, LINK-048). Only a code valid for this keypad\'s home partition works (decision 1). Arm-only and guest codes are refused. A wrong code shows its reason on the pad, as on the alarm panel, and counts toward this keypad\'s own lockout (5 wrong codes), never the alarm panel\'s. The pad waits at most 3 s for the answer. Locking and closing never ask.',
  render: () => kpMain({view: 'devices', content: devicesView({note: FAVORITES_NOTE, tiles: DEVICE_FAVORITES}),
    over: M.modal_pad({title: 'Unlock Front Door', tone: 'amber', glyph: 'lock-open'})}),
});

// Every tile offline: the master's look when Home Assistant drops (ui_devices.c tile_words, note).
const LOST = 'Offline since 9:12 PM';
const FAVORITES_LOST = [
  {glyph: 'lightbulb', name: 'Porch Light', state: LOST, wide: true, offline: true,
    control: resting(M.device_level({value: 80, tone: 'gray'}), 'device_level')},
  {glyph: 'lock', name: 'Front Door', state: LOST, offline: true},
  {glyph: 'lock-open', name: 'Back Door', state: LOST, offline: true},
  {glyph: 'warehouse', name: 'Garage Door', state: LOST, offline: true},
  {glyph: 'thermometer', name: 'Hallway', state: LOST, wide: true, offline: true,
    control: resting(M.device_stepper({label: 'Heat to', value: '72°'}), 'device_stepper')},
  {glyph: 'sparkles', name: 'Good Night', state: LOST, more: false, offline: true},
  {glyph: 'lightbulb', name: 'Living Room Lamp', state: LOST, wide: true, offline: true,
    control: resting(M.device_level({value: 40, tone: 'gray'}), 'device_level')},
  {glyph: 'lightbulb', name: 'Kitchen Lights', state: LOST, offline: true},
];

M.screen({
  id: 'bl-216-kp-devices-lost', area: 'Keypad', title: 'Keypad: Smart Home while Harbor House can\'t be reached', shot: null,
  proposal: {bl: 'BL-216', status: 'approved', replaces: null,
    decided: '2026-10-16: "Approve P1, recommendations OK"',
    why: 'When the keypad loses Harbor House, its devices\' last states must not read as current, and a tap must not seem to work.'},
  notes: 'The link is down, or no device data came for 30 s (LINK-046): every tile reads "Offline since 9:12 PM", the time of the last device data, and the note "Your devices can\'t be reached right now". This is the alarm panel\'s own look when Home Assistant drops (HCTL-006, BL-181): names and times readable, chips grey at 70 %, controls resting at 40 % in grey, no arrows. A tap sends nothing. The frame is the keypad\'s while it has no system communication (kp-home-lost): grey logo, no factory-code reminder, Chime dimmed. Where only the device data stops and Harbor House still answers, the tiles and the note are the same and the frame stays the connected one. When device data comes again, each tile shows its state in one step. When only Harbor House\'s Home Assistant link drops, both screens show offline since the drop. The mockup has no screen of the alarm panel in this state yet, and the preview has no picture of it. On the panel the note is one line: in the body font the sentence is about 365 px, the line beside Favorites\' four tabs about 344 px, and today\'s note() makes no room from the tabs on this path, so the alarm panel likely ends it in "…" (not seen on a panel or in the preview). Drawn whole here: making room as the other notes do is a fix of today\'s screen, with no new words.',
  render: () => kpMain({view: 'devices', tone: 'gray', bar: {banner: null, chimeOff: true, time: '9:13'},
    content: devicesView({note: 'Your devices can\'t be reached right now', tiles: FAVORITES_LOST})}),
});

M.screen({
  id: 'bl-216-kp-devices-unconfirmed', area: 'Keypad', title: 'Keypad: Smart Home, an unlock not confirmed', shot: null,
  proposal: {bl: 'BL-216', status: 'approved', replaces: null,
    decided: '2026-10-16: "Approve P1, recommendations OK"',
    why: 'An action whose answer was lost may still have happened: the keypad says so and never sends it again by itself (HCTL-012).'},
  notes: 'After the code, Harbor House\'s answer didn\'t come: the keypad resent the request every 500 ms for 3 s, then gave up (LINK-047). The pad waited those 3 s and closed. Now Front Door\'s tile says "Not confirmed" in amber and the note "Couldn\'t confirm Front Door unlocked" (the catalog\'s "Couldn\'t confirm {0} unlocked", devices-failed-note). An action without a code (a light) holds the screen at most 1 s, then shows as under way until the answer or the give-up ("Turning on…" with its chip dimmed, the alarm panel\'s words). Back Door was locked meanwhile, so the outcome is the note\'s first fact; "2 lights on" doesn\'t fit beside it and gives way whole, as on the alarm panel. On the panel the note is one line (about 340 px of 344); the mockup\'s tabs are wider, so it wraps here, as today\'s long notes do. Harbor House runs a request at most once, so a copy that arrives late never unlocks twice. A tap tries again, with the code pad. The next state from Harbor House replaces the words.',
  render: () => kpMain({view: 'devices', content: devicesView({note: 'Couldn\'t confirm Front Door unlocked',
    tiles: withState(withState(DEVICE_FAVORITES, 'Front Door', {state: 'Not confirmed', stateTone: 'amber'}),
      'Back Door', {glyph: 'lock', tone: 'green', state: 'Locked'})})}),
});
