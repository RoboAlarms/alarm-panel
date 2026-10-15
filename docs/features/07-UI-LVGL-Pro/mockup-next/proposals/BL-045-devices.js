// BL-045: the Devices screen's tiles (docs/features/24), with the redesign's rail (BL-131): Smart
// Home, a house with Wi-Fi, beside Security (decided 2026-10-01). Ideas
// taken from Tessera's tiles (patterns only, no code or pictures: it is AGPL-3.0): one or two
// cells, the control on the tile, the state in words and colour, including what is under way or
// failed, a set point between minus and plus, a cover's Open, Stop and Close. Not taken: its
// two-tap Confirm circle that beats (UI-020; an unlock takes a code here, HCTL-005), a spinner
// over a waiting tile (the words say it; nothing moves) and a pastel background per tile (here
// the colour is the state).

const P = (why) => ({bl: 'BL-045', status: 'approved', decided: '2026-10-01: approved as drawn: "start working on getting all of the screen changes in place"', why});

// A Favorites page: twelve cells, three rows, no scrolling.
const FAVORITES = [
  {glyph: 'lightbulb', tone: 'blue', name: 'Porch Light', state: 'On · 80%', wide: true, control: M.device_level({value: 80})},
  {glyph: 'lock', tone: 'green', name: 'Front Door', state: 'Locked'},
  {glyph: 'lock-open', tone: 'amber', name: 'Back Door', state: 'Unlocked'},
  {glyph: 'warehouse', tone: 'green', name: 'Garage Door', state: 'Closed'},
  {glyph: 'thermometer', tone: 'orange', name: 'Hallway', state: 'Heating · 71° now', wide: true, control: M.device_stepper({label: 'Heat to', value: '72°'})},
  {glyph: 'moon', tone: 'violet', name: 'Good Night', state: 'Ran at 11:05 PM', more: false},
  {glyph: 'lamp', tone: 'blue', name: 'Living Room Lamp', state: 'On · 40%', wide: true, control: M.device_level({value: 40})},
  {glyph: 'lightbulb', name: 'Kitchen Lights', state: 'Off'},
  {glyph: 'cloud-sun', name: 'Patio', state: '58° · 64% humidity', more: false},
];

// Outside: what a tile shows under way, failed and offline, and the kinds Favorites hasn't.
const OUTSIDE = [
  {glyph: 'lightbulb', tone: 'blue', name: 'Driveway Light', state: 'On · 60%', wide: true, control: M.device_level({value: 60})},
  {glyph: 'warehouse', tone: 'amber', name: 'Garage Door', state: 'Closing…', pending: true},
  {glyph: 'lock', tone: 'amber', name: 'Side Gate', state: 'Jammed'},
  {glyph: 'lightbulb', name: 'Patio Lights', state: 'Didn\'t turn on', stateTone: 'amber'},
  {glyph: 'plug', name: 'Pool Pump', state: 'Offline since 9:12 PM', offline: true},
  {glyph: 'blinds', tone: 'blue', name: 'Patio Awning', state: 'Open', wide: true,
    control: M.device_keys({keys: [{glyph: 'arrow-up', text: 'Open'}, {glyph: 'square', text: 'Stop'}, {glyph: 'arrow-down', text: 'Close'}]})},
  {glyph: 'fan', tone: 'blue', name: 'Porch Fan', state: 'On · Low', wide: true, control: M.device_keys({keys: ['Off', 'Low', 'Medium', 'High'], active: 1})},
  {glyph: 'droplet', name: 'Garden Water', state: 'Closed'},
  {glyph: 'cloud-sun', name: 'Patio', state: '58° · 64% humidity', more: false},
];

const AREAS = ['Favorites', 'Living room', 'Kitchen', 'Outside'];
const devices = ({note, area = 0, tiles}) => `<div class="view column">
  <div class="row" style="gap:var(--space_md)">${M.view_header({title: 'Smart Home', note})}${M.segments(AREAS, area)}
    <div class="round_button" data-go="n-devices-arrange">${M.icon('pencil', 20)}</div></div>
  <div class="device_grid scroll grow">${tiles.map(M.device_tile).join('')}</div></div>`;
const favorites = () => devices({note: 'Back Door unlocked · 2 lights on', tiles: FAVORITES});

