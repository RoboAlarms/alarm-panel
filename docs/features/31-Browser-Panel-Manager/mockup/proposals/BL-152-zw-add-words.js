// BL-152: the Z-Wave add card names the likely cause, shows the time left and says how a radio restart ended
// (docs/features/31 mockup/README.md; docs/features/36 screens.md §4; ZW-005, ZW-006, ZW-029).
// Drawn straight in panel-pages.js (zwOpCard, zwDone), the same words as the touchscreen's.
proposal({
  bl: 'BL-152',
  title: 'The Z-Wave add card names the likely cause, the time left and a radio restart',
  status: 'approved',
  decided: '2026-10-03: the Z-Wave TLC changes were to be drawn, then built without waiting for review',
  role: 'installer',
  why: [
    'When nothing joins, the card never says the commonest cause: the device still belongs to another system and must be removed first. The security and PIN questions say "some devices give up after a few seconds" while the panel waits four minutes, and show no clock. A restart of the Z-Wave radio during Add or Remove ended as "It couldn\'t be done".',
    'The nothing-joined card adds "If it was used with another system, remove it first: put it in pairing mode on the Remove page, then add it." with a Remove a device button. The two questions show "Time left" counting down. A radio restart says "The Z-Wave radio restarted, so it stopped listening. Try again." with a Try again button.',
  ],
  look: [
    ['zwave', 'Add a device and let it run out: the Nothing joined card with Remove a device'],
    ['zwave', 'Add a device and press the pretend sensor: the allow card and the PIN card show Time left'],
  ],
  replaces: ['zwave'],
  notes: [
    'The clock is one value (#zw-left) the page updates every second in place, so ticked boxes and a PIN being typed stay as they are.',
    'No API change: the result word "radio_restarted" is appended to the progress result words.',
  ],
  css: ``,
  apply() {},
});
