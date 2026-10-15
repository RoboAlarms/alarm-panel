// BL-131: patterns that repeat across pages (mockup-next).
// - An integration's state was a card above a switch that said the same ("Off" / "Home Assistant
//   integration"): now the switch carries the state in its detail.
// - Destructive buttons sat mid-page (Delete this user above the Panel Manager section): now last.
// - Steppers held words ("Source default", "Not supervised") that wrap: a list is a picker row.
// - Sheets were 520 px tall around one choice: a sheet is as tall as its choices.
// - Toasts repeated what the screen shows, and the setup's last step offered "Leave setup".

const RAIL = {troubles: 1};
const BAR = {time: '10:52', troubles: 1, wifi: 'wifi'};
const GO = {display: 'n-display', users: 'settings-users', 'wifi': 'n-wifi', time: 'n-time', integrations: 'n-integrations',
  walk: 'settings-walk', about: 'n-about', installer: 'n-installer-menu'};
const P = (replaces, why) => ({bl: 'BL-131', status: 'approved', decided: '2026-10-01: approved as drawn: "start working on getting all of the screen changes in place"', replaces, why});
const page = ({id, area = 'Settings', label, title, replaces, why, notes, over, ...s}) => M.screen({
  id, area, title: label || title, shot: null, proposal: P(replaces, why), notes,
  render: () => M.main({next: true, view: 'settings', rail: RAIL, bar: BAR, content: M.settings({next: true, go: GO, title, ...s}), over: over ? over() : ''}),
});

// ---- Home Assistant ----
page({
  id: 'n-ha-off', title: 'Home Assistant', replaces: 'integration-ha', section: 'integrations', back: 'n-integrations',
  why: 'Two rows said one thing: a card "Off - Switch it on so Home Assistant can find the panel" above the switch "Home Assistant integration". Now one switch, its state in the detail, and one sentence on how pairing works.',
  body: [
    M.changed(M.row_toggle({title: 'Home Assistant', detail: 'Off · switch it on so Home Assistant can find the panel'})),
    M.paragraph('Home Assistant finds the panel on your network and you allow it here. Nothing to type, and no Home Assistant password.'),
  ].join(''),
});

page({
  id: 'n-ha-code', label: 'Home Assistant: comparing the code', title: 'Home Assistant', replaces: 'ha-pairing-code', section: 'integrations', back: 'n-integrations',
  why: 'The six digits a person has to compare were the smallest thing on the page, under a status card, a switch and a section. Now they are big, with the question and two answers side by side.',
  notes: 'Green "Yes, allow" on the right where Next usually is; red "No, reject" on the left.',
  body: [
    M.row_toggle({title: 'Home Assistant', detail: 'On · pairing', on: true}),
    M.changed(M.big_value({label: 'Does Home Assistant show this code?', value: '588 460'})),
    M.changed(M.action_pair({glyph: 'circle-x', text: 'No, reject', color: 'red'}, {glyph: 'circle-check', text: 'Yes, allow', color: 'green'})),
    M.paragraph('If the codes differ, something on your network is in between: reject it.', 'amber'),
  ].join(''),
});

page({
  id: 'n-ha-paired', label: 'Home Assistant: paired', title: 'Home Assistant', replaces: 'ha-paired', section: 'integrations', back: 'n-integrations',
  why: '"Connected - aioroboalarms 0.1.0" and "Paired with aioroboalarms 0.1.0": the name of the library inside Home Assistant, twice. Now it says connected and where to find the panel in Home Assistant; Unpair stands apart.',
  notes: 'The library\'s name and version go to System details (or the event log), for support.',
  body: [
    M.changed(M.row_toggle({title: 'Home Assistant', detail: 'On · connected and paired', on: true})),
    M.changed(M.paragraph('Find the panel in Home Assistant under Settings > Devices & services.')),
    M.section(''),
    M.row_action({glyph: 'unlink', text: 'Unpair', color: 'red'}),
    M.paragraph('Removes the panel from Home Assistant until it pairs again.'),
  ].join(''),
});

