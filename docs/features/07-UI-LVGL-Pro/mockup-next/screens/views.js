// The rail's other views: Zones, Activity, Troubles, Emergency (screen_main.xml), the code pad
// over the screen (modal_pad), and the screen saver (screen_saver.xml).

const ZONES = [
  {glyph: 'door-closed', title: 'Front Door', detail: 'Closed'},
  {glyph: 'warehouse', title: 'Garage Door', detail: 'Closed'},
  {glyph: 'app-window', title: 'Living Room Window', detail: 'Open', tone: 'amber'},
  {glyph: 'door-closed', title: 'Back Door', detail: 'Closed'},
  {glyph: 'footprints', title: 'Hallway Motion', detail: 'Bypassed · No motion', tone: 'text_2', bypass: 'Unbypass'},
  {glyph: 'alarm-smoke', title: 'Kitchen Smoke', detail: 'Normal', bypass: ''},
  {glyph: 'cloud-fog', title: 'Hallway CO', detail: 'Normal', bypass: ''},
  {glyph: 'droplets', title: 'Basement Water', detail: 'Low battery', tone: 'amber', bypass: ''},
];
const VIEW_RAIL = {zones: 1, troubles: 1};
const VIEW_BAR = {time: '10:48', troubles: 1};

M.screen({
  id: 'zones', area: 'Views', title: 'Zones', shot: 'zones',
  notes: 'A card per zone, its icon tinted by state. Fire, CO and water zones can\'t be bypassed, so they have no button.',
  render: () => M.main({view: 'zones', tone: 'amber', rail: VIEW_RAIL, bar: VIEW_BAR, content: `<div class="view column">
    <div class="row">${M.view_header({title: 'Zones', note: '8 zones · 1 open · 1 bypassed'})}${M.segments(['All', 'Open', 'Bypassed', 'Trouble'])}</div>
    <div class="zone_list scroll grow">${ZONES.map(M.zone_card).join('')}</div></div>`}),
});

// An empty filter (BL-177; screen_main.xml zones_none): the green check Troubles shows when there are
// none, and the filter's words, in the list's place.
const zonesEmpty = ({filter, words, note, troubles = 0}) => M.main({view: 'zones', bar: {time: '10:48', troubles}, content: `<div class="view column">
  <div class="row">${M.view_header({title: 'Zones', note})}${M.segments(['All', 'Open', 'Bypassed', 'Trouble'], filter)}</div>
  <div class="col center grow" style="gap:var(--space_md)">${M.icon_chip({glyph: 'circle-check', size: 112, font: 40, color: 'green'})}
    ${M.text(words, 'title')}</div></div>`});

M.screen({
  id: 'zones-open-none', area: 'Views', title: 'Zones, Open with none open', shot: 'zones_open_none',
  notes: 'A filter with no zone in it says so (BL-177): the green check and "No open zones", "No bypassed zones" or "No zone troubles", in the list\'s place. A filter tap, or opening the view, shows it at once. From a zone\'s change the list and the check swap only once the list has settled, at most once a second either way, so a chattering contact never pulses the check in and out (UI-020, UI-023). Here zone 3 closed with Open picked: the check came a second later.',
  render: () => zonesEmpty({filter: 1, words: 'No open zones', note: '8 zones · 0 open · 1 bypassed', troubles: 1}),
});

M.screen({
  id: 'zones-bypassed-none', area: 'Views', title: 'Zones, Bypassed with none bypassed', shot: 'zones_bypassed_none',
  notes: 'The quiet house, Bypassed picked: "No bypassed zones", at once (a tap).',
  render: () => zonesEmpty({filter: 2, words: 'No bypassed zones', note: '8 zones · 0 open · 0 bypassed'}),
});

M.screen({
  id: 'zones-trouble-none', area: 'Views', title: 'Zones, Trouble with none in trouble', shot: 'zones_trouble_none',
  notes: '"No zone troubles": the filter counts a zone\'s trouble, tamper, low battery and lost supervision. Not "No troubles", the Troubles view\'s words for the whole panel (power, network, sensors).',
  render: () => zonesEmpty({filter: 3, words: 'No zone troubles', note: '8 zones · 0 open · 0 bypassed'}),
});

M.screen({
  id: 'activity', area: 'Views', title: 'Activity', shot: 'activity',
  notes: 'The event sentences come from components/core/notify/src/notify_words.c (one copy for the screen and a phone).',
  render: () => M.main({view: 'activity', tone: 'amber', rail: VIEW_RAIL, bar: VIEW_BAR, content: `<div class="view column">
    ${M.view_header({title: 'Activity', note: 'Newest first · Settings has the full event log'})}
    <div class="list scroll">${[
      {glyph: 'circle-slash', title: 'Hallway Motion bypassed', detail: 'By Master'},
      {glyph: 'battery-warning', title: 'Basement Water · Low battery', detail: 'Check the sensor', tone: 'amber'},
      {glyph: 'check', title: 'Alarm memory cleared', detail: 'By Master'},
      {glyph: 'lock-open', title: 'Disarmed', detail: 'By Master', tone: 'green'},
      {glyph: 'circle-check', title: 'Alarm aborted', detail: 'Disarmed before it was reported', tone: 'green'},
      {glyph: 'siren', title: 'Alarm right after arming', detail: 'Front Door', tone: 'red'},
      {glyph: 'lock', title: 'Armed Away', detail: 'By Master', tone: 'rose'},
    ].map(M.event_row).join('')}</div></div>`}),
});

const troublesHead = (n) => `<div class="row">${M.view_header({title: 'Troubles', note: n})}
  ${n === 'No troubles' ? '' : `<div class="style_button font_label">${M.icon('check', 20)}Acknowledge</div>`}</div>`;

M.screen({
  id: 'troubles', area: 'Views', title: 'Troubles', shot: 'troubles',
  render: () => M.main({view: 'troubles', tone: 'amber', rail: {zones: 1, troubles: 2}, bar: {...VIEW_BAR, troubles: 2, wifi: 'wifi-off', wifiOk: false},
    content: `<div class="view column">${troublesHead('2 active')}<div class="list scroll">
      ${M.trouble_row({glyph: 'zap', title: 'Mains power lost', detail: 'Running on battery'})}
      ${M.trouble_row({glyph: 'battery-warning', title: 'Basement Water · Low battery', detail: 'Check the sensor'})}</div></div>`}),
});

M.screen({
  id: 'troubles-none', area: 'Views', title: 'Troubles, none', shot: null,
  notes: 'Not in the preview\'s walk; drawn from screen_main.xml "troubles_none".',
  render: () => M.main({view: 'troubles', bar: {time: '10:48'}, content: `<div class="view column">${troublesHead('No troubles')}
    <div class="col center grow" style="gap:var(--space_md)">${M.icon_chip({glyph: 'circle-check', size: 112, font: 40, color: 'green'})}
      ${M.text('No troubles', 'title')}${M.text('Power, network and sensors are all fine', 'body', 'text_2')}</div></div>`}),
});

M.screen({
  id: 'panic', area: 'Views', title: 'Emergency, a button held', shot: 'emergency_hold',
  notes: 'Hold 2 s; the ring fills while held. The small print says what the panel can pass on, never a promise of help (UI-029).',
  render: () => M.main({view: 'panic', rail: {troubles: 1}, bar: {time: '10:49', troubles: 1, wifi: 'wifi-low'},
    content: `<div class="view col center" style="gap:var(--space_xxl)">
      <div class="col center" style="gap:4px">${M.text('Emergency', 'headline')}
        ${M.text('Hold a button for 2 seconds to set off the alarm on this panel', 'body', 'text_2')}
        ${M.text('Nothing is connected to pass this on · The panel can\'t call emergency services', 'caption', 'text_3')}</div>
      <div class="row" style="gap:56px">${M.panic_button({glyph: 'siren', text: 'Police', color: 'red', held: 0.6})}
        ${M.panic_button({glyph: 'flame', text: 'Fire', color: 'orange'})}${M.panic_button({glyph: 'heart-pulse', text: 'Medical', color: 'rose'})}</div></div>`}),
});

