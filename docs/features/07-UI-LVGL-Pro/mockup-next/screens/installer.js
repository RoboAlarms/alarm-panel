// Settings > Installer (ui_installer.c): locked, then installer mode with arming off. Who may open
// what follows docs/features/14 "Who can change what".

const INST_RAIL = {troubles: 1, tone: 'blue'};
const INST_BAR = {banner: 'installer', time: '10:52', troubles: 1};
const inst = (meta, page, extra = {}) => M.screen({
  ...meta, area: 'Installer', ...extra,
  render: () => M.main({view: 'settings', tone: 'blue', rail: INST_RAIL, bar: INST_BAR,
    content: M.settings({section: 'installer', ...page()}), ...(extra.main ? extra.main() : {})}),
});

M.screen({
  id: 'installer-locked', area: 'Installer', title: 'Installer, before the code', shot: 'installer_locked',
  render: () => M.main({view: 'settings', rail: {troubles: 1}, bar: {time: '10:52', troubles: 1}, content: M.settings({
    section: 'installer', title: 'Installer', note: 'Installer settings change how the alarm works',
    body: [
      M.row_action({glyph: 'key-round', text: 'Enter the installer code', color: 'amber'}),
      M.paragraph('The factory installer code is 69420 until you change it.', 'amber'),
      M.row_link({glyph: 'life-buoy', color: 'gray', title: 'Lost the installer code?', goTo: 'installer-lost-code'}),
      M.paragraph('While you change settings nothing can be armed. Fire, CO and panic buttons keep working.'),
    ].join('')})}),
});

M.screen({
  id: 'installer-lost-code', area: 'Installer', title: 'Installer, before the code: a lost code', shot: 'installer_lost_code',
  notes: 'BL-131 small fix: the lost-code paragraph became a row that opens this page, before the code.',
  render: () => M.main({view: 'settings', rail: {troubles: 1}, bar: {time: '10:52', troubles: 1}, content: M.settings({
    section: 'installer', back: 'installer-locked', title: 'Lost the installer code?', note: '',
    body: [
      M.paragraph('Unplug the panel for ten seconds. For two minutes after it starts, Installer opens without the code (unless the installer turned that off).'),
    ].join('')})}),
});

inst({id: 'installer-menu', title: 'Installer mode: the menu', shot: 'installer_menu'}, () => ({
  title: 'Installer',
  body: [
    M.section('Sensors and devices'),
    M.row_link({glyph: 'door-closed', color: 'green', title: 'Zones', detail: 'Sensors, types, names', value: '8 in use', goTo: 'installer-zones'}),
    M.row_link({glyph: 'warehouse', color: 'violet', title: 'Partitions', detail: 'Areas that arm separately', value: '1 in use'}),
    M.row_link({glyph: 'cpu', title: 'Keypads and devices', detail: 'Supervised keypads and expanders', value: 'None'}),
    M.section('How the alarm works'),
    M.row_link({glyph: 'sliders-horizontal', title: 'Delays and arming', detail: 'Entry and exit times, sirens, panic keys, codes', goTo: 'installer-options'}),
    M.section('This panel'),
    M.row_link({glyph: 'house', color: 'violet', title: 'System name', detail: 'Shown on the screen and in the event log', value: 'Home'}),
    M.row_link({glyph: 'key-round', color: 'amber', title: 'Installer and master codes', detail: 'Factory codes still in use'}),
    M.row_toggle({glyph: 'monitor', title: 'Panel Manager', detail: 'Change settings from a browser on your network'}),
    M.section('Panel Manager'),
    M.row_link({glyph: 'globe', title: 'Allow browser sign-in', detail: 'User name: Installer', value: 'Off'}),
    M.row_toggle({glyph: 'code-xml', color: 'gray', title: 'Developer mode', detail: 'Enrolled development tools and signed network updates'}),
    M.section('Test and start over'),
    M.row_link({glyph: 'footprints', title: 'Walk test', detail: 'Leaves installer mode and starts one'}),
    M.row_link({glyph: 'sparkles', color: 'rose', title: 'Run the setup again', detail: 'Wi-Fi, time zone, codes and name, step by step', goTo: 'setup-welcome'}),
    M.row_link({glyph: 'rotate-ccw', color: 'red', title: 'Factory reset', detail: 'Erase everything', goTo: 'factory-reset'}),
    M.row_action({glyph: 'log-out', text: 'Leave installer mode', color: 'blue'}),
  ].join(''),
}), {notes: 'Four groups, the risky ones last (BL-131 finding 18). With Z-Wave built in, a Z-Wave row follows Zones (docs/features/36). No note: the banner says installer mode is on. The Panel Manager is one switch with the installer\'s own sign-in under it; while it runs, its detail is its address and certificate (WEB-002). The installer\'s sign-in rows are the ones a user\'s page has (ui_login_rows), as on n-user.'});

