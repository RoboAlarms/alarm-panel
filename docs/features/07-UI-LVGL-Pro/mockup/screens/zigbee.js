// Settings > Installer > Zigbee and the pages under it (ui_zigbee.c): the radio's page, adding a
// device (with an install code too), a device's page, removing, radio details, channel, reset,
// install codes waiting, the zone editor's Zigbee rows, the trouble, and the siren, keypad and key
// fob pages. Built from the BL-144 proposal with BL-147's siren and BL-148's keypad and key fob
// (docs/features/20), in Z-Wave's words wherever the two do the same thing.
//
// Who: looking at any of these pages needs installer mode (the trouble asks for the code first,
// ZB-027), and every change needs installer mode with every partition disarmed. Nothing on these
// pages blinks; countdowns and live words change at most once a second (UI-020, UI-023).
// Departures from the drawing are written in the notes of the screens they touch. All data is fictional.

const RAIL = {tone: 'blue'};
const BAR = {banner: 'installer', time: '10:52'};
const AREA = 'Zigbee';
// A page in Settings > Installer, in installer mode. extra.over: a sheet or dialog over it; extra.toast.
const page = (meta, body, extra = {}) => M.screen({
  shot: null, area: AREA, ...meta,
  render: () => M.main({view: 'settings', tone: 'blue', rail: RAIL, bar: extra.bar || BAR,
    content: M.settings({section: 'installer', ...body()}), over: extra.over ? extra.over() : '', toast: extra.toast}),
});
const dim = (html) => `<div class="disabled">${html}</div>`;

// ---- the devices (fictional): sorted unreachable first, then by name ----
const DEVICES = [
  {g: 'droplets', name: 'Basement Water', detail: 'Aqara water sensor · Battery 64 % · Signal weak', sub: 'Not heard for 2 h 30 min', value: 'Zone 8', go: 'zigbee-device-unreachable'},
  {g: 'key-round', name: 'Alex\'s key fob', detail: 'Aqara key fob · Battery 97 % · Signal good', value: 'Key fob', go: 'zigbee-fob'},
  {g: 'app-window', name: 'Bedroom Window', detail: 'Aqara door sensor · Signal good', sub: 'Battery low: change it soon', value: 'Zone 7'},
  {g: 'keyboard', name: 'Entry Keypad', detail: 'Develco keypad · Battery 81 % · Signal good', value: 'Keypad', go: 'zigbee-keypad'},
  {g: 'door-closed', name: 'Front Door', detail: 'Aqara door sensor · Battery 92 % · Signal good', value: 'Zone 1', go: 'zigbee-device'},
  {g: 'siren', name: 'Hall Siren', detail: 'Heiman siren · Mains · Signal good', value: 'Siren', go: 'zigbee-siren'},
  {g: 'footprints', name: 'Hallway Motion', detail: 'Aqara motion sensor · Battery 75 % · Signal fair', value: 'Zone 5'},
  {g: 'alarm-smoke', name: 'Kitchen Smoke', detail: 'Heiman smoke and CO alarm · Battery 100 % · Signal good', value: 'Zones 6, 9'},
];
const deviceRow = (d) => M.row_link_alert({glyph: d.g, color: d.sub ? 'amber' : 'green', title: d.name, detail: d.detail,
  sub: d.sub, value: d.value, goTo: d.go});

// ---- the radio's row at the top of the Zigbee page ----
const RADIO = {
  working: {tone: 'blue', word: 'Working', detail: 'Channel 15 · 8 devices'},
  empty: {tone: 'blue', word: 'Working', detail: 'Channel 15 · No devices'},
  starting: {tone: 'blue', word: 'Starting', detail: 'A few seconds'},
  absent: {tone: 'gray', word: 'Not connected', detail: 'Nothing answers in the wireless socket'},
  silent: {tone: 'amber', word: 'Not answering', detail: 'Check that it sits firmly in its socket; the panel keeps trying', warn: true},
  paused: {tone: 'amber', word: 'Paused after restarts', detail: 'It restarted the panel twice, so the panel stopped using it', warn: true},
  zwave: {tone: 'amber', word: 'Socket used by Z-Wave', detail: 'The wireless socket holds the Z-Wave radio', warn: true},
  noisy: {tone: 'amber', word: 'Interference', detail: 'Something nearby drowns channel 15: sensors may not be heard', warn: true},
};
const radioRow = (k) => {
  const r = RADIO[k];
  return M.row_link_alert({glyph: 'radio', color: r.tone, title: `Radio · ${r.word}`, detail: r.detail,
    detailColor: r.warn ? 'amber' : null, goTo: k === 'paused' ? 'zigbee-radio-paused' : 'zigbee-radio'});
};
const actionRows = (waiting = '2') => [
  M.row_link({glyph: 'plus', color: 'green', title: 'Add a device', detail: 'Any Zigbee 3.0 device, for 3 minutes', goTo: 'zigbee-add-listening'}),
  M.row_link({glyph: 'qr-code', title: 'Add with an install code', detail: 'For a device whose label has one', goTo: 'zigbee-code'}),
  M.row_link({glyph: 'trash-2', color: 'red', title: 'Remove a device', detail: 'Asks it to leave the network', goTo: 'zigbee-remove-pick'}),
  M.row_link({glyph: 'key-round', color: 'amber', title: 'Install codes waiting', detail: 'Devices that may join with their own key', value: waiting, goTo: 'zigbee-codes'}),
].join('');
const networkRows = (devices = 8) => [
  M.section('Network'),
  M.row_link({glyph: 'waves', title: 'Channel', detail: 'Where the Zigbee network talks', value: '15', goTo: 'zigbee-channel'}),
  M.row_link({glyph: 'rotate-ccw', color: 'red', title: 'Reset the Zigbee network', detail: devices ? 'Every device must be added again' : 'A new network, with new keys', goTo: 'zigbee-reset'}),
].join('');
const zigbeePage = (k, extraTop = '') => ({
  title: 'Zigbee', note: 'Sensors on the panel\'s Zigbee radio', back: 'zigbee-installer-menu',
  body: radioRow(k) + extraTop + (usable(k) ? actionRows() : dim(actionRows())) +
    M.section('Devices') + DEVICES.map(deviceRow).join('') + networkRows(),
});
const usable = (k) => ['working', 'empty', 'noisy'].includes(k);

