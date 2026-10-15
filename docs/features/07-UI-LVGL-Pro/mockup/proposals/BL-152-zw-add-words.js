// BL-152: the Z-Wave Add pages say the likely cause, what Cancel does, the time left and how a radio restart ended
// (docs/features/36 screens.md §4; ZW-005, ZW-006, ZW-019, ZW-029). Drawn from the preview pictures
// zwave_add_security, zwave_add_pin and zwave_add_setup_stopped, with what changes wrapped in M.changed.
const zwAddProp = (id, title, what, page, notes) => M.screen({
  id, area: 'Installer', title, shot: null,
  proposal: {bl: 'BL-152', status: 'approved', replaces: 'zwave-region',
    decided: '2026-10-03: the Z-Wave TLC changes were to be drawn, then built without waiting for review',
    why: what},
  notes,
  render: () => M.main({view: 'settings', tone: 'blue', rail: {troubles: 1, tone: 'blue'}, bar: {banner: 'installer', time: '10:52', troubles: 1},
    content: M.settings({section: 'installer', ...page})}),
});

zwAddProp('bl-152-zw-nothing-joined', 'Z-Wave add: nothing joined',
  'The commonest reason nothing joins is a device that still belongs to another network; the page never said so.',
  {title: 'Add a Z-Wave device', note: 'Nothing joined', back: true, body: [
    M.paragraph('Nothing joined. Check the device\'s pairing steps, then try again.'),
    M.changed(M.paragraph('If it was used with another system, remove it first: put it in pairing mode on the Remove page, then add it.')),
    M.changed(M.row_action({glyph: 'trash', text: 'Remove a device', color: 'blue'})),
    M.row_action({glyph: 'refresh-cw', text: 'Try again', color: 'blue'}),
  ].join('')},
  'ZW-006. "Remove a device" opens the Remove page.');

zwAddProp('bl-152-zw-allow-time', 'Z-Wave add: the secure connection question, with its time left',
  'The Allow page said "4 minutes to decide" once, when built, and never counted down.',
  {title: 'Secure connection', note: 'The device asks for its messages to be encrypted', back: true, body: [
    M.row_toggle({title: 'Access control', detail: 'For locks, keypads and garage doors', on: true}),
    M.row_toggle({title: 'With its PIN', detail: 'Needs the 5-digit PIN printed on it', on: true}),
    M.row_toggle({title: 'Without a PIN', detail: 'Encrypted, no PIN needed', on: true}),
    M.paragraph('Allow it. Without encryption its messages could be faked: a sensor can\'t guard fire or CO, a switch can be worked by others.'),
    M.changed(M.row_value({title: 'Time left', value: '3 min 41 s'})),
    M.row_action({glyph: 'circle-check', text: 'Allow', color: 'green'}),
    M.row_action({glyph: 'circle-x', text: 'Add it without encryption', color: 'red'}),
  ].join('')},
  'ZW-029. The value counts down once a second, like the PIN page\'s.');

zwAddProp('bl-152-zw-pin-cancel', 'Z-Wave add: the PIN page\'s way out says what it does',
  '"Cancel" led to "Stop adding it?" but kept the device in the network without encryption.',
  {title: 'Enter its PIN', note: 'The first 5 digits of the key on its label or box', back: true, body: [
    M.paragraph('The rest of the key should read: 40311-11111-22222-33333-44444-55555-60000'),
    M.paragraph('If it doesn\'t match the label, this is not your device: stop.', 'amber'),
    M.row_value({title: 'Time left', value: '3 min 2 s'}),
    M.row_link({glyph: 'key-round', color: 'blue', title: 'Enter the PIN'}),
    M.changed(M.row_action({glyph: 'circle-x', text: 'Add it without encryption', color: 'gray'})),
  ].join('')},
  'ZW-005. Its confirm reads "Add it without encryption?" with the body "It stays in the network without encryption." and the button "Add it without encryption".');

zwAddProp('bl-152-zw-setup-stopped', 'Z-Wave add: a stopped setup says so on its checklist row',
  'When the setup stopped, its row kept a blue dot and "Step 2 of 16", as if it were working.',
  {title: 'Adding the device', note: 'Keep the device close to the panel', back: true, body: [
    M.row_status({glyph: 'circle-check', color: 'green', title: 'Connected', detail: 'Device 2'}),
    M.row_status({glyph: 'circle-alert', color: 'amber', title: 'Secured', detail: 'Not confirmed: its setup didn\'t finish'}),
    M.changed(M.row_status({glyph: 'circle-alert', color: 'amber', title: 'Reading what it can do', detail: 'Stopped: it didn\'t answer'})),
    M.row_status({glyph: 'circle-dot', color: 'gray', title: 'Ready to use'}),
    M.paragraph('It stopped answering before its setup finished. Keep it close to the panel, check its power, then try again.', 'amber'),
    M.row_action({glyph: 'refresh-cw', text: 'Try again', color: 'blue'}),
  ].join('')},
  'ZW-019. The device page\'s sentence loses its step numbers: "Its setup stopped: it didn\'t answer. Bring it closer or check its power, then try again. It can\'t be used until then."');

zwAddProp('bl-152-zw-radio-restarted', 'Z-Wave add: the radio restarted',
  'A restart of the radio while it listened ended the page with the generic "Something went wrong".',
  {title: 'Couldn\'t add it', note: '', back: true, body: [
    M.changed(M.paragraph('The Z-Wave radio restarted, so it stopped listening. Try again.', 'amber')),
    M.row_action({glyph: 'refresh-cw', text: 'Try again', color: 'blue'}),
  ].join('')},
  'ZW-005. The Remove page says the same words with its own Try again.');
