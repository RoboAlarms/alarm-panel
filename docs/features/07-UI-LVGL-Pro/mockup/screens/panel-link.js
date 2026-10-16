// The panel link (docs/features/11, BL-048; ui_link.c, ui_wizard.c): a panel is the alarm panel itself
// (the "master", every panel before BL-048) or a keypad for an alarm panel elsewhere in the house, over
// Wi-Fi. Built from the approved proposal BL-048-panel-link.js (2026-10-15) with its decisions: "Stop"
// ends the add flow, the section is "Keypads", a keypad that answers is "Connected", a new keypad is
// "Keypad N" until renamed, removing is a hold with no dialog, supervision is 1 min, 5 min (the default),
// 15 min or Not supervised, a keypad shows the factory-code reminder pointing to the alarm panel, and a
// removed keypad has no Panic. Three places:
//   A. the setup on a panel's first start: the role after Welcome and, for a keypad, Wi-Fi it can't skip
//      and the pairing step (docs/features/09 NET-001);
//   B. the alarm panel, installer mode, Keypads and devices: the keypads on Wi-Fi, adding one (the code
//      compared on both screens, LINK-004, LINK-016), a keypad's page and removing it (LINK-014);
//   C. a panel in the keypad role: Home while it connects, without the alarm panel ("No system
//      communication", LINK-008) and not paired, Emergency, the Settings it keeps, About and how it
//      becomes a stand-alone panel again (a factory reset, LINK-020, LINK-025).
// Nothing blinks; every countdown and live word changes at most once a second (UI-020, UI-023). The
// screens never name how the link works. A firmware without the link (NULL link entries) shows none of
// this. The preview draws them over a demo link backend (host/ui_preview/host_link_demo.c), Harbor House
// at 192.0.2.42; fictional data throughout.

// ================================ A. The setup on a panel's first start ================================
// Dots: the alarm panel's setup counts Role, Wi-Fi, Time zone, Codes, Name and Done (6); a keypad's
// Role, Wi-Fi, Pair and Done (4). Language and Welcome come before the dots, as today.
const LINK_MASTER_STEPS = 6;
const LINK_KEYPAD_STEPS = 4;
const linkSetup = (meta, notes, props) => M.screen({...meta, area: 'Setup', notes,
  render: () => M.wizard(props)});

linkSetup({id: 'setup-welcome-first', shot: 'bl-048-setup-welcome', title: 'Setup: Welcome on a panel\'s first start, the role first'},
  'On a panel\'s first start in a firmware with the panel link (ui_options_t.run_setup; ui_app_first_setup in the preview): the role\'s row on top, the rest as setup-welcome. The list scrolls a little (the shade at its foot). Run again from Installer the role isn\'t asked (it is chosen once: changing it is a factory reset), so Welcome lists five rows and the dots stay five. No Leave setup on a first start.',
  {step: 0, steps: LINK_MASTER_STEPS, glyph: 'sparkles', title: 'Welcome',
    note: 'Let\'s get your alarm panel ready. It takes about two minutes, and you can change everything later in Settings.', next: 'Start',
    body: [
      M.row_link({glyph: 'shield', color: 'violet', title: 'Choose its role', detail: 'The alarm panel, or a keypad for one', trail: ''}),
      M.row_link({glyph: 'wifi', title: 'Join Wi-Fi', detail: 'For the clock and updates', trail: ''}),
      M.row_link({glyph: 'globe', title: 'Choose your time zone', detail: 'So the event log shows your local time', trail: ''}),
      M.row_link({glyph: 'key', color: 'amber', title: 'Change the factory codes', detail: 'Your own installer and master codes', trail: ''}),
      M.row_link({glyph: 'tag', color: 'violet', title: 'Name your system', detail: 'How it shows up on the screen', trail: ''}),
      M.row_link({glyph: 'party-popper', color: 'green', title: 'Look it over', detail: 'A last look at what you chose', trail: ''}),
    ].join('')});

const roleBody = (keypad) => [
  M.row_link({glyph: 'shield-check', color: 'green', title: 'The alarm panel', detail: 'Sensors and the alarm live here',
    trail: keypad ? '' : 'check', trailColor: 'blue', goTo: 'setup-role'}),
  M.row_link({glyph: 'keyboard', color: 'blue', title: 'A keypad', detail: 'For an alarm panel elsewhere in the house',
    trail: keypad ? 'check' : '', trailColor: 'blue', goTo: 'setup-role-keypad'}),
  M.paragraph('To change this later, the panel must be reset to its factory settings.'),
].join('');
linkSetup({id: 'setup-role', shot: 'bl-048-setup-role', title: 'Setup: the role, the alarm panel chosen'},
  'After Welcome, only on a panel\'s first start (or after a factory reset) in a firmware with the link and only while it is the alarm panel (link_info). The alarm panel is chosen to start with: Next goes on to Wi-Fi as today and the dots count its six steps. A tap on a row chooses it; the dots change at once to the branch\'s count (not animated).',
  {step: 1, steps: LINK_MASTER_STEPS, glyph: 'shield', title: 'How will this panel be used?',
    note: 'Each home has one alarm panel. More panels can be keypads for it.', back: true, next: 'Next', body: roleBody(false)});

