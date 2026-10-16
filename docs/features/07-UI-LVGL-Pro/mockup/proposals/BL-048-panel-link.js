// BL-048: the panel link (docs/features/11): a panel is the alarm panel itself (the "master", every
// panel today) or a keypad for an alarm panel elsewhere in the house, over Wi-Fi. Three places change:
//   A. the setup on a new panel: a role step after Welcome and, for a keypad, a pairing step after Wi-Fi
//      (docs/features/09 NET-001);
//   B. the alarm panel, installer mode, Keypads and devices: the keypads on Wi-Fi, adding one (the code
//      compared on both screens, LINK-004, LINK-016), a keypad's page and removing it (LINK-014);
//   C. a panel in the keypad role: Home while it connects and without the alarm panel ("No system
//      communication", LINK-008, KPD-007), the Settings it keeps, About and how it becomes a
//      stand-alone panel again (a factory reset, LINK-020).
// The alarm panel is "Harbor House" (its system name), on the Wi-Fi network of the same name; the new
// keypad gets number 2 because the installer's table already uses 1. Fictional data throughout.
// Nothing blinks; every countdown and live word changes at most once a second (UI-020, UI-023).
// Screens never name how the link works (no protocols): "Wi-Fi", "keypad", "alarm panel", "code".

const P = (why, replaces = null) => ({bl: 'BL-048', status: 'draft', replaces, why});

// ================================ A. The setup on a new panel ================================
// Dots: the alarm panel's setup counts Role, Wi-Fi, Time zone, Codes, Name and Done (6); a keypad's
// Role, Wi-Fi, Pair and Done (4). Language and Welcome come before the dots, as today.
const MASTER_STEPS = 6;
const KEYPAD_STEPS = 4;
const setup = (id, title, p, notes, props) => M.screen({id, area: 'Setup', title, shot: null, proposal: p, notes,
  render: () => M.wizard(props)});

setup('bl-048-setup-welcome', 'Setup: Welcome on a new panel, with the role first', P(
  'A new panel can now be a keypad for another one, so the setup asks first how it will be used; Welcome names that step with the others.'),
  'Welcome on a panel\'s first start (today\'s setup-welcome is the setup run again, which stays as it is): one row added at the top, the rest as today (they describe the alarm panel\'s steps, the default). The list now has six rows and scrolls a little (the shade at its foot shows there is more). Run again from Installer on an alarm panel, the setup skips the role (it is chosen once: changing it is a factory reset), so Welcome lists today\'s five rows and the dots stay five.',
  {step: 0, steps: MASTER_STEPS, glyph: 'sparkles', title: 'Welcome',
    note: 'Let\'s get your alarm panel ready. It takes about two minutes, and you can change everything later in Settings.', next: 'Start',
    body: [
      M.changed(M.row_link({glyph: 'shield', color: 'violet', title: 'Choose its role', detail: 'The alarm panel, or a keypad for one', trail: ''})),
      M.row_link({glyph: 'wifi', title: 'Join Wi-Fi', detail: 'For the clock and updates', trail: ''}),
      M.row_link({glyph: 'globe', title: 'Choose your time zone', detail: 'So the event log shows your local time', trail: ''}),
      M.row_link({glyph: 'key', color: 'amber', title: 'Change the factory codes', detail: 'Your own installer and master codes', trail: ''}),
      M.row_link({glyph: 'tag', color: 'violet', title: 'Name your system', detail: 'How it shows up on the screen', trail: ''}),
      M.row_link({glyph: 'party-popper', color: 'green', title: 'Look it over', detail: 'A last look at what you chose', trail: ''}),
      M.paragraph('The panel already protects your home: fire, CO and panic work right now.'),
    ].join('')});

const ROLE_WHY = 'A panel by the bedroom or the back door can be a keypad for the alarm panel by the front door; most homes need one alarm panel, so that is the default.';
const roleBody = (keypad) => [
  M.changed([
    M.row_link({glyph: 'shield-check', color: 'green', title: 'The alarm panel', detail: 'Sensors and the alarm live here',
      trail: keypad ? '' : 'check', trailColor: 'blue', goTo: 'bl-048-setup-role'}),
    M.row_link({glyph: 'keyboard', color: 'blue', title: 'A keypad', detail: 'For an alarm panel elsewhere in the house',
      trail: keypad ? 'check' : '', trailColor: 'blue', goTo: 'bl-048-setup-role-keypad'}),
  ].join('')),
  M.paragraph('To change this later, the panel must be reset to its factory settings.'),
].join('');
setup('bl-048-setup-role', 'Setup: the role, the alarm panel chosen', P(ROLE_WHY),
  'A new step after Welcome, only on a panel that has never been set up (or after a factory reset), in a build with the panel link. The alarm panel is chosen to start with: Next goes on to Wi-Fi as today, and the dots count its six steps. A tap on a row chooses it; the dots change at once to the branch\'s count (they are not animated).',
  {step: 1, steps: MASTER_STEPS, glyph: 'shield', title: 'How will this panel be used?',
    note: 'Each home has one alarm panel. More panels can be keypads for it.', back: true, next: 'Next', body: roleBody(false)});

