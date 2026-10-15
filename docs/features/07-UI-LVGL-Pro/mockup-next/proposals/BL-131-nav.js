// BL-131: the left side (mockup-next). The rail is for places: Security (today's Home), Zones, Smart Home
// (docs/features/24's Devices screen, a house with Wi-Fi, on two lines; shown when there is a device to control), Activity, and Panic at its foot. Chime is a switch, not a
// place (UI-025): it becomes a round switch in the status bar beside the gear, where its state shows
// everywhere, as the rail's bell did. Troubles was a count said twice (the rail's badge and the status
// bar's amber count): the status bar's count stays, as a pill that opens the list. The Settings list is
// one line per section (kit: settings({next})); every BL-131 page shows both changes. Where
// Smart Home leads is BL-045's proposal (BL-045-devices.js).

const P = (replaces, why) => ({bl: 'BL-131', status: 'approved', decided: '2026-10-01: approved as drawn: "start working on getting all of the screen changes in place"', replaces, why});

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

M.screen({
  id: 'n-home', area: 'Home', title: 'Security: the rail and the status bar', shot: null,
  proposal: P('home-ready', 'Six entries under the logo filled the rail, and two of them are not places: Chime is a switch (UI-025) and Troubles repeated the count the status bar already shows. With the Devices screen coming (docs/features/24 HCTL-003), there was no room for it. Now the rail has Security, Zones, Smart Home and Activity, with Panic at its foot; Chime is a round switch beside the gear. Security and Smart Home are the two halves ("Security & Smart Home"); Smart Home is wider than the rail\'s buttons, so it takes two lines and its button a line more. Home is also an arm mode (Home Assistant\'s Armed home), Alarm is the event here (Burglary alarm), and Automation means rules and schedules in the apps people know.'),
  notes: 'Chime keeps its code panel over the screen (UI-025) and its state in its icon: a bell in blue when on, a crossed-out bell when off. It is dimmed while armed, as the gear is (UI-022). Without a device to control, the rail is Security, Zones, Activity and Panic (HCTL-003). The setup\'s last button says Done, not Go to Home. Changes the decided words of UI-022, UI-025 and UI-030, HCTL-003\'s name, and docs/features/07 "The navigation rail".',
  render: () => M.main({next: true, content: M.home(
    {tone: 'green', glyph: 'shield-check', headline: 'Disarmed', detail: 'Ready to arm', note: 'All zones closed'}, M.panel_arm())}),
});

M.screen({
  id: 'n-armed', area: 'Home', title: 'Armed Away: Chime and the gear dimmed', shot: null,
  proposal: P('armed-away', 'While armed, the status bar\'s Chime and gear are dimmed and only say why, as the rail\'s Chime and the gear do today (UI-022). The rail stays: Security, Zones, Smart Home, Activity and Panic can all be used while armed.'),
  notes: 'Smart Home while armed is docs/features/24\'s open question (leaning yes; unlocking always takes a code).',
  render: () => M.main({next: true, tone: 'rose', rail: {locked: true}, bar: {banner: null, time: '10:47'}, content: M.home(
    {tone: 'rose', glyph: 'lock', headline: 'Armed Away', detail: 'The whole house is protected', note: 'Since 10:46 PM · By Master'},
    M.code_pad({title: 'Disarm', tone: 'green', glyph: 'lock-open', close: false}))}),
});

M.screen({
  id: 'n-zones', area: 'Views', title: 'Zones: a trouble in the status bar', shot: null,
  proposal: P('zones', 'A trouble was counted twice, on the rail\'s Troubles and in the status bar. Now it is said once, in the status bar, as an amber pill that opens Troubles.'),
  render: () => M.main({next: true, view: 'zones', tone: 'amber', rail: {zones: 1}, bar: {time: '10:48', troubles: 1}, content: `<div class="view column">
    <div class="row">${M.view_header({title: 'Zones', note: '8 zones · 1 open · 1 bypassed'})}${M.segments(['All', 'Open', 'Bypassed', 'Trouble'])}</div>
    <div class="zone_list scroll grow">${ZONES.map(M.zone_card).join('')}</div></div>`}),
});

M.screen({
  id: 'n-troubles', area: 'Views', title: 'Troubles, opened from the status bar', shot: null,
  proposal: P('troubles', 'Opened from the pill in the status bar, which stays lit while the list is open, as the gear does for Settings; no rail entry is lit.'),
  notes: 'With nothing in the list, the pill isn\'t there. The count is everything the list holds, restored troubles not yet acknowledged included (UI-030).',
  render: () => M.main({next: true, view: 'troubles', tone: 'amber', rail: {zones: 1}, bar: {time: '10:48', troubles: 2, wifi: 'wifi-off', wifiOk: false},
    content: `<div class="view column"><div class="row">${M.view_header({title: 'Troubles', note: '2 active'})}
      <div class="style_button font_label">${M.icon('check', 20)}Acknowledge</div></div><div class="list scroll">
      ${M.trouble_row({glyph: 'zap', title: 'Mains power lost', detail: 'Running on battery'})}
      ${M.trouble_row({glyph: 'battery-warning', title: 'Basement Water · Low battery', detail: 'Check the sensor'})}</div></div>`}),
});