linkSetup({id: 'setup-role-keypad', shot: 'bl-048-setup-role-keypad', title: 'Setup: the role, a keypad chosen'},
  'With A keypad chosen the dots count a keypad\'s four steps: Role, Wi-Fi, Pair, Done. A keypad has no time zone, codes or name to set: it takes the time and its partition from the alarm panel, and codes are checked there. The role is stored when the pairing\'s Done is tapped (until then a restart comes back to this step).',
  {step: 1, steps: LINK_KEYPAD_STEPS, glyph: 'shield', title: 'How will this panel be used?',
    note: 'Each home has one alarm panel. More panels can be keypads for it.', back: true, next: 'Next', body: roleBody(true)});

linkSetup({id: 'setup-keypad-wifi', shot: 'bl-048-setup-keypad-wifi', title: 'Setup: Wi-Fi on a keypad, no skipping'},
  'A keypad\'s version of setup-wifi: the networks, the password on the keyboard and Other network... as today (ui_net.c), with its own side words and Next dimmed until it is connected: no Skip for now. The line on top shows until it is connected; then the connection row says so and Next lights.',
  {step: 2, steps: LINK_KEYPAD_STEPS, glyph: 'wifi', title: 'Join Wi-Fi',
    note: 'A keypad reaches the alarm panel over Wi-Fi, so this step can\'t be skipped.', back: true, next: 'Next', nextDisabled: true,
    body: [
      M.paragraph('Join the network the alarm panel uses.'),
      M.row_status({glyph: 'wifi-off', color: 'gray', title: 'Not connected', detail: 'Choose a network below'}),
      M.section('Networks nearby'),
      M.row_link({glyph: 'wifi', title: 'Harbor House', detail: 'Password · strong signal', trail: 'lock'}),
      M.row_link({glyph: 'wifi-high', title: 'Harbor House 5G', detail: 'Password · good signal', trail: 'lock'}),
      M.row_link({glyph: 'wifi-high', title: 'Maple Street', detail: 'Password · good signal', trail: 'lock'}),
      M.row_link({glyph: 'wifi-low', title: 'DIRECT-42 Printer', detail: 'Password · fair signal', trail: 'lock'}),
    ].join('')});

// ---- the pairing step (keypad only) ----
const PAIR = {step: 3, steps: LINK_KEYPAD_STEPS, glyph: 'house', title: 'Pair with the alarm panel',
  note: 'It finds the alarm panel on your Wi-Fi. The installer allows it there.', back: true, next: 'Next', nextDisabled: true};
const onTheAlarmPanel = () => [
  M.section('On the alarm panel'),
  M.row_link({glyph: 'wrench', title: 'Open Installer', detail: 'Settings > About > Installer, with the installer code', trail: ''}),
  M.row_link({glyph: 'cpu', title: 'Keypads and devices', detail: 'In the Installer menu', trail: ''}),
  M.row_link({glyph: 'plus', color: 'green', title: 'Add a keypad', detail: 'This keypad then finds it by itself', trail: ''}),
].join('');
const pair = (meta, notes, body, extra = {}) => linkSetup(meta, notes, {...PAIR, ...extra, body});

pair({id: 'setup-pair-looking', shot: 'bl-048-setup-pair-looking', title: 'Setup, keypad: looking for the alarm panel'},
  'The step after Wi-Fi (KP_PAIR_START: a keypad starts an attempt only when a person asks, LINK-016). It keeps looking; what to do on the alarm panel stays on the page. Nothing moves: no spinner, the words change only when something is found. Next is dimmed: the step goes on by itself. Back returns to Wi-Fi and stops looking (KP_PAIR_STOP); on a panel becoming a keypad, leaving this branch (Back, the alarm panel chosen instead) also forgets a pairing that had just finished, so nothing of the other alarm panel stays.',
  [M.row_status({glyph: 'search', color: 'blue', title: 'Looking for the alarm panel', detail: 'On your Wi-Fi'}), onTheAlarmPanel()].join(''));

pair({id: 'setup-pair-code', shot: 'bl-048-setup-pair-code', title: 'Setup, keypad: found, compare the code'},
  'The code is the six digits the alarm panel shows (keypad-add-code), in two groups of three; the two answers have the same weight, as on the Home Assistant page. Yes sends this keypad\'s answer (KP_PAIR_CONFIRM); No ends it (nothing is stored on either side). The address is in the detail so an installer with two panels on the bench can tell them apart.',
  [
    M.row_status({glyph: 'house', color: 'green', title: 'Found Harbor House', detail: 'The alarm panel · 192.0.2.42'}),
    M.big_value({label: 'Does the alarm panel show this code?', value: '588 460'}),
    M.action_pair({glyph: 'circle-x', text: 'No, cancel', color: 'red', goTo: 'setup-pair-mismatch'}, {glyph: 'circle-check', text: 'Yes, pair', color: 'green', goTo: 'setup-pair-waiting'}),
    M.paragraph('If the codes differ, something on your network is in between: tap No.', 'amber'),
  ].join(''));