// ================================ 1: the installer menu ================================
const menuBody = (zigbee) => [
  M.section('Sensors and devices'),
  M.row_link({glyph: 'door-closed', color: 'green', title: 'Zones', detail: 'Sensors, types, names', value: '12 in use', goTo: 'installer-zones'}),
  M.row_link({glyph: 'radio-tower', title: 'Z-Wave', detail: 'Sensors on the panel\'s Z-Wave radio', value: '5 devices'}),
  zigbee,
  M.row_link({glyph: 'warehouse', color: 'violet', title: 'Partitions', detail: 'Areas that arm separately', value: '1 in use'}),
  M.row_link({glyph: 'cpu', title: 'Keypads and devices', detail: 'Supervised keypads and expanders', value: 'None'}),
  M.section('How the alarm works'),
  M.row_link({glyph: 'sliders-horizontal', title: 'Delays and arming', detail: 'Entry and exit times, sirens, panic keys, codes', goTo: 'installer-options'}),
].join('');
page({id: 'zigbee-installer-menu', shot: 'bl-144-installer-menu', title: 'Installer menu: the Zigbee row',
  notes: 'A Zigbee row after Z-Wave (Zigbee is in every P4 image, ADR-047). Its value: "8 devices", "No devices", "Not connected" (gray), "Radio problem" or "Interference" (amber), read at most once a second. The rest of the menu is unchanged (abridged here). Icon: Lucide "radio", to be added to the icon fonts.'},
() => ({title: 'Installer', body: menuBody(M.row_link({glyph: 'radio', title: 'Zigbee', detail: 'Sensors on the panel\'s Zigbee radio', value: '8 devices', goTo: 'zigbee'}))}));
page({id: 'zigbee-installer-menu-problem', shot: 'bl-144-installer-menu-problem', title: 'Installer menu: Zigbee with interference',
  notes: 'The row\'s value turns amber for a problem: "Interference" here, "Radio problem" when the radio doesn\'t answer, is paused or the socket holds the Z-Wave radio.'},
() => ({title: 'Installer', body: menuBody(M.row_link_alert({glyph: 'radio', title: 'Zigbee', detail: 'Sensors on the panel\'s Zigbee radio', value: 'Interference', valueColor: 'amber', goTo: 'zigbee-interference'}))}));

// ================================ 2: the Zigbee page ================================
page({id: 'zigbee', shot: 'bl-144-zigbee', title: 'Zigbee: 8 devices, two need a look',
  notes: 'One row per device, not per endpoint: its name is its zone\'s name, else its model. Unreachable devices first, then by name. The amber second line is the one thing to do (not heard for 2 h 30 min, battery low, "Wake it to finish setting it up", "Signal weak: move it closer or add a mains device between"). Its words change in place, at most once a second; the page is not rebuilt. The value: "Zone 3", "Zones 3, 4", "Not in a zone", or what it is (Siren, Keypad, Key fob, BL-147 and BL-148). The radio row opens Radio details.'},
() => zigbeePage('working'));
page({id: 'zigbee-devices', shot: 'bl-144-zigbee-devices', title: 'Zigbee: scrolled to the devices',
  notes: 'Basement Water is first because it is not heard; Bedroom Window\'s battery is low. Both chips and second lines turn amber; the rest are green.'},
() => ({title: 'Zigbee', note: 'Sensors on the panel\'s Zigbee radio', back: 'zigbee-installer-menu',
  body: M.section('Devices') + DEVICES.slice(0, 5).map(deviceRow).join('')}));
page({id: 'zigbee-end', shot: 'bl-144-zigbee-end', title: 'Zigbee: scrolled to the end',
  notes: 'The end of the device list and the Network section: Channel and the red reset, kept last.'},
() => ({title: 'Zigbee', note: 'Sensors on the panel\'s Zigbee radio', back: 'zigbee-installer-menu',
  body: DEVICES.slice(5).map(deviceRow).join('') + networkRows()}));
page({id: 'zigbee-empty', shot: 'bl-144-zigbee-empty', title: 'Zigbee: no devices yet',
  notes: 'After the first start or a network reset. Install codes waiting says None.'},
() => ({title: 'Zigbee', note: 'Sensors on the panel\'s Zigbee radio', back: 'zigbee-installer-menu',
  body: radioRow('empty') + actionRows('None') + M.section('Devices') +
    M.paragraph('No devices yet. Add one: most sensors join when you hold their button until their light blinks.') + networkRows(0)}));
page({id: 'zigbee-starting', shot: 'bl-144-zigbee-starting', title: 'Zigbee: the radio starting',
  notes: 'For a few seconds after the panel or the radio starts. Adding and removing wait until it works; the rows are dimmed and say nothing else.'},
() => zigbeePage('starting'));
page({id: 'zigbee-not-connected', shot: 'bl-144-zigbee-not-connected', title: 'Zigbee: no radio in the socket',
  notes: 'Gray, not amber: an empty socket is a choice, not a fault. Devices added before are kept with their zones (they show CHECK).'},
() => ({...zigbeePage('absent', M.paragraph('Devices added before are kept. They are heard again once the Zigbee radio answers.'))}));
page({id: 'zigbee-not-answering', shot: 'bl-144-zigbee-not-answering', title: 'Zigbee: the radio not answering',
  notes: 'The system trouble "Sensor radio problem" opens this page (13). The panel keeps trying by itself.'},
() => zigbeePage('silent', M.paragraph('Zigbee sensors aren\'t heard until it answers. Their zones keep their last state; a Sensor radio problem follows if it lasts.', 'amber')));
page({id: 'zigbee-paused', shot: 'bl-144-zigbee-paused', title: 'Zigbee: paused after restarts',
  notes: 'The restart guard stopped using the radio after it caused two panel restarts (zigbee_service). Try again clears the guard and starts the radio once more (the console\'s zigbee retry).'},
() => zigbeePage('paused', M.row_action({glyph: 'refresh-cw', text: 'Try again', color: 'blue'}) +
  M.paragraph('Zigbee sensors aren\'t heard while it is paused. Their zones keep their last state; a Sensor radio problem follows if it lasts.', 'amber')));