setup('bl-048-setup-role-keypad', 'Setup: the role, a keypad chosen', P(ROLE_WHY),
  'With A keypad chosen the dots count a keypad\'s four steps: Role, Wi-Fi, Pair, Done. A keypad has no time zone, codes or name to set: it takes the time and its partition from the alarm panel, and codes are checked there. Next stores the role once the keypad is paired (until then a restart comes back to this step).',
  {step: 1, steps: KEYPAD_STEPS, glyph: 'shield', title: 'How will this panel be used?',
    note: 'Each home has one alarm panel. More panels can be keypads for it.', back: true, next: 'Next', body: roleBody(true)});

setup('bl-048-setup-keypad-wifi', 'Setup: Wi-Fi on a keypad, no skipping', P(
  'A keypad reaches the alarm panel over Wi-Fi, so its setup can\'t go on without it; today the step offers "Skip for now".'),
  'A keypad\'s version of setup-wifi (the alarm panel\'s stays as it is): the networks, the password on the keyboard and Other network... as today (ui_net.c), with its own side words and Next dimmed until it is connected: no "Skip for now" on a keypad. Once connected, the connection row appears on top as today and Next lights.',
  {step: 2, steps: KEYPAD_STEPS, glyph: 'wifi', title: 'Join Wi-Fi',
    note: 'A keypad reaches the alarm panel over Wi-Fi, so this step can\'t be skipped.', back: true, next: 'Next', nextDisabled: true,
    body: [
      M.changed(M.paragraph('Join the network the alarm panel uses.')),
      M.section('Networks nearby'),
      M.row_link({glyph: 'wifi', title: 'Harbor House', detail: 'Password · strong signal', trail: 'lock'}),
      M.row_link({glyph: 'wifi-high', title: 'Harbor House 5G', detail: 'Password · good signal', trail: 'lock'}),
      M.row_link({glyph: 'wifi-high', title: 'Maple Street', detail: 'Password · good signal', trail: 'lock'}),
      M.row_link({glyph: 'wifi-zero', title: 'Café Guest', detail: 'Open · weak signal', trail: ''}),
    ].join('')});

// ---- the pairing step (keypad only) ----
const PAIR = {step: 3, steps: KEYPAD_STEPS, glyph: 'house', title: 'Pair with the alarm panel',
  note: 'It finds the alarm panel on your Wi-Fi. The installer allows it there.', back: true, next: 'Next', nextDisabled: true};
const PAIR_WHY = 'A keypad must be paired with the alarm panel before it shows or arms anything; the code compared on both screens keeps anything else on the network from pairing in between (LINK-004).';
const onTheAlarmPanel = () => [
  M.section('On the alarm panel'),
  M.row_link({glyph: 'wrench', title: 'Open Installer', detail: 'Settings > About > Installer, with the installer code', trail: ''}),
  M.row_link({glyph: 'cpu', title: 'Keypads and devices', detail: 'In the Installer menu', trail: ''}),
  M.row_link({glyph: 'plus', color: 'green', title: 'Add a keypad', detail: 'This keypad then finds it by itself', trail: ''}),
].join('');
const pair = (id, title, notes, body, extra = {}) => setup(id, title, P(PAIR_WHY), notes, {...PAIR, ...extra, body: M.changed(body)});

pair('bl-048-setup-pair-looking', 'Setup, keypad: looking for the alarm panel',
  'The step after Wi-Fi. It looks at once and keeps looking; what to do on the alarm panel stays on the page. Nothing moves: no spinner, the words change only when something is found (then the next screen). Next is dimmed: the step goes on by itself. Back returns to Wi-Fi and stops looking. If two alarm panels have pairing open at once, the step lists both by name and asks which one, instead of picking one.',
  [M.row_status({glyph: 'search', color: 'blue', title: 'Looking for the alarm panel', detail: 'On your Wi-Fi'}), onTheAlarmPanel()].join(''));

pair('bl-048-setup-pair-code', 'Setup, keypad: found, compare the code',
  'The code is the same six digits the alarm panel shows (bl-048-add-code); the two answers have the same weight, as on the Home Assistant page. Yes sends this keypad\'s answer; No ends it (nothing is stored on either side). The question waits up to 2 minutes, then ends as timed out. The address is in the detail so an installer with two panels on the bench can tell them apart.',
  [
    M.row_status({glyph: 'house', color: 'green', title: 'Found Harbor House', detail: 'The alarm panel · 192.0.2.42'}),
    M.big_value({label: 'Does the alarm panel show this code?', value: '588 460'}),
    M.action_pair({glyph: 'circle-x', text: 'No, cancel', color: 'red'}, {glyph: 'circle-check', text: 'Yes, pair', color: 'green', goTo: 'bl-048-setup-pair-waiting'}),
    M.paragraph('If the codes differ, something on your network is in between: tap No.', 'amber'),
  ].join(''));