pair({id: 'setup-pair-waiting', shot: 'bl-048-setup-pair-waiting', title: 'Setup, keypad: waiting for the installer to allow it'},
  'After Yes here, until the installer allows it at the alarm panel. If the installer allowed first, this screen is skipped. The code stays on show so the installer can still compare it. Back cancels the pairing.',
  [
    M.row_status({glyph: 'circle-check', color: 'green', title: 'The codes match', detail: 'Now allow it on the alarm panel'}),
    M.big_value({label: 'The code on both screens', value: '588 460'}),
    M.paragraph('The installer has 2 minutes to allow it.'),
  ].join(''));

linkSetup({id: 'setup-pair-done', shot: 'bl-048-setup-pair-done', title: 'Setup, keypad: paired'},
  'The keypad branch\'s last step (4 of 4), shown by itself once both sides confirmed (LINK-016). The keypad\'s number comes from the alarm panel, the time zone too, and its home partition by the name it has there (link_info.home_name; "Partition 2" when not known). Until Done this panel sends Harbor House nothing more, so a setup left on this step for over 10 minutes is dropped there (pair again). No Back: undoing a pairing is done at the alarm panel (remove the keypad) or here by a factory reset. Done stores the role (KP_DONE), marks the setup done and the panel restarts as a keypad (kp-home-connecting).',
  {step: 4, steps: LINK_KEYPAD_STEPS, glyph: 'party-popper', title: 'You\'re all set',
    note: 'This panel is now a keypad. The alarm, zones and codes live on the alarm panel.', next: 'Done',
    body: [
      M.row_status({glyph: 'keyboard', color: 'green', title: 'Keypad 2 of Harbor House', detail: 'Shows and arms Home'}),
      M.row_link({glyph: 'wifi', color: 'green', title: 'Wi-Fi', detail: 'Harbor House', trail: 'check', trailColor: 'green'}),
      M.row_link({glyph: 'globe', color: 'green', title: 'Time zone', detail: 'Eastern Time, from Harbor House', trail: 'check', trailColor: 'green'}),
    ].join('')});

// ---- the ways pairing ends without a keypad: Try again (KP_PAIR_RETRY) ----
const failed = (meta, notes, look, words, tone = 'gray') => pair(meta, notes,
  [M.row_status(look), M.paragraph(words, tone)].join(''), {next: 'Try again', nextDisabled: false});
failed({id: 'setup-pair-mismatch', shot: 'bl-048-setup-pair-mismatch', title: 'Setup, keypad: the codes didn\'t match'},
  'After No on either screen (fail MISMATCH). Nothing was stored on either side. Try again starts looking again.',
  {glyph: 'circle-x', color: 'red', title: 'Not paired: the codes didn\'t match', detail: 'Nothing was saved'},
  'If they differ again, don\'t pair: something on your network is in between.', 'amber');
failed({id: 'setup-pair-refused', shot: 'bl-048-setup-pair-refused', title: 'Setup, keypad: rejected at the alarm panel'},
  'The installer tapped No, reject at the alarm panel (fail REFUSED or CANCELLED): the words say only what happened, and nothing was stored.',
  {glyph: 'circle-x', color: 'red', title: 'Not paired: rejected at the alarm panel', detail: 'Nothing was saved'},
  'Ask the installer to allow it there, then try again.');
failed({id: 'setup-pair-timeout', shot: 'bl-048-setup-pair-timeout', title: 'Setup, keypad: no answer in time'},
  'Either side waited too long: 30 s for a step, 2 minutes for the codes (fail TIMEOUT, CLOSED or INTERRUPTED, LINK-016). Words the proposal didn\'t draw, in its style: no number free at the alarm panel (FULL), two alarm panels open at once (TWO_MASTERS: the contract names one peer, so the step can\'t list both to choose from), another keypad pairing too (OVERLAP), and any other failure ("Not paired", "Open Add a keypad on the alarm panel again, then try again.").',
  {glyph: 'clock-alert', color: 'amber', title: 'Not paired: no answer in time', detail: 'Nothing was saved'},
  'Both screens must answer within 2 minutes. Open Add a keypad on the alarm panel again, then try again.');

pair({id: 'setup-pair-not-found', shot: 'bl-048-setup-pair-not-found', title: 'Setup, keypad: nothing found yet'},
  'After 30 s with no alarm panel found (link_pair_status left_s, looking for this long), the page swaps the instructions for what to check, once, and keeps looking (nothing blinks, no counter). Found later, it goes on to the code. The same page after 3 failed attempts (LIMIT) or when the firmware stopped looking (NOT_FOUND), then with Try again lit. Back returns to Wi-Fi, for a keypad on the wrong network.',
  [
    M.row_status({glyph: 'search', color: 'amber', title: 'Still looking', detail: 'No alarm panel with pairing open yet'}),
    M.section('Check'),
    M.row_link({glyph: 'wifi', title: 'The same Wi-Fi', detail: 'This keypad is on Harbor House: the alarm panel must be too', trail: ''}),
    M.row_link({glyph: 'plus', title: 'Pairing open on the alarm panel', detail: 'Installer > Keypads and devices > Add a keypad, for 2 minutes', trail: ''}),
    M.row_link({glyph: 'shield', title: 'Not a guest network', detail: 'A guest network keeps devices apart', trail: ''}),
  ].join(''));