page({id: 'zigbee-socket-zwave', shot: 'bl-144-zigbee-socket-zwave', title: 'Zigbee: the socket holds the Z-Wave radio',
  notes: 'Both radios are in the image; only one fits the wireless socket. The manual says where else the Z-Wave radio can go (docs/features/36).'},
() => zigbeePage('zwave', M.paragraph('Zigbee needs the wireless socket. The manual says where else the Z-Wave radio can go.')));
page({id: 'zigbee-interference', shot: 'bl-144-zigbee-interference', title: 'Zigbee: interference',
  notes: 'From the radio\'s jamming report. Background noise in dBm is on Radio details only.'},
() => zigbeePage('noisy', M.paragraph('Move Wi-Fi routers and other radios away from the panel, or choose a quieter channel below.', 'amber')));
// No "locked" Zigbee page: looking at these pages needs installer mode, so installer mode ending pops back
// to the locked Installer page and the Troubles row asks for the installer code first (ZB-027).

// ================================ 3: adding a device ================================
const STEPS = ['Listening', 'Joining', 'Reading what it can do', 'Setting it up', 'Ready'];
const steps = (now, detail) => STEPS.map((s, i) => M.row_link({
  glyph: i < now ? 'circle-check' : 'circle-dot', color: i < now ? 'green' : i === now ? 'blue' : 'gray',
  title: s, detail: i < now ? 'Done' : i === now ? detail : 'Waiting', trail: ''})).join('');
const listenButtons = () => M.action_pair({glyph: 'timer', text: 'Keep listening', color: 'blue'}, {glyph: 'x', text: 'Stop', color: 'gray', goTo: 'zigbee'});
const ADD = {title: 'Add a Zigbee device', back: 'zigbee'};

page({id: 'zigbee-add-listening', shot: 'bl-144-add-listening', title: 'Adding: listening, with the countdown',
  notes: 'Opening the page opens the network for 3 minutes (ZB-002); leaving it closes it. The countdown in the subtitle changes once a second, nothing else moves. Keep listening starts the 3 minutes again. Any Zigbee 3.0 device may join: those with a code under Install codes waiting join with their own key.'},
() => ({...ADD, note: 'Listening · 2 min 41 s left', body: [
  M.paragraph('Put the device in pairing mode now: most sensors, hold their button until their light blinks. Keep it close to the panel.'),
  steps(0, 'Any Zigbee 3.0 device may join'), listenButtons(),
  M.paragraph('Leaving this page stops listening.'),
].join('')}));
page({id: 'zigbee-add-found', shot: 'bl-144-add-found', title: 'Adding: two devices joined',
  notes: 'Devices show as rows as they arrive, newest last, each with its step in words: Joining, Reading what it can do (step 3 of 5), Setting it up, Ready. A ready one opens its result. Others may still join until the time runs out.'},
() => ({...ADD, note: 'Listening · 1 min 58 s left', body: [
  M.paragraph('2 devices joined. More can join until the time runs out.'),
  M.section('Joined'),
  M.row_link_alert({glyph: 'circle-check', color: 'green', title: 'Aqara door sensor', detail: 'Ready · Joined with the standard key', detailColor: 'green', value: 'Use it', goTo: 'zigbee-add-added'}),
  M.row_link_alert({glyph: 'circle-dot', color: 'blue', title: 'Aqara motion sensor', detail: 'Reading what it can do · step 3 of 5', detailColor: 'blue', trail: ''}),
  listenButtons(),
].join('')}));
page({id: 'zigbee-add-asleep', shot: 'bl-144-add-asleep', title: 'Adding: a sleepy device to wake',
  notes: 'A battery device that dozed off before its setup finished. The box takes the quirk\'s own words when the model has them (zb_quirks), else "Press its button to wake it." The panel goes on by itself once it wakes.'},
() => ({...ADD, note: 'Listening · 1 min 12 s left', body: [
  M.section('Joined'),
  M.row_link_alert({glyph: 'circle-check', color: 'green', title: 'Aqara door sensor', detail: 'Ready · Joined with the standard key', detailColor: 'green', value: 'Use it', goTo: 'zigbee-add-added'}),
  M.row_link_alert({glyph: 'footprints', color: 'amber', title: 'Aqara motion sensor', detail: 'Setting it up · step 4 of 5', sub: 'It went to sleep before it finished', trail: ''}),
  M.paragraph('Wake it: press its button once, then again every few seconds until it says Ready.', 'amber'),
  listenButtons(),
].join('')}));
page({id: 'zigbee-add-added', shot: 'bl-144-add-added', title: 'Adding: added, use it for',
  notes: 'New zone suggests the type for what it is (contact: Perimeter; motion: Interior follower; smoke: Fire; CO: CO; water: 24-hour) and opens its zone page to name it. An existing zone asks before its sensor is replaced. How it joined stays on its device page; a device on the standard key may still feed fire, CO and panic zones (decided 2026-10-02). From the zone editor there is no Use it for: the picker opens with it on top (11).'},
() => ({title: 'Added', note: 'Aqara door sensor · Joined with the standard key', back: 'zigbee-add-found', body: [
  M.row_status({glyph: 'circle-check', color: 'green', title: 'Added: Aqara door sensor', detail: 'Battery 100 % · Signal good · Closed'}),
  M.section('Use it for'),
  M.row_link({glyph: 'plus', color: 'green', title: 'A new zone', detail: 'Perimeter, as Zone 12', goTo: 'zigbee-zone-edit'}),
  M.row_link({glyph: 'arrow-left-right', title: 'An existing zone', detail: 'Its sensor is replaced, after a question'}),
  M.paragraph('Tamper and low battery come with the zone: nothing else to set up.'),
  M.row_action({glyph: 'x', text: 'Not now: keep it without a zone', color: 'gray', goTo: 'zigbee-add-found'}),
].join('')}));
const ending = (meta, title, look, words, buttons) => page({...meta},
  () => ({...ADD, note: title, body: M.row_status(look) + M.paragraph(words) + buttons}));
ending({id: 'zigbee-add-nothing', shot: 'bl-144-add-nothing', title: 'Adding: nothing joined',
  notes: 'The 3 minutes ran out with no device. Try again opens the window again.'}, 'Stopped listening',
  {glyph: 'circle-dot', color: 'gray', title: 'Nothing joined', detail: 'The 3 minutes ran out'},
  'Check the device\'s pairing steps in its manual: some need a reset first. Then try again.',
  M.row_action({glyph: 'refresh-cw', text: 'Try again', color: 'blue', goTo: 'zigbee-add-listening'}));