M.screen({
  id: 'chime-pad', area: 'Views', title: 'Chime: the code pad over the screen', shot: 'chime_panel',
  notes: 'modal_pad (UI-025): the panel stays on the view you were on and says what the code will do.',
  render: () => M.main({view: 'zones', tone: 'amber', rail: {troubles: 1}, bar: {time: '12:45', ampm: 'AM', troubles: 1, wifi: 'wifi-off', wifiOk: false},
    content: `<div class="view column"><div class="row">${M.view_header({title: 'Zones', note: '10 zones · 0 open'})}${M.segments(['All', 'Open', 'Bypassed', 'Trouble'])}</div>
      <div class="zone_list scroll grow">${ZONES.slice(0, 6).map((z) => M.zone_card({...z, tone: 'green', detail: 'Closed', bypass: 'Bypass'})).join('')}</div></div>`,
    over: M.modal_pad({title: 'Turn chime on', tone: 'blue', glyph: 'bell',
      note: 'A beep at the panel each time a door or window opens, while the system is disarmed'})}),
});

M.screen({
  id: 'screen-saver', area: 'Views', title: 'Screen saver', shot: 'screen_saver',
  notes: 'A dimmed photo; nothing moves. Any touch wakes the panel; entry time, alarms and troubles wake it on their own.',
  render: () => M.saver({trouble: '1 trouble'}),
});

M.screen({
  id: 'screen-saver-armed', area: 'Views', title: 'Screen saver, armed', shot: 'screen_saver_armed',
  render: () => M.saver({tone: 'rose', glyph: 'lock', headline: 'Armed Away', detail: 'The whole house is protected', note: 'Since 10:46 PM · By Master'}),
});

// ---- Smart Home (docs/features/24, BL-045; ui_devices.c): mockup-next's proposals/BL-045-devices.js
// n-devices, n-devices-outside and n-device-unlock, as built. The pencil arranges the page (devices-arrange, below); the detail sheets follow. A scene shows sparkles (the
// device model has no icon of its own); a sensor readout (Patio) isn't a device kind yet.
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
const DEVICE_OUTSIDE = [
  DEVICE_PORCH,
  {glyph: 'warehouse', tone: 'amber', name: 'Garage Door', state: 'Closing…', pending: true},
  {glyph: 'lock', tone: 'amber', name: 'Side Gate', state: 'Jammed'},
  {glyph: 'lightbulb', name: 'Patio Lights', state: 'Didn\'t turn on', stateTone: 'amber'},
  {glyph: 'plug', name: 'Pool Pump', state: 'Offline since 9:12 PM', offline: true},
  {glyph: 'blinds', tone: 'blue', name: 'Patio Awning', state: 'Open', wide: true,
    control: M.device_keys({keys: [{glyph: 'arrow-up', text: 'Open'}, {glyph: 'square', text: 'Stop'}, {glyph: 'arrow-down', text: 'Close'}]})},
  {glyph: 'fan', tone: 'blue', name: 'Porch Fan', state: 'On · Low', wide: true, control: M.device_keys({keys: ['Off', 'Low', 'Medium', 'High'], active: 1})},
  {glyph: 'droplet', name: 'Garden Water', state: 'Closed'},
];
const devicesView = ({note, area = 0, tiles}) => `<div class="view column">
  <div class="row" style="gap:var(--space_md)">${M.view_header({title: 'Smart Home', note})}${M.segments(DEVICE_AREAS, area)}
    <div class="round_button" data-go="devices-arrange">${M.icon('pencil', 20)}</div></div>
  <div class="device_grid scroll grow">${tiles.map(M.device_tile).join('')}</div></div>`;
const devicesMain = (o) => M.main({next: true, view: 'devices', tone: 'amber', rail: VIEW_RAIL, bar: VIEW_BAR, ...o});

M.screen({
  id: 'devices', area: 'Smart Home', title: 'Smart Home: Favorites', shot: 'devices',
  notes: 'The rail\'s Smart Home, shown while there is a device (HCTL-003). Favorites first, then the areas. A one-cell tile does what a tap can do; a two-cell tile carries its control: a dimmable light\'s brightness, a thermostat\'s set point. The note says what needs you and what is on, at most once a second (UI-023). Unlocking asks for a code (device-unlock).',
  render: () => devicesMain({content: devicesView({note: 'Back Door unlocked · 2 lights on', tiles: DEVICE_FAVORITES})}),
});

M.screen({
  id: 'devices-outside', area: 'Smart Home', title: 'Smart Home: under way, failed and offline', shot: 'devices_outside',
  notes: 'Garage Door is closing: the words say so and its chip dims. Patio Lights\' last action failed (amber): a tap tries again, nothing retries by itself (HCTL-012). Pool Pump is offline: its name and since when as clear as any tile\'s, the chip dimmed to 70 %, no arrow (HCTL-006, BL-181). A cover\'s Open, Stop and Close, a fan\'s speeds.',
  render: () => devicesMain({content: devicesView({note: 'Side Gate jammed · Pool Pump offline', area: 3, tiles: DEVICE_OUTSIDE})}),
});

M.screen({
  id: 'device-unlock', area: 'Smart Home', title: 'Smart Home: unlocking asks for a code', shot: 'device_unlock',
  notes: 'A tap on a locked lock opens the code pad over the screen, the one disarming uses (HCTL-005); the panel checks the code with the engine, and a wrong one counts toward the lockout. Opening a garage door or a valve asks the same way; locking and closing never do.',
  render: () => devicesMain({content: devicesView({note: 'Back Door unlocked · 2 lights on', tiles: DEVICE_FAVORITES}),
    over: M.modal_pad({title: 'Unlock Front Door', tone: 'amber', glyph: 'lock-open'})}),
});

// The detail sheets (BL-045 part B, HCTL-004; ui_devices.c): what the arrow, or a long press on any tile,
// opens. A change applies at once, so each closes with an X. As built, a sheet shows a control only where
// the device reports it: the light's warmth and the thermostat's fan need actions home_control doesn't
// carry yet, and the humidity isn't reported, so the drawing's n-device-light and n-device-climate lose them.
M.screen({
  id: 'device-light', area: 'Smart Home', title: 'A light\'s sheet: brightness', shot: 'device_light',
  notes: 'What the arrow, or a long press, opens on a light: its switch beside the name, then its brightness, a slider that applies when the finger lifts and shows its level while it moves. A light that only switches has no brightness row, and its tile no arrow (a long press still opens its sheet). The note says what the tile says: under way, failed, offline.',
  render: () => devicesMain({content: devicesView({note: 'Back Door unlocked · 2 lights on', tiles: DEVICE_FAVORITES}),
    over: M.device_sheet({glyph: 'lightbulb', tone: 'blue', title: 'Porch Light', note: 'On · 80%', on: true,
      body: M.row_level({glyph: 'sun', title: 'Brightness', value: 80})})}),
});

// A thermostat's words (HCTL-004, BL-171; ui_devices.c thermostat_words): what it is doing where its
// source says (Home Assistant's hvac_action), else its mode's label, and the room's temperature, one
// catalog sentence each. The set point's label, the lit mode key and the colour say the mode. The
// sheets open over Favorites as the preview's walk has it then (Kitchen Lights on).
const withState = (tiles, name, o) => tiles.map((t) => (t.name === name ? {...t, ...o} : t));
const DEVICE_FAVORITES_LIT = withState(DEVICE_FAVORITES, 'Kitchen Lights', {tone: 'blue', state: 'On · 100%'});
const CLIMATE_MODES = [{glyph: 'power', text: 'Off'}, {glyph: 'flame', text: 'Heat'}, {glyph: 'snowflake', text: 'Cool'}, {glyph: 'sun-snow', text: 'Auto'}];
const climateSheet = ({title, note, label, value, mode, tone}) => M.device_sheet({glyph: 'thermometer', tone, title, note,
  body: `${M.device_setpoint({label, value, tone})}
    ${M.section('Mode')}${M.device_keys({keys: CLIMATE_MODES, active: mode, tone})}`});