inst({id: 'installer-options', title: 'System options', shot: 'installer_options'}, () => ({
  title: 'System options', note: 'Saved when you go back · SIA CP-01 limits apply', back: 'installer-menu',
  body: [
    M.paragraph('No zone is a night zone yet, so Night arms like Stay.'),
    M.section('Delays'),
    M.row_number({title: 'Exit time', detail: 'Time to leave after arming', value: '60 s'}),
    M.row_number({title: 'Entry time 1', detail: 'Time to disarm after the main door opens', value: '30 s'}),
    M.row_number({title: 'Entry time 2', detail: 'For a second door, like the garage', value: '45 s'}),
    M.row_number({title: 'Report delay', detail: 'Disarm within this time and no burglary alarm is sent', value: '30 s'}),
    M.row_number({title: 'Siren time', detail: 'How long a burglary alarm sounds', value: '4 min'}),
    M.row_toggle({title: 'Fire and CO sirens stop by themselves', detail: 'Off: they sound until silenced (recommended)'}),
    M.row_number({title: 'Cancel window', detail: 'Disarming this soon after an alarm was reported sends a cancel', value: '5 min'}),
    M.section('Alarms'),
    M.row_link({glyph: 'siren', color: 'red', title: 'Panic keys', detail: 'What the keypad\'s A, B and C keys send'}),
    M.row_link({glyph: 'bell', color: 'amber', title: 'Trouble beeps', detail: 'Which troubles make the keypad beep'}),
    M.section('Arming'),
    M.row_toggle({title: 'Auto-stay', detail: 'Away becomes Stay when nobody leaves through a door', on: true}),
    M.row_toggle({title: 'Exit beeps', detail: 'Beep through Away exit time (faster in the last 10 seconds)', on: true}),
    M.row_toggle({title: 'Arm without a code', detail: 'Quick arm. Disarming always needs a code'}),
    M.section('Codes'),
    M.row_number({title: 'Shortest new code', detail: 'Existing shorter codes keep working', value: '4'}),
    M.row_number({title: 'Wrong codes before lockout', detail: 'The keypad locks for a while', value: '5'}),
  ].join(''),
}), {notes: 'Abridged: the full list is ui_installer.c build_options().'});

const ZONE_ROWS = [
  ['door-closed', 'Front Door', 'Zone 1 · Entry/exit 1 · Simulator'],
  ['warehouse', 'Garage Door', 'Zone 2 · Entry/exit 2 · Simulator'],
  ['app-window', 'Living Room Window', 'Zone 3 · Perimeter · Simulator'],
  ['door-closed', 'Back Door', 'Zone 4 · Perimeter · Simulator'],
  ['footprints', 'Hallway Motion', 'Zone 5 · Interior follower · Simulator'],
  ['alarm-smoke', 'Kitchen Smoke', 'Zone 6 · Fire · Simulator'],
];
inst({id: 'installer-zones', title: 'Zones (installer)', shot: 'installer_zones'}, () => ({
  title: 'Zones', note: '8 of 128 zones in use', back: 'installer-menu',
  body: M.row_action({glyph: 'plus', text: 'Add a zone', color: 'blue'}) +
    ZONE_ROWS.map(([g, t, d]) => M.row_link({glyph: g, color: 'green', title: t, detail: d, goTo: t === 'Hallway Motion' ? 'zone-edit' : ''})).join(''),
}));

const zoneEditBody = () => [
  M.row_status({glyph: 'footprints', color: 'green', title: 'No motion', detail: 'Simulator · For testing: nothing is wired, the service console opens and closes it.'}),
  M.section('The zone'),
  M.row_link({glyph: 'tag', title: 'Name', value: 'Hallway Motion'}),
  M.row_link({glyph: 'shield', title: 'Type', value: 'Interior follower (04)', goTo: 'zone-type'}),
  M.section('Its sensor'),
  M.row_link({glyph: 'footprints', color: 'green', title: 'Sensor', value: 'Motion'}),
  M.row_link({glyph: 'wrench', title: 'Source', value: 'Simulator'}),
  M.row_link({glyph: 'sliders-horizontal', title: 'Sensor settings', detail: 'Open when on · 350 ms · not supervised', goTo: 'zone-sensor'}),
  M.section('What it does'),
  M.row_toggle({title: 'Night zone', detail: 'Armed in Night too, not only Away', on: true}),
  M.row_toggle({title: 'Chime', detail: 'Ding when it opens while disarmed and chime is on'}),
  M.row_toggle({title: 'Report this zone', detail: 'Its events go to phones and integrations, not only the event log', on: true}),
  M.section('Advanced'),
  M.row_link({glyph: 'shield', title: 'Cross-zone partner', detail: 'Both must trip close together to cause an alarm', value: 'None'}),
  M.row_link({glyph: 'siren', title: 'Alarm limit', detail: 'Stops sounding after this many alarms in one arming', value: 'System setting'}),
  M.section(''),
  M.row_action({glyph: 'trash-2', text: 'Remove this zone', color: 'red'}),
].join('');
inst({id: 'zone-edit', title: 'One zone', shot: 'zone_edit'}, () => ({
  title: 'Hallway Motion', note: 'Zone 5 · saved when you go back', back: 'installer-zones', body: zoneEditBody(),
}), {notes: 'How the zone is right now on top: not a button; its words change at most once a second, after the input settles (UI-023); its detail is the source and what it promises (amber when a warning). Then four groups and Remove apart (BL-131 finding 23). Partition shows with more than one partition, "Acts as" for a button, "Can be bypassed" for a 24-hour zone. Contact and Response time are on Sensor settings only; Alarm limit is one picker (System setting, 1 to 6 times, Never) over the two stored fields the stepper and the "Never shut down" switch set. The preview also shoots it scrolled: zone_edit_sensor, zone_edit_end.'});

