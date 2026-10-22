// BL-223: a keypad is a full front end to its alarm panel (docs/features/11 "The master's settings and
// installer menu on keypads", LINK-057..LINK-065). With the alarm panel in a closet (the headless S3,
// docs/features/42), an installer sets the whole system up from any keypad: the alarm panel's users,
// integrations, automations, Smart Home, walk test and installer menu open on the keypad, read from
// and saved on the alarm panel, which checks every code and every change. The keypad's own pages stay
// its own: Screen and sound, Wi-Fi and network, Time and language, About.
//
// What changes on the screen: the keypad's Settings list gets two groups with a heading each (a new
// part, settings_heading), the alarm panel's sections come back under "Harbor House", the alarm
// panel's pages show "Harbor House" in their note, and four new states: the first read ("Getting it
// from Harbor House"), installer mode held elsewhere, the link lost, a save not confirmed. The
// alarm panel's own pages, rows and words are otherwise the ones it shows today.
// Fictional data: Harbor House, Alex, Pat and Sam.

const P = (why) => ({bl: 'BL-223', status: 'approved', why,
  decided: '2026-10-21: the owner, "Approve the BL-223 proposal and its five questions as recommended. Start building it on a branch with a draft PR."'});
const KP_BAR = {banner: null, time: '10:42'};
const INST_KP_BAR = {banner: 'installer', time: '10:52'};
// The keypad's Settings list: its own four, then the alarm panel's under its name. Smart Home's line
// shows under the alarm panel's rule (it has a device). Installer is a section here (question 1).
const KP_GROUPS = [
  {heading: 'This keypad', changed: true},
  {key: 'display', glyph: 'sun', title: 'Screen and sound'},
  {key: 'wifi', glyph: 'wifi', title: 'Wi-Fi and network'},
  {key: 'time', glyph: 'calendar-clock', title: 'Time and language'},
  {key: 'about', glyph: 'info', title: 'About'},
  {heading: 'Harbor House', changed: true},
  {key: 'users', glyph: 'users', title: 'Users', goTo: 'bl-223-kp-users-code'},
  {key: 'integrations', glyph: 'puzzle', title: 'Integrations'},
  {key: 'automations', glyph: 'zap', title: 'Automations'},
  {key: 'smarthome', glyph: 'house-wifi', title: 'Smart Home'},
  {key: 'walk', glyph: 'footprints', title: 'Walk test'},
  {key: 'installer', glyph: 'wrench', title: 'Installer', goTo: 'bl-223-kp-installer-locked'},
];
const kpScreen = (meta, notes, page, extra = {}) => M.screen({...meta, area: 'Keypad', notes,
  proposal: P(meta.why || 'An installer can set the whole system up from any keypad, not only at the alarm panel.'),
  render: () => M.main({view: 'settings', tone: extra.tone || 'green', rail: {devices: true, troubles: 0, ...(extra.rail || {})},
    bar: extra.bar || KP_BAR, content: M.settings({sections: KP_GROUPS, ...page}), over: extra.over || ''})});

// ---- 1. The Settings list ----
kpScreen({id: 'bl-223-kp-settings', title: 'Keypad: Settings in two groups, its own and Harbor House\'s', shot: null},
  'Replaces kp-settings\'s list. Two headings, steady words in text_3: "This keypad" over the four pages the keypad keeps (LINK-057), then the alarm panel\'s name over its sections, which a keypad hid until now (the role gate). The open page here is the keypad\'s own Screen and sound, unchanged. A keypad whose alarm panel has older firmware (no remote settings, LINK-065) keeps today\'s list: the four, with no headings. A section the alarm panel doesn\'t have (no automations in its firmware, no Smart Home device) is left out, as on the alarm panel. The list scrolls when it is longer than the screen (ten lines and two headings); opened on one of Harbor House\'s pages it starts scrolled so the open section shows. The two headings are what is new (outlined); the sections under Harbor House are the alarm panel\'s, which a keypad hid until now.',
  {section: 'display', title: 'Screen and sound', note: '', body: [
    M.section('Sound'),
    M.row_level({glyph: 'bell', title: 'Chime volume', value: 70}),
    M.row_level({glyph: 'megaphone', title: 'Voice volume', value: 70}),
    M.row_level({glyph: 'volume-2', title: 'Beeps volume', value: 70}),
    M.paragraph('Alarms, fire and CO always sound at full volume.'),
    M.row_toggle({title: 'Key clicks', detail: 'A short click for every touch', on: true}),
  ].join('')});