const hallwaySheet = (note) => devicesMain({content: devicesView({note: 'Back Door unlocked · 3 lights on', tiles: DEVICE_FAVORITES_LIT}),
  over: climateSheet({title: 'Hallway', note, label: 'Heat to', value: '72°', mode: 1, tone: 'orange'})});

M.screen({
  id: 'device-climate', area: 'Smart Home', title: 'A thermostat\'s sheet: set point and mode', shot: 'device_climate',
  notes: 'The set point large between minus and plus (a degree a tap), then the modes the thermostat has as keys, the lit one in the mode\'s colour (orange Heat, blue Cool and Auto). The note says what the thermostat is doing where Home Assistant says (its hvac_action): Hallway is heating, "Heating · 71° now". Without an action it says the mode\'s label ("Heat · 71° now"). The set point\'s label ("Heat to"), the lit key and the colour always say the mode. The tile already has minus and plus; the sheet is for the mode. A thermostat that doesn\'t report its modes shows only the set point.',
  render: () => hallwaySheet('Heating · 71° now'),
});

M.screen({
  id: 'device-climate-idle', area: 'Smart Home', title: 'A thermostat\'s sheet, idle in Heat', shot: 'device_climate_idle',
  notes: 'Home Assistant says Hallway is idle (on, but neither heating nor cooling): the note and the tile say "Idle · 71° now". Heat stays lit and "Heat to 72°" stays orange: the mode hasn\'t changed, only what the thermostat is doing about it. The words change at most once a second, as every tile\'s do (UI-023).',
  render: () => hallwaySheet('Idle · 71° now'),
});

const thermostat = (name, state, mode, setpoint) => ({glyph: 'thermometer', name, state, wide: true,
  tone: mode === 'heat' ? 'orange' : mode === 'off' ? 'gray' : 'blue',
  control: M.device_stepper({label: mode === 'heat' ? 'Heat to' : mode === 'cool' ? 'Cool to' : 'Set to', value: setpoint})});
const climateTiles = (den = 'Idle · 71° now') => [
  thermostat('Hallway', 'Idle · 72° now', 'heat', '70°'),
  {glyph: 'lock', tone: 'green', name: 'Front Door', state: 'Locked'},
  {glyph: 'sparkles', tone: 'violet', name: 'Good Night', state: 'Ran at 8:30 PM', more: false},
  thermostat('Bedroom', 'Preheating · 66° now', 'heat', '68°'),
  thermostat('Office', 'Cool · 77° now', 'cool', '74°'),
  thermostat('Den', den, 'auto', '70°'),
  thermostat('Basement', 'Off · 61° now', 'off', '65°'),
];

M.screen({
  id: 'devices-climate-doing', area: 'Smart Home', title: 'Thermostats: what they are doing', shot: null,
  notes: 'What each thermostat is doing comes first, the room\'s temperature second. Where Home Assistant says what it is doing (hvac_action): "Heating", "Cooling", "Preheating", "Idle" (on, but doing neither), "Off" - Hallway (Heat, 72° against 70°), Bedroom (Heat), Den (Auto) and Basement. Where it doesn\'t (an older integration, or an action without words: drying, fan, defrosting), the mode\'s label: "Heat", "Cool", "Auto", "Off", "On" for a dry or fan-only mode - Office (Cool) here. Never "Heating" or "Cooling" guessed from the mode, nor from the temperature against the set point. Each pair is one sentence in the catalog ("Idle · {0} now"); with the temperature unknown, the words alone ("Idle", "Heat"). The set point\'s label ("Heat to", "Cool to", "Set to"), the lit mode key and the colour say the mode, so Heat that is idle stays orange. At rest, whichever of off or idle the source sends, it reads "Idle" while its mode is Heat, Cool, Auto or On and "Off" only in mode Off (BL-171): Hallway rests in Heat, Basement is switched off. The words change at most once a second (UI-023).',
  render: () => devicesMain({content: devicesView({note: 'Nothing needs you', tiles: climateTiles()})}),
});

M.screen({
  id: 'device-climate-auto', area: 'Smart Home', title: 'A thermostat\'s sheet, heating in Auto', shot: null,
  notes: 'In Auto the lit key says the mode and the note what it is doing: "Heating · 68° now" (and "Cooling · 76° now" or "Idle · 71° now" in the same mode; "Auto · 68° now" where the action isn\'t known). The colours stay Auto\'s blue.',
  render: () => devicesMain({content: devicesView({note: 'Nothing needs you', tiles: climateTiles('Heating · 68° now')}),
    over: climateSheet({title: 'Den', note: 'Heating · 68° now', label: 'Set to', value: '70°', mode: 3, tone: 'blue'})}),
});

// At rest (HCTL-004, BL-171, decided 2026-10-07; ui_devices.c resting_action, inside
// thermostat_words): only a reported off or idle changes, and only by the reported mode: Heat, Cool,
// Auto or On make it Idle, Off makes it Off; an unknown mode keeps what was reported. Never from the
// modes the thermostat has, a mode asked for and still pending, the temperature against the set
// point, or which integration it is. Heating, Cooling and Preheating show as reported, even in mode
// Off; a missing action, or drying, fan or defrosting, keeps the mode's word. Presentation only: the
// reported action stays as it came. Offline, under way and failed words still come first.
const REST_TILES = [
  thermostat('Hallway', 'Idle · 72° now', 'heat', '70°'), // Heat / off
  thermostat('Office', 'Idle · 72° now', 'cool', '74°'), // Cool / off
  thermostat('Den', 'Idle · 72° now', 'auto', '70°'), // Auto / off
  thermostat('Basement', 'Off · 72° now', 'off', '65°'), // Off / idle
  thermostat('Bedroom', 'Preheating · 66° now', 'heat', '68°'),
  thermostat('Sunroom', 'Cooling · 77° now', 'cool', '74°'),
];
const restPage = (over) => devicesMain({content: devicesView({note: 'Nothing needs you', tiles: REST_TILES}), over});

M.screen({
  id: 'devices-climate-rest', area: 'Smart Home', title: 'Thermostats at rest: Idle, or Off when off', shot: null,
  notes: 'Hallway (Heat), Office (Cool) and Den (Auto) rest and their source says off (some thermostat integrations say off whenever the system rests): "Idle · 72° now". Basement\'s mode is Off and its source says idle: "Off · 72° now". Bedroom ("Preheating · 66° now") and Sunroom ("Cooling · 77° now") are busy. A thermostat that reports idle in Heat, Cool or Auto (Z-Wave JS\'s operating state, say) reads Idle as before. "Idle · {0} now" and "Off · {0} now" are the catalog\'s sentences; the words change at most once a second (UI-023).',
  render: () => restPage(),
});

M.screen({
  id: 'devices-climate-rest-more', area: 'Smart Home', title: 'Thermostats: what stays, and no temperature', shot: null,
  notes: 'Unchanged by the rest rule: "Heating · 64° now"; "Heating · 50° now" with the mode Off (an explicit Heating, Cooling or Preheating always shows); "Cooling · 75° now" in Auto; "Cool · 77° now" where no action is reported (drying, fan or defrosting keep the mode\'s word too). Without a temperature the word stands alone: Guest Room (Heat, reports off) "Idle", Workshop (Off, reports idle) "Off". With the mode unknown and no action, only "72° now".',
  render: () => devicesMain({content: devicesView({note: 'Nothing needs you', tiles: [
    thermostat('Attic', 'Heating · 64° now', 'heat', '68°'),
    thermostat('Garage', 'Heating · 50° now', 'off', '55°'),
    thermostat('Den', 'Cooling · 75° now', 'auto', '72°'),
    thermostat('Office', 'Cool · 77° now', 'cool', '74°'),
    thermostat('Guest Room', 'Idle', 'heat', '68°'),
    thermostat('Workshop', 'Off', 'off', '60°'),
  ]})}),
});

