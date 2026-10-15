// The first-boot setup (screen_wizard.xml, ui_wizard.c): where you are and why on the left, the
// step on the right.

M.screen({
  id: 'setup-welcome', area: 'Setup', title: 'Welcome', shot: 'setup_welcome',
  render: () => M.wizard({step: 0, glyph: 'sparkles', title: 'Welcome',
    note: 'Let\'s get your alarm panel ready. It takes about two minutes, and you can change everything later in Settings.',
    leaveSide: true, exit: true, next: 'Start', toast: 'Installer mode ended · arming is on again',
    body: [
      M.row_link({glyph: 'wifi', title: 'Join Wi-Fi', detail: 'For the clock and updates', trail: ''}),
      M.row_link({glyph: 'globe', title: 'Choose your time zone', detail: 'So the event log shows your local time', trail: ''}),
      M.row_link({glyph: 'key', color: 'amber', title: 'Change the factory codes', detail: 'Your own installer and master codes', trail: ''}),
      M.row_link({glyph: 'tag', color: 'violet', title: 'Name your system', detail: 'How it shows up on the screen', trail: ''}),
      M.row_link({glyph: 'party-popper', color: 'green', title: 'Look it over', detail: 'A last look at what you chose', trail: ''}),
      M.paragraph('The panel already protects your home: fire, CO and panic work right now.'),
    ].join('')}),
});

M.screen({
  id: 'setup-wifi', area: 'Setup', title: 'Joining Wi-Fi', shot: 'setup_wifi',
  render: () => M.wizard({step: 1, glyph: 'wifi', title: 'Join Wi-Fi',
    note: 'The alarm works without it. Wi-Fi sets the clock and keeps the panel up to date.', leaveSide: true, exit: true, back: true, next: 'Next',
    body: [
      M.row_link({glyph: 'wifi', color: 'green', title: 'Harbor House', detail: 'Connected · 192.0.2.42 · strong signal', trail: ''}),
      M.section('Networks nearby'),
      M.row_link({glyph: 'wifi', title: 'Harbor House', detail: 'Password · strong signal', trail: 'lock'}),
      M.row_link({glyph: 'wifi-high', title: 'Harbor House 5G', detail: 'Password · good signal', trail: 'lock'}),
      M.row_link({glyph: 'wifi-high', title: 'Maple Street', detail: 'Password · good signal', trail: 'lock'}),
      M.row_link({glyph: 'wifi-low', title: 'DIRECT-42 Printer', detail: 'Password · fair signal', trail: 'lock'}),
      M.row_link({glyph: 'wifi-zero', title: 'Café Guest', detail: 'Open · weak signal', trail: ''}),
    ].join('')}),
});

M.screen({
  id: 'setup-clock', area: 'Setup', title: 'Setting the clock by hand', shot: 'setup_clock',
  notes: 'Shown only when the panel has no time and no internet (RTC-015). The calendar is narrower here (320 px, 40 px days) to sit beside the rollers; Next sets the clock to what they show (BL-131 finding 3, built).',
  render: () => M.wizard({step: 2, glyph: 'calendar-clock', title: 'Date and time',
    note: 'No internet yet, so set the clock here. The panel corrects it once it\'s online.',
    leaveSide: true, exit: true, back: true, next: 'Next',
    body: `<div class="row" style="gap:var(--space_lg);align-items:flex-start">
      ${M.calendar({sel: 30, width: 320})}
      <div style="padding-top:var(--space_md)"><div class="time_rollers">${M.roller({label: 'Hour', items: ['10', '11', '12', '1', '2'], width: 76})}${M.roller({label: 'Minute', items: ['52', '53', '54', '55', '56'], width: 76})}${M.roller({items: ['', '', 'AM', 'PM', ''], width: 72})}</div></div></div>`
      + `<div class="result_line">${M.text('The clock will say', 'body', 'text_2')}${M.text('Wed, Sep 30, 2026 · 12:54 AM', 'label')}</div>`}),
});

M.screen({
  id: 'setup-codes', area: 'Setup', title: 'A new installer code', shot: 'setup_new_code',
  render: () => M.wizard({step: 3, glyph: 'key', title: 'Change the factory codes',
    note: 'Anyone can look up the factory codes, so choose your own for the installer and the master.',
    pad: M.code_pad({title: 'New installer code', tone: 'blue', glyph: 'key', digits: 4, width: 428})}),
});

M.screen({
  id: 'setup-done', area: 'Setup', title: 'Done', shot: 'setup_done',
  render: () => M.wizard({step: 5, glyph: 'party-popper', title: 'You\'re all set', leaveSide: true,
    note: 'Your panel is protecting your home. Everything here can be changed later in Settings.',
    back: true, next: 'Done',
    body: [
      M.row_link({glyph: 'wifi', color: 'green', title: 'Wi-Fi', detail: 'Harbor House', trail: 'check', trailColor: 'green'}),
      M.row_link({glyph: 'globe', color: 'green', title: 'Time zone', detail: 'Eastern Time', trail: 'check', trailColor: 'green'}),
      M.row_link({glyph: 'key-round', color: 'amber', title: 'Codes', detail: 'The master code is still the factory code', value: 'Change', trail: ''}),
      M.row_link({glyph: 'house', color: 'green', title: 'Name', detail: 'Home', trail: 'check', trailColor: 'green'}),
    ].join('')}),
});

M.screen({
  id: 'setup-codes-one', area: 'Setup', title: 'One code changed', shot: 'setup_codes',
  render: () => M.wizard({step: 3, glyph: 'key', title: 'Change the factory codes', leaveSide: true,
    note: 'Anyone can look up the factory codes, so choose your own for the installer and the master.',
    exit: true, back: true, next: 'Next',
    body: [
      M.row_link({glyph: 'shield-check', color: 'green', title: 'Installer code', detail: 'Changed · for programming the panel', value: 'Change again'}),
      M.row_link({glyph: 'key-round', color: 'amber', title: 'Master code', detail: 'Still the factory code · arms, disarms, adds users', value: 'Change'}),
      M.paragraph('Skip it for now and the status bar reminds you until the master code is changed.', 'amber'),
    ].join('')}),
});