// ================================ B. The alarm panel: Keypads and devices ================================
// Installer mode, under About (BL-189), as installer-menu.
const LINK_INST_SECTIONS = M.settings_sections({drop: ['installer']}).filter((s) => !s.divider);
const linkInst = (meta, notes, page, extra = {}) => M.screen({...meta, area: 'Installer', notes,
  render: () => M.main({view: 'settings', tone: 'blue', rail: {tone: 'blue'},
    bar: {banner: 'installer', time: '10:58'},
    content: M.settings({section: 'about', sections: LINK_INST_SECTIONS, ...page}), toast: extra.toast})});

const DEVICES_PAGE = {title: 'Keypads and devices', note: 'A supervised device that stops answering shows a trouble', back: 'installer-menu'};
// The installer's own table, as before, without the numbers a keypad on Wi-Fi holds.
const tableRows = (skip = []) => Array.from({length: 16}, (_, i) => i + 1).filter((d) => !skip.includes(d))
  .map((d) => M.row_link({glyph: 'cpu', color: 'gray', title: `Device ${d}`, detail: 'Not in use', goTo: d === 1 ? 'installer-device' : ''})).join('');
const ADD_ROW = M.row_action({glyph: 'plus', text: 'Add a keypad', color: 'blue', goTo: 'keypad-add-open'});
const HALL = M.row_link({glyph: 'keyboard', color: 'green', title: 'Hall keypad', detail: 'Keypad 2 · Home', value: 'Connected', goTo: 'keypad-page'});
const BEDROOM = M.row_link_alert({glyph: 'keyboard', color: 'amber', title: 'Bedroom keypad', detail: 'Keypad 3 · Home', sub: 'Not heard since 10:25 PM',
  value: 'Not responding', valueColor: 'amber', goTo: 'keypad-page-silent'});

linkInst({id: 'installer-devices', shot: 'bl-048-devices-empty', title: 'Installer: Keypads and devices, no keypad on Wi-Fi yet'},
  'In a firmware with the panel link, on the alarm panel: the Keypads section on top, then the installer\'s own table of the 16 device numbers under a heading of its own (a row each: the name, or "Device 4", and its partition when in use, else "Not in use"). Add a keypad is there in installer mode with every partition disarmed. The installer menu\'s row counts both. Without the link the page is the table alone, as before.',
  {...DEVICES_PAGE, body: [
    M.section('Keypads'), ADD_ROW,
    M.paragraph('Another RoboAlarms panel on this Wi-Fi can be a keypad for this one.'),
    M.section('Supervised devices'),
    tableRows(),
  ].join('')});

linkInst({id: 'installer-devices-keypads', shot: 'bl-048-devices', title: 'Installer: Keypads and devices, two keypads on Wi-Fi'},
  'A row per keypad (link_keypads): its name (a new one is "Keypad 2" until renamed, then the number moves to the line under it), its home partition, and Connected, Connecting (just added) or Not responding (amber when its supervision trouble is raised, with since when: LINK-007, CORE-TRBL-008). The words follow it at most once a second; the page is built again only when they change. Numbers 2 and 3 are left out of the table below: a keypad on Wi-Fi holds them (numbers 1-8 are shared with Zigbee keypads and this table, LINK-001).',
  {...DEVICES_PAGE, body: [
    M.section('Keypads'), ADD_ROW, HALL, BEDROOM, M.section('Supervised devices'),
    tableRows([2, 3]),
  ].join('')});

// ---- adding a keypad ----
const ADD = {title: 'Add a keypad', back: 'installer-devices'};
linkInst({id: 'keypad-add-open', shot: 'bl-048-add-open', title: 'Installer: Add a keypad, pairing open'},
  'Opening the page opens pairing for 2 minutes (PAIR_OPEN); leaving it, or Stop, closes it (PAIR_STOP), as Zigbee\'s Add a device. The time left in the line under the title changes once a second, nothing else moves. A refusal says why on the page: "Not started: only in installer mode, with everything disarmed." (NOT_ALLOWED) or "Not started: that can\'t be done now." (BUSY, FAILED), with Try again.',
  {...ADD, note: 'Waiting for a keypad · 1 min 48 s left', body: [
    M.section('On the new keypad'),
    M.row_link({glyph: 'wifi', title: 'Join the same Wi-Fi', detail: 'Harbor House, as this panel', trail: ''}),
    M.row_link({glyph: 'keyboard', title: 'Choose A keypad', detail: 'Its setup asks how it will be used', trail: ''}),
    M.row_link({glyph: 'circle-check', title: 'Compare the codes', detail: 'This page shows the code when it asks', trail: ''}),
    M.row_action({glyph: 'x', text: 'Stop', color: 'gray', goTo: 'installer-devices'}),
    M.paragraph('Leaving this page closes pairing.'),
  ].join('')});