M.screen({
  id: 'device-climate-rest-heat', area: 'Smart Home', title: 'A thermostat\'s sheet, at rest in Heat', shot: 'device_climate_rest_heat',
  notes: 'Hallway rests in Heat and its source says off: the note and the tile say "Idle · 71° now", as for a source that says idle (device-climate-idle). Heat stays lit and "Heat to 72°" orange: the mode is the same, the thermostat just isn\'t heating. The reported action stays off.',
  render: () => hallwaySheet('Idle · 71° now'),
});

M.screen({
  id: 'device-climate-rest-cool', area: 'Smart Home', title: 'A thermostat\'s sheet, at rest in Cool', shot: null,
  notes: 'Office, Cool to 74° in a room at 72°, reports off: "Idle · 72° now"; Cool stays lit, "Cool to 74°" blue.',
  render: () => restPage(climateSheet({title: 'Office', note: 'Idle · 72° now', label: 'Cool to', value: '74°', mode: 2, tone: 'blue'})),
});

M.screen({
  id: 'device-climate-rest-auto', area: 'Smart Home', title: 'A thermostat\'s sheet, at rest in Auto', shot: null,
  notes: 'Den, Auto around 70° in a room at 72°, reports off: "Idle · 72° now"; Auto stays lit. Heating and Cooling in Auto read as reported (device-climate-auto).',
  render: () => restPage(climateSheet({title: 'Den', note: 'Idle · 72° now', label: 'Set to', value: '70°', mode: 3, tone: 'blue'})),
});

M.screen({
  id: 'device-climate-rest-off', area: 'Smart Home', title: 'A thermostat\'s sheet, in Off', shot: 'device_climate_rest_off',
  notes: 'Hallway switched to Off and its source says idle: "Off · 71° now" on the sheet and the tile, Off lit, "Set to 72°" in grey. A thermostat switched off is Off, whatever its integration calls its rest; an explicit Heating in mode Off still reads "Heating" (devices-climate-rest-more).',
  render: () => devicesMain({content: devicesView({note: 'Back Door unlocked · 3 lights on',
    tiles: withState(DEVICE_FAVORITES_LIT, 'Hallway', thermostat('Hallway', 'Off · 71° now', 'off', '72°'))}),
  over: climateSheet({title: 'Hallway', note: 'Off · 71° now', label: 'Set to', value: '72°', mode: 0, tone: 'gray'})}),
});

M.screen({
  id: 'device-cover', area: 'Smart Home', title: 'A cover\'s sheet: position and keys', shot: 'device_cover',
  notes: 'Open, Stop and Close, and the position where the cover reports one. Opening an entrance asks for the code, as the tile does (HCTL-005).',
  render: () => devicesMain({content: devicesView({note: 'Side Gate jammed · Pool Pump offline', area: 3, tiles: DEVICE_OUTSIDE}),
    over: M.device_sheet({glyph: 'blinds', tone: 'blue', title: 'Patio Awning', note: 'Open',
      body: `${M.row_level({glyph: 'blinds', title: 'Position', value: 100})}
        ${M.device_keys({keys: [{glyph: 'arrow-up', text: 'Open'}, {glyph: 'square', text: 'Stop'}, {glyph: 'arrow-down', text: 'Close'}]})}`})}),
});

M.screen({
  id: 'device-fan', area: 'Smart Home', title: 'A fan\'s sheet: speeds', shot: 'device_fan',
  notes: 'Off, Low, Medium and High, the lit one in the state\'s colour.',
  render: () => devicesMain({content: devicesView({note: 'Side Gate jammed · Pool Pump offline', area: 3, tiles: DEVICE_OUTSIDE}),
    over: M.device_sheet({glyph: 'fan', tone: 'blue', title: 'Porch Fan', note: 'On · Low',
      body: M.device_keys({keys: ['Off', 'Low', 'Medium', 'High'], active: 1})})}),
});

const DEVICE_LIVING = [
  {glyph: 'lock', tone: 'green', name: 'Front Door', state: 'Locked'},
  {glyph: 'thermometer', tone: 'blue', name: 'Hallway', state: 'Cool · 71° now', wide: true, control: M.device_stepper({label: 'Cool to', value: '73°'})},
  {glyph: 'sparkles', tone: 'violet', name: 'Good Night', state: 'Ran at 8:30 PM', more: false},
  {glyph: 'lightbulb', tone: 'blue', name: 'Living Room Lamp', state: 'On · 40%', wide: true, control: M.device_level({value: 40})},
  {glyph: 'fan', tone: 'blue', name: 'Ceiling Fan', state: 'On', more: false},
  {glyph: 'blinds', tone: 'blue', name: 'Blinds', state: 'Opening…', wide: true,
    control: M.device_keys({keys: [{glyph: 'arrow-up', text: 'Open'}, {glyph: 'arrow-down', text: 'Close'}]})},
  {glyph: 'thermometer', tone: 'blue', name: 'Upstairs', state: 'Auto · 70° now', wide: true},
];

M.screen({
  id: 'devices-limits', area: 'Smart Home', title: 'Smart Home: what a device can\'t do', shot: 'devices_limits',
  notes: 'Decided 2026-10-06 (BL-171; the fixes of docs/features/23 validation.md part 5): where Home Assistant says what a device can do, its tile and its sheet offer only that. Ceiling Fan has no speeds: one cell, a tap switches it, no arrow (a long press opens its sheet: the switch beside the name and nothing else). Blinds can\'t stop: Open and Close, no Stop, on the tile and the sheet; a tap while they move turns them back, as Home Assistant\'s toggle does: opening -> Close, closing -> Open, which asks for the code where opening does (HCTL-005; devices-cover-turned-back). Upstairs holds a range (no single set point): no - and +, as on its sheet. Hallway was switched to Cool and no action is known yet: the mode\'s label, "Cool · 71° now". A device whose source doesn\'t say (an older integration) keeps every control, as Porch Fan and Patio Awning do (devices-outside), and a tap on such a cover while it moves stops it.',
  render: () => devicesMain({content: devicesView({note: '1 light on', area: 1, tiles: DEVICE_LIVING})}),
});

M.screen({
  id: 'devices-cover-turned-back', area: 'Smart Home', title: 'A tap on moving blinds that can\'t stop', shot: null,
  notes: 'Blinds were opening and can\'t stop: a tap closed them, as Home Assistant\'s toggle does (opening -> Close, closing -> Open). The tap goes the way the keys go, so turning back to open asks for the code first where opening does (the pad "Open Blinds", HCTL-005). "Closing…" with the chip dimmed while under way, until Home Assistant reports the cover; the preview\'s simulator applies an action at once, so its walk never draws this step. A cover that can stop, or whose source doesn\'t say, still stops on a tap.',
  render: () => devicesMain({content: devicesView({note: '1 light on', area: 1,
    tiles: withState(DEVICE_LIVING, 'Blinds', {state: 'Closing…', pending: true})})}),
});