ending({id: 'zigbee-add-didnt-join', shot: 'bl-144-add-didnt-join', title: 'Adding: it didn\'t join',
  notes: 'It began to join but never finished (no key exchange, or it left again).'}, 'Stopped listening',
  {glyph: 'circle-x', color: 'red', title: 'Didn\'t join', detail: 'It started joining but didn\'t finish'},
  'Reset the device (see its manual), keep it close to the panel and try again.',
  M.row_action({glyph: 'refresh-cw', text: 'Try again', color: 'blue', goTo: 'zigbee-add-listening'}));
ending({id: 'zigbee-add-already', shot: 'bl-144-add-already', title: 'Adding: already added',
  notes: 'A device the panel knows rejoined (its address is in the list). Nothing changes: its zones stay.'}, 'Listening · 1 min 30 s left',
  {glyph: 'info', color: 'blue', title: 'Already added', detail: 'Aqara door sensor'},
  'This device is already in the network: it\'s Front Door.',
  M.row_action({glyph: 'chevron-right', text: 'Open it', color: 'blue', goTo: 'zigbee-device'}));
ending({id: 'zigbee-add-not-sensor', shot: 'bl-144-add-not-sensor', title: 'Adding: not a sensor',
  notes: 'Nothing on it the panel can use as a zone (no IAS zone, occupancy, contact or temperature input). Plugs and lights become Smart Home devices later (BL-149); until then Remove it asks it to leave.'}, 'Listening · 1 min 05 s left',
  {glyph: 'plug', color: 'amber', title: 'Not a sensor', detail: 'Sonoff smart plug · Mains'},
  'It has nothing the panel can use as a zone. Kept, it still extends the network for battery sensors.',
  M.action_pair({glyph: 'check', text: 'Keep it', color: 'blue'}, {glyph: 'trash-2', text: 'Remove it', color: 'red'}));
ending({id: 'zigbee-add-not-finished', shot: 'bl-144-add-not-finished', title: 'Adding: setup not finished',
  notes: 'The window closed and its setup stopped after its tries: never "Added". A stopped setup never goes on by itself, so no step count and no wake to wait for: only Set it up again on its page goes on (ZB-012).'}, 'Stopped listening',
  {glyph: 'triangle-alert', color: 'amber', title: 'Setup not finished', detail: 'Aqara motion sensor'},
  'It joined, but its setup stopped before it finished. Open it and set it up again (wake it first if it sleeps).',
  M.action_pair({glyph: 'chevron-right', text: 'Open it', color: 'blue', goTo: 'zigbee-device-new'}, {glyph: 'check', text: 'Done', color: 'gray', goTo: 'zigbee'}));

// ================================ 4: adding with an install code ================================
const CODE_PAGE = () => ({title: 'Add with an install code', note: 'For a device whose label has one', back: 'zigbee', body: [
  M.row_link({glyph: 'tag', title: 'Its address', detail: '16 characters on its label, often after MAC or IEEE', value: '0012 4B00 2A1F 3C9D', goTo: 'zigbee-code-address'}),
  M.row_link_alert({glyph: 'key-round', color: 'amber', title: 'Its install code', detail: 'On its label or box, often beside a QR code', value: 'Checked', valueColor: 'green', goTo: 'zigbee-code-entry'}),
  M.row_action({glyph: 'radio', text: 'Listen for this device', color: 'blue', goTo: 'zigbee-code-listening'}),
  M.paragraph('Only this device may join while the panel listens. It joins with its own key, not the standard one.'),
  M.paragraph('A phone can scan the QR code instead, in the Panel Manager.'),
].join('')});
page({id: 'zigbee-code', shot: 'bl-144-code', title: 'Install code: address and code typed',
  notes: 'Listen is offered once both are typed and the code\'s check matches. Not listened for now, the code waits under Install codes waiting.'},
CODE_PAGE);
page({id: 'zigbee-code-address', shot: 'bl-144-code-address', title: 'Install code: typing the address',
  notes: 'Built differently from the drawing: install codes and addresses are typed on the panel\'s own keyboard (the text sheet, opening on its number page; abc for the letters A-F), not a keyboard of its own. The note under the title is the count and the check, once a second: "16 characters on its label...", "7 of 16 characters", "All 16 characters", "Only 0 to 9 and A to F: compare it with the label". Done takes exactly 16 characters.'},
CODE_PAGE, {over: () => M.text_sheet({title: 'Its address', note: 'All 16 characters', value: '0012 4B00 2A1F 3C9D', numbers: true})});
page({id: 'zigbee-code-entry', shot: 'bl-144-code-entry', title: 'Install code: typing the code, its check matches',
  notes: 'The panel\'s own keyboard, as for the address. An install code is 16, 20, 28 or 36 characters, its last 4 a check (CRC) of the rest. The note under the title counts while typing ("28 of 36 characters"), then says whether the check matches.'},
CODE_PAGE, {over: () => M.text_sheet({title: 'Its install code', note: 'Its check matches: the code is right', value: '83FE D340 7A93 9723 A5C6 39B2 6916 D505 C3B5', numbers: true})});
page({id: 'zigbee-code-wrong', shot: 'bl-144-code-wrong', title: 'Install code: its check doesn\'t match',
  notes: 'Done is refused until the check matches (a toast says why); nothing is sent to the radio. The words are in the note under the title.'},
CODE_PAGE, {over: () => M.text_sheet({title: 'Its install code', note: 'It doesn\'t check out: compare it with the label', value: '83FE D340 7A93 9723 A5C6 39B2 6916 D505 C3B6', numbers: true})});
page({id: 'zigbee-code-listening', shot: 'bl-144-code-listening', title: 'Install code: listening for that device',
  notes: 'The same steps as Add a device, for this address only. When the time runs out its code stays under Install codes waiting. Its device page then says "How it joined: With its install code".'},
() => ({title: 'Add with an install code', note: 'Listening · 2 min 50 s left', back: 'zigbee-code', body: [
  M.paragraph('Power it up or put it in pairing mode now. Only the device 0012 4B00 2A1F 3C9D may join.'),
  steps(0, 'Only this device may join'), listenButtons(),
].join('')}));