pair('bl-048-setup-pair-waiting', 'Setup, keypad: waiting for the installer to allow it',
  'After Yes here, until the installer allows it at the alarm panel (Yes, allow). If the installer allowed first, this screen is skipped. The code stays on show so the installer can still compare it. Back cancels the pairing (the alarm panel says so too).',
  [
    M.row_status({glyph: 'circle-check', color: 'green', title: 'The codes match', detail: 'Now allow it on the alarm panel'}),
    M.big_value({label: 'The code on both screens', value: '588 460'}),
    M.paragraph('The installer has 2 minutes to allow it.'),
  ].join(''));

setup('bl-048-setup-pair-done', 'Setup, keypad: paired', P(PAIR_WHY),
  'The keypad branch\'s last step (4 of 4). Both sides checked each other before anything was stored (LINK-016). The keypad\'s number and partition come from the alarm panel; the time zone too. No Back: undoing a pairing is done at the alarm panel (remove the keypad) or here by a factory reset. Done opens Home (bl-048-kp-home-ready).',
  {step: 4, steps: KEYPAD_STEPS, glyph: 'party-popper', title: 'You\'re all set',
    note: 'This panel is now a keypad. The alarm, zones and codes live on the alarm panel.', next: 'Done',
    body: M.changed([
      M.row_status({glyph: 'keyboard', color: 'green', title: 'Keypad 2 of Harbor House', detail: 'Shows and arms Home'}),
      M.row_link({glyph: 'wifi', color: 'green', title: 'Wi-Fi', detail: 'Harbor House', trail: 'check', trailColor: 'green'}),
      M.row_link({glyph: 'globe', color: 'green', title: 'Time zone', detail: 'Eastern Time, from Harbor House', trail: 'check', trailColor: 'green'}),
    ].join(''))});

// ---- the ways pairing ends without a keypad ----
const failed = (id, title, notes, look, words, tone = 'gray') => pair(id, title, notes,
  [M.row_status(look), M.paragraph(words, tone)].join(''), {next: 'Try again', nextDisabled: false});
failed('bl-048-setup-pair-mismatch', 'Setup, keypad: the codes didn\'t match',
  'After No on either screen. Nothing was stored on either side. Try again starts looking again (the alarm panel\'s window stays open for the rest of its 2 minutes; after 3 failed attempts it closes, and the looking screen then shows what to check).',
  {glyph: 'circle-x', color: 'red', title: 'Not paired: the codes didn\'t match', detail: 'Nothing was saved'},
  'If they differ again, don\'t pair: something on your network is in between.', 'amber');
failed('bl-048-setup-pair-refused', 'Setup, keypad: rejected at the alarm panel',
  'The installer tapped No, reject at the alarm panel. The alarm panel\'s answer can\'t be proven (nothing is shared yet), so the words say only what happened, and nothing was stored.',
  {glyph: 'circle-x', color: 'red', title: 'Not paired: rejected at the alarm panel', detail: 'Nothing was saved'},
  'Ask the installer to allow it there, then try again.');
failed('bl-048-setup-pair-timeout', 'Setup, keypad: no answer in time',
  'Either side waited too long: 30 s for a step, 2 minutes for the codes (LINK-016). The same screen when the alarm panel stopped answering half way.',
  {glyph: 'clock-alert', color: 'amber', title: 'Not paired: no answer in time', detail: 'Nothing was saved'},
  'Both screens must answer within 2 minutes. Open Add a keypad on the alarm panel again, then try again.');

pair('bl-048-setup-pair-not-found', 'Setup, keypad: nothing found yet',
  'After 30 s with no alarm panel found, the page swaps the instructions for what to check, once, and keeps looking (nothing blinks, no counter). Found later, it goes on to the code. Back returns to Wi-Fi, for a keypad on the wrong network.',
  [
    M.row_status({glyph: 'search', color: 'amber', title: 'Still looking', detail: 'No alarm panel with pairing open yet'}),
    M.section('Check'),
    M.row_link({glyph: 'wifi', title: 'The same Wi-Fi', detail: 'This keypad is on Harbor House: the alarm panel must be too', trail: ''}),
    M.row_link({glyph: 'plus', title: 'Pairing open on the alarm panel', detail: 'Installer > Keypads and devices > Add a keypad, for 2 minutes', trail: ''}),
    M.row_link({glyph: 'shield', title: 'Not a guest network', detail: 'A guest network keeps devices apart', trail: ''}),
  ].join(''));