// ---- 2. A code, checked by the alarm panel ----
kpScreen({id: 'bl-223-kp-users-code', title: 'Keypad: Harbor House\'s Users ask for a code', shot: null},
  'The same pad as the alarm panel\'s Users (settings-users-code). The code goes to Harbor House, which checks it (LINK-059): a wrong one counts toward this keypad\'s lockout, as at the alarm panel\'s own pad. A right one opens the alarm panel\'s Users page, its note "Signed in as Master · Codes are never shown" as there. The code is good for the alarm panel\'s pages until 5 minutes without a change, or until the person leaves them.',
  {section: 'users', pad: M.code_pad({title: 'Users', tone: 'violet', glyph: 'users', hint: 'Enter a master code for Harbor House', digits: 4, width: 428})});

// ---- 3. Installer ----
kpScreen({id: 'bl-223-kp-installer-locked', title: 'Keypad: Harbor House\'s Installer, before the code', shot: null},
  'The alarm panel\'s Installer page (installer-locked) as a section of the keypad\'s list. The note names the alarm panel. "Lost the installer code?" is left out: unplugging a keypad doesn\'t open the alarm panel\'s recovery, so the words would mislead; the alarm panel\'s own page and the manual keep it. The installer code is checked on Harbor House.',
  {section: 'installer', title: 'Installer', note: 'Changes how Harbor House works', body: [
    M.row_action({glyph: 'key-round', text: 'Enter the installer code', color: 'amber'}),
    M.paragraph('While you change settings nothing can be armed, here or on Harbor House. Fire, CO and panic buttons keep working.'),
  ].join('')});

kpScreen({id: 'bl-223-kp-installer-menu', title: 'Keypad: Harbor House\'s installer menu', shot: null},
  'Installer mode on the alarm panel, entered from Keypad 2 (LINK-060). The banner is today\'s installer banner, and a tap on it leaves. The rows are the alarm panel\'s menu (installer-menu) with Z-Wave and Zigbee as the alarm panel has them; each opens the alarm panel\'s page, read from it and saved on it. Left out, with the line that says where they are (question 2): the alarm panel\'s Update server, Update channel, Developer mode, Run the setup again and Factory reset. Keypads and devices lists Harbor House\'s keypads; Add a keypad works from here too (question 5). Saving a zone, a delay or a name takes about a second; meanwhile taps wait, nothing on the screen changes.',
  {section: 'installer', title: 'Installer', note: 'Harbor House', body: [
    M.section('Sensors and devices'),
    M.row_link({glyph: 'door-closed', color: 'green', title: 'Zones', detail: 'Sensors, types, names', value: '8 in use', goTo: 'bl-223-kp-zones'}),
    M.row_link({glyph: 'warehouse', color: 'violet', title: 'Partitions', detail: 'Areas that arm separately', value: '1 in use'}),
    M.row_link({glyph: 'cpu', title: 'Keypads and devices', detail: 'Supervised keypads and expanders', value: '2 keypads'}),
    M.row_link({glyph: 'radio-tower', title: 'Z-Wave', detail: 'Sensors on Harbor House\'s Z-Wave radio'}),
    M.row_link({glyph: 'radio', title: 'Zigbee', detail: 'Sensors on Harbor House\'s Zigbee radio', goTo: 'bl-223-kp-zigbee-add'}),
    M.row_link({glyph: 'siren', color: 'red', title: 'Sirens and outputs', detail: 'Wired and 433 MHz'}),
    M.section('How the alarm works'),
    M.row_link({glyph: 'sliders-horizontal', title: 'Delays and arming', detail: 'Entry and exit times, sirens, panic keys, codes'}),
    M.section('This panel'),
    M.row_link({glyph: 'house', color: 'violet', title: 'System name', detail: 'Shown on the screen and in the event log', value: 'Harbor House'}),
    M.row_link({glyph: 'key-round', color: 'amber', title: 'Installer and master codes', detail: 'Factory codes still in use'}),
    M.row_toggle({glyph: 'monitor', title: 'Panel Manager', detail: 'Change settings from a browser on your network', on: true}),
    M.changed(M.paragraph('Updates, developer mode, the setup and a factory reset of Harbor House are on its Panel Manager.')),
    M.row_action({glyph: 'log-out', text: 'Leave installer mode', color: 'blue'}),
  ].join('')}, {tone: 'blue', rail: {tone: 'blue'}, bar: INST_KP_BAR});