// ================================ 5: the device page ================================
const DEVICE = {title: 'Front Door', note: 'Aqara door sensor · Zone 1', back: 'zigbee'};
const facts = (f) => [
  M.section('This device'),
  M.row_link({glyph: 'tag', title: 'Name', value: f.name}),
  M.row_value({title: 'Model', value: f.model}),
  M.row_value({title: 'What it is', value: f.kind}),
  M.row_value({title: 'Power', value: f.power}),
  M.row_value({title: 'Signal', value: f.signal}),
  M.row_value({title: 'Last heard', value: f.heard}),
  M.row_value({title: 'How it joined', value: f.joined}),
  M.row_value({title: 'Software', value: f.sw}),
].join('');
const checkRows = () => [
  M.section('Check it'),
  M.row_link({glyph: 'lightbulb', title: 'Blink its light', detail: 'Find which device this is', trail: ''}),
  M.row_link({glyph: 'activity', title: 'Check it now', detail: 'Asks it to answer', trail: ''}),
  M.row_link({glyph: 'refresh-cw', title: 'Set it up again', detail: 'Reads what it can do and how it reports', trail: ''}),
].join('');
// role: a siren, keypad or key fob feeds no zone, so it has no "Replace it" (decided in building it, alarm-devices.md)
const removeRows = (name, role = false) => [
  M.section('Removing it'),
  role ? '' : M.row_link({glyph: 'arrow-left-right', title: 'Replace it', detail: 'A new device takes its zones'}),
  M.row_link({glyph: 'trash-2', color: 'red', title: 'Remove this device', detail: 'Asks it to leave the network', goTo: 'zigbee-remove-confirm'}),
  M.row_link({glyph: 'unlink', color: 'red', title: 'Forget it: it\'s gone or broken', detail: 'Removed without its answer', goTo: name === 'Basement Water' ? 'zigbee-forget-confirm' : ''}),
].join('');
const FRONT = {name: 'Front Door', model: 'Aqara MCCGQ11LM', kind: 'Door and window contact', power: 'Battery 92 %',
  signal: 'Good', heard: '2 min ago', joined: 'With its install code', sw: '3.0.1'};
page({id: 'zigbee-device', shot: 'bl-144-device', title: 'A device\'s page',
  notes: 'Facts first, then what uses it, then what to do with it. Name renames it (the text sheet); a device in a zone is named after the zone until renamed. How it joined: "With its install code" or "With the standard key". Its words follow it in place, at most once a second. Only installer mode changes anything.'},
() => ({...DEVICE, body: facts(FRONT) + M.section('Used by') +
  M.row_link({glyph: 'door-closed', color: 'green', title: 'Zone 1 · Front Door', detail: 'Entry/exit 1', goTo: 'zigbee-zone-edit'})}));
page({id: 'zigbee-device-end', shot: 'bl-144-device-end', title: 'A device\'s page, scrolled: its actions',
  notes: 'Blink its light: the device\'s own light, for a few seconds (toast "Its light blinks for a few seconds") - never the panel\'s screen. Check it now: the toast says "It answered" or "No answer". Set it up again reads it again and sets its reporting; a sleepy one says "Press its button to wake it". Replace it: confirmed, then the add page as "Replace Front Door"; the new device takes its zones. Forget: only for one that is gone or broken (it isn\'t asked to leave).'},
() => ({...DEVICE, body: M.row_value({title: 'How it joined', value: FRONT.joined}) + M.row_value({title: 'Software', value: FRONT.sw}) +
  M.section('Used by') +
  M.row_link({glyph: 'door-closed', color: 'green', title: 'Zone 1 · Front Door', detail: 'Entry/exit 1', goTo: 'zigbee-zone-edit'}) +
  checkRows() + removeRows('Front Door')}), {toast: 'It answered'});
page({id: 'zigbee-device-unreachable', shot: 'bl-144-device-unreachable', title: 'A device\'s page: not heard',
  notes: 'An amber box leads with the one thing to do; Replace it and Forget come first in the list once it is missing (here scrolled to them).'},
() => ({title: 'Basement Water', note: 'Aqara water sensor · Zone 8', back: 'zigbee', body: [
  M.paragraph('Not heard for 2 h 30 min. Change its battery or bring it closer, then press its button to wake it.', 'amber'),
  M.row_value({title: 'Last heard', value: '2 h 30 min ago'}),
  M.row_value({title: 'Signal', value: 'Weak, when last heard'}),
  removeRows('Basement Water'),
  M.section('Used by'),
  M.row_link({glyph: 'droplets', color: 'amber', title: 'Zone 8 · Basement Water', detail: 'Shows CHECK'}),
].join('')}));
page({id: 'zigbee-device-new', shot: 'bl-144-device-new', title: 'A device\'s page: not in a zone yet',
  notes: 'Named after its model until it is in a zone or renamed. Make it a zone offers its inputs (as Use it for after adding); a device still being set up says "Finish its setup first" instead, gray.'},
() => ({title: 'Aqara motion sensor', note: 'Not in a zone', back: 'zigbee', body: [
  M.section('Used by'),
  M.row_link({glyph: 'plus', color: 'green', title: 'Make it a zone', detail: 'Motion: Interior follower'}),
  facts({name: 'Aqara motion sensor', model: 'Aqara RTCGQ11LM', kind: 'Motion sensor', power: 'Battery 100 %',
    signal: 'Fair', heard: 'Just now', joined: 'With the standard key', sw: '1.2.4'}),
].join('')}));
page({id: 'zigbee-forget-confirm', shot: 'bl-144-forget-confirm', title: 'Forgetting a device that is gone',
  notes: 'Forget removes it without asking it to leave. If it ever comes back it isn\'t trusted: it has to be added again.'},
() => ({title: 'Basement Water', note: 'Aqara water sensor · Zone 8', back: 'zigbee', body: removeRows('Basement Water')}),
{over: () => M.confirm_dialog({title: 'Forget Basement Water?', note: 'Only when it\'s gone or broken: it isn\'t asked to leave. Zone 8 keeps its settings and needs a new sensor.', ok: 'Forget it'})});

// ================================ 6: removing ================================
page({id: 'zigbee-remove-pick', shot: 'bl-144-remove-pick', title: 'Removing: pick the device',
  notes: 'Unlike Z-Wave, nothing is pressed first: the panel asks the device chosen to leave.'},
() => zigbeePage('working'),
{over: () => M.pick_sheet({title: 'Remove a device', note: 'It is asked to leave the network', rows:
  DEVICES.map((d) => M.row_link({glyph: d.g, color: d.sub ? 'amber' : 'green', title: d.name, detail: d.value, trail: '', goTo: d.name === 'Front Door' ? 'zigbee-remove-confirm' : ''})).join('')})});
