// BL-151: a Zigbee request the radio has taken but not yet answered says "still running", not "busy"
// (docs/features/20 devices.md §6; zbsvc_submit). Today such a request answers UI_ZB_REQ_BUSY after
// 4 s, though it runs: the Add page says "Not started: the radio is busy" and offers Try again for a
// window that is about to open, and a device's action toasts "busy" for something it does. A new
// answer, UI_ZB_REQ_PENDING, keeps BUSY for a request the radio never took (it didn't run).
const zbPendProp = (id, title, replaces, why, notes, page, toast) => M.screen({
  id, area: 'Zigbee', title, shot: null,
  proposal: {bl: 'BL-151', status: 'draft', replaces, why},
  notes,
  render: () => M.main({view: 'settings', tone: 'blue', rail: {tone: 'blue'}, bar: {banner: 'installer', time: '10:52'},
    content: M.settings({section: 'installer', ...page}), toast}),
});
// The add page's step list (screens/zigbee.js), with the first step's words given.
const zbPendSteps = (detail) => ['Listening', 'Joining', 'Reading what it can do', 'Setting it up', 'Ready'].map((s, i) =>
  M.row_link({glyph: 'circle-dot', color: i === 0 ? 'blue' : 'gray', title: s, detail: i === 0 ? detail : 'Waiting', trail: ''})).join('');

zbPendProp('bl-151-zb-add-starting', 'Adding: the radio is slow to start listening', 'zigbee-add-listening',
  'Opening the Add page while the radio is still finishing something (a slow save, a device being removed) said "Not started: the radio is busy" though the window opened a few seconds later; Try again then asked for it twice.',
  'The page shows the listening steps at once, the first one saying it is starting; no countdown and no Keep listening until the radio says it listens, then it becomes today\'s listening page (polled once a second, nothing moves). Back still leaves the page. If the radio has not started listening 20 s later, the page ends as "Not started" (next screen). Leaving while it starts sends Stop as today; a Stop the radio can\'t take yet is sent again once it listens, so the window never stays open with nobody on the page.',
  {title: 'Add a Zigbee device', note: 'Starting', back: 'zigbee', body: [
    M.changed(M.paragraph('The radio is taking longer than usual to start listening. This page follows it.')),
    M.changed(zbPendSteps('Starting')),
    M.paragraph('Leaving this page stops listening.'),
  ].join('')});

zbPendProp('bl-151-zb-add-no-answer', 'Adding: the radio never started listening', 'zigbee-add-listening',
  'A request the radio took can still fail after the page stopped waiting for its answer (the radio stopped, or something else took it); the page must not wait for ever.',
  'After 20 s of "Starting" with no listening in the progress. Try again asks once more. Today\'s "Not started: the radio is busy" stays for a request the radio never took.',
  {title: 'Add a Zigbee device', note: 'Not started', back: 'zigbee', body: [
    M.changed(M.paragraph('Not started: the radio didn\'t answer. Try again.', 'amber')),
    M.row_action({glyph: 'refresh-cw', text: 'Try again', color: 'blue'}),
  ].join('')});

zbPendProp('bl-151-zb-request-pending', 'A device\'s action the radio is slow to answer', 'zigbee-device-end',
  'Blink its light, Check it now, Set it up again, Rename, Remove and the radio\'s own actions toasted "The radio is busy: try again when it\'s done" when the radio took the request but answered late; trying again did it twice.',
  'The toast replaces the action\'s own words (here "Its light blinks for a few seconds"), once; the page then shows the outcome when it comes, as today. Every Zigbee request uses it: the device page, the radio\'s page (restart, channel, reset), install codes, the siren, keypad and key fob pages. The Panel Manager says the same (its BL-151 proposal).',
  {title: 'Front Door', note: 'Aqara door sensor · Zone 1', back: 'zigbee', body: [
    M.section('Check it'),
    M.row_link({glyph: 'lightbulb', title: 'Blink its light', detail: 'Find which device this is', trail: ''}),
    M.row_link({glyph: 'activity', title: 'Check it now', detail: 'Asks it to answer', trail: ''}),
    M.row_link({glyph: 'refresh-cw', title: 'Set it up again', detail: 'Reads what it can do and how it reports', trail: ''}),
    M.section('Removing it'),
    M.row_link({glyph: 'arrow-left-right', title: 'Replace it', detail: 'A new device takes its zones'}),
    M.row_link({glyph: 'trash-2', color: 'red', title: 'Remove this device', detail: 'Asks it to leave the network'}),
  ].join('')},
  'Still running: the radio is taking longer than usual');