// ================================ B. The alarm panel: Keypads and devices ================================
// Installer mode, under About (BL-189), as today's installer-devices.
const INST_SECTIONS = M.settings_sections({drop: ['installer']}).filter((s) => !s.divider);
const inst = (id, title, p, notes, page, extra = {}) => M.screen({id, area: 'Installer', title, shot: null, proposal: p, notes,
  render: () => M.main({view: 'settings', tone: 'blue', rail: {troubles: extra.troubles ?? 1, tone: 'blue'},
    bar: {banner: 'installer', time: '10:58', troubles: extra.troubles ?? 1},
    content: M.settings({section: 'about', sections: INST_SECTIONS, ...page}), toast: extra.toast})});

const LIST_WHY = 'Keypads on Wi-Fi are paired, not switched on: the page lists them with whether they answer, and adds one.';
const DEVICES_PAGE = {title: 'Keypads and devices', note: 'A supervised device that stops answering shows a trouble', back: 'installer-menu'};
// The installer's own table, as today, without the numbers a keypad on Wi-Fi holds.
const tableRows = (skip = []) => [
  M.row_link({glyph: 'cpu', color: 'blue', title: 'Garage keypad', detail: 'All partitions', goTo: 'installer-device'}),
  ...Array.from({length: 15}, (_, i) => i + 2).filter((d) => !skip.includes(d))
    .map((d) => M.row_link({glyph: 'cpu', color: 'gray', title: `Device ${d}`, detail: 'Not in use'})),
].join('');
const ADD_ROW = M.row_action({glyph: 'plus', text: 'Add a keypad', color: 'blue', goTo: 'bl-048-add-open'});
const HALL = M.row_link_alert({glyph: 'keyboard', color: 'green', title: 'Hall keypad', detail: 'Keypad 2 · Home', value: 'Online', goTo: 'bl-048-keypad-page'});
const BEDROOM = M.row_link_alert({glyph: 'keyboard', color: 'amber', title: 'Bedroom keypad', detail: 'Keypad 3 · Home', sub: 'Not heard since 10:12 PM',
  value: 'Not responding', valueColor: 'amber', goTo: 'bl-048-keypad-page-silent'});

inst('bl-048-devices-empty', 'Alarm panel: Keypads and devices, no keypad on Wi-Fi yet', P(LIST_WHY, 'installer-devices'),
  'A new section on top: keypads on Wi-Fi, then today\'s table under a heading of its own. Add a keypad is there in installer mode with every partition disarmed, as adding a Zigbee device. The installer menu\'s row counts both (here "1 in use", the Garage keypad).',
  {...DEVICES_PAGE, body: [
    M.changed([
      M.section('Keypads on Wi-Fi'),
      ADD_ROW,
      M.paragraph('Another RoboAlarms panel on this Wi-Fi can be a keypad for this one.'),
      M.section('Supervised devices'),
    ].join('')),
    tableRows(),
  ].join('')});

inst('bl-048-devices', 'Alarm panel: Keypads and devices, two keypads on Wi-Fi', P(LIST_WHY),
  'A row per keypad: its name (a new one is "Keypad 2" until renamed, then the number moves to the line under it), its home partition, and Online or Not responding (amber, with since when: the device supervision trouble, LINK-007, CORE-TRBL-008). The words follow it at most once a second. Numbers 2 and 3 are left out of the table below: a keypad on Wi-Fi holds them (numbers 1-8 are shared with Zigbee keypads and this table, LINK-001).',
  {...DEVICES_PAGE, body: [
    M.changed([M.section('Keypads on Wi-Fi'), ADD_ROW, HALL, BEDROOM, M.section('Supervised devices')].join('')),
    tableRows([2, 3]),
  ].join('')}, {troubles: 2});

// ---- adding a keypad ----
const ADD_WHY = 'Pairing needs the installer at the alarm panel and the same code on both screens, so nothing else on the network can join as a keypad (LINK-004, LINK-016).';
const ADD = {title: 'Add a keypad', back: 'bl-048-devices-empty'};
inst('bl-048-add-open', 'Alarm panel: Add a keypad, pairing open', P(ADD_WHY),
  'Opening the page opens pairing for 2 minutes; leaving it, or Cancel, closes it (as Zigbee\'s Add a device). The time left in the line under the title changes once a second, nothing else moves. Only while it is open (or a keypad is paired) can a keypad find this panel on the network.',
  {...ADD, note: 'Waiting for a keypad · 1 min 47 s left', body: M.changed([
    M.section('On the new keypad'),
    M.row_link({glyph: 'wifi', title: 'Join the same Wi-Fi', detail: 'Harbor House, as this panel', trail: ''}),
    M.row_link({glyph: 'keyboard', title: 'Choose A keypad', detail: 'Its setup asks how it will be used', trail: ''}),
    M.row_link({glyph: 'circle-check', title: 'Compare the codes', detail: 'This page shows the code when it asks', trail: ''}),
    M.row_action({glyph: 'x', text: 'Cancel', color: 'gray', goTo: 'bl-048-devices-empty'}),
    M.paragraph('Leaving this page closes pairing.'),
  ].join(''))});