page({id: 'zigbee-remove-confirm', shot: 'bl-144-remove-confirm', title: 'Removing: confirm', },
() => zigbeePage('working'),
{over: () => M.confirm_dialog({title: 'Remove Front Door?', note: 'It is asked to leave the network. Zone 1 keeps its settings and needs a new sensor.', ok: 'Remove'})});
const REMOVE = {title: 'Remove Front Door', back: 'zigbee'};
page({id: 'zigbee-remove-asked', shot: 'bl-144-remove-asked', title: 'Removing: asked it to leave',
  notes: 'A mains device answers at once. A sleepy one answers when it next wakes: the panel waits up to 2 minutes, the time left in the subtitle once a second.'},
() => ({...REMOVE, note: 'Waiting · 1 min 40 s left', body: [
  M.row_status({glyph: 'clock', color: 'blue', title: 'Asked it to leave', detail: 'Aqara door sensor · Zone 1'}),
  M.paragraph('A sleeping sensor answers when it wakes: press its button now.'),
  M.row_action({glyph: 'x', text: 'Stop waiting', color: 'gray'}),
].join('')}));
page({id: 'zigbee-remove-done', shot: 'bl-144-remove-done', title: 'Removing: removed', },
() => ({...REMOVE, note: 'Done', body: [
  M.row_status({glyph: 'circle-check', color: 'green', title: 'Removed: Front Door', detail: 'Aqara door sensor left the network'}),
  M.paragraph('Zone 1 needs a new sensor: it shows CHECK until it has one.', 'amber'),
  M.action_pair({glyph: 'trash-2', text: 'Remove another', color: 'gray', goTo: 'zigbee-remove-pick'}, {glyph: 'check', text: 'Done', color: 'blue', goTo: 'zigbee'}),
].join('')}));
page({id: 'zigbee-remove-no-answer', shot: 'bl-144-remove-no-answer', title: 'Removing: it didn\'t answer',
  notes: 'Forget it anyway asks once more (the Forget dialog) and removes it without its answer.'},
() => ({...REMOVE, note: 'No answer', body: [
  M.row_status({glyph: 'triangle-alert', color: 'amber', title: 'It didn\'t answer', detail: 'Front Door is still in the network'}),
  M.paragraph('If it is gone or broken it can\'t answer. Forget it anyway? The panel stops trusting it.'),
  M.action_pair({glyph: 'refresh-cw', text: 'Try again', color: 'blue'}, {glyph: 'unlink', text: 'Forget it anyway', color: 'red', goTo: 'zigbee-forget-confirm'}),
].join('')}));

// ================================ 7: radio details ================================
const radioDetails = (state, paused) => ({title: 'Radio details', note: 'The panel\'s Zigbee radio', back: 'zigbee', body: [
  M.row_value({title: 'State', value: state}),
  M.row_value({title: 'Radio software', value: '1.4.1 (ESP32-H2)'}),
  M.row_value({title: 'Channel', value: '15'}),
  M.row_value({title: 'Network id (PAN)', value: '1A62'}),
  M.row_value({title: 'Extended id', value: '02:AB:CD:00:00:00:00:FF'}),
  M.row_value({title: 'The panel\'s Zigbee address', value: '02:AB:CD:00:00:00:00:FE'}),
  M.row_value({title: 'Background noise', value: '-92 dBm'}),
  M.row_value({title: 'Panel restarts it caused', value: paused ? '2: paused' : '1 (it pauses at 2)'}),
  M.section(''),
  paused ? M.row_action({glyph: 'refresh-cw', text: 'Try again', color: 'blue'})
    : M.row_link({glyph: 'refresh-cw', title: 'Restart the radio', detail: 'Takes a few seconds; zones keep their state', trail: ''}),
].join('')});
page({id: 'zigbee-radio', shot: 'bl-144-radio', title: 'Radio details',
  notes: 'The one page with the radio\'s numbers, for the installer. Background noise is the latest reading on its channel, at most once a second. Restart asks first ("Restart the Zigbee radio?"); the State row follows it in place.'},
() => radioDetails('Working', false));
page({id: 'zigbee-radio-paused', shot: 'bl-144-radio-paused', title: 'Radio details: paused', },
() => radioDetails('Paused after restarts', true));

// ================================ 8: channel ================================
const channelBody = () => ({title: 'Channel', note: 'Checked for noise 10 min ago', back: 'zigbee', body: [
  M.row_link_alert({glyph: 'waves', color: 'gray', title: 'Channel 11', detail: 'Busy: Wi-Fi nearby', trail: ''}),
  M.row_link_alert({glyph: 'waves', title: 'Channel 15', detail: 'Some noise', value: 'Now', trail: 'check', trailColor: 'blue'}),
  M.row_link_alert({glyph: 'waves', color: 'green', title: 'Channel 20', detail: 'Quiet', value: 'Quietest', valueColor: 'green', trail: '', goTo: 'zigbee-channel-confirm'}),
  M.row_link_alert({glyph: 'waves', color: 'green', title: 'Channel 25', detail: 'Quiet', trail: ''}),
  M.paragraph('Sleeping devices move when they next check in, within 2 hours. Some may need to be added again.', 'amber'),
  M.row_link({glyph: 'activity', title: 'Check for noise again', detail: 'Sensors aren\'t heard for a few seconds', trail: ''}),
].join('')});
page({id: 'zigbee-channel', shot: 'bl-144-channel', title: 'Channel: the choice',
  notes: 'Four channels clear of the usual Wi-Fi ones (11, 15, 20, 25: the build\'s channel mask). The quietest from the latest noise check is marked; words, not dBm.'},
channelBody);
page({id: 'zigbee-channel-confirm', shot: 'bl-144-channel-confirm', title: 'Channel: confirm the move', },
channelBody, {over: () => M.confirm_dialog({title: 'Move to channel 20?', note: 'Every device follows. Sleeping ones move when they next check in; some may need to be added again.', ok: 'Move'})});