// Arranging a page: the header holds the actions, the tiles show grips. pick: the tile picked.
const arrangeHead = (pick) => (pick
  ? `${M.view_header({title: pick.name, note: 'Use the arrows, or drag it'})}
    <div class="round_button">${M.icon('arrow-left', 20)}</div><div class="round_button">${M.icon('arrow-right', 20)}</div>
    ${M.segments(['Small', 'Wide'], pick.wide ? 1 : 0)}${M.button('Remove', '', 'star-off')}${M.button('Done', 'primary')}`
  : `${M.view_header({title: 'Arrange Favorites', note: 'Drag a tile, or tap it to move, resize or remove it'})}
    <div data-go="n-devices-arrange-add">${M.button('Add', '', 'plus')}</div><div data-go="n-devices">${M.button('Done', 'primary')}</div>`);
const arrange = (pick) => `<div class="view column">
  <div class="row" style="gap:var(--space_md)">${arrangeHead(pick)}</div>
  <div class="device_grid scroll grow">${FAVORITES.map((t) => M.device_tile({...t, arrange: true, selected: pick === t})).join('')}</div></div>`;

M.screen({
  id: 'n-devices', area: 'Smart Home', title: 'Smart Home: Favorites, with wide tiles', shot: null,
  proposal: P('Where the rail\'s Smart Home leads (docs/features/24\'s Devices screen, HCTL-003; a house with Wi-Fi beside Security). A tile does what a tap can do: switch, lock, run. A device that needs more than a tap gets a tile two cells wide with its control on it: a dimmable light its brightness bar, a thermostat its set point between minus and plus, a cover with a position its Open, Stop and Close, a fan its speeds. Twelve cells fill the view, so Favorites needs no scrolling. The note under the title says what needs you and what is on, not how to use the screen.'),
  notes: 'A tap: a light, switch or fan switches; a lock locks, or asks for a code to unlock (HCTL-005); a garage door closes, or asks for a code to open; a scene runs (no arrow; its words say when it last ran); a sensor only reads. Dragging a light\'s bar sets its brightness; minus and plus move the set point a degree. The arrow, or a long press, opens the detail sheet (HCTL-004). The pencil arranges the page (n-devices-arrange). A tile starts at its kind\'s width and keeps the order it is given; a one-cell tile fills a gap a wide one leaves at a row\'s end. Colour is the state: blue on, green locked or closed, amber unlocked or open, orange heating, grey off. The note is live words: at most once a second, after the input settles (UI-023).',
  render: () => M.main({next: true, view: 'devices', content: favorites()}),
});

M.screen({
  id: 'n-devices-outside', area: 'Smart Home', title: 'Smart Home: under way, failed and offline', shot: null,
  proposal: P('How a tile shows an action under way, an action that failed and a device that is offline (HCTL-006), in words and without motion; and the kinds Favorites doesn\'t show: a cover, a fan and a valve.'),
  notes: 'Garage Door is closing: the words say so and its chip dims. When the action\'s time is up (HCTL-012), the words say what didn\'t happen, in amber, as on Patio Lights; a tap tries again, and nothing retries by itself. Side Gate reports a jam. Pool Pump is offline: its words stay readable and say since when, its chip dims and it has no arrow (BL-181). If the whole link is down, every tile says so and so does the note, without naming the integration (UI-018). Garden Water is a valve: opening it asks for a code (HCTL-005), closing it doesn\'t. Blinds and awnings are blue open and grey closed, like a light; a garage door, an entrance, is green closed and amber open.',
  render: () => M.main({next: true, view: 'devices', content: devices({note: 'Side Gate jammed · Pool Pump offline', area: 3, tiles: OUTSIDE})}),
});

M.screen({
  id: 'n-device-light', area: 'Smart Home', title: 'A light\'s sheet: brightness and warmth', shot: null,
  proposal: P('What the arrow (or a long press) opens on a light: its switch beside the name, then brightness and warmth. A change applies at once, so the sheet closes with an X, with no Cancel or Done.'),
  notes: 'Colour isn\'t drawn: LVGL 9 has no colour wheel, so it would be a row of presets (later). A light that only switches has no sheet worth opening; its tile has no arrow.',
  render: () => M.main({next: true, view: 'devices', content: favorites(),
    over: M.device_sheet({glyph: 'lightbulb', tone: 'blue', title: 'Porch Light', note: 'On · 80%', on: true,
      body: `<div class="col" style="gap:var(--space_sm)">${M.row_level({glyph: 'sun', title: 'Brightness', value: 80})}
        ${M.row_level({glyph: 'thermometer-sun', title: 'Warmth', value: 30, label: 'Warm white'})}</div>`})}),
});

