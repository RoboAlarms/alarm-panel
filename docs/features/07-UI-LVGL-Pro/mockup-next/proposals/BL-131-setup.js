// BL-131: the first-boot setup (mockup-next). "Leave setup" moves away from Back and Next, the
// clock is set with a calendar and rollers, and no toast repeats what the screen shows.

const P = (replaces, why) => ({bl: 'BL-131', status: 'approved', decided: '2026-10-01: approved as drawn: "start working on getting all of the screen changes in place"', replaces, why});
const rollers = `<div class="time_rollers">${M.roller({label: 'Hour', items: ['10', '11', '12', '1', '2'], width: 76})}${M.roller({label: 'Minute', items: ['52', '53', '54', '55', '56'], width: 76})}${M.roller({items: ['', '', 'AM', 'PM', ''], width: 72})}</div>`;

M.screen({
  id: 'n-setup-wifi', area: 'Setup', title: 'Joining Wi-Fi', shot: null,
  proposal: P('setup-wifi', '"Leave setup" sat beside Back, a reach away from a mis-tap that ends the setup. It moves to the left, under the step\'s words. The joined network isn\'t listed twice.'),
  render: () => M.wizard({step: 1, glyph: 'wifi', title: 'Join Wi-Fi', leaveSide: true,
    note: 'The alarm works without it. Wi-Fi sets the clock and keeps the panel up to date.', exit: true, back: true, next: 'Next',
    body: [
      M.row_link({glyph: 'wifi', color: 'green', title: 'Harbor House', detail: 'Connected · strong signal', trail: 'check', trailColor: 'green'}),
      M.changed(M.section('Other networks nearby')),
      M.row_link({glyph: 'wifi-high', title: 'Harbor House 5G', detail: 'Password · good signal', trail: 'lock'}),
      M.row_link({glyph: 'wifi-high', title: 'Maple Street', detail: 'Password · good signal', trail: 'lock'}),
      M.row_link({glyph: 'wifi-high', title: 'DIRECT-42 Printer', detail: 'Password · fair signal', trail: 'lock'}),
      M.row_link({glyph: 'wifi-zero', title: 'Café Guest', detail: 'Open · weak signal', trail: ''}),
    ].join('')}),
});

M.screen({
  id: 'n-setup-clock', area: 'Setup', title: 'Setting the clock by hand', shot: null,
  proposal: P('setup-clock', 'Five stepper rows scrolled under the buttons and the step wore the time zone\'s globe. Now a calendar and rollers that fit, a clock icon, and shorter words.'),
  notes: 'Shown only when the panel has no time and no internet. The calendar is narrower here (300 px, 40 px days) to sit beside the rollers.',
  render: () => M.wizard({step: 2, glyph: 'calendar-clock', title: 'Date and time', leaveSide: true,
    note: 'No internet yet, so set the clock here. The panel corrects it once it\'s online.',
    exit: true, back: true, next: 'Next',
    body: M.changed(`<div class="row" style="gap:var(--space_lg);align-items:flex-start">
      ${M.calendar({sel: 30, width: 320})}
      <div style="padding-top:var(--space_md)">${rollers}</div></div>`)
      + `<div class="result_line">${M.text('The clock will say', 'body', 'text_2')}${M.text('Wed, Sep 30, 2026 · 12:54 AM', 'label')}</div>`}),
});

M.screen({
  id: 'n-setup-codes', area: 'Setup', title: 'One code changed', shot: null,
  proposal: P('setup-codes-one', 'A toast said "Installer code changed" while the row said "Changed" (toasts only for what the screen doesn\'t show). "Keep for now" as the Next button read as keeping the installer code too; it is "Next", and the amber line says what stays.'),
  render: () => M.wizard({step: 3, glyph: 'key', title: 'Change the factory codes', leaveSide: true,
    note: 'Anyone can look up the factory codes, so choose your own for the installer and the master.',
    exit: true, back: true, next: 'Next',
    body: [
      M.row_link({glyph: 'shield-check', color: 'green', title: 'Installer code', detail: 'Changed · for programming the panel', value: 'Change again'}),
      M.row_link({glyph: 'key-round', color: 'amber', title: 'Master code', detail: 'Still the factory code · arms, disarms, adds users', value: 'Change'}),
      M.changed(M.paragraph('Skip it for now and the status bar reminds you until the master code is changed.', 'amber')),
    ].join('')}),
});