linkInst({id: 'keypad-add-nothing', shot: 'bl-048-add-nothing', title: 'Installer: Add a keypad, nothing asked'},
  'The 2 minutes ran out with no keypad (fail TIMEOUT, no attempt). Try again opens pairing again. After a keypad asked and pairing still ended, the row says "No keypad was added" with "Closed after 3 tries" (LIMIT, LINK-016), "The 2 minutes ran out" or "Nothing was saved".',
  {...ADD, note: 'Pairing closed', body: [
    M.row_status({glyph: 'circle-dot', color: 'gray', title: 'No keypad asked to pair', detail: 'The 2 minutes ran out'}),
    M.paragraph('Check that the new keypad is on the same Wi-Fi and on its pairing step, then try again.'),
    M.row_action({glyph: 'refresh-cw', text: 'Try again', color: 'blue', goTo: 'keypad-add-open'}),
  ].join('')});

linkInst({id: 'keypad-add-code', shot: 'bl-048-add-code', title: 'Installer: Add a keypad, compare the code'},
  'The Home Assistant pairing\'s pattern (ha-pairing-code): the six digits big, the two answers side by side, red on the left (PAIR_DECIDE). One keypad at a time. Reject ends it on both screens.',
  {...ADD, note: 'A keypad asked to pair', body: [
    M.row_status({glyph: 'keyboard', color: 'blue', title: 'A new keypad', detail: 'On your Wi-Fi · 192.0.2.57'}),
    M.big_value({label: 'Does the keypad show this code?', value: '588 460'}),
    M.action_pair({glyph: 'circle-x', text: 'No, reject', color: 'red', goTo: 'keypad-add-nothing'}, {glyph: 'circle-check', text: 'Yes, allow', color: 'green', goTo: 'keypad-add-allowed'}),
    M.paragraph('If the codes differ, something on your network is in between: reject it.', 'amber'),
  ].join('')});

linkInst({id: 'keypad-add-allowed', shot: 'bl-048-add-allowed', title: 'Installer: allowed, waiting for the keypad'},
  'After Yes, allow here while the keypad hasn\'t answered yet; if it answered first, this is skipped and the keypad is added at once. Nothing is stored until both have answered. Stop ends it on both screens.',
  {...ADD, note: 'Allowed here · waiting for the keypad', body: [
    M.row_status({glyph: 'circle-check', color: 'green', title: 'Allowed here', detail: 'Now tap Yes, pair on the keypad'}),
    M.big_value({label: 'The code on both screens', value: '588 460'}),
    M.row_action({glyph: 'x', text: 'Stop', color: 'gray', goTo: 'installer-devices'}),
  ].join('')});

linkInst({id: 'keypad-add-done', shot: 'bl-048-add-done', title: 'Installer: a keypad added'},
  'Both answered: the keypad takes the lowest number free (1 is the table\'s here), its home partition is this panel\'s first, and it is supervised once heard (5 min). Back on the list with a toast saying the number; its row says Connected within a second or two. Its page renames it.',
  {...DEVICES_PAGE, body: [
    M.section('Keypads'), ADD_ROW,
    M.row_link({glyph: 'keyboard', color: 'green', title: 'Keypad 2', detail: 'Home', value: 'Connected', goTo: 'keypad-page'}),
    M.section('Supervised devices'),
    tableRows([2]),
  ].join('')}, {toast: 'Added as Keypad 2'});

linkInst({id: 'keypad-add-full', shot: 'bl-048-add-full', title: 'Installer: Add a keypad, all 8 numbers taken'},
  'Opening Add a keypad with no number free (PAIR_OPEN answers FULL): pairing doesn\'t open. Z-Wave keypads (numbers 9 to 16) don\'t count. The words follow Z-Wave\'s "storage is full" page (zwave-add-full).',
  {...ADD, note: '', body: [
    M.paragraph('Not started: keypad numbers 1 to 8 are all in use.', 'amber'),
    M.paragraph('Keypads on Wi-Fi share them with Zigbee keypads and the supervised devices. Remove one you no longer use first.'),
    M.row_action({glyph: 'refresh-cw', text: 'Try again', color: 'blue'}),
  ].join('')});

// ---- a keypad's page ----
const keypadBody = (name, status, held = 0) => [
  status,
  M.section('The keypad'),
  M.row_link({glyph: 'tag', title: 'Name', value: name}),
  M.row_link({glyph: 'warehouse', color: 'violet', title: 'Home partition', detail: 'The partition a keypad shows and arms', value: 'Home'}),
  M.row_link({glyph: 'clock-alert', title: 'Supervision', detail: 'Silent this long: a trouble', value: '5 min'}),
  M.section(''),
  M.row_hold({text: 'Remove this keypad', held}),
  M.paragraph('It stops working at once. To use it again, pair it again.'),
].join('');
linkInst({id: 'keypad-page', shot: 'bl-048-keypad-page', title: 'Installer: a keypad\'s page'},
  'The device page (installer-device) for a keypad on Wi-Fi: no In use switch (removing it is the hold at the foot), the state on top, then Name (the keyboard: "Keypad name", "Hall keypad, Bedroom keypad..."), Home partition (the partitions in use) and Supervision (1 min, 5 min, 15 min, Not supervised; a value set another way stays offered). Each change is sent as it is made (KEYPAD_SET); a refusal is a toast. Its state\'s words follow it at most once a second.',
  {title: 'Hall keypad', note: 'Keypad 2 · saved when you go back', back: 'installer-devices-keypads',
    body: keypadBody('Hall keypad', M.row_status({glyph: 'keyboard', color: 'green', title: 'Connected', detail: 'Version 0.6.0 · 192.0.2.57'}))});