inst({id: 'zone-type', title: 'Choosing a zone type', shot: 'zone_type'}, () => ({
  title: 'Hallway Motion', note: 'Zone 5 · saved when you go back', back: true, body: zoneEditBody(),
}), {main: () => ({over: M.pick_sheet({fit: true, title: 'Zone type', note: 'How the panel reacts when this sensor trips', rows: [
  M.row_link({title: 'Entry/exit 1', detail: 'The door you come in by: starts entry time 1', trail: ''}),
  M.row_link({title: 'Entry/exit 2', detail: 'A second door, like the garage: entry time 2', trail: ''}),
  M.row_link({title: 'Perimeter', detail: 'Doors and windows: alarm right away', trail: ''}),
  M.row_link({title: 'Interior follower', detail: 'Motion: waits while entry time runs', trail: 'check', trailColor: 'blue'}),
  M.row_link({title: 'Interior with delay', detail: 'Motion that starts entry time', trail: ''}),
  M.row_link({title: '24-hour', detail: 'Alarms armed or not', trail: ''}),
].join('')})})});

// ---- added with mockup-next ----
inst({id: 'zone-sensor', title: 'A zone\'s sensor settings', shot: 'installer_zone_sensor'}, () => ({
  title: 'Sensor settings', note: 'Front Door · changes apply at once', back: 'zone-edit',
  body: [
    M.section('How it reads'),
    M.row_link({glyph: 'sliders-horizontal', title: 'Open when', value: 'On or open'}),
    M.row_toggle({glyph: 'arrow-left-right', title: 'Reverse open and closed', detail: 'For a normally open contact, or a sensor that reports the other way round'}),
    M.row_link({glyph: 'timer', title: 'Response time', value: '350 ms'}),
    M.section('When it goes quiet'),
    M.row_link({glyph: 'clock-alert', title: 'Supervision', detail: 'Shows Check when it misses a check-in', value: 'Not supervised'}),
    M.row_link({glyph: 'unlink', title: 'Unreachable after', detail: 'Shows Check when its source loses it', value: 'Source default'}),
    M.section('Other inputs'),
    M.row_link({glyph: 'shield-alert', color: 'amber', title: 'Tamper input', value: 'None'}),
    M.row_link({glyph: 'battery-warning', color: 'amber', title: 'Low-battery input', value: 'None'}),
    M.section(''),
    M.row_action({glyph: 'trash-2', text: 'Remove the sensor, keep the zone', color: 'red'}),
  ].join(''),
}), {notes: 'Pickers, not steppers holding words (BL-131 findings 19, 24). Supervision is how long the sensor itself may stay silent (Not supervised, 1, 4 or 12 hours: the stored field holds at most 18 h); Unreachable after how long its source may say it is gone (Source default, 1, 5, 15 or 60 min); a value set another way stays offered as it is. For a wired sensor the reverse switch is "Contact: Normally closed / Normally open". The preview shoots its end: installer_zone_sensor_end.'});

inst({id: 'factory-reset', title: 'Factory reset, held', shot: 'factory_reset'}, () => ({
  title: 'Factory reset', note: 'Start again from the box', back: 'installer-menu',
  body: [
    M.paragraph('Erases the zones, users and codes, Wi-Fi and every setting, then restarts with the factory codes (installer 69420, master 123456) and the setup.'),
    M.row_toggle({title: 'Also erase the event log', detail: 'Off keeps the history, with the reset in it'}),
    M.row_hold({text: 'Hold to erase everything', held: 0.53}),
  ].join(''),
}));