inst('bl-048-add-nothing', 'Alarm panel: Add a keypad, nothing asked', P(ADD_WHY),
  'The 2 minutes ran out with no keypad. Try again opens pairing again. The same page after 3 failed attempts in one window ("Closed after 3 tries", LINK-016).',
  {...ADD, note: 'Pairing closed', body: M.changed([
    M.row_status({glyph: 'circle-dot', color: 'gray', title: 'No keypad asked to pair', detail: 'The 2 minutes ran out'}),
    M.paragraph('Check that the new keypad is on the same Wi-Fi and on its pairing step, then try again.'),
    M.row_action({glyph: 'refresh-cw', text: 'Try again', color: 'blue', goTo: 'bl-048-add-open'}),
  ].join(''))});

inst('bl-048-add-code', 'Alarm panel: Add a keypad, compare the code', P(ADD_WHY),
  'The Home Assistant pairing\'s pattern (ha-pairing-code): the six digits big, the two answers side by side, red on the left. One keypad at a time: a second one asking now is turned away. Reject ends it on both screens and counts as a failed attempt. 2 minutes to answer, then it ends as not paired.',
  {...ADD, note: 'A keypad asked to pair', body: M.changed([
    M.row_status({glyph: 'keyboard', color: 'blue', title: 'A new keypad', detail: 'On your Wi-Fi · 192.0.2.57'}),
    M.big_value({label: 'Does the keypad show this code?', value: '588 460'}),
    M.action_pair({glyph: 'circle-x', text: 'No, reject', color: 'red', goTo: 'bl-048-add-nothing'}, {glyph: 'circle-check', text: 'Yes, allow', color: 'green', goTo: 'bl-048-add-allowed'}),
    M.paragraph('If the codes differ, something on your network is in between: reject it.', 'amber'),
  ].join(''))});

inst('bl-048-add-allowed', 'Alarm panel: allowed, waiting for the keypad', P(ADD_WHY),
  'After Yes, allow here while the keypad hasn\'t answered yet; if it answered first, this is skipped and the keypad is added at once. Nothing is stored until both have answered. Cancel ends it on both screens.',
  {...ADD, note: 'Allowed here · waiting for the keypad', body: M.changed([
    M.row_status({glyph: 'circle-check', color: 'green', title: 'Allowed here', detail: 'Now tap Yes, pair on the keypad'}),
    M.big_value({label: 'The code on both screens', value: '588 460'}),
    M.row_action({glyph: 'x', text: 'Cancel', color: 'gray', goTo: 'bl-048-devices-empty'}),
  ].join(''))});

inst('bl-048-add-done', 'Alarm panel: a keypad added', P(ADD_WHY),
  'Both answered: the keypad takes the lowest number free (1 is the Garage keypad\'s), its home partition is this panel\'s first, and it is supervised at once (1 min). Back on the list with a toast saying the number; its row says Online within a second or two. Its page renames it.',
  {...DEVICES_PAGE, body: [
    M.section('Keypads on Wi-Fi'), ADD_ROW,
    M.changed(M.row_link_alert({glyph: 'keyboard', color: 'green', title: 'Keypad 2', detail: 'Home', value: 'Online', goTo: 'bl-048-keypad-page'})),
    M.section('Supervised devices'),
    tableRows([2]),
  ].join('')}, {toast: 'Added as Keypad 2'});

inst('bl-048-add-full', 'Alarm panel: Add a keypad, all 8 numbers taken', P(
  'Keypads on Wi-Fi share numbers 1 to 8 with Zigbee keypads and the supervised devices (LINK-001); when none is free the page must say why and what to do.'),
  'Opening Add a keypad with no number free: pairing doesn\'t open. Z-Wave keypads (numbers 9 to 16) don\'t count. The words follow Z-Wave\'s "storage is full" page (zwave-add-full).',
  {...ADD, note: '', body: M.changed([
    M.paragraph('Not started: keypad numbers 1 to 8 are all in use.', 'amber'),
    M.paragraph('Keypads on Wi-Fi share them with Zigbee keypads and the supervised devices. Remove one you no longer use first.'),
    M.row_action({glyph: 'refresh-cw', text: 'Try again', color: 'blue'}),
  ].join(''))});