// ================================ 9: reset ================================
page({id: 'zigbee-reset', shot: 'bl-144-reset', title: 'Reset the Zigbee network: confirm',
  notes: 'Asked twice, as the Panel Manager asks (ZB-017): Continue, then "Reset it now?" (next screen), then the installer code once more; afterwards the page shows "Radio · Working" and "No devices".'},
() => zigbeePage('working'),
{over: () => M.confirm_dialog({title: 'Reset the Zigbee network?', note: 'All 8 devices lose the panel. Each must be reset and added again, and their zones show CHECK until then.', ok: 'Continue'})});
page({id: 'zigbee-reset-again', shot: 'bl-144-reset-again', title: 'Reset the Zigbee network: asked a second time',
  notes: 'The second question, in the Panel Manager\'s words; then the installer code.'},
() => zigbeePage('working'),
{over: () => M.confirm_dialog({title: 'Reset it now?', note: 'This can\'t be undone. Install codes waiting are kept.', ok: 'Reset the network'})});
page({id: 'zigbee-reset-code', shot: 'bl-144-reset-code', title: 'Reset the Zigbee network: the installer code again',
  notes: 'Only the installer code is taken; a master or user code is refused like a wrong one, and wrong codes count toward the lockout.'},
() => zigbeePage('working'),
{over: () => M.modal_pad({title: 'Confirm the reset', tone: 'red', glyph: 'rotate-ccw', hint: 'Enter the installer code once more', digits: 3})});

// ================================ 10: install codes waiting ================================
const codesBody = () => ({title: 'Install codes waiting', note: 'They may join whenever the panel listens', back: 'zigbee', body: [
  M.row_link({glyph: 'key-round', color: 'amber', title: '0012 4B00 2A1F 3C9D', detail: 'Added today at 10:41', value: 'Waiting', goTo: 'zigbee-codes-forget'}),
  M.row_link({glyph: 'key-round', color: 'amber', title: '0015 8D00 0A4C 71E2', detail: 'Added on September 28', value: 'Waiting'}),
  M.row_action({glyph: 'plus', text: 'Add an install code', color: 'blue', goTo: 'zigbee-code'}),
  M.paragraph('A device with its code here joins with its own key when the panel listens for new devices. Joined, it leaves this list.'),
].join('')});
page({id: 'zigbee-codes', shot: 'bl-144-codes', title: 'Install codes waiting',
  notes: 'Tapping one asks whether to forget it. Codes scanned in the Panel Manager (BL-145) land here too.'},
codesBody);
page({id: 'zigbee-codes-forget', shot: 'bl-144-codes-forget', title: 'Install codes waiting: forget one', },
codesBody, {over: () => M.confirm_dialog({title: 'Forget this install code?', note: '0012 4B00 2A1F 3C9D can then join only with the standard key.', ok: 'Forget'})});

// ================================ 11, 12: the zone editor ================================
page({id: 'zigbee-sensor-picker', shot: 'bl-144-sensor-picker', title: 'Pick the sensor: Zigbee, a device just added',
  notes: 'The zone editor\'s learn picker (ui_sources.c), which has no screen in the mockup yet. Its first row opens Add a device (3); coming back, the new device is on top, marked "Just added", and Use it for is skipped. Inputs in use by another zone are gray and not tappable. The list changes at most once a second.'},
() => ({title: 'Pick the sensor', note: 'Zigbee sensor · for Zone 12', back: 'zone-edit', body: [
  M.row_link({glyph: 'plus', color: 'green', title: 'Add a new Zigbee device', detail: 'Join it now, then pick its input', goTo: 'zigbee-add-listening'}),
  M.row_link_alert({glyph: 'door-closed', color: 'amber', title: 'Aqara door sensor', detail: 'Contact', sub: 'Just added', value: 'Closed', trail: ''}),
  M.row_link({glyph: 'door-closed', color: 'gray', title: 'Front Door', detail: 'In use: Zone 1', trail: ''}),
  M.row_link({glyph: 'footprints', color: 'gray', title: 'Hallway Motion', detail: 'In use: Zone 5', trail: ''}),
  M.paragraph('Open or move a sensor: it moves to the top and turns amber.'),
].join('')}));
page({id: 'zigbee-zone-edit', shot: 'bl-144-zone-edit', title: 'A zone with a Zigbee sensor, scrolled to its sensor',
  notes: 'Drawn scrolled to Its sensor; above it, as today, the zone\'s state (its detail "Zigbee · Works without Wi-Fi · Shows CHECK after 2 h silent") and The zone. The page gains a Zigbee device row (battery, signal, when last heard, at most once a second) that opens its device page. Source names Zigbee and the model and opens the picker again; tamper and low battery come with the device, so Sensor settings hides those inputs.'},
() => ({title: 'Front Door', note: 'Zone 1 · saved when you go back', back: 'installer-zones', body: [
  M.section('Its sensor'),
  M.row_link({glyph: 'door-closed', color: 'green', title: 'Sensor', value: 'Contact'}),
  M.row_link({glyph: 'wrench', title: 'Source', value: 'Zigbee · Aqara door sensor', goTo: 'zigbee-sensor-picker'}),
  M.row_link({glyph: 'radio', title: 'Zigbee device', detail: 'Battery 92 % · Signal good · Heard 2 min ago', goTo: 'zigbee-device'}),
  M.row_link({glyph: 'sliders-horizontal', title: 'Sensor settings', detail: 'Closed when off · supervised'}),
  M.section('What it does'),
  M.row_toggle({title: 'Chime', detail: 'Ding when it opens while disarmed and chime is on', on: true}),
].join('')}));

// ================================ 13: the trouble ================================
// Troubles is a rail view, not a settings page: its own frame.
M.screen({
  id: 'zigbee-troubles', area: AREA, title: 'Troubles: the radio\'s trouble opens its page', shot: 'bl-144-troubles',
  notes: '"Sensor radio problem" is the one trouble every sensor radio shares (sensor_radio). Tapped in installer mode, it opens the page of the radio that has the problem: here the Zigbee page, not answering. Outside installer mode it opens the locked Installer page with the toast "Enter the installer code to see the radio"; once the code is taken the Zigbee page follows (looking needs installer mode, built so). Its second line says it can be tapped. Nothing else on Troubles changes.',
  render: () => M.main({view: 'troubles', tone: 'amber', rail: {zones: 1, troubles: 2}, bar: {time: '10:48', troubles: 2}, content: `<div class="view column">
    <div class="row">${M.view_header({title: 'Troubles', note: '2 active'})}${M.button('Acknowledge', '', 'check')}</div><div class="list scroll">
      <div data-go="zigbee-not-answering">${M.trouble_row({glyph: 'radio-tower', title: 'Sensor radio problem', detail: 'Wireless sensors may not be heard · Tap to see the radio'})}</div>
      ${M.trouble_row({glyph: 'battery-warning', title: 'Bedroom Window · Low battery', detail: 'Check the sensor'})}</div></div>`}),
});

