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
