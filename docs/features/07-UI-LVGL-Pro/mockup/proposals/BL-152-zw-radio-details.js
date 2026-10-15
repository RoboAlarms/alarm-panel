// BL-152 (the Z-Wave TLC run): Installer > Z-Wave > Radio details gains a "Background noise" row,
// and "Restart the radio" goes gray while the radio's own software installs.
const ZWR_PAGE = (rows) => () => M.main({view: 'settings', tone: 'blue', rail: {troubles: 1, tone: 'blue'},
  bar: {banner: 'installer', time: '10:52', troubles: 1},
  content: M.settings({section: 'installer', back: 'installer-locked', title: 'Z-Wave radio',
    note: 'What the panel knows about its radio', body: rows.join('')})});
const ZWR_TOP = (state) => [
  M.row_value({title: 'State', value: state}),
  M.row_value({title: 'Radio software', value: '2.30 · Z-Wave 7.24.2'}),
  M.row_value({title: 'Region', value: 'United States'}),
];
const ZWR_RESTART = M.row_link({glyph: 'rotate-ccw', color: 'blue', title: 'Restart the radio',
  detail: 'Takes a few seconds; zones keep their state', goTo: null});

M.screen({
  id: 'bl-152-zw-radio-measured', area: 'Installer', title: 'Z-Wave radio details: noise measured', shot: null,
  proposal: {bl: 'BL-152', status: 'approved', replaces: null,
    decided: '2026-10-03: the Z-Wave TLC changes were to be drawn, then built without waiting for review',
    why: 'An installer hunting interference needs the radio\'s background noise, as the Zigbee page has.'},
  notes: 'Today: State, Radio software, Region, Restart. New: the Background noise row (the quietest channel, the one dBm on the touchscreen).',
  render: ZWR_PAGE([...ZWR_TOP('Working'), M.changed(M.row_value({title: 'Background noise', value: '-94 dBm (quietest channel)'})), ZWR_RESTART]),
});

M.screen({
  id: 'bl-152-zw-radio-unmeasured', area: 'Installer', title: 'Z-Wave radio details: not measured yet', shot: null,
  proposal: {bl: 'BL-152', status: 'approved', replaces: null,
    decided: '2026-10-03: the Z-Wave TLC changes were to be drawn, then built without waiting for review',
    why: 'Before the first reading the row says so, never a made-up number.'},
  notes: 'The row updates in place, once a second at most.',
  render: ZWR_PAGE([...ZWR_TOP('Working'), M.changed(M.row_value({title: 'Background noise', value: 'Not measured yet'})), ZWR_RESTART]),
});

M.screen({
  id: 'bl-152-zw-radio-updating', area: 'Installer', title: 'Z-Wave radio details: while its software installs', shot: null,
  proposal: {bl: 'BL-152', status: 'approved', replaces: null,
    decided: '2026-10-03: the Z-Wave TLC changes were to be drawn, then built without waiting for review',
    why: 'A restart during the radio\'s own update ends in a refusal; grey it out, as the Panel Manager does.'},
  notes: 'Restart is gray with no action; another running change reads "Not while another change runs".',
  render: ZWR_PAGE([...ZWR_TOP('Updating'), M.row_value({title: 'Background noise', value: '-94 dBm (quietest channel)'}),
    M.changed(M.row_link({glyph: 'rotate-ccw', color: 'gray', title: 'Restart the radio', detail: 'Not while its software installs'}))]),
});