linkInst({id: 'keypad-page-silent', shot: 'bl-048-keypad-page-silent', title: 'Installer: a keypad not responding, being removed'},
  'Amber while it is silent past its supervision time. Remove this keypad is held for 2 seconds (here half way; the page scrolled to it): the hold is the confirmation, so no dialog follows (KEYPAD_REMOVE). It is cut off at once, told if it answers (it then shows kp-removed), and the list says "Bedroom keypad removed".',
  {title: 'Bedroom keypad', note: 'Keypad 3 · saved when you go back', back: 'installer-devices-keypads',
    body: keypadBody('Bedroom keypad', M.row_status({glyph: 'keyboard', color: 'amber', title: 'Not responding', detail: 'Not heard since 10:25 PM'}), 0.55)});

// ================================ C. A panel in the keypad role ================================
// A keypad runs no alarm of its own (LINK-020): its rail has no Smart Home, its Settings keep what is
// this screen's own, and Home shows what the alarm panel says, or that it can't.
const CODES_KP = 'Factory codes · Change them on the alarm panel';
const kp = (meta, notes, main) => M.screen({...meta, area: 'Keypad', notes,
  render: () => {
    const m = main();
    return M.main({...m, rail: {devices: false, ...(m.rail || {})}, bar: {time: '10:42', ...(m.bar || {})}});
  }});
const ONLINE_BAR = {banner: 'codes', bannerText: CODES_KP};
const READY = {tone: 'green', glyph: 'shield-check', headline: 'Disarmed', detail: 'Ready to arm', note: 'All zones closed'};

kp({id: 'kp-home-ready', shot: 'bl-048-kp-home-ready', title: 'Keypad: Home, connected'},
  'The same Home as the alarm panel\'s (home-ready): the state, the words and the arming tiles come from Harbor House within a second (LINK-024), and a code typed here is checked there. The rail has no Smart Home (a keypad controls no devices). While the alarm panel\'s factory codes are in use, the reminder points to the alarm panel, where codes change (not a button here; decided 2026-10-15).',
  () => ({bar: ONLINE_BAR, content: M.home(READY, M.panel_arm())}));

kp({id: 'kp-home-connecting', shot: 'bl-048-kp-home-connecting', title: 'Keypad: Home while it connects'},
  'From start-up (or the setup\'s Done) until the first word from Harbor House. The words don\'t change while it waits and nothing moves. The tiles are dimmed and don\'t open the pad; Chime is dimmed (it is the alarm panel\'s). Panic in the rail works: held, it is sent as soon as Harbor House answers, for up to a minute (LINK-023). No factory-code reminder: nothing stale is shown as current.',
  () => ({tone: 'blue', bar: {banner: null, chimeOff: true}, content: M.home({tone: 'blue', glyph: 'wifi', headline: 'Connecting', detail: 'To Harbor House', note: 'Arming waits until it answers'},
    M.panel_arm('', {disabled: true}))}));

kp({id: 'kp-home-lost', shot: 'bl-048-kp-home-lost', title: 'Keypad: Home with no system communication'},
  'After 30 s without a word from Harbor House (LINK-008). Steady: gray ring and words (the state is unknown, not armed or disarmed), when it last answered on a line of its own (a line broken inside "10:36" read badly), the fix above the dimmed tiles. Its sounds stop (no old entry or alarm tone; fire and CO keep sounding, the firmware\'s). Panic in the rail still sends (next screen). The Zones view shows every zone as Unknown, with no Bypass; Chime is dimmed. When Harbor House answers again, Home shows its state at once.',
  () => ({tone: 'gray', bar: {banner: null, chimeOff: true}, content: M.home({tone: 'gray', glyph: 'unlink', headline: 'No system communication',
    detail: 'Harbor House hasn\'t answered\nSince 10:36 PM', note: 'State unknown · Arming is off'},
  M.panel_arm(M.arm_strip({glyph: 'wifi-off', tone: 'amber', title: 'Check the alarm panel', detail: 'Is it on, and on the same Wi-Fi?'}), {disabled: true}))}));

kp({id: 'kp-home-lost-silence', shot: 'bl-048-kp-home-lost-silence', title: 'Keypad: no system communication, its fire alarm still sounding'},
  'A fire or CO alarm was sounding when Harbor House stopped answering: the keypad keeps that sound (decided 2026-10-15: until OFF is pressed on the keypad or the link returns), and in the dimmed tiles\' place offers Silence (link_info.local_alarm), under Check the alarm panel. The state card stays the steady "No system communication" (the state is unknown): no alarm look, nothing blinks. Silence stops the sound on this keypad only, with the toast "Silenced on this keypad" (the command can\'t reach Harbor House; a Disarm or Silence keyed while the link was still up but not answered counts too); the dimmed tiles come back. Built from the decision without a proposal of its own (BL-048 review).',
  () => ({tone: 'gray', bar: {banner: null, chimeOff: true}, content: M.home({tone: 'gray', glyph: 'unlink', headline: 'No system communication',
    detail: 'Harbor House hasn\'t answered\nSince 10:36 PM', note: 'State unknown · Arming is off'},
  `<div class="pane_stack">${M.arm_strip({glyph: 'wifi-off', tone: 'amber', title: 'Check the alarm panel', detail: 'Is it on, and on the same Wi-Fi?'})}${M.action_tile({glyph: 'bell-ring', title: 'Silence', subtitle: 'Stops the alarm sound on this keypad', color: 'red'})}</div>`)}));