// ---- ntfy (Pushover and MQTT follow the same order) ----
page({
  id: 'n-ntfy', title: 'ntfy', replaces: 'ntfy-page', section: 'integrations', back: 'n-integrations',
  why: 'Getting alerts on a phone meant scrolling past the server settings to two buttons and a paragraph; "What to send" was two screens down; a status card repeated the switch. Now: the switch with its state, adding a phone, what to send, and where the alerts go, last.',
  notes: '"Add a phone" opens the QR code with the sentence that is a paragraph today. Pushover: switch, what to send, alarm sound, then the account. MQTT: switch, then the broker.',
  note: 'An alert on your phone, with no account to make', body: [
    M.changed(M.row_toggle({title: 'Send notifications', detail: 'Off · alerts go to every phone on this topic'})),
    M.changed(M.section('Your phones')),
    M.changed(M.row_link({glyph: 'qr-code', color: 'violet', title: 'Add a phone', detail: 'Install ntfy, then scan a QR code or type the topic'})),
    M.row_action({glyph: 'send', text: 'Send a test notification', color: 'blue'}),
    M.section('What to send'),
    M.row_toggle({title: 'Alarms', detail: 'Burglary, fire, carbon monoxide, panic', on: true}),
    M.row_toggle({title: 'Arming and disarming', detail: 'And who did it', on: true}),
    M.row_toggle({title: 'Troubles', detail: 'Mains power, batteries, tamper, a sensor gone quiet', on: true}),
    M.row_toggle({title: 'Zone activity', detail: 'Bypassed zones and the door chime'}),
    M.row_toggle({title: 'System and admin', detail: 'Restarts, updates, code and setting changes'}),
    M.changed(M.section('Where they go')),
    M.row_link({glyph: 'globe', title: 'Server', detail: 'The free hosted service', value: 'ntfy.sh'}),
    M.changed(M.row_link({glyph: 'tag', title: 'Topic', detail: 'Keep it private: anyone with it can read your alerts', value: 'roboalarms-a6efk9p7'})),
    M.row_link({glyph: 'key-round', title: 'Access token', detail: 'Only if your server asks for one', value: 'None'}),
    M.changed(M.row_link({glyph: 'refresh-cw', color: 'gray', title: 'New topic', detail: 'Phones on the old topic stop getting alerts'})),
  ].join(''),
});

// ---- Users ----
page({
  id: 'n-user', label: 'A user with browser sign-in', replaces: 'user-page', section: 'users', back: 'settings-users',
  why: '"Change the browser password" with "On" beside it read as two things in one row, a red "Turn off browser sign-in" followed it, and "Delete this user" sat above the whole section. Now browser sign-in is a switch with the password under it, and Delete is last, on its own.',
  notes: 'Turning the switch off asks "Turn off browser sign-in?" as today; on asks for the password. A Guest has no Panel Manager section; the master has no Delete.',
  title: 'Pat', note: 'User 03 · Standard', body: [
    M.row_link({glyph: 'tag', title: 'Name', value: 'Pat'}),
    M.row_link({glyph: 'shield', title: 'What this code can do', detail: 'Arms, disarms, bypasses, chime', value: 'Standard'}),
    M.row_link({glyph: 'key-round', color: 'amber', title: 'Change the code', detail: '4 to 10 digits'}),
    M.section('Panel Manager'),
    M.changed([
      M.row_toggle({glyph: 'globe', title: 'Browser sign-in', detail: 'User name: Pat · a password, not the keypad code', on: true}),
      M.row_link({glyph: 'key-round', title: 'Change the browser password'}),
    ].join('')),
    M.section(''),
    M.changed(M.row_action({glyph: 'trash-2', text: 'Delete this user', color: 'red'})),
  ].join(''),
});

// ---- Pickers: as tall as their choices; words in a picker, not a stepper ----
page({
  id: 'n-language-pick', label: 'Choosing the language (one installed)', replaces: 'language-pick', section: 'time',
  why: 'A sheet 520 px tall around one choice. Now a sheet is as tall as its choices (up to 520 px, then it scrolls).',
  notes: 'pick_sheet fit: every picker, not only this one (Region, Clock, Zone type, the authority).',
  title: 'Time and language', body: [
    M.row_link({glyph: 'languages', title: 'Language', value: 'English'}),
    M.row_link({glyph: 'map-pin', title: 'Region', value: 'United States'}),
  ].join(''),
  over: () => M.pick_sheet({fit: true, title: 'Language', note: 'The words on this screen. Names stay as they were typed', rows: [
    M.row_link({title: 'English', trail: 'check', trailColor: 'blue'})].join('')}),
});

