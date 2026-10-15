// BL-152 (Z-Wave TLC): the Troubles row of a Z-Wave radio problem names Z-Wave and opens its page
// (screens.md section 10; ZW-019). Today the shared "Sensor radio problem" row has no link and
// doesn't say which radio. Zigbee's row already says "Tap to see the radio" (screens/zigbee.js).

const ZW_TROUBLE_NOTE = 'Wireless sensors may not be heard \u00b7 Tap to see the radio';

M.screen({
  id: 'bl-152-zwave-trouble-today', area: 'Troubles', title: 'Troubles: a Z-Wave radio problem, today', shot: null,
  proposal: {bl: 'BL-152', status: 'approved', replaces: 'zigbee-troubles',
    decided: '2026-10-03: the Z-Wave TLC changes were to be drawn, then built without waiting for review',
    why: 'When the Z-Wave radio is jammed or silent, the installer sees a shared row that names no radio and opens nothing.'},
  notes: 'Today: the shared row. It has no link and does not say Z-Wave.',
  render: () => M.main({view: 'troubles', tone: 'amber', rail: {zones: 1, troubles: 1}, bar: {time: '10:48', troubles: 1}, content: `<div class="view column">
    <div class="row">${M.view_header({title: 'Troubles', note: '1 active'})}${M.button('Acknowledge', '', 'check')}</div><div class="list scroll">
      ${M.trouble_row({glyph: 'radio-tower', title: 'Sensor radio problem', detail: 'Wireless sensors may not be heard'})}</div></div>`}),
});

M.screen({
  id: 'bl-152-zwave-trouble', area: 'Troubles', title: 'Troubles: the Z-Wave radio\'s row opens its page', shot: null,
  proposal: {bl: 'BL-152', status: 'approved', replaces: 'zigbee-troubles',
    decided: '2026-10-03: the Z-Wave TLC changes were to be drawn, then built without waiting for review',
    why: 'The installer sees which radio has the problem and gets to its page, as Zigbee\'s row does.'},
  notes: 'The row names Z-Wave and says it can be tapped. Tapped in installer mode it opens the Z-Wave page on top of the installer menu; outside it, the locked Installer page asks for the code first (toast "Enter the installer code to see the radio"), then the Z-Wave page follows.',
  render: () => M.main({view: 'troubles', tone: 'amber', rail: {zones: 1, troubles: 1}, bar: {time: '10:48', troubles: 1}, content: `<div class="view column">
    <div class="row">${M.view_header({title: 'Troubles', note: '1 active'})}${M.button('Acknowledge', '', 'check')}</div><div class="list scroll">
      ${M.changed(M.trouble_row({glyph: 'radio-tower', title: 'Z-Wave radio problem', detail: ZW_TROUBLE_NOTE}))}</div></div>`}),
});

M.screen({
  id: 'bl-152-both-radios-trouble', area: 'Troubles', title: 'Troubles: both radios in trouble, one row each', shot: null,
  proposal: {bl: 'BL-152', status: 'approved', replaces: 'zigbee-troubles',
    decided: '2026-10-03: the Z-Wave TLC changes were to be drawn, then built without waiting for review',
    why: 'Both sensor radios can have the problem at once: each gets its own row and its own page.'},
  notes: 'With both radios in trouble the one shared trouble shows as two rows, each opening its own page. With one radio in trouble, Zigbee\'s row keeps today\'s title.',
  render: () => M.main({view: 'troubles', tone: 'amber', rail: {zones: 1, troubles: 2}, bar: {time: '10:48', troubles: 2}, content: `<div class="view column">
    <div class="row">${M.view_header({title: 'Troubles', note: '2 active'})}${M.button('Acknowledge', '', 'check')}</div><div class="list scroll">
      ${M.changed(M.trouble_row({glyph: 'radio-tower', title: 'Zigbee radio problem', detail: ZW_TROUBLE_NOTE}))}
      ${M.changed(M.trouble_row({glyph: 'radio-tower', title: 'Z-Wave radio problem', detail: ZW_TROUBLE_NOTE}))}</div></div>`}),
});