kpScreen({id: 'bl-223-kp-zones', title: 'Keypad: Harbor House\'s zones, a new one being added', shot: null},
  'The alarm panel\'s Zones (installer-zones) on the keypad. Add a zone and the zone pages are the alarm panel\'s, their sensor pickers listing what Harbor House hears: its wired terminals, each keypad\'s terminals (LINK-055), its 433 MHz, Z-Wave and Zigbee sensors. Listening for a sensor runs on Harbor House and stops when the page closes (LINK-062).',
  {section: 'installer', title: 'Zones', note: 'Harbor House · 8 of 128 zones in use', back: 'bl-223-kp-installer-menu', body: [
    M.row_action({glyph: 'plus', text: 'Add a zone', color: 'blue'}),
    M.row_link({glyph: 'door-closed', color: 'green', title: 'Front Door', detail: 'Zone 1 · Entry/exit 1 · Wired terminal 1'}),
    M.row_link({glyph: 'warehouse', color: 'green', title: 'Garage Door', detail: 'Zone 2 · Entry/exit 2 · Keypad 2, terminal 2'}),
    M.row_link({glyph: 'app-window', color: 'green', title: 'Living Room Window', detail: 'Zone 3 · Perimeter · Zigbee'}),
    M.row_link({glyph: 'footprints', color: 'green', title: 'Hallway Motion', detail: 'Zone 5 · Interior follower · 433 MHz'}),
    M.row_link({glyph: 'alarm-smoke', color: 'green', title: 'Kitchen Smoke', detail: 'Zone 6 · Fire · Z-Wave'}),
  ].join('')}, {tone: 'blue', rail: {tone: 'blue'}, bar: INST_KP_BAR});

kpScreen({id: 'bl-223-kp-zigbee-add', title: 'Keypad: pairing a Zigbee device on Harbor House', shot: null},
  'The alarm panel\'s Add a Zigbee device (zigbee-add-listening), started from the keypad: Harbor House\'s radio listens, so the paragraph says to keep the device close to Harbor House, not to this keypad (the one word that changes). The countdown is Harbor House\'s, read once a second; nothing else moves. Leaving the page stops listening on Harbor House, and so does losing the link (LINK-062). Z-Wave\'s Add and Remove and 433 MHz\'s pairing work the same way.',
  {section: 'installer', title: 'Add a Zigbee device', note: 'Listening · 2 min 41 s left', back: 'bl-223-kp-installer-menu', body: [
    M.changed(M.paragraph('Put the device in pairing mode now: most sensors, hold their button until their light blinks. Keep it close to Harbor House.')),
    M.row_link({glyph: 'circle-dot', color: 'blue', title: 'Listening', detail: 'Any Zigbee 3.0 device may join', trail: ''}),
    M.row_link({glyph: 'circle-dot', color: 'gray', title: 'Joining', detail: 'Waiting', trail: ''}),
    M.row_link({glyph: 'circle-dot', color: 'gray', title: 'Ready', detail: 'Waiting', trail: ''}),
    M.action_pair({glyph: 'timer', text: 'Keep listening', color: 'blue'}, {glyph: 'x', text: 'Stop', color: 'gray'}),
    M.paragraph('Leaving this page stops listening.'),
  ].join('')}, {tone: 'blue', rail: {tone: 'blue'}, bar: INST_KP_BAR});

// ---- 4. The new states ----
kpScreen({id: 'bl-223-kp-getting', title: 'Keypad: a page of Harbor House\'s, its first read', shot: null},
  'LINK-063. A page of the alarm panel\'s whose answer hasn\'t come yet: one steady line, no spinner, nothing to tap. On a working network it lasts a fraction of a second; after that the page reads again in the background at most once a second, without redrawing what didn\'t change.',
  {section: 'installer', title: 'Z-Wave', note: 'Harbor House', back: 'bl-223-kp-installer-menu', body: [
    M.changed(M.paragraph('Getting it from Harbor House...')),
  ].join('')}, {tone: 'blue', rail: {tone: 'blue'}, bar: INST_KP_BAR});

kpScreen({id: 'bl-223-kp-installer-busy', title: 'Keypad: installer mode is open somewhere else', shot: null},
  'LINK-060: one place at a time. While Keypad 1 (or Harbor House\'s own screen, or its Panel Manager) holds installer mode, this keypad\'s Installer says where, in amber, and Enter the installer code is dimmed. It opens by itself once installer mode ends there. The status bar shows the installer banner as everywhere while installer mode is on: "Installer at Keypad 1 · Arming off".',
  {section: 'installer', title: 'Installer', note: 'Changes how Harbor House works', body: [
    M.changed(M.paragraph('Installer mode is open at Keypad 1. It can be open in one place at a time.', 'amber')),
    M.row_action({glyph: 'key-round', text: 'Enter the installer code', color: 'amber', disabled: true}),
    M.paragraph('While you change settings nothing can be armed, here or on Harbor House. Fire, CO and panic buttons keep working.'),
  ].join('')}, {bar: {banner: {glyph: 'wrench', tone: 'blue', text: 'Installer at Keypad 1 · Arming off'}, time: '10:52'}});