// After a failed action (HCTL-006, HCTL-012, BL-171; ui_devices.c failed_sentence, note): the note says
// what didn't happen, or that it isn't known whether it did.
const DEVICE_OUTSIDE_FAILED = [
  DEVICE_PORCH,
  {glyph: 'warehouse', tone: 'green', name: 'Garage Door', state: 'Closed'},
  {glyph: 'lock', tone: 'green', name: 'Side Gate', state: 'Locked'},
  {glyph: 'lightbulb', name: 'Patio Lights', state: 'Didn\'t turn on', stateTone: 'amber'},
  {glyph: 'plug', tone: 'blue', name: 'Pool Pump', state: 'On'},
  {glyph: 'blinds', tone: 'blue', name: 'Patio Awning', state: 'Didn\'t stop', stateTone: 'amber', wide: true,
    control: M.device_keys({keys: [{glyph: 'arrow-up', text: 'Open'}, {glyph: 'square', text: 'Stop'}, {glyph: 'arrow-down', text: 'Close'}]})},
  {glyph: 'fan', tone: 'blue', name: 'Porch Fan', state: 'On · Low', wide: true, control: M.device_keys({keys: ['Off', 'Low', 'Medium', 'High'], active: 1})},
  {glyph: 'droplet', name: 'Garden Water', state: 'Closed'},
];

M.screen({
  id: 'devices-failed-note', area: 'Smart Home', title: 'Smart Home: two actions failed', shot: null,
  notes: 'Patio Lights didn\'t turn on and Patio Awning didn\'t stop (both refused). The note says what didn\'t happen, one whole sentence in the catalog for each action, as its tile says it: "{0} didn\'t turn on", "{0} didn\'t turn off", "{0} didn\'t switch", "{0} didn\'t lock", "{0} didn\'t unlock", "{0} didn\'t open", "{0} didn\'t close", "{0} didn\'t stop", "{0} didn\'t run", "{0} didn\'t change" for a brightness, a position, a speed, a set point or a mode, and "{0} didn\'t respond" for an action without words. Where no answer came in time, or the link dropped with the action in flight, it may have happened (HCTL-012): "Couldn\'t confirm {0} turned on" (turned off, switched, locked, unlocked, opened, closed, stopped, ran, changed; "Couldn\'t confirm the result for {0}"), and the tile says "Not confirmed" (devices-unconfirmed-note). The order and the two-fact limit: a jam, then an unlocked lock or an open entrance, then offline, then a failure, then the lights on. The note is one line: a long name shortens with "…" so the outcome is never cut, and where two facts still don\'t fit, the second gives way whole - here "Patio Awning didn\'t stop", which its tile still says.',
  render: () => devicesMain({content: devicesView({note: 'Patio Lights didn\'t turn on', area: 3, tiles: DEVICE_OUTSIDE_FAILED})}),
});

M.screen({
  id: 'devices-failed-note-scene', area: 'Smart Home', title: 'Smart Home: a scene didn\'t run', shot: 'devices_failed_note',
  notes: 'Good Night was refused: its tile says "Didn\'t run" in amber and the note "Good Night didn\'t run · 1 light on". A tap tries it again; nothing retries by itself (HCTL-012). A newer state from the device\'s source clears the failure\'s words, on the tile and here (BL-175).',
  render: () => devicesMain({content: devicesView({note: 'Good Night didn\'t run · 1 light on', area: 1,
    tiles: withState(DEVICE_LIVING.slice(0, 4), 'Good Night', {state: 'Didn\'t run', stateTone: 'amber'})})}),
});

M.screen({
  id: 'devices-unconfirmed-note', area: 'Smart Home', title: 'Smart Home: a scene not confirmed', shot: 'devices_unconfirmed_note',
  notes: 'No answer came in time, or the link dropped with Good Night\'s run in flight (HCTL-012): it may have run, so the tile says "Not confirmed" in amber and the note "Couldn\'t confirm Good Night ran", never "didn\'t". With "· 1 light on" the note is wider than the Living room\'s line beside the tabs, so the second fact gives way whole. A tap tries it again, with the code pad where the action needs one.',
  render: () => devicesMain({content: devicesView({note: 'Couldn\'t confirm Good Night ran', area: 1,
    tiles: withState(DEVICE_LIVING.slice(0, 4), 'Good Night', {state: 'Not confirmed', stateTone: 'amber'})})}),
});

// When it happened (HCTL-006, BL-179; ui_app.c ui_when_text): today's time, an earlier day's date, the
// year for an earlier year.
M.screen({
  id: 'devices-ran-on', area: 'Smart Home', title: 'A scene that ran on an earlier day', shot: 'devices_ran_on',
  notes: 'Good Night last ran the day before: "Ran on Sep 18", the region\'s short date. A run today shows its time ("Ran at 8:30 PM"), an earlier year adds the year (devices-ran-on-year). Days are the panel\'s time zone\'s, from local midnight. No date while the clock isn\'t set, or for a time more than a minute ahead of the panel\'s clock: "Scene".',
  render: () => devicesMain({content: devicesView({note: 'Back Door unlocked · 2 lights on',
    tiles: withState(DEVICE_FAVORITES, 'Good Night', {state: 'Ran on Sep 18'})})}),
});

M.screen({
  id: 'devices-ran-on-year', area: 'Smart Home', title: 'A scene that ran in an earlier year', shot: 'devices_ran_on_year',
  notes: '"Ran on Sep 18, 2025": a run in an earlier year carries its year.',
  render: () => devicesMain({content: devicesView({note: 'Back Door unlocked · 2 lights on',
    tiles: withState(DEVICE_FAVORITES, 'Good Night', {state: 'Ran on Sep 18, 2025'})})}),
});

M.screen({
  id: 'devices-offline-day', area: 'Smart Home', title: 'A device offline since an earlier day', shot: 'devices_offline_day',
  notes: 'Pool Pump has been offline since the day before: "Offline since Sep 18", and "Offline since Sep 18, 2025" from an earlier year. Since today, the time ("Offline since 9:12 PM", devices-outside). No date while the clock isn\'t set, or for a time more than a minute ahead: "Offline". Readable, its chip at 70 %, without an arrow (BL-181).',
  render: () => devicesMain({bar: {...VIEW_BAR, time: '10:49'}, content: devicesView({note: 'Side Gate jammed · Pool Pump offline', area: 3,
    tiles: withState(withState(DEVICE_OUTSIDE, 'Porch Light', {state: 'On · 30%', control: M.device_level({value: 30})}),
      'Pool Pump', {state: 'Offline since Sep 18'})})}),
});

// Offline (HCTL-006, BL-181, decided 2026-10-07; ui_devices.c tile_update, sheet_update): the tile,
// the sheet's card and its body stay at full opacity, the name, the reason and the date in their
// usual colours; only the chip dims, to 70 % (opa_device_offline_chip), and the controls a person
// can't use rest at 40 % in their neutral grey (LV_STATE_DISABLED: no press, no pressed colour, no
// click): a slider's fill grey, no key lit, the sheet's switch shown off. The tile keeps its long
// press, so a tap on it gives the key click and sends nothing. The reported values are kept, so a
// device that comes back shows them again in one step. Nothing fades or moves.
const resting = (html, c) => html.replace(`class="${c}"`, `class="${c} disabled"`);
const COVER_KEYS = [{glyph: 'arrow-up', text: 'Open'}, {glyph: 'square', text: 'Stop'}, {glyph: 'arrow-down', text: 'Close'}];
const FAN_KEYS = ['Off', 'Low', 'Medium', 'High'];
const OFFLINE_BAR = {...VIEW_BAR, time: '10:51'};
const DEVICE_OUTSIDE_OFFLINE = [
  {glyph: 'lightbulb', name: 'Porch Light', state: 'Offline since 9:12 PM', wide: true, offline: true,
    control: resting(M.device_level({value: 30, tone: 'gray'}), 'device_level')},
  {glyph: 'warehouse', tone: 'amber', name: 'Garage Door', state: 'Closing…', pending: true},
  {glyph: 'lock', tone: 'amber', name: 'Side Gate', state: 'Jammed'},
  {glyph: 'lightbulb', tone: 'blue', name: 'Patio Lights', state: 'On', more: false},
  {glyph: 'plug', name: 'Pool Pump', state: 'Offline since 9:12 PM', offline: true},
  {glyph: 'blinds', name: 'Patio Awning', state: 'Offline since Sep 18, 2025', wide: true, offline: true,
    control: resting(M.device_keys({keys: COVER_KEYS}), 'device_keys')},
  {glyph: 'fan', name: 'Porch Fan', state: 'Offline since Sep 18', wide: true, offline: true,
    control: resting(M.device_keys({keys: FAN_KEYS}), 'device_keys')},
  {glyph: 'droplet', name: 'Garden Water', state: 'Closed'},
];
const offlinePage = (o) => devicesMain({bar: OFFLINE_BAR, content: devicesView({note: 'Side Gate jammed', area: 3, tiles: DEVICE_OUTSIDE_OFFLINE}), ...o});
// A light's brightness row while it is offline: the chip and the fill grey, the slider resting
const offlineLevel = (value) => M.row_level({glyph: 'sun', title: 'Brightness', value})
  .replace('color:var(--color_blue)', 'color:var(--color_text_2)')
  .replace('class="slider"', 'class="slider disabled"')
  .replace('<div class="fill" style="', '<div class="fill" style="background:var(--color_text_2);');