// ================================ 14: a siren (BL-147) ================================
const SIREN_PAGE = {title: 'Hall Siren', note: 'Heiman siren · Mains · Extends the network', back: 'zigbee'};
page({id: 'zigbee-siren', shot: 'bl-147-siren', title: 'A Zigbee siren\'s page',
  notes: 'Built with two switches, not three: "Burglary and panic" (an audible panic sounds it; a silent one never does) and "Fire and CO". Fire sounds its fire pattern; CO sounds its emergency tone (decided in building it, alarm-devices.md). Below these: how it sounds (next screen), then Removing it: Remove and Forget, no Replace it (a siren feeds no zone).'},
() => ({...SIREN_PAGE, body: [
  M.section('It sounds for'),
  M.row_toggle({glyph: 'siren', color: 'red', title: 'Burglary and panic', detail: 'Its partitions\' alarms; a silent panic stays silent', on: true}),
  M.row_toggle({glyph: 'flame', color: 'red', title: 'Fire and CO', detail: 'Its fire sound for fire, its emergency sound for CO', on: true}),
].join('')}));
page({id: 'zigbee-siren-end', shot: 'bl-147-siren-end', title: 'A Zigbee siren\'s page, scrolled',
  notes: 'Chirp: one chirp to arm, two to disarm (squawk), off for a new siren. Volume: Low, Medium, High, Very high. Partitions: All or a list. Test it sounds it for 2 seconds at its own volume (its row says "Sounding now: listen for it", then "Last test: it answered and sounded") and is refused while a partition is armed (toast "Not while one of its partitions is armed"). Supervision raises the siren trouble (the engine\'s bell trouble, not a new type) when it isn\'t heard. Its strobe is the siren\'s own light: nothing on the panel\'s screen flashes.'},
() => ({...SIREN_PAGE, body: [
  M.section('How it sounds'),
  M.row_toggle({glyph: 'sun', title: 'Strobe with it', detail: 'Its own light, while it sounds', on: true}),
  M.row_toggle({glyph: 'bell', title: 'Chirp when arming and disarming', detail: 'One chirp to arm, two to disarm'}),
  M.row_link({glyph: 'volume-2', title: 'Volume', value: 'High'}),
  M.row_link({glyph: 'warehouse', color: 'violet', title: 'Partitions', value: 'All'}),
  M.section('Check it'),
  M.row_link({glyph: 'bell-ring', title: 'Test it', detail: 'Sounds it for 2 seconds', trail: ''}),
  M.row_link({glyph: 'clock-alert', color: 'amber', title: 'Supervision', detail: 'A siren trouble when it isn\'t heard', value: 'After 1 hour'}),
  removeRows('Hall Siren', true),
].join('')}));

// ================================ 15, 16: a keypad and a key fob (BL-148) ================================
page({id: 'zigbee-keypad', shot: 'bl-148-keypad', title: 'A Zigbee keypad\'s page',
  notes: 'A code typed on it goes to the panel, which checks it like a code on this screen: 5 wrong codes lock it (and this screen) for a while. Show the panel\'s state sends armed, exit and entry time and alarms to its own lights and sounds. Built with a Keypad number row (the number the panel gave it, 1 to 8, which its events and the panel\'s keypad list use). Each button offers Off or its one alarm: Panic is an audible panic alarm, Fire a fire alarm, Emergency a medical alarm; no silent panic from a keypad (decided in building it, alarm-devices.md). Partition may be Off: it then arms and disarms nothing. Below: Removing it, with Remove and Forget but no Replace it.'},
() => ({title: 'Entry Keypad', note: 'Develco keypad · Battery 81 % · Signal good', back: 'zigbee', body: [
  M.section('Arming'),
  M.row_link({glyph: 'warehouse', color: 'violet', title: 'Partition', detail: 'The one it arms and disarms', value: 'Home'}),
  M.paragraph('Codes typed on it are checked by the panel. Five wrong codes lock it for a while, like this screen.'),
  M.row_toggle({glyph: 'shield', title: 'Show the panel\'s state', detail: 'Its lights and sounds follow the panel', on: true}),
  M.row_value({title: 'Keypad number', value: '1'}),
  M.section('Its buttons'),
  M.row_link({glyph: 'siren', color: 'red', title: 'Panic', value: 'Panic alarm, audible'}),
  M.row_link({glyph: 'flame', color: 'red', title: 'Fire', value: 'Fire alarm'}),
  M.row_link({glyph: 'bell-ring', color: 'red', title: 'Emergency', value: 'Medical alarm'}),
  removeRows('Entry Keypad', true),
].join('')}));
page({id: 'zigbee-fob', shot: 'bl-148-fob', title: 'A Zigbee key fob\'s page',
  notes: 'A fob belongs to a user: its presses are logged with that name. It arms without a code. May disarm and Panic button are off for a new fob; the installer turns them on per fob. Below: Removing it, with Remove and Forget but no Replace it.'},
() => ({title: 'Alex\'s key fob', note: 'Aqara key fob · Battery 97 % · Signal good', back: 'zigbee', body: [
  M.section('Who uses it'),
  M.row_link({glyph: 'user', color: 'violet', title: 'Belongs to', detail: 'Its presses are logged with this name', value: 'Alex'}),
  M.row_link({glyph: 'warehouse', color: 'violet', title: 'Arms', value: 'Home'}),
  M.section('What it may do'),
  M.row_toggle({glyph: 'lock-open', color: 'green', title: 'May disarm', detail: 'Off: it only arms, so a lost fob can\'t open the house'}),
  M.row_toggle({glyph: 'siren', color: 'red', title: 'Panic button', detail: 'Sounds a panic alarm'}),
  M.paragraph('Disarming from a fob takes no code: turn it on only for a fob its owner keeps with their keys.'),
  removeRows('Alex\'s key fob', true),
].join('')}));
