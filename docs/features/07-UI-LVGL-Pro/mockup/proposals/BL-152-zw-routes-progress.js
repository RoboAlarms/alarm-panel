// BL-152 (the Z-Wave TLC run): rebuilding routes names the devices it waits for, and its end stays on
// the Z-Wave page, naming what failed or was skipped, until it is cleared (screens.md section 3, ZW-024).
// Today: only "3 of 7 done" while it runs, then a toast "Routes rebuilt for 5 of 7: the others didn't answer".
const ZWRT_PAGE = (rows) => () => M.main({view: 'settings', tone: 'blue', rail: {troubles: 1, tone: 'blue'},
  bar: {banner: 'installer', time: '10:52', troubles: 1},
  content: M.settings({section: 'installer', back: 'installer-locked', title: 'Z-Wave',
    note: '7 devices', body: rows.join('')})});
const ZWRT_ROW = (value) => M.row_link({glyph: 'waves', color: 'blue', title: 'Rebuild routes',
  detail: 'Refresh how signals reach the panel', value});
const ZWRT_PROOF = {bl: 'BL-152', status: 'approved', replaces: null,
  decided: '2026-10-03: the Z-Wave TLC changes were to be drawn, then built without waiting for review'};

M.screen({
  id: 'bl-152-zw-routes-running', area: 'Installer', title: 'Z-Wave: rebuilding routes, sleepers to wake', shot: null,
  proposal: {...ZWRT_PROOF, why: 'A battery sensor is repaired only when it wakes; the installer needs to know which ones to wake.'},
  notes: 'A note under the row names the sleepers still waiting (Wake these: ...). It changes at most once a second; no toast.',
  render: ZWRT_PAGE([ZWRT_ROW('3 of 7 done'),
    M.changed(M.row_link({glyph: 'waves', color: 'blue', title: 'Wake these: Front Door, Garage tilt', detail: 'Press each one\'s button', trail: ''}))]),
});

M.screen({
  id: 'bl-152-zw-routes-result', area: 'Installer', title: 'Z-Wave: the rebuild\'s result, kept until cleared', shot: null,
  proposal: {...ZWRT_PROOF, why: 'The result was a toast that vanished; failed and skipped devices are counted apart and named.'},
  notes: 'The result row says Clear and puts itself away when tapped. Each device named below it opens its device page. Skipped means dead, Long Range or stopped; they are not "didn\'t answer".',
  render: ZWRT_PAGE([ZWRT_ROW(''),
    M.changed(M.row_link({glyph: 'waves', color: 'blue', title: 'Routes rebuilt for 4 of 7', detail: '1 didn\'t answer · 2 skipped', value: 'Clear', trail: ''})),
    M.changed(M.row_link({glyph: 'radio-tower', color: 'amber', title: 'Front Door', detail: 'Didn\'t answer'})),
    M.changed(M.row_link({glyph: 'radio-tower', color: 'amber', title: 'Shed', detail: 'Skipped: dead, Long Range or stopped'})),
    M.changed(M.row_link({glyph: 'radio-tower', color: 'amber', title: 'Garage tilt', detail: 'Skipped: dead, Long Range or stopped'}))]),
});