M.screen({
  id: 'devices-offline-controls', area: 'Smart Home', title: 'Offline tiles with controls: readable', shot: 'devices_offline_controls',
  notes: 'Porch Light, Patio Awning and Porch Fan are offline beside Pool Pump: each name and "Offline since 9:12 PM", "Offline since Sep 18, 2025", "Offline since Sep 18" read as clearly as an online tile\'s (color_text 15.8:1, color_text_2 6.8:1 on color_surface). The chip at 70 % (about 3.3:1, still above the 3:1 a meaningful graphic needs); the slider and the keys at 40 % in grey, the slider\'s fill grey, no speed lit. A short tap sends nothing and shows no pressed colour, though the tile gives its key click (it keeps its long press); the arrow stays hidden, and a long press still opens the sheet to read. A thermostat\'s stepper rests the same way (its minus and plus at 40 %). No Offline badge, no source label; a device that goes offline or comes back changes in one step. The note: the jam first, then the first offline tile, Porch Light; "Side Gate jammed · Porch Light offline" is a few pixels wider than the line beside the tabs and the name too short to shorten, so the second fact gives way whole (HCTL-006).',
  render: () => offlinePage(),
});

M.screen({
  id: 'devices-offline-small', area: 'Smart Home', title: 'Offline small and wide tiles: readable', shot: null,
  notes: 'Tiles without a control: the name and since when stay clear, the chip dims to 70 %. The four words, as ui_when_text writes them: "Offline" (the time isn\'t known: the clock wasn\'t set, or the time is ahead of the panel\'s), "Offline since 9:12 PM" (today), "Offline since Sep 18" (an earlier day), "Offline since Sep 18, 2025" (an earlier year). On a small tile the last one is wider than the line (about 188 px in 176) and ends in dots there only (BL-184); the sheet shows the whole sentence (device-sheet-offline-climate). The note names the first two offline tiles in the page\'s order (HCTL-006).',
  render: () => devicesMain({bar: {...VIEW_BAR, time: '10:49'}, content: devicesView({note: 'Side Gate offline · Movie Night offline', area: 3, tiles: [
    {glyph: 'lock', tone: 'green', name: 'Front Door', state: 'Locked'},
    {glyph: 'lock', name: 'Side Gate', state: 'Offline', offline: true},
    {glyph: 'sparkles', tone: 'violet', name: 'Good Night', state: 'Ran at 8:30 PM', more: false},
    {glyph: 'sparkles', name: 'Movie Night', state: 'Offline since 9:12 PM', more: false, offline: true},
    {glyph: 'lightbulb', name: 'Kitchen Lights', state: 'Off', more: false},
    {glyph: 'droplet', name: 'Garden Water', state: 'Offline since Sep 18', offline: true},
    {glyph: 'plug', tone: 'blue', name: 'Fountain', state: 'On', more: false},
    {glyph: 'plug', name: 'Pond Pump', state: 'Offline since Sep 18, 2025', more: false, offline: true},
    {glyph: 'thermometer', tone: 'blue', name: 'Upstairs', state: 'Auto · 70° now', wide: true},
    {glyph: 'thermometer', name: 'Attic', state: 'Offline since Sep 18', wide: true, offline: true},
  ]})}),
});

M.screen({
  id: 'device-sheet-offline', area: 'Smart Home', title: 'A light\'s sheet when it goes offline', shot: 'device_sheet_offline',
  notes: 'Porch Light\'s sheet was open when it went offline. The card and its body stay at full opacity: the name, "Offline since 9:12 PM" and "Brightness" clear, the row\'s icon grey. The chip at 70 %; the switch (shown off: the panel doesn\'t know the state) and the slider at 40 % in grey. Close stays enabled at full opacity (color_text_2 on color_raised, 6.0:1) and closes the sheet; nothing is sent from it. The sheet stays open.',
  render: () => offlinePage({over: M.device_sheet({glyph: 'lightbulb', tone: 'gray', title: 'Porch Light', note: 'Offline since 9:12 PM',
    on: false, offline: true, body: offlineLevel(30)})}),
});

M.screen({
  id: 'device-sheet-recovered', area: 'Smart Home', title: 'The same sheet when the light comes back', shot: 'device_sheet_recovered',
  notes: 'Porch Light answered again, on at 30 %: the next refresh (at most once a second, UI-023) lights the chip, the switch and the slider again in one step, on the sheet and the tile, as an online sheet (device-light). Nothing fades in.',
  render: () => devicesMain({bar: OFFLINE_BAR, content: devicesView({note: 'Side Gate jammed · Pool Pump offline', area: 3,
    tiles: withState(DEVICE_OUTSIDE_OFFLINE, 'Porch Light', {tone: 'blue', state: 'On · 30%', offline: false, control: M.device_level({value: 30})})}),
  over: M.device_sheet({glyph: 'lightbulb', tone: 'blue', title: 'Porch Light', note: 'On · 30%', on: true,
    body: M.row_level({glyph: 'sun', title: 'Brightness', value: 30})})}),
});

M.screen({
  id: 'device-sheet-offline-climate', area: 'Smart Home', title: 'A thermostat\'s sheet, offline', shot: 'device_sheet_offline_climate',
  notes: 'A long press on Hallway\'s offline tile opens its sheet to read: the whole sentence, "Offline since Sep 18, 2025" (a small tile may end it in dots). "Mode" at full contrast; the set point\'s stepper and the mode keys at 40 % in their neutral grey, none lit (the reported mode is kept and lit again when the thermostat answers). The chip at 70 %. A tap on them does nothing and shows no pressed colour; Close works.',
  render: () => devicesMain({bar: OFFLINE_BAR, content: devicesView({note: 'Hallway offline · 1 light on', area: 1,
    tiles: withState(DEVICE_LIVING.slice(0, 4), 'Hallway', {tone: 'gray', state: 'Offline since Sep 18, 2025', offline: true,
      control: resting(M.device_stepper({label: 'Cool to', value: '73°'}), 'device_stepper')})}),
  over: M.device_sheet({glyph: 'thermometer', tone: 'gray', title: 'Hallway', note: 'Offline since Sep 18, 2025', offline: true,
    body: `${resting(M.device_setpoint({label: 'Cool to', value: '73°', tone: 'gray'}), 'device_setpoint')}
      ${M.section('Mode')}${resting(M.device_keys({keys: CLIMATE_MODES, tone: 'gray'}), 'device_keys')}`})}),
});