// ---- a keypad's page ----
const KP_WHY = 'The installer names a keypad, sets the partition it shows and arms, how long it may be silent, and removes it (LINK-007, LINK-014).';
const keypadBody = (name, status, held = 0) => [
  M.changed(status),
  M.section('The keypad'),
  M.row_link({glyph: 'tag', title: 'Name', value: name}),
  M.row_link({glyph: 'warehouse', color: 'violet', title: 'Home partition', detail: 'The partition a keypad shows and arms', value: 'Home'}),
  M.row_link({glyph: 'clock-alert', title: 'Supervision', detail: 'Silent this long: a trouble', value: '1 min'}),
  M.section(''),
  M.changed(M.row_hold({text: 'Remove this keypad', held})),
  M.paragraph('It stops working at once. To use it again, pair it again.'),
].join('');
inst('bl-048-keypad-page', 'Alarm panel: a keypad\'s page', P(KP_WHY),
  'Today\'s device page (installer-device) for a keypad on Wi-Fi: no In use switch (removing it is the hold at the foot), the state on top, then Name, Home partition and Supervision with today\'s words. Saved when the page is left. Its state\'s words follow it at most once a second.',
  {title: 'Hall keypad', note: 'Keypad 2 · saved when you go back', back: 'bl-048-devices',
    body: keypadBody('Hall keypad', M.row_status({glyph: 'keyboard', color: 'green', title: 'Online', detail: 'Version 0.6.0 · 192.0.2.57'}))});

inst('bl-048-keypad-page-silent', 'Alarm panel: a keypad not responding, being removed', P(KP_WHY),
  'Amber while it is silent past its supervision time; the trouble on Home names it ("Keypad 3 not responding"). Remove this keypad is held for 2 seconds (here half way): the hold is the confirmation, as Unpair on the Home Assistant page and Factory reset, so no dialog follows. It is cut off at once, told if it answers (it then shows bl-048-kp-removed), its trouble clears, and the list says "Bedroom keypad removed".',
  {title: 'Bedroom keypad', note: 'Keypad 3 · saved when you go back', back: 'bl-048-devices',
    body: keypadBody('Bedroom keypad', M.row_status({glyph: 'keyboard', color: 'amber', title: 'Not responding', detail: 'Not heard since 10:12 PM'}), 0.55)},
  {troubles: 2});

// ================================ C. A panel in the keypad role ================================
// A keypad runs no alarm of its own (LINK-020): its rail has no Smart Home, its Settings keep what is
// this screen's own, and Home shows what the alarm panel says, or that it can't. These screens are a
// keypad's versions of today's (the alarm panel's stay as they are), so none replaces one.
const kp = (id, title, p, notes, main) => M.screen({id, area: 'Keypad', title, shot: null, proposal: p, notes,
  render: () => {
    const m = main();
    return M.main({...m, rail: {devices: false, ...(m.rail || {})}, bar: {banner: null, time: '10:47', ...(m.bar || {})}});
  }});
const READY = {tone: 'green', glyph: 'shield-check', headline: 'Disarmed', detail: 'Ready to arm', note: 'All zones closed'};

kp('bl-048-kp-home-ready', 'Keypad: Home, connected', P(
  'A keypad looks and works like the alarm panel\'s own screen, so nobody has to learn a second one.'),
  'The same Home as the alarm panel\'s (home-ready): the state, the words and the arming tiles come from Harbor House within a second (LINK-024), and a code typed here is checked there. The rail has no Smart Home (a keypad controls no devices). No factory-code banner: codes are changed on the alarm panel (an open question).',
  () => ({content: M.home(READY, M.panel_arm())}));

kp('bl-048-kp-home-connecting', 'Keypad: Home while it connects', P(
  'At start-up a keypad knows nothing about the alarm until Harbor House answers: it must not show an old state, nor let anyone arm.'),
  'From start-up until the first word from Harbor House (usually a second or two). The words don\'t change while it waits and nothing moves (no spinner). The tiles are dimmed and don\'t open the pad. Panic in the rail works: held, it is sent as soon as Harbor House answers, for up to a minute (LINK-023). After 30 s with no answer: the next screen.',
  () => ({tone: 'blue', content: M.home({tone: 'blue', glyph: 'wifi', headline: 'Connecting', detail: 'To Harbor House', note: 'Arming waits until it answers'},
    M.changed(M.panel_arm('', {disabled: true})))}));

kp('bl-048-kp-home-lost', 'Keypad: Home with no system communication', P(
  'A keypad that has lost the alarm panel must say so and offer nothing it can\'t do, never its last state as if it were now (LINK-008, KPD-007).'),
  'After 30 s without a word from Harbor House. Steady: gray ring and words (the state is unknown, not armed or disarmed), the time it was last heard, the fix above the dimmed tiles. Its sounds stop (no old entry or alarm tone). Panic in the rail still sends (next screen). The Zones view shows every zone as Unknown and Activity keeps what it had, with "Not updated since 10:41 PM"; Chime is dimmed (it is the alarm panel\'s). When Harbor House answers again, Home shows its state at once. The ring icon is new at 96 px (unlink, E19C).',
  () => ({tone: 'gray', content: M.home({tone: 'gray', glyph: 'unlink', headline: 'No system communication',
    detail: 'Harbor House hasn\'t answered since 10:41 PM', note: 'State unknown · Arming is off'},
  M.changed(M.panel_arm(M.arm_strip({glyph: 'wifi-off', tone: 'amber', title: 'Check the alarm panel', detail: 'Is it on, and on the same Wi-Fi?'}), {disabled: true})))}));

