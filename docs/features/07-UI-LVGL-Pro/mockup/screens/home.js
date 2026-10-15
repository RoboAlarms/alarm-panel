// Home: the state card beside the arming tiles, the code pad or an action (screen_main.xml
// "view_home", ui_app.c). The words are the panel's English as the PC preview shows it.

const DISARM_PAD = {title: 'Disarm', tone: 'green', glyph: 'lock-open', close: false};
const ARMED_BAR = {banner: null, time: '10:47'};
const ARMED_RAIL = {locked: true};

M.screen({
  id: 'home-ready', area: 'Home', title: 'Disarmed, ready to arm', shot: 'home_ready',
  notes: 'The start screen. The factory-code banner shows until both codes are changed.',
  render: () => M.main({content: M.home(
    {tone: 'green', glyph: 'shield-check', headline: 'Disarmed', detail: 'Ready to arm', note: 'All zones closed'}, M.panel_arm())}),
});

M.screen({
  id: 'home-not-ready', area: 'Home', title: 'Disarmed, a zone open', shot: 'home_not_ready',
  render: () => M.main({tone: 'amber', rail: {zones: 1}, content: M.home(
    {tone: 'amber', glyph: 'shield-alert', headline: 'Not ready', detail: 'Living Room Window is open', note: 'Close it, or bypass when arming'},
    M.panel_arm())}),
});

M.screen({
  id: 'pad-arm-away', area: 'Home', title: 'Arming Away: the code pad', shot: 'pad_arm_away',
  notes: 'A tile opens the pad in its place, in the level\'s tone. Silent exit is offered for Away only.',
  render: () => M.main({content: M.home(
    {tone: 'green', glyph: 'shield-check', headline: 'Disarmed', detail: 'Ready to arm', note: 'All zones closed'},
    M.code_pad({title: 'Arm Away', tone: 'rose', glyph: 'log-out', digits: 3,
      options: [{text: 'No entry delay'}, {text: 'Silent exit'}]}))}),
});

M.screen({
  id: 'pad-arm-not-ready', area: 'Home', title: 'Arming while a zone is open', shot: 'pad_arm_not_ready_options',
  notes: 'Bypass open shows only when something is open; the options wrap to a second row.',
  render: () => M.main({tone: 'amber', rail: {zones: 1}, bar: {battery: {glyph: 'battery-medium', text: '62%'}},
    content: M.home(
      {tone: 'amber', glyph: 'shield-alert', headline: 'Not ready', detail: 'Front Door is open', note: 'Close it, or bypass when arming'},
      M.code_pad({title: 'Arm Away', tone: 'rose', glyph: 'log-out',
        options: [{text: 'No entry delay'}, {text: 'Silent exit'}, {text: 'Bypass open'}]}))}),
});

M.screen({
  id: 'exit-delay', area: 'Home', title: 'Exit time, counting down', shot: 'exit_delay',
  notes: 'The ring empties as the seconds run out. Settings and Chime are locked while armed.',
  render: () => M.main({tone: 'rose', rail: ARMED_RAIL, bar: {...ARMED_BAR, time: '10:46', locked: true}, content: M.home(
    {tone: 'rose', countdown: 45, fill: 0.75, headline: 'Arming Away', detail: 'Leave now through the exit door', note: 'Exit time'},
    M.code_pad({title: 'Cancel arming', tone: 'green', glyph: 'lock-open', close: false}))}),
});

M.screen({
  id: 'armed-away', area: 'Home', title: 'Armed Away', shot: 'armed_away',
  render: () => M.main({tone: 'rose', rail: ARMED_RAIL, bar: {...ARMED_BAR, locked: true}, content: M.home(
    {tone: 'rose', glyph: 'lock', headline: 'Armed Away', detail: 'The whole house is protected', note: 'Since 10:46 PM · By Master'},
    M.code_pad(DISARM_PAD))}),
});

M.screen({
  id: 'armed-night', area: 'Home', title: 'Armed Night', shot: 'armed_night',
  render: () => M.main({tone: 'violet', rail: {...ARMED_RAIL, troubles: 1}, bar: {...ARMED_BAR, time: '10:49', troubles: 1, locked: true}, content: M.home(
    {tone: 'violet', glyph: 'moon', headline: 'Armed Night', detail: 'Doors, windows and night zones', note: 'Since 10:48 PM · By Master'},
    M.code_pad(DISARM_PAD))}),
});