// Arrange (BL-045 part C, HCTL-008; ui_devices.c): the pencil starts it on the page you are on. A tile
// held down (a long press) is lifted and dragged: it takes the place of the tile the finger is well
// inside, the others put again around it, the grid stepping at its top and bottom edges; a tap picks a
// tile, and the arrows still move it a place at a time (the way without dragging, WCAG 2.5.7). The
// drag was asked for; a room's page arranges too, without Add and Remove.
const arrangeHead = (pick) => (pick
  ? `${M.view_header({title: pick.name, note: 'Hold to drag, or use arrows'})}
    <div class="round_button">${M.icon('arrow-left', 20)}</div><div class="round_button">${M.icon('arrow-right', 20)}</div>
    ${M.segments(['Small', 'Wide'], pick.wide ? 1 : 0)}${M.button('Remove', '', 'star-off')}<div data-go="devices">${M.button('Done', 'primary')}</div>`
  : `${M.view_header({title: 'Arrange Favorites', note: 'Hold a tile to drag it, or tap it to resize or remove it'})}
    <div data-go="devices-arrange-add">${M.button('Add', '', 'plus')}</div><div data-go="devices">${M.button('Done', 'primary')}</div>`);
const arrangeView = (pick, tiles = DEVICE_FAVORITES) => `<div class="view column">
  <div class="row" style="gap:var(--space_md)">${arrangeHead(pick)}</div>
  <div class="device_grid scroll grow">${tiles.map((t) => M.device_tile({...t, arrange: true, selected: pick === t})).join('')}</div></div>`;

M.screen({
  id: 'devices-arrange', area: 'Smart Home', title: 'Arranging Favorites', shot: 'devices_arrange',
  notes: 'Every tile shows a grip and a dashed edge and rests: a tap picks it instead of switching it, and holding it lifts it to drag (a swipe that starts without a hold still scrolls the page). No code: arranging only changes how the screen looks, as brightness does; the layout is the panel\'s, the same for everyone. Done keeps it.',
  render: () => devicesMain({content: arrangeView(null)}),
});

M.screen({
  id: 'devices-arrange-tile', area: 'Smart Home', title: 'Arranging: one tile picked', shot: 'devices_arrange_tile',
  notes: 'The picked tile is edged in blue and the header turns into its actions: the arrows (one place earlier or later; one that can\'t is dimmed), Small and Wide, Remove (takes it off Favorites; it stays in its room) and Done. On a room\'s page only the arrows, Small and Wide and Done.',
  render: () => devicesMain({content: arrangeView(DEVICE_PORCH)}),
});

const DEVICE_BACK = DEVICE_FAVORITES[2];
M.screen({
  id: 'devices-arrange-drag', area: 'Smart Home', title: 'Arranging: a tile dragged', shot: 'devices_arrange_drag',
  notes: 'Back Door, held down, was lifted (picked, as a tap picks it) and dragged onto Front Door: it took that place and the others were put again after it, at once (nothing slides). It moves only when the finger is well inside another tile, and the tile under a still finger after a move is no target until the finger leaves it, so nothing jumps back and forth where wide and small tiles fall differently. Held at the grid\'s top or bottom edge, the page scrolls a step at a time. Lifting the finger leaves it picked; Done keeps the order.',
  render: () => devicesMain({content: arrangeView(DEVICE_BACK,
    [DEVICE_BACK, DEVICE_FAVORITES[1], DEVICE_PORCH, ...DEVICE_FAVORITES.slice(3)])}),
});

const ADD_ROWS = [
  {glyph: 'sparkles', color: 'violet', title: 'Good Night', room: 'Living room', state: 'Ran at 8:30 PM'},
  {glyph: 'lock', color: 'amber', title: 'Side Gate', room: 'Outside', state: 'Jammed'},
  {glyph: 'lightbulb', color: 'blue', title: 'Patio Lights', room: 'Outside', state: 'On'},
  {glyph: 'plug', color: 'gray', title: 'Pool Pump', room: 'Outside', state: 'Offline since 9:12 PM'},
  {glyph: 'blinds', color: 'gray', title: 'Patio Awning', room: 'Outside', state: 'Closed'},
];
const addSheet = (rooms) => M.pick_sheet({title: 'Add to Favorites', note: 'Tap a device to add it at the end', fit: true,
  rows: ADD_ROWS.map(({room, state, ...d}) => M.row_link({...d, detail: rooms ? `${room} · ${state}` : state, trail: 'plus', trailColor: 'blue'})).join('')});

M.screen({
  id: 'devices-arrange-add', area: 'Smart Home', title: 'Arranging: adding to Favorites', shot: 'devices_arrange_add',
  notes: 'Add lists the devices not on Favorites, each with its room where it has one; a tap puts one at the end of the page at its kind\'s size and the list stays open (made again without it) for the next. The note says what a tap does, with rooms or without (devices-arrange-add-no-rooms) (BL-177); "Every device is on Favorites already" when nothing is left to add.',
  render: () => devicesMain({content: arrangeView(null, DEVICE_FAVORITES.filter((t) => t.name !== 'Good Night')), over: addSheet(true)}),
});

M.screen({
  id: 'devices-arrange-add-no-rooms', area: 'Smart Home', title: 'Arranging: adding, no device in a room', shot: null,
  notes: 'Where Home Assistant puts none of the devices in an area, the rows show their state only; the note is the same.',
  render: () => devicesMain({content: arrangeView(null, DEVICE_FAVORITES.filter((t) => t.name !== 'Good Night')), over: addSheet(false)}),
});

// Long names (HCTL-004, BL-184, decided 2026-10-07; ui_devices.c): a device's name may be 47 bytes
// (DEV_NAME_LEN 48). On a tile it takes whole lines of its font with no line spacing and ends in a
// stationary "..." (LVGL's dots; the browser draws an ellipsis): one line on a small tile and on any
// tile with a slider, keys or a thermostat's stepper, two on a wide tile with no control. The state
// is one line. The bounds are worked out again when a thermostat's stepper shows or hides; Arrange
// keeps them. The sheet shows the whole name and state, wrapped without dots (an unbroken name
// breaks between characters, never inside one), with the chip, the switch and Close on the header's
// first row; where a long name and a tall body don't fit the card (at most 520 high), the body
// scrolls by hand under the header. Nothing moves to show the rest, and the stored name keeps every
// byte. The preview's walk gives Favorites' and Outside's devices these names.
const LONG_W = 'W'.repeat(47);
const LONG_GARAGE = {glyph: 'warehouse', tone: 'amber', name: 'GarageDoorOpenerNorthBayBesideTheWorkbenchUnit2', state: 'Closing…', pending: true};
const LONG_LAMP = {glyph: 'lightbulb', tone: 'blue', name: 'LivingRoomNorthWindowFloorLampOverTheArmchair01', state: 'On · 30%', wide: true, control: M.device_level({value: 30})};
const longFavorites = (stepper = true) => [
  LONG_LAMP,
  {glyph: 'lock', tone: 'green', name: 'Upstairs Hallway Door Lock Beside The Linen Bin', state: 'Locked'},
  {glyph: 'lock-open', tone: 'amber', name: 'Upstairs Hallway Door Lock Beside The Guest Bed', state: 'Unlocked'},
  LONG_GARAGE,
  {glyph: 'thermometer', tone: 'blue', name: LONG_W, state: 'Cool · 71° now', wide: true, control: stepper ? M.device_stepper({label: 'Cool to', value: '73°'}) : ''},
  {glyph: 'sparkles', tone: 'violet', name: 'Good Night: Lights Off Doors Locked Shades Down', state: 'Ran at 8:30 PM', more: false},
  {glyph: 'lightbulb', tone: 'blue', name: 'Lámpara de pie del salón junto a las ventanas', state: 'On · 40%', wide: true, control: M.device_level({value: 40})},
  {glyph: 'lightbulb', tone: 'blue', name: LONG_W, state: 'On · 100%'},
];
const LONG_NOTE = 'Upstairs Hallw… unlocked · 3 lights on';
const LONG_BAR = {...VIEW_BAR, time: '10:52'};
const longPage = (over, tiles = longFavorites()) => devicesMain({bar: LONG_BAR, content: devicesView({note: LONG_NOTE, tiles}), over});
const LONG_OUTSIDE = [
  LONG_LAMP,
  LONG_GARAGE,
  {glyph: 'lock', tone: 'amber', name: 'Side Gate Smart Lock Beside The Recycling Bin 2', state: 'Jammed'},
  {glyph: 'lightbulb', tone: 'blue', name: 'Patio String Lights Along The Fence And Pergola', state: 'On', more: false},
  {glyph: 'plug', name: 'Garden Fountain Pump Beside The Back Patio Wall', state: 'Offline since Sep 18, 2025', offline: true},
  {glyph: 'blinds', name: 'Patio Awning Over The Back Deck By The Hot Tubs', state: 'Closed', wide: true, control: M.device_keys({keys: COVER_KEYS})},
  {glyph: 'fan', tone: 'blue', name: 'Back Porch Ceiling Fan Beside The Kitchen Doors', state: 'On · Medium', wide: true, control: M.device_keys({keys: FAN_KEYS, active: 2})},
  {glyph: 'droplet', name: 'Garden Drip Irrigation Valve For The North Beds', state: 'Closed'},
];
const outsidePage = (over) => devicesMain({bar: LONG_BAR, content: devicesView({note: 'Side Gate Smart Lock Besid… jammed', area: 3, tiles: LONG_OUTSIDE}), over});
const LONG_SHEET = 'The card stays 600 wide and at most 520 high, padding 24: the title (font_title, 33 a line) and the state (font_body, 26 a line) wrap in full in a column of 358 px beside a switch or 434 px without one (chip 48, Close 44, gaps 12).';