kp({id: 'kp-home-update', shot: 'bl-048-kp-home-update', title: 'Keypad: Home when it needs an update'},
  'Harbor House answers, but its settings are newer than this keypad\'s software reads (link_info.needs_update), so the keypad never gets to show the state: it stays Connecting (or No system communication after a loss), and the foot of the state card says where to update it, steady. Software update then opens without a code (no code can be checked at Harbor House meanwhile), and Settings opens even if the last status said armed; the update installs only when Harbor House\'s partitions allow it, as on a connected keypad. Built without a proposal of its own (BL-048 review).',
  () => ({tone: 'blue', bar: {banner: null, chimeOff: true}, content: M.home({tone: 'blue', glyph: 'wifi', headline: 'Connecting', detail: 'To Harbor House', note: 'Update this keypad: Settings > About'},
    M.panel_arm('', {disabled: true}))}));

kp({id: 'kp-emergency-lost', shot: 'bl-048-kp-emergency-lost', title: 'Keypad: Emergency with no system communication'},
  'The Emergency view (panic); its words name the alarm panel, and while there is no system communication (or still connecting with a panic waiting) the small print says the keypad keeps trying (LINK-023: kept for 60 s, run once). When Harbor House takes it, the small print goes back to its usual words and the alarm shows on Home.',
  () => ({view: 'panic', tone: 'gray', bar: {banner: null, chimeOff: true}, content: `<div class="view col center" style="gap:var(--space_xxl)">
    <div class="col center" style="gap:4px">${M.text('Emergency', 'headline')}
      ${M.text('Hold a button for 2 seconds to set off the alarm on Harbor House', 'body', 'text_2')}
      ${M.text('No system communication · This keypad keeps trying for 1 minute', 'caption', 'amber')}</div>
    <div class="row" style="gap:56px">${M.panic_button({glyph: 'siren', text: 'Police', color: 'red'})}
      ${M.panic_button({glyph: 'flame', text: 'Fire', color: 'orange'})}${M.panic_button({glyph: 'heart-pulse', text: 'Medical', color: 'rose'})}</div></div>`}));

kp({id: 'kp-removed', shot: 'bl-048-kp-removed', title: 'Keypad: removed at the alarm panel'},
  'Shown when Harbor House says it removed this keypad (link_info: NOT_PAIRED, removed). It stays a keypad: Pair again opens the setup\'s pairing step (setup-pair-looking), with Leave setup back to here. To use it as an alarm panel of its own: About > Make it a stand-alone panel. No Panic in the rail: there is nothing to send it to (decided 2026-10-15). Never paired (no removal): "It isn\'t paired with an alarm panel".',
  () => ({tone: 'gray', bar: {banner: null, chimeOff: true}, rail: {panic: false}, content: M.home({tone: 'gray', glyph: 'unlink', headline: 'Not paired', detail: 'Harbor House removed this keypad', note: 'Arming is off'},
    M.action_tile({glyph: 'house', title: 'Pair again', subtitle: 'With Harbor House or another alarm panel', color: 'blue', goTo: 'setup-pair-looking'}))}));

// ---- Settings on a keypad ----
// Kept: Screen and sound, Wi-Fi and network, Time and language, About (Software update is About's).
// Left out (the role gate): Users, Integrations, Automations, Walk test, and Installer under About.
const KP_SECTIONS = M.settings_sections({drop: ['users', 'integrations', 'automations', 'walk', 'installer']}).filter((s) => !s.divider);
const kpSet = (meta, notes, page) => kp(meta, notes,
  () => ({view: 'settings', bar: ONLINE_BAR, content: M.settings({sections: KP_SECTIONS, ...page})}));

kpSet({id: 'kp-settings', shot: 'bl-048-kp-settings', title: 'Keypad: Settings, Screen and sound'},
  'A keypad\'s settings-display: no Voice volume and no Announcements (a keypad has no voice); the chime and beeps play what Harbor House says, at this keypad\'s volumes. Like today, Screen and sound needs no code. After 10 minutes without system communication (LINK-025), Wi-Fi and Software update open without a code, and Settings opens even if the last status said armed or in alarm (it closes once Harbor House answers armed).',
  {section: 'display', title: 'Screen and sound', note: '', body: [
    M.section('Sound'),
    M.row_level({glyph: 'bell', title: 'Chime volume', value: 70}),
    M.row_level({glyph: 'volume-2', title: 'Beeps volume', value: 70}),
    M.paragraph('Alarms, fire and CO always sound at full volume.'),
    M.row_toggle({title: 'Key clicks', detail: 'A short click for every touch', on: true}),
    M.section('Screen'),
    M.row_link({glyph: 'sun-moon', title: 'Appearance', value: 'Dark'}),
    M.row_toggle({glyph: 'contrast', title: 'High contrast', detail: 'Stronger colors, every button outlined'}),
  ].join('')});