kp('bl-048-kp-emergency-lost', 'Keypad: Emergency with no system communication', P(
  'Panic must still be tried when the alarm panel doesn\'t answer (KPD-007): the person holding the button needs to know it hasn\'t got through yet.'),
  'The Emergency view as today (panic); its words name the alarm panel, and while there is no system communication the small print says the keypad keeps trying (LINK-023: kept for 60 s, run once). When Harbor House takes it, the small print goes back to its usual words and the alarm shows on Home.',
  () => ({view: 'panic', tone: 'gray', content: `<div class="view col center" style="gap:var(--space_xxl)">
    <div class="col center" style="gap:4px">${M.text('Emergency', 'headline')}
      ${M.changed(M.text('Hold a button for 2 seconds to set off the alarm on Harbor House', 'body', 'text_2'))}
      ${M.changed(M.text('No system communication · This keypad keeps trying for 1 minute', 'caption', 'amber'))}</div>
    <div class="row" style="gap:56px">${M.panic_button({glyph: 'siren', text: 'Police', color: 'red'})}
      ${M.panic_button({glyph: 'flame', text: 'Fire', color: 'orange'})}${M.panic_button({glyph: 'heart-pulse', text: 'Medical', color: 'rose'})}</div></div>`}));

kp('bl-048-kp-removed', 'Keypad: removed at the alarm panel', P(
  'A keypad removed at the alarm panel is told (LINK-014): it must say so and offer the way back, not wait for ever.'),
  'Shown when Harbor House says it removed this keypad. It stays a keypad: Pair again opens the setup\'s pairing step (bl-048-setup-pair-looking). To use it as an alarm panel of its own: About > Make it a stand-alone panel. Panic in the rail says there is nothing to send it to (an open question).',
  () => ({tone: 'gray', content: M.home({tone: 'gray', glyph: 'unlink', headline: 'Not paired', detail: 'Harbor House removed this keypad', note: 'Arming is off'},
    M.changed(M.action_tile({glyph: 'house', title: 'Pair again', subtitle: 'With Harbor House or another alarm panel', color: 'blue', goTo: 'bl-048-setup-pair-looking'})))}));

// ---- Settings on a keypad ----
// Kept: Screen and sound, Wi-Fi and network, Time and language, About (Software update is About's).
// Hidden: Users, Integrations, Automations, Walk test, and Installer under About (the alarm panel's).
const KP_SECTIONS = M.settings_sections({drop: ['users', 'integrations', 'automations', 'walk', 'installer']}).filter((s) => !s.divider);
const SET_WHY = 'A keypad holds no users, zones or integrations: its Settings keep only what belongs to this screen, so nothing there pretends to change the alarm panel.';
const kpSet = (id, title, why, notes, page) => kp(id, title, P(why), notes,
  () => ({view: 'settings', content: M.settings({sections: KP_SECTIONS, ...page})}));

kpSet('bl-048-kp-settings', 'Keypad: Settings, Screen and sound', SET_WHY,
  'A keypad\'s version of settings-display. Four sections: Users, Integrations, Automations and Walk test are the alarm panel\'s and are left out. Screen and sound has no Voice volume and no Announcements (a keypad has no voice); the chime and beeps play what Harbor House says, at this keypad\'s volumes. Like today, Screen and sound needs no code.',
  {section: 'display', title: 'Screen and sound', note: '', body: [
    M.changed([
      M.section('Sound'),
      M.row_level({glyph: 'bell', title: 'Chime volume', value: 70}),
      M.row_level({glyph: 'volume-2', title: 'Beeps volume', value: 70}),
    ].join('')),
    M.paragraph('Alarms, fire and CO always sound at full volume.'),
    M.row_toggle({title: 'Key clicks', detail: 'A short click for every touch', on: true}),
    M.section('Screen'),
    M.row_link({glyph: 'sun-moon', title: 'Appearance', value: 'Dark'}),
    M.row_toggle({glyph: 'contrast', title: 'High contrast', detail: 'Stronger colors, every button outlined'}),
    M.row_level({glyph: 'sun', title: 'Brightness', value: 80}),
    M.row_link({glyph: 'image', title: 'Screen saver', detail: 'A dimmed photo with the time and the alarm state', value: 'After 1 minute'}),
    M.row_link({glyph: 'monitor-off', title: 'Screen off', detail: 'Dark until a touch wakes it', value: 'Never'}),
    M.row_link({glyph: 'moon', color: 'violet', title: 'Night hours', detail: '10:00 PM to 7:00 AM · dimmer, and dark sooner', value: 'Off'}),
  ].join('')});