M.screen({
  id: 'devices-long-names', area: 'Smart Home', title: 'Long names on Favorites: one line', shot: 'devices_long_names',
  notes: 'Every name here is 47 bytes: wide letters, names without a space, a multibyte one, two that start the same. Each takes one line and ends in dots: the small tiles have one line under the chip, and the light, the thermostat and the lamp carry a control. The two "Upstairs Hallwa…" locks read the same on their tiles; a hold opens either sheet with the whole name. The note shortens a name with "…" so the fact is never cut (the page order and note rules, BL-182). A poll with nothing new leaves every label as it is: a shortened one is not drawn again.',
  render: () => longPage(),
});

M.screen({
  id: 'devices-long-names-no-stepper', area: 'Smart Home', title: 'Long names: a wide tile without a control', shot: 'devices_long_names_no_stepper',
  notes: 'The thermostat no longer reports a single set point, so its stepper hides and its name gets two lines, then dots; the bounds are worked out again when the stepper comes back (one line). A wide tile without a control always has two (devices-long-names-wide).',
  render: () => longPage(undefined, longFavorites(false)),
});

M.screen({
  id: 'devices-long-names-wide', area: 'Smart Home', title: 'Long names on wide tiles: two lines without a control', shot: null,
  notes: 'Three thermostats holding a range (no stepper): 47 W\'s, an unbroken name broken between letters, a multibyte one (never broken inside a letter), each two lines and dots. "Main Floor Hallway Thermostat Beside The Stairs" has a set point, so one line. The fan and the offline light are small tiles: one line.',
  render: () => devicesMain({content: devicesView({note: 'Upstairs Hallw… offline · 1 light on', area: 1, tiles: [
    {glyph: 'thermometer', tone: 'blue', name: LONG_W, state: 'Auto · 70° now', wide: true},
    {glyph: 'thermometer', tone: 'blue', name: 'GuestRoomThermostatBesideTheWindowOnTheLanding2', state: 'Cool · 74° now', wide: true},
    {glyph: 'thermometer', tone: 'orange', name: 'Calefacción del baño de invitados del ático', state: 'Heat · 68° now', wide: true},
    {glyph: 'fan', tone: 'blue', name: 'Back Porch Ceiling Fan Beside The Kitchen Doors', state: 'On', more: false},
    {glyph: 'lightbulb', name: 'Upstairs Hallway Ceiling Light Over The Landing', state: 'Offline since 9:12 PM', more: false, offline: true},
    {glyph: 'thermometer', tone: 'orange', name: 'Main Floor Hallway Thermostat Beside The Stairs', state: 'Heating · 69° now', wide: true, control: M.device_stepper({label: 'Heat to', value: '70°'})},
    {glyph: 'lightbulb', tone: 'blue', name: LONG_W, state: 'On · 40%', wide: true, control: M.device_level({value: 40})},
  ]})}),
});

M.screen({
  id: 'devices-long-names-arrange', area: 'Smart Home', title: 'Arranging long names', shot: 'devices_long_names_arrange',
  notes: 'Arrange draws the same tiles, so the same lines: the grip sits in the 64 x 72 corner the arrow uses, above a small tile\'s name and beside a wide tile\'s (its 40 px are kept).',
  render: () => devicesMain({bar: LONG_BAR, content: arrangeView(null, longFavorites())}),
});

M.screen({
  id: 'devices-long-names-outside', area: 'Smart Home', title: 'Long names on Outside: keys and an offline device', shot: 'devices_long_names_outside',
  notes: 'The awning\'s and the fan\'s keys stay under one line of name; the offline fountain\'s "Offline since Sep 18, 2025" ends in dots on its small tile only (its sheet shows it whole, device-sheet-long-offline). The note shortens the jammed lock\'s name to keep "jammed".',
  render: () => outsidePage(),
});

M.screen({
  id: 'device-sheet-long-name', area: 'Smart Home', title: 'A long name in a sheet with a switch', shot: 'device_sheet_long_name',
  notes: `The sheet is where the whole name is read: 47 W's in four lines of 358 px beside the switch, the state under it; the chip, the switch and Close on the first row, where Close is looked for. ${LONG_SHEET}`,
  render: () => longPage(M.device_sheet({glyph: 'lightbulb', tone: 'blue', title: LONG_W, note: 'On · 100%', on: true,
    body: M.row_level({glyph: 'sun', title: 'Brightness', value: 100})})),
});

M.screen({
  id: 'device-sheet-long-climate', area: 'Smart Home', title: 'A long name in a sheet without a switch', shot: 'device_sheet_long_climate',
  notes: `Without a switch the column is 434 px: the thermostat's 47 W's in three lines; Close on the first row. ${LONG_SHEET}`,
  render: () => longPage(M.device_sheet({glyph: 'thermometer', tone: 'blue', title: LONG_W, note: 'Cool · 71° now',
    body: `${M.device_setpoint({label: 'Cool to', value: '73°', tone: 'blue'})}
      ${M.section('Mode')}${M.device_keys({keys: CLIMATE_MODES, active: 2, tone: 'blue'})}`})),
});

M.screen({
  id: 'device-sheet-long-unbroken', area: 'Smart Home', title: 'An unbroken name in a sheet', shot: 'device_sheet_long_unbroken',
  notes: '"GarageDoorOpenerNorthBayBesideTheWorkbenchUnit2" has no space: it shows whole in two lines of 434 px, broken between letters (never inside a multibyte letter); Close on the first row.',
  render: () => longPage(M.device_sheet({glyph: 'warehouse', tone: 'amber', title: LONG_GARAGE.name, note: 'Closing…',
    body: M.device_keys({keys: COVER_KEYS})})),
});

M.screen({
  id: 'device-sheet-long-offline', area: 'Smart Home', title: 'Holding an offline tile with a long name', shot: 'device_sheet_long_offline',
  notes: 'A hold on the offline fountain opens its sheet: "Garden Fountain Pump Beside The Back Patio Wall" and "Offline since Sep 18, 2025" whole (the tile ends both in dots). It reads only: the chip at 70 %, the switch shown off and resting at 40 % (BL-181); Close works.',
  render: () => outsidePage(M.device_sheet({glyph: 'plug', tone: 'gray', title: 'Garden Fountain Pump Beside The Back Patio Wall',
    note: 'Offline since Sep 18, 2025', on: false, offline: true, body: ''})),
});