M.screen({
  id: 'n-device-climate', area: 'Smart Home', title: 'A thermostat\'s sheet: set point, mode and fan', shot: null,
  proposal: P('A thermostat\'s sheet: the set point large between minus and plus, and the mode and the fan as keys, the lit one in the state\'s colour (orange heating, blue cooling).'),
  notes: 'The tile already has minus and plus, so the sheet is for the mode and the fan. Degrees follow the region setting (°F or °C).',
  render: () => M.main({next: true, view: 'devices', content: favorites(),
    over: M.device_sheet({glyph: 'thermometer', tone: 'orange', title: 'Hallway', note: 'Heating · 71° now · 40% humidity',
      body: `${M.device_setpoint({label: 'Heat to', value: '72°', tone: 'orange'})}
        ${M.section('Mode')}${M.device_keys({keys: [{glyph: 'power', text: 'Off'}, {glyph: 'flame', text: 'Heat'}, {glyph: 'snowflake', text: 'Cool'}, {glyph: 'sun-snow', text: 'Auto'}], active: 1, tone: 'orange'})}
        ${M.section('Fan')}${M.device_keys({keys: ['Auto', 'On'], active: 0})}`})}),
});

M.screen({
  id: 'n-device-unlock', area: 'Smart Home', title: 'Unlocking asks for a code', shot: null,
  proposal: P('A tap on a locked lock opens the code pad over the screen, the one disarming uses (HCTL-005); the unlock is logged with who did it (HCTL-007). Locking never asks.'),
  notes: 'Opening a garage door, a cover marked as an entrance or a water valve asks the same way. A wrong code counts toward the keypad\'s lockout, as any code does.',
  render: () => M.main({next: true, view: 'devices', content: favorites(),
    over: M.modal_pad({title: 'Unlock Front Door', tone: 'amber', glyph: 'lock-open'})}),
});

// ---- Arranging the screen (decided 2026-10-01: people lay the screen out as they want) ----
M.screen({
  id: 'n-devices-arrange', area: 'Smart Home', title: 'Arranging Favorites', shot: null,
  proposal: P('The pencil beside the rooms arranges the page you are on, right there: drag a tile to move it, or tap it to move it with arrows, make it small or wide, or take it off Favorites; Add puts more devices on Favorites. Tiles show a grip and a dashed edge, and a tap picks a tile instead of switching it, so nothing is switched by mistake while arranging.'),
  notes: 'No code: it only changes how the screen looks, as brightness does; hiding and renaming a device stay in Settings > Devices with the master code (HCTL-008 changes: reordering, sizes and favourites move here). The layout is the panel\'s, the same for everyone. Tiles that make room for a dragged one jump into place without sliding, as nothing on the panel animates (UI-020); the arrows are the way to move a tile without dragging (WCAG 2.5.7). An area page arranges the same way, without Add and Remove.',
  render: () => M.main({next: true, view: 'devices', content: arrange(null)}),
});

M.screen({
  id: 'n-devices-arrange-tile', area: 'Smart Home', title: 'Arranging: one tile picked', shot: null,
  proposal: P('A tile tapped while arranging is picked: edged in blue, and the header turns into its actions: the arrows move it one place earlier or later, Small and Wide set its size, Remove takes it off Favorites (it stays in its room), Done ends arranging.'),
  notes: 'Any tile can be small or wide. A small tile keeps its main action and its arrow; a wide one shows its control too (brightness, set point, keys), or, for a kind without one, its state in more room. Tapping another tile picks that one.',
  render: () => M.main({next: true, view: 'devices', content: arrange(FAVORITES[0])}),
});

const ADD = [
  {glyph: 'lightbulb', color: 'blue', title: 'Driveway Light', detail: 'Outside · On · 60%'},
  {glyph: 'blinds', color: 'blue', title: 'Patio Awning', detail: 'Outside · Open'},
  {glyph: 'fan', color: 'blue', title: 'Porch Fan', detail: 'Outside · On · Low'},
  {glyph: 'lock', color: 'amber', title: 'Side Gate', detail: 'Outside · Jammed'},
  {glyph: 'droplet', color: 'gray', title: 'Garden Water', detail: 'Outside · Closed'},
];
M.screen({
  id: 'n-devices-arrange-add', area: 'Smart Home', title: 'Arranging: adding to Favorites', shot: null,
  proposal: P('Add lists the devices not on Favorites yet, by room; a tap adds one at the end of the page, at its kind\'s size, and the list stays open for the next.'),
  notes: 'Only devices the panel was given show here (picked in Settings > Devices, HCTL-008); a hidden one doesn\'t.',
  render: () => M.main({next: true, view: 'devices', content: arrange(null),
    over: M.pick_sheet({title: 'Add to Favorites', note: 'Devices in your rooms', fit: true,
      rows: ADD.map((d) => M.row_link({...d, trail: 'plus', trailColor: 'blue'})).join('')})}),
});