M.screen({
  id: 'entry-delay', area: 'Home', title: 'Entry time: disarm now', shot: 'entry_delay',
  render: () => M.main({tone: 'orange', rail: ARMED_RAIL, bar: {...ARMED_BAR, locked: true}, content: M.home(
    {tone: 'orange', countdown: 22, fill: 0.73, headline: 'Disarm now', detail: 'Entry through Front Door', note: 'The alarm sounds when time runs out'},
    M.code_pad({...DISARM_PAD, digits: 2}))}),
});

M.screen({
  id: 'pad-wrong-code', area: 'Home', title: 'A wrong code', shot: 'pad_wrong_code',
  render: () => M.main({tone: 'orange', rail: ARMED_RAIL, bar: {...ARMED_BAR, locked: true}, content: M.home(
    {tone: 'orange', countdown: 21, fill: 0.7, headline: 'Disarm now', detail: 'Entry through Front Door', note: 'The alarm sounds when time runs out'},
    M.code_pad({...DISARM_PAD, hint: 'Wrong code, try again', error: true}))}),
});

M.screen({
  id: 'pad-locked-out', area: 'Home', title: 'Too many wrong codes', shot: 'pad_locked_out',
  notes: 'The countdown in the hint changes once a second (UI-020).',
  render: () => M.main({content: M.home(
    {tone: 'green', glyph: 'shield-check', headline: 'Disarmed', detail: 'Ready to arm', note: 'All zones closed'},
    M.code_pad({title: 'Arm Away', tone: 'rose', glyph: 'log-out', hint: 'Keypad locked. Try again in 4:59', error: true, options: [{text: 'No entry delay'}, {text: 'Silent exit'}]}))}),
});

M.screen({
  id: 'alarm-burglary', area: 'Home', title: 'Burglary alarm', shot: 'alarm_burglary',
  notes: 'The screen turns red; the state ring glows slowly (a 1.3 s fade, UI-020), the only thing on the panel that moves.',
  render: () => M.main({tone: 'rose', alarm: true, rail: ARMED_RAIL, bar: {...ARMED_BAR, locked: true}, content: M.home(
    {tone: 'red', glyph: 'siren', headline: 'Burglary alarm', detail: 'Front Door', note: 'Enter your code to disarm', alarm: true},
    M.code_pad(DISARM_PAD))}),
});

M.screen({
  id: 'alarm-fire', area: 'Home', title: 'Fire alarm, with Silence', shot: 'fire_alarm',
  render: () => M.main({tone: 'rose', alarm: true, rail: {...ARMED_RAIL, troubles: 1, zones: 1}, bar: {...ARMED_BAR, time: '10:49', troubles: 1, locked: true}, content: M.home(
    {tone: 'red', glyph: 'flame', headline: 'Fire alarm', detail: 'Kitchen Smoke', note: 'Get everyone out. Silence stops the siren', alarm: true},
    M.code_pad({...DISARM_PAD, silence: true}))}),
});

M.screen({
  id: 'alarm-memory', area: 'Home', title: 'After an alarm: alarm memory', shot: 'alarm_memory',
  notes: '"Alarm canceled" when the disarm stopped the report; "Alarm occurred" otherwise (docs/features/38).',
  render: () => M.main({tone: 'amber', bar: {time: '10:47'}, content: M.home(
    {tone: 'amber', glyph: 'bell-ring', headline: 'Alarm canceled', detail: 'Burglary alarm: Front Door', note: 'No alert was sent. Clear it with your code'},
    M.action_tile({glyph: 'bell-ring', title: 'Clear alarm', subtitle: 'Enter your code to clear it', color: 'amber'}))}),
});

const WALK_BAR = {battery: {glyph: 'battery-medium', text: '62%'}, troubles: 1};
const walkPane = () => `<div class="panel_walk">
  <div class="walk_card">${M.icon_chip({glyph: 'footprints', size: 128, font: 96, color: 'blue'})}
    ${M.text('3 of 5 tested', 'headline')}${M.text('Ends in 60 min', 'body', 'text_2')}</div>
  ${M.row_action({glyph: 'chevron-right', text: 'Show zones', color: 'blue', goTo: 'walk-test-list'})}
  <div class="row" style="gap:var(--space_md)"><div class="grow">${M.row_action({glyph: 'clock', text: 'Extend', color: 'blue'})}</div>
    <div class="grow">${M.row_action({glyph: 'check', text: 'End test', color: 'green', goTo: 'walk-test-end-confirm'})}</div></div></div>`;
