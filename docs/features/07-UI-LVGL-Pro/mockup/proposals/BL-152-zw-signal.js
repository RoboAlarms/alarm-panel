// BL-152 (the Z-Wave TLC run, ZW-040): a Z-Wave device's page says how well the panel hears it, in the words
// of the Zigbee device page's Signal row, under "Last heard". Nothing is shown until a frame was measured.
const ZWS_PAGE = (signal) => () => M.main({view: 'settings', tone: 'blue', rail: {troubles: 0, tone: 'blue'},
  bar: {banner: 'installer', time: '10:52', troubles: 0},
  content: M.settings({section: 'installer', back: 'installer-locked', title: 'Door sensor',
    note: 'Z-Wave device 4', body: [
      M.row_value({title: 'Security', value: 'Encrypted: its signal can\'t be copied'}),
      M.row_value({title: 'Battery', value: '92 %'}),
      M.row_value({title: 'Last heard', value: '2 min ago'}),
      signal ? M.changed(M.row_value({title: 'Signal', value: signal})) : '',
    ].join('')})});
const ZWS_DECIDED = '2026-10-03: the Z-Wave TLC changes were to be drawn, then built without waiting for review';
for (const [id, label, word] of [['good', 'Good', 'Good'], ['fair', 'Fair', 'Fair'], ['weak', 'Weak', 'Weak'], ['unknown', 'not measured yet', null]]) {
  M.screen({
    id: 'bl-152-zw-signal-' + id, area: 'Installer', title: 'Z-Wave device page: signal ' + label, shot: null,
    proposal: {bl: 'BL-152', status: 'approved', replaces: null, decided: ZWS_DECIDED,
      why: 'An installer placing a sensor needs to see whether the panel hears it well, as on a Zigbee device.'},
    notes: word ? 'A Signal row under Last heard; the word follows the last frame heard (margin over the background noise). It changes in place, once a second at most.'
      : 'Before any frame was measured the row is left out, never a made-up word.',
    render: ZWS_PAGE(word),
  });
}