kpSet({id: 'kp-time', shot: 'bl-048-kp-time', title: 'Keypad: Settings, Time and language'},
  'A keypad\'s settings-time, opened with a master code (checked by Harbor House). The clock and the time zone come from Harbor House, so they are read-only here and say so; no Time server. The 12/24-hour clock, the language and the region are this keypad\'s. Without a code: the clock rows, Clock and "Enter a master or installer code", with the line "A master or installer code changes the language".',
  {section: 'time', title: 'Time and language', body: [
    M.row_link({glyph: 'calendar-clock', color: 'green', title: 'Saturday, September 19 · 10:42 PM', detail: 'From Harbor House', trail: ''}),
    M.row_link({glyph: 'globe', title: 'Time zone', detail: 'Set on Harbor House', value: 'Eastern Time', trail: ''}),
    M.row_link({glyph: 'clock', title: 'Clock', detail: '2:30 PM or 14:30', value: '12-hour'}),
    M.section('Language'),
    M.row_link({glyph: 'languages', title: 'Language', value: 'English'}),
    M.row_link({glyph: 'map-pin', title: 'Region', detail: 'How dates and numbers are written', value: 'United States'}),
  ].join('')});

// ---- About on a keypad ----
kpSet({id: 'kp-about', shot: 'bl-048-kp-about', title: 'Keypad: About'},
  'A keypad\'s settings-about. Under the panel\'s own row, what it is a keypad of: Connected, Connecting, or "No system communication" (amber), at most once a second. Then the details as today, with an "Alarm panel" section (Name, Address, Keypad number; the address from link_info.master_addr) instead of Set up (zones, users and the event log are Harbor House\'s). Software update works as on any panel (a keypad updates itself, LINK-020).',
  {section: 'about', title: 'About this panel', body: [
    M.row_status({glyph: 'shield-check', color: 'green', title: 'RoboAlarms Panel', detail: 'Version 0.6.0 · Panel ID b7c8d9e0f1a2'}),
    M.row_status({glyph: 'keyboard', color: 'green', title: 'Keypad 2 of Harbor House', detail: 'Connected · shows and arms Home'}),
    M.row_link({glyph: 'refresh-cw', title: 'Software update', value: 'Up to date'}),
    M.row_link({glyph: 'hard-drive', color: 'violet', title: 'SD card', detail: '44.7 GB free of 59.6 GB', value: 'Ready'}),
    M.section('This panel'),
    M.row_value({title: 'Built', value: 'Sep 19, 2026'}),
    M.row_value({title: 'Running for', value: '3 days'}),
  ].join('')});

kpSet({id: 'kp-about-end', shot: 'bl-048-kp-about-end', title: 'Keypad: About, scrolled to the end'},
  'A keypad\'s settings-about-end: the Alarm panel section above Hardware; where an alarm panel has Installer, a keypad has the way back to a panel of its own, apart under a gap. It opens the next page.',
  {section: 'about', title: 'About this panel', body: [
    M.row_value({title: 'Keypad number', value: '2'}),
    M.section('Hardware'),
    M.row_value({title: 'Processor', value: 'ESP32-P4 v1.0 (v1.x image)'}),
    M.row_value({title: 'Wi-Fi module', value: 'ESP32-C6 2.12.3'}),
    M.row_value({title: 'ESP-IDF', value: 'v6.1'}),
    M.row_value({title: 'Free memory', value: '214 KB internal · 29.1 MB PSRAM'}),
    M.row_value({title: 'Firmware space', value: '2.3 MB of 4.5 MB used'}),
    M.section(''),
    M.row_link({glyph: 'rotate-ccw', color: 'red', title: 'Make it a stand-alone panel', detail: 'A factory reset: erases the pairing and every setting', goTo: 'kp-standalone'}),
  ].join('')});

kpSet({id: 'kp-standalone', shot: 'bl-048-kp-standalone', title: 'Keypad: make it a stand-alone panel (factory reset, held)'},
  'A keypad\'s factory-reset page, held half way after the installer code (checked by Harbor House through authenticate; any other code: "That isn\'t the installer code"). Before the code the hold is "Enter the installer code"; after 10 minutes with no system communication (link_info.maintenance, LINK-025) it opens without one, so a keypad whose alarm panel is gone isn\'t stuck. It erases everything and restarts in the setup, where the role is asked again. No event-log switch: a keypad has none.',
  {section: 'about', title: 'Make it a stand-alone panel', note: 'A factory reset', back: 'kp-about-end', body: [
    M.paragraph('Erases the pairing with Harbor House, Wi-Fi and every setting, then restarts with the setup.'),
    M.paragraph('Harbor House shows a trouble for this keypad until it is removed there.', 'amber'),
    M.row_hold({text: 'Hold to erase everything', held: 0.53}),
  ].join('')});