const walkHero = {tone: 'blue', glyph: 'footprints', headline: 'Walk test', detail: 'Open and close each zone', note: 'The panel beeps for every zone it sees'};

M.screen({
  id: 'home-walk-test', area: 'Home', title: 'Walk test running', shot: 'home_walk_test',
  notes: 'docs/features/40. Show zones opens without a code; Extend asks for one.',
  render: () => M.main({tone: 'blue', rail: {zones: 1, troubles: 1}, bar: WALK_BAR, content: M.home(walkHero, walkPane())}),
});

M.screen({
  id: 'walk-test-end-confirm', area: 'Home', title: 'Ending the walk test', shot: 'walk_test_end_confirm',
  render: () => M.main({tone: 'blue', bar: {...WALK_BAR, troubles: 0, time: '10:42'}, content: M.home(walkHero, walkPane()),
    over: M.confirm_dialog({title: 'End the walk test?', note: 'The sensors stop beeping and work as usual again', ok: 'End test', cancel: 'Keep testing'})}),
});

// ---- An automatic arming's warning (docs/features/41, BL-170; ui_auto_warn.c): a card above the arming tiles ----
// ---- the warning at the wall ----
// D19: the arming tiles stay exactly as they are (not hidden, dimmed or relabelled); the warning is a
// short card above them in the same pane (kit arm_strip with panel_arm(above)), never in their place.
// Order of a put-off (D26): Put off -> the choice -> the code pad. The card has one button.
const READY = {tone: 'green', glyph: 'shield-check', headline: 'Disarmed', detail: 'Ready to arm', note: 'All zones closed'};
const PUT_OFF = {glyph: 'clock', text: 'Put off', goTo: 'home-arm-put-off-choice'};
const WARN = {title: 'Arming Night at 10:30 PM', detail: 'Set by an automation', action: PUT_OFF};
const home = ({id, title, shot = null, notes, hero = READY, tone = 'green', rail = {}, banner = 'arm', bannerText, card, over, pane, time = '10:26'}) => M.screen({
  id, area: 'Home', title, shot, notes,
  render: () => M.main({tone, rail, bar: {banner, bannerText, time, ampm: 'PM'}, over,
    content: M.home(hero, pane || M.panel_arm(card ? M.arm_strip(card) : ''))}),
});

home({id: 'home-arm-warning', shot: 'home_arm_warning', title: 'Arming soon: the warning above the tiles',
  notes: 'From 10:25 PM: a steady amber banner in the status bar on every screen (a tap opens Home) and, on Home, a short card above the arming tiles (AUT-012) with one button, "Put off" (D26). The tiles stay exactly as they are and work as always (D19): anyone can arm by hand at the level they want, and the automation then finds the partition armed and does nothing (AUT-014). No countdown and nothing moves; the chime sounds once a minute, never over an alarm sound (D22). The warning runs on the panel\'s own timer, so a clock change can\'t shorten it. Every warning has the button, also the one after an hour\'s put-off.',
  card: WARN});

// The choice: a small sheet over Home, two large choices and Cancel. Plain blue, not red: nothing is wrong.
const tile96 = (t) => `<div style="height:96px;display:flex">${M.arm_tile(t)}</div>`;
const choice = (a, b) => `<div class="style_backdrop"><div class="sheet_card centered" style="width:600px">
  <div class="col" style="gap:2px">${M.text('Put off arming?', 'title')}${M.text('Arming Night at 10:30 PM', 'body', 'text_2')}</div>
  ${tile96(a)}${tile96(b)}
  <div class="row" style="justify-content:flex-end">${M.button('Cancel')}</div></div></div>`;
home({id: 'home-arm-put-off-choice', shot: 'home_arm_put_off_choice', title: 'Put off: an hour, or not tonight',
  notes: 'Touching Put off opens this small sheet: the order is Put off, this choice, the code pad (D26). Two large choices say what will happen: "An hour later" (the new time) and "Not tonight" (when it arms again). For a daytime arm time the second reads "Not today" with "Arms again tomorrow at 10:30 AM". Plain blue, not red. Picking one opens the code pad; Cancel closes the sheet and leaves the warning as it was. Not tonight holds until 6:00 AM (daytime: midnight).',
  card: WARN,
  over: choice({glyph: 'clock', title: 'An hour later', subtitle: 'Arms Night at 11:30 PM', color: 'blue', goTo: 'home-arm-put-off'},
    {glyph: 'calendar-x', title: 'Not tonight', subtitle: 'Arms again tomorrow at 10:30 PM', color: 'blue', goTo: 'home-arm-put-off'})});