kpScreen({id: 'bl-223-kp-lost', title: 'Keypad: Harbor House can\'t be reached, in installer mode', shot: null},
  'LINK-063, as Smart Home\'s lost state (kp-devices-lost): the page stays, every row dimmed, nothing sent. The note is the line. Harbor House ends this keypad\'s installer mode a minute after it stops hearing it, so the house can be armed again; if the link comes back sooner the page reads again and goes on. The keypad\'s own pages (This keypad) work as before, and after 10 minutes LINK-025\'s maintenance opens as today.',
  {section: 'installer', title: 'Installer', note: 'Harbor House can\'t be reached right now', body: [
    M.changed(M.paragraph('Nothing here can change until Harbor House answers. Its installer mode ends a minute after it stops hearing this keypad.', 'amber')),
    M.row_link({glyph: 'door-closed', color: 'green', title: 'Zones', detail: 'Sensors, types, names', value: '8 in use', disabled: true}),
    M.row_link({glyph: 'warehouse', color: 'violet', title: 'Partitions', detail: 'Areas that arm separately', value: '1 in use', disabled: true}),
    M.row_link({glyph: 'cpu', title: 'Keypads and devices', detail: 'Supervised keypads and expanders', value: '2 keypads', disabled: true}),
    M.row_link({glyph: 'sliders-horizontal', title: 'Delays and arming', detail: 'Entry and exit times, sirens, panic keys, codes', disabled: true}),
  ].join('')}, {tone: 'blue', rail: {tone: 'blue'}, bar: {banner: null, time: '10:53'}});

kpScreen({id: 'bl-223-kp-not-confirmed', title: 'Keypad: a change Harbor House didn\'t confirm', shot: null},
  'LINK-063: a save that got no answer within 3 s says so in one amber line under the page\'s head, never "Saved". Saving again is safe: Harbor House runs a change once (LINK-058). If the configuration changed on Harbor House since the page was read (someone else saved), the line is "Changed on Harbor House: here it is again" and the page shows Harbor House\'s values (LINK-061).',
  {section: 'installer', title: 'Hallway Motion', note: 'Zone 5 · saved when you go back', back: 'bl-223-kp-zones', body: [
    M.changed(M.paragraph('Not confirmed: Harbor House didn\'t answer. Try again.', 'amber')),
    M.section('The zone'),
    M.row_link({glyph: 'tag', title: 'Name', value: 'Hallway Motion'}),
    M.row_link({glyph: 'shield', title: 'Type', value: 'Interior follower (10)'}),
    M.section('Its sensor'),
    M.row_link({glyph: 'wrench', title: 'Source', value: '433 MHz'}),
  ].join('')}, {tone: 'blue', rail: {tone: 'blue'}, bar: INST_KP_BAR});

// ---- 5. On the alarm panel's own screen (a 7" or 10.1" master) ----
M.screen({id: 'bl-223-master-installer-at-keypad', area: 'Home', title: 'Alarm panel: installer mode held at Keypad 2', shot: null,
  proposal: P('Whoever stands at the alarm panel sees that an installer is working from a keypad, and can end it.'),
  notes: 'LINK-060. On the alarm panel\'s own screen (and every other keypad\'s) the installer banner names where installer mode is held; only its words change (outlined), and Home is as in installer mode today, arming off. A tap on the banner asks "End installer mode at Keypad 2?" (End it / Cancel) with the installer code, and ends it there: that keypad\'s pages go back to Installer\'s first page. The Panel Manager\'s installer has the same.',
  render: () => M.main({view: 'home', tone: 'blue', rail: {tone: 'blue'},
    bar: {banner: {glyph: 'wrench', tone: 'blue', text: 'Installer at Keypad 2 · Arming off · Tap to end'}, time: '10:52'},
    content: M.home({tone: 'green', glyph: 'shield-check', headline: 'Disarmed', detail: 'Ready to arm', note: 'All zones closed'}, M.panel_arm())})
    .replace('class="bar_banner', 'class="changed bar_banner'),
});