kpSet('bl-048-kp-time', 'Keypad: Settings, Time and language', SET_WHY,
  'A keypad\'s version of settings-time. The clock and the time zone come from Harbor House, so they are read-only here and say so; no Time server. The 12/24-hour clock, the language and the region are this keypad\'s. As today, changing the language or the region asks for a master or installer code, which Harbor House checks.',
  {section: 'time', title: 'Time and language', body: [
    M.changed([
      M.row_link({glyph: 'calendar-clock', color: 'green', title: 'Saturday, September 19 · 10:47 PM', detail: 'From Harbor House', trail: ''}),
      M.row_link({glyph: 'globe', title: 'Time zone', detail: 'Set on Harbor House', value: 'Eastern Time', trail: ''}),
    ].join('')),
    M.row_link({glyph: 'clock', title: 'Clock', detail: '2:30 PM or 14:30', value: '12-hour'}),
    M.section('Language'),
    M.row_link({glyph: 'languages', title: 'Language', value: 'English'}),
    M.row_link({glyph: 'map-pin', title: 'Region', detail: 'How dates and numbers are written', value: 'United States'}),
    M.row_value({title: 'Looks like', value: 'Sep 19, 10:47 PM · 12,345.6'}),
  ].join('')});

// ---- About on a keypad ----
const ABOUT_WHY = 'A keypad says what it belongs to, and how to make it a panel of its own again (its role changes only by a factory reset).';
kpSet('bl-048-kp-about', 'Keypad: About', ABOUT_WHY,
  'A keypad\'s version of settings-about. Under the panel\'s own row, what it is a keypad of, then the details as today, with "Alarm panel" instead of "Set up" (zones, users and the event log are Harbor House\'s). The keypad row says Connected or "No system communication" (amber), at most once a second. Software update works as on any panel (a keypad updates itself, LINK-020).',
  {section: 'about', title: 'About this panel', body: [
    M.row_status({glyph: 'shield-check', color: 'green', title: 'RoboAlarms Panel', detail: 'Version 0.6.0 · Panel ID b7c8d9e0f1a2'}),
    M.changed(M.row_status({glyph: 'keyboard', color: 'green', title: 'Keypad 2 of Harbor House', detail: 'Connected · shows and arms Home'})),
    M.row_link({glyph: 'refresh-cw', title: 'Software update', value: 'Up to date'}),
    M.row_link({glyph: 'hard-drive', color: 'violet', title: 'SD card', detail: '44.7 GB free of 59.6 GB', value: 'Ready'}),
    M.section('This panel'),
    M.row_value({title: 'Built', value: 'Sep 19, 2026'}),
    M.row_value({title: 'Running for', value: '3 days'}),
    M.section('Network'),
    M.row_value({title: 'Wi-Fi', value: 'Harbor House'}),
    M.row_value({title: 'IP address', value: '192.0.2.57'}),
    M.changed([
      M.section('Alarm panel'),
      M.row_value({title: 'Name', value: 'Harbor House'}),
      M.row_value({title: 'Address', value: '192.0.2.42'}),
      M.row_value({title: 'Keypad number', value: '2'}),
    ].join('')),
  ].join('')});

kpSet('bl-048-kp-about-end', 'Keypad: About, scrolled to the end', ABOUT_WHY,
  'A keypad\'s version of settings-about-end: where an alarm panel has Installer, a keypad has the way back to a panel of its own, apart under a gap. It opens the next page.',
  {section: 'about', title: 'About this panel', body: [
    M.section('Hardware'),
    M.row_value({title: 'Processor', value: 'ESP32-P4 v1.0 (v1.x image)'}),
    M.row_value({title: 'Wi-Fi module', value: 'ESP32-C6 2.12.3'}),
    M.row_value({title: 'Free memory', value: '214 KB internal · 29.1 MB PSRAM'}),
    M.section(''),
    M.changed(M.row_link({glyph: 'rotate-ccw', color: 'red', title: 'Make it a stand-alone panel', detail: 'A factory reset: erases the pairing and every setting', goTo: 'bl-048-kp-standalone'})),
    M.paragraph('Made with LVGL (MIT), the Inter typeface (SIL Open Font License 1.1), Lucide icons (ISC) and parts of Z-Wave JS (MIT).'),
  ].join('')});

kpSet('bl-048-kp-standalone', 'Keypad: make it a stand-alone panel (factory reset, held)', ABOUT_WHY,
  'A keypad\'s version of today\'s Factory reset page (factory-reset), with its words, held half way: holding 2 seconds is the confirmation, as today. It erases the pairing keys first, then everything, and restarts in the setup, where the role is asked again. Who may: proposed, the installer code checked by Harbor House; after 10 minutes with no system communication it opens without one, so a keypad whose alarm panel is gone isn\'t stuck (an open question). No event-log switch: a keypad has none.',
  {section: 'about', title: 'Make it a stand-alone panel', note: 'A factory reset', back: 'bl-048-kp-about-end', body: M.changed([
    M.paragraph('Erases the pairing with Harbor House, Wi-Fi and every setting, then restarts with the setup.'),
    M.paragraph('Harbor House shows a trouble for this keypad until it is removed there.', 'amber'),
    M.row_hold({text: 'Hold to erase everything', held: 0.53}),
  ].join(''))});