const NOT_READY = {tone: 'amber', glyph: 'shield-alert', headline: 'Not ready', detail: 'Kitchen Window is open', note: 'Close it, or bypass when arming'};
home({id: 'home-arm-warning-not-ready', shot: 'home_arm_warning_not_ready', title: 'Arming soon, with a window open',
  notes: 'One short line, in amber: the panel is not ready (D28). Which zone is open stays on the state card and the zone screen, not repeated here. Closing the window clears the line. The tiles are unchanged (D19): arming by hand works as it does today, bypass included. If the arming is put off, the hold note (home-arm-put-off-home) takes the card\'s place, so the hold is still seen; the state card keeps saying Not ready.',
  tone: 'amber', rail: {zones: 1}, hero: NOT_READY,
  card: {...WARN, note: 'Not ready: it won\'t arm yet'}});

home({id: 'home-arm-warning-bypass', title: 'Arming soon, with a window open, bypassing',
  notes: 'The same moment for an automation with "Bypass open zones" on (D29): the line says the open zones will be bypassed, not that it won\'t arm. Amber, because arming with a zone left out lowers protection; the zone screen has which. A zone in tamper or trouble is not bypassed and would still stop it. The tiles are unchanged (D19).',
  tone: 'amber', rail: {zones: 1}, hero: NOT_READY,
  card: {...WARN, note: 'Open zones will be bypassed'}});

const PUT_OFF_PAD = {title: 'Put off an hour', tone: 'amber', glyph: 'clock', note: 'Any code puts it off an hour. Arms Night at 11:30 PM.', digits: 2};
home({id: 'home-arm-put-off', shot: 'home_arm_put_off', title: 'Putting it off: the code',
  notes: 'The code pad over the screen (modal_pad), after the choice. Its title and note follow the choice: this one is "An hour later". After "Not tonight" the title reads "Not tonight" and the note "Arms again tomorrow at 10:30 PM. Needs a code that may disarm Home." (PROPOSED, for the maintainers to decide; an arm-only or guest code gets "That code can only put it off an hour" and nothing is put off). Any user\'s code puts off an hour, every time; a wrong one counts toward the lockout like any code. It arms nothing and changes nothing else. A duress code also sends its silent duress report. The put-off is logged. Under the pad the tiles are as always (D19).',
  card: WARN,
  over: M.modal_pad(PUT_OFF_PAD)});

const HOLD = {glyph: 'clock', tone: 'gray'};
home({id: 'home-arm-put-off-home', shot: 'home_arm_put_off_home', title: 'Put off an hour: Home with a quiet note',
  notes: 'Where the warning card was, a quiet steady note in grey (nothing is wrong, nothing to do): what will happen and who held it. The banner goes (D20); the tiles stay exactly as they are (D19) and nothing moves. The note goes when the new warning starts at 11:25 PM (then the card with Put off is back) or when the panel is armed by hand; while armed it never shows. The Panel Manager\'s Overview shows the same note (D27).',
  banner: null,
  card: {...HOLD, title: 'Arms Night at 11:30 PM', detail: 'Put off by Pat'}});

home({id: 'home-arm-skipped-home', title: 'Not tonight: Home with a quiet note',
  notes: 'The same note after "Not tonight": "Won\'t arm tonight", by whom. It goes at 6:00 AM (for a daytime arm time, "Not today" until midnight), or when the panel is armed by hand; the next trigger arms as usual, and the Automations list says "Arms again tomorrow at 10:30 PM". No new warning starts tonight.',
  banner: null,
  card: {...HOLD, title: 'Won\'t arm tonight', detail: 'Put off by Pat'}});

// ---- D30: who did it ----
home({id: 'home-armed-by-automation', title: 'Armed by an automation, one zone bypassed',
  notes: 'Home after the automation armed Night with the Kitchen Window bypassed. The state card\'s foot says "Since 10:30 PM · By automation" and "1 zone bypassed", never "By Alex". Armed Home shows no warning card or hold note. The disarm pad is as always.',
  tone: 'violet', rail: {locked: true}, banner: null, time: '10:31',
  hero: {tone: 'violet', glyph: 'moon', headline: 'Armed Night', detail: 'Doors, windows and night zones', note: 'Since 10:30 PM · By automation · 1 zone bypassed'},
  pane: M.code_pad({title: 'Disarm', tone: 'green', glyph: 'lock-open', close: false})});