// ---- A zone, and its sensor's settings (installer) ----
// Each page is long, so it is drawn three or two times: the top, then scrolled (bodyScroll).
const INST = ({back, title, note, body, scroll = 0}) => M.main({next: true, view: 'settings', tone: 'blue', rail: {troubles: 1, tone: 'blue'},
  bar: {banner: 'installer', time: '10:52', troubles: 1},
  content: M.settings({next: true, go: GO, section: 'installer', back, title, note, body, bodyScroll: scroll})});

const zoneBody = () => [
  M.changed(M.row_status({glyph: 'footprints', color: 'green', title: 'No motion', detail: 'Simulator · the service console opens and closes it'})),
  M.changed(M.section('The zone')),
  M.row_link({glyph: 'tag', title: 'Name', value: 'Hallway Motion'}),
  M.row_link({glyph: 'shield', title: 'Type', value: 'Interior follower (04)'}),
  M.changed(M.section('Its sensor')),
  M.row_link({glyph: 'footprints', color: 'green', title: 'Sensor', value: 'Motion'}),
  M.row_link({glyph: 'wrench', title: 'Source', value: 'Simulator'}),
  M.changed(M.row_link({glyph: 'sliders-horizontal', title: 'Sensor settings', detail: 'Open when on · 350 ms · not supervised', goTo: 'n-zone-sensor'})),
  M.changed(M.section('What it does')),
  M.row_toggle({title: 'Night zone', detail: 'Armed in Night too, not only Away', on: true}),
  M.row_toggle({title: 'Chime', detail: 'Ding when it opens while disarmed and chime is on'}),
  M.changed(M.row_toggle({title: 'Report this zone', detail: 'Its events go to phones and integrations, not only the event log', on: true})),
  M.changed(M.section('Advanced')),
  M.row_link({glyph: 'shield', title: 'Cross-zone partner', detail: 'Both must trip close together to cause an alarm', value: 'None'}),
  M.changed(M.row_link({glyph: 'siren', title: 'Alarm limit', detail: 'Stops sounding after this many alarms in one arming', value: 'System setting'})),
  M.section(''),
  M.row_action({glyph: 'trash-2', text: 'Remove this zone', color: 'red'}),
].join('');
const ZONE = {back: 'installer-zones', title: 'Hallway Motion', note: 'Zone 5 · saved when you go back'};

M.screen({
  id: 'n-zone-edit', area: 'Installer', title: 'One zone, grouped', shot: null,
  proposal: P('zone-edit', 'Up to twenty rows in one run, starting at three different edges (rows with an icon, switches without, a flat "Right now" line), with a paragraph between them. For a wired sensor, Contact and Response time sit on this page and again on Sensor settings, under other names ("Reverse open and closed"). "Alarm limit" and "Never shut down" are one setting in two rows. Now: how the zone is right now on top, then four groups, and Remove last, apart.'),
  notes: 'The top row is not a button; its words change at most once a second, after the input settles (UI-023). Its detail is the source and what it means (today the paragraph under "Right now"): for a wired zone, "Wired · works without Wi-Fi, a cut wire shows as open". Partition shows with more than one partition, "Acts as" for a button, "Can be bypassed" for a 24-hour zone, as today. The next two screens are this page scrolled.',
  render: () => INST({...ZONE, body: zoneBody()}),
});

M.screen({
  id: 'n-zone-edit-2', area: 'Installer', title: 'One zone, grouped: further down', shot: null,
  proposal: P('zone-edit', 'The sensor\'s group: Contact and Response time have one home, Sensor settings, and its row says what they are set to ("Open when on · 350 ms · not supervised"), so nothing needs opening to check them. The zone\'s switches are a group of their own; "Report this zone" says what it does when on (today\'s detail described Off beside a switch that was on).'),
  render: () => INST({...ZONE, body: zoneBody(), scroll: 300}),
});

M.screen({
  id: 'n-zone-edit-3', area: 'Installer', title: 'One zone, grouped: the end', shot: null,
  proposal: P('zone-edit', '"Alarm limit" (a stepper reading "System setting") and the "Never shut down" switch were one setting in two rows: now one picker row (the system setting, 1 to 6, or Never). Supervision moves to Sensor settings. Remove this zone is last, apart.'),
  render: () => INST({...ZONE, body: zoneBody(), scroll: 9999}),
});

const sensorBody = () => [
  M.section('How it reads'),
  M.row_link({glyph: 'sliders-horizontal', title: 'Open when', value: 'On or open'}),
  M.changed(M.row_toggle({glyph: 'arrow-left-right', title: 'Reverse open and closed', detail: 'For a normally open contact, or a sensor that reports the other way round'})),
  M.changed(M.row_link({glyph: 'timer', title: 'Response time', value: '350 ms'})),
  M.changed(M.section('When it goes quiet')),
  M.changed(M.row_link({glyph: 'clock-alert', title: 'Supervision', detail: 'Shows Check when it misses a check-in', value: 'Not supervised'})),
  M.changed(M.row_link({glyph: 'unlink', title: 'Unreachable after', detail: 'Shows Check when its source loses it', value: 'Default · 1 min'})),
  M.section('Other inputs'),
  M.row_link({glyph: 'shield-alert', color: 'amber', title: 'Tamper input', value: 'None'}),
  M.row_link({glyph: 'battery-warning', color: 'amber', title: 'Low-battery input', value: 'None'}),
  M.section(''),
  M.row_action({glyph: 'trash-2', text: 'Remove the sensor, keep the zone', color: 'red'}),
].join('');
const SENSOR = {back: 'n-zone-edit', title: 'Sensor settings', note: 'Hallway Motion · changes apply at once'};

M.screen({
  id: 'n-zone-sensor', area: 'Installer', title: 'A zone\'s sensor settings', shot: null,
  proposal: P('zone-sensor', 'A stepper whose value is a word ("Source default", "Not supervised") wraps in its box, and stepping through times one tap at a time is slow. Two settings in two places said nearly the same thing: Supervision on the zone page ("A wireless sensor silent this long shows Check") and Unreachable after here ("A sensor silent this long shows Check"). Now both are here, under one heading, each saying what it watches, as picker rows; the switch has an icon so its row lines up with the others.'),
  notes: 'Supervision is how long the sensor itself may stay silent (supervision_s); Unreachable after is how long its source may say it is gone (grace_s): both exist on purpose (docs/features/22). Supervision\'s picker: Not supervised, 1, 4, 12 or 24 hours; Unreachable after\'s: the source\'s default (with what that is), 1, 5, 15 or 60 minutes, Never. For a wired sensor, "Reverse open and closed" reads "Contact: Normally closed / Normally open", the words the zone page uses today. The two clock icons become a timer and a clock with an alert.',
  render: () => INST({...SENSOR, body: sensorBody()}),
});

M.screen({
  id: 'n-zone-sensor-2', area: 'Installer', title: 'A zone\'s sensor settings: further down', shot: null,
  proposal: P('zone-sensor', 'The two ways a sensor can go quiet side by side, then its other inputs, and Remove the sensor last, apart.'),
  render: () => INST({...SENSOR, body: sensorBody(), scroll: 9999}),
});

// ---- Setup: the last step ----
M.screen({
  id: 'n-setup-done', area: 'Setup', title: 'Done', shot: null,
  proposal: P('setup-done', 'The last step offered "Leave setup" beside "Go to Home" (the same thing), a toast "Installer code changed" from two steps back was still up, and the factory-code warning was a grey line. Now: no Leave setup, no stale toast, and the codes row is amber with Change. The button says Done, as the new rail has no Home (Security).'),
  render: () => M.wizard({step: 5, glyph: 'party-popper', title: 'You\'re all set', leaveSide: true,
    note: 'Your panel is protecting your home. Everything here can be changed later in Settings.',
    back: true, next: 'Done',
    body: [
      M.row_link({glyph: 'wifi', color: 'green', title: 'Wi-Fi', detail: 'Harbor House', trail: 'check', trailColor: 'green'}),
      M.row_link({glyph: 'globe', color: 'green', title: 'Time zone', detail: 'Eastern Time', trail: 'check', trailColor: 'green'}),
      M.changed(M.row_link({glyph: 'key-round', color: 'amber', title: 'Codes', detail: 'The master code is still the factory code', value: 'Change'})),
      M.row_link({glyph: 'house', color: 'green', title: 'Name', detail: 'Home', trail: 'check', trailColor: 'green'}),
    ].join('')}),
});
