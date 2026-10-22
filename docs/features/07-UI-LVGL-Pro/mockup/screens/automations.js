// Settings > Automations (docs/features/41-Automations, built by BL-168 and BL-170; ui_automations.c,
// ui_auto_edit.c, ui_installer.c): the list, an automation's page, the sheets and confirms the editor
// opens, and the Arm the panel sheet. The page opens with the system master's or the installer's code
// (ACCESS_SYSTEM). Names are generated sentences, never typed. Screens without a picture are built
// and walked in the sheets' tests; the preview's walk (walk_automations) saves the ones with a shot.
// The Home warning, the put-off and "By automation" are screens/home.js and views.js.

const RAIL = {};
const BAR = {banner: null, time: '10:41', ampm: 'PM'};

// A page of Settings > Automations, with something over it or not.
const auto = ({id, title, shot = null, notes, page, over}) => M.screen({
  id, area: 'Settings', title, shot, notes,
  render: () => M.main({view: 'settings', rail: RAIL, bar: BAR, over,
    content: M.settings({section: 'automations', ...page})}),
});

// ---- the list ----
const ADD = (o = {}) => M.row_action({glyph: 'plus', text: 'Add an automation', color: 'blue', goTo: 'automation-add', ...o});
const ROWS = {
  porch: {glyph: 'lightbulb', color: 'blue', title: 'When Front Door opens', detail: 'Turn on Porch Light for 10 minutes', goTo: 'automation'},
  away: {glyph: 'lock', color: 'green', title: 'When the panel is armed Away', detail: 'Lock Front Door · Run Good Night',
    sub: 'Didn\'t work at 8:02 AM', goTo: 'automation-failed'},
  fire: {glyph: 'thermometer', color: 'orange', title: 'When a fire or CO alarm sounds', detail: 'Turn off Hallway'},
  leak: {glyph: 'droplet', color: 'blue', title: 'When Basement Leak detects water', detail: 'Close Water Main'},
  garage: {glyph: 'warehouse', color: 'gray', title: 'At 11:00 PM on weekdays', detail: 'Close Garage Door', value: 'Off'},
};
const LIST = [ROWS.porch, ROWS.away, ROWS.fire, ROWS.leak, ROWS.garage];
const rows = (list) => list.map((r) => M.row_link_alert(r)).join('');

auto({id: 'automations', shot: 'automations', title: 'Automations: the list',
  notes: 'One row per automation, in the order they were made: the trigger sentence as the title, what it does under it, "Off" when switched off, an amber line when its last run didn\'t work. The chip is the first action\'s device (grey when off). No toast after adding one: the row shows it.',
  page: {title: 'Automations', note: '5 of 16 automations', body: ADD() + rows(LIST)}});

auto({id: 'automations-empty', shot: 'automations_empty', title: 'Automations: none yet',
  notes: 'The Add row stays first; one paragraph says what an automation is, with examples any panel can do (BL-224: no device needed). No count in the note while there are none.',
  page: {title: 'Automations', body: [ADD(),
    M.paragraph('The panel can do things by itself, like arming at bedtime or telling your phone when a door opens at night.')].join('')}});

const FULL = [ROWS.porch, ROWS.away, ROWS.fire, ROWS.leak, ROWS.garage,
  {glyph: 'lightbulb', title: 'When the panel is disarmed', detail: 'Turn on Hallway Light'},
  {glyph: 'lightbulb', title: 'When Back Door opens', detail: 'Turn on Hallway Light for 5 minutes'},
  {glyph: 'lightbulb', title: 'When an alarm sounds', detail: 'Turn on Porch Light · Turn on Hallway Light'},
  {glyph: 'lightbulb', title: 'When Hall Motion sees motion', detail: 'Turn on Hallway Light for 5 minutes'},
  {glyph: 'plug', title: 'When the panel is armed Night', detail: 'Turn off Living Room Lamp · Lock Front Door'},
  {glyph: 'plug', title: 'At 5:00 PM every day', detail: 'Turn on Christmas Lights'},
  {glyph: 'plug', title: 'At 11:30 PM every day', detail: 'Turn off Christmas Lights'},
  {glyph: 'lock', color: 'green', title: 'When Front Door closes', detail: 'Lock Front Door'},
  {glyph: 'warehouse', color: 'green', title: 'When the panel is armed Stay', detail: 'Close Garage Door'},
  {glyph: 'thermometer', color: 'orange', title: 'When Kitchen Window opens', detail: 'Turn off Hallway'},
  {glyph: 'lightbulb', title: 'At 6:30 AM on weekdays', detail: 'Turn on Living Room Lamp'}];
auto({id: 'automations-full', shot: 'automations_full', title: 'Automations: 16, the most',
  notes: 'At 16 the Add row is dimmed and the line under it says why and what to do. Sixteen rows scroll; the page shows the first.',
  page: {title: 'Automations', note: '16 of 16 automations', body: [ADD({disabled: true}),
    M.paragraph('16 is the most. Delete one to add another.'), rows(FULL)].join('')}});

auto({id: 'automations-clock', shot: 'automations_clock', title: 'Automations: the clock isn\'t set',
  notes: 'While the clock isn\'t set, time rules and "Only between" windows wait (AUT-005, T1): one amber line above the rows says so, once. When the clock is set nothing missed runs late.',
  page: {title: 'Automations', note: '5 of 16 automations', body: [ADD(),
    M.paragraph('The clock isn\'t set, so times wait until it is.', 'amber'), rows(LIST)].join('')}});

auto({id: 'automations-problems', shot: 'automations_problems', title: 'Automations: rows with a problem',
  notes: 'The other amber lines a row can carry: a device that left the panel, a record saved by newer software (only Delete works), and one that can\'t run until it is opened (a zone retyped, a device that changed kind). Each opens the automation\'s page, which says more.',
  page: {title: 'Automations', note: '4 of 16 automations', body: ADD() + rows([
    {...ROWS.porch, color: 'gray', sub: 'Porch Light isn\'t on the panel now', goTo: 'automation-missing'},
    {glyph: 'zap', color: 'gray', title: 'When the panel is disarmed', detail: 'Something this software can\'t show', sub: 'Made by newer software', goTo: 'automation-newer'},
    {glyph: 'lightbulb', color: 'gray', title: 'When Back Door opens', detail: 'Turn on Hallway Light for 5 minutes', sub: 'Can\'t run: open it to fix it', goTo: 'automation-cant-run'},
    ROWS.leak])}});

auto({id: 'automations-locked', shot: 'automations_locked', title: 'Automations: asks for the code first',
  notes: 'Without the system master\'s or the installer\'s code the page is the locked one (ui_settings_locked with ACCESS_SYSTEM). A partition master, standard, arm-only or guest code doesn\'t open it (docs/features/14 gains the row).',
  page: {title: 'Automations', note: 'Automations are changed with the master or installer code.', body:
    M.row_action({glyph: 'key', text: 'Enter the master or installer code', color: 'violet'})}});

// ---- adding one: the ideas, then the pickers ----
const LIST_PAGE = {title: 'Automations', note: '5 of 16 automations', body: ADD() + rows(LIST)};
// BL-224 (docs/features/41 DESIGN.md section 15): first the ideas that need only the panel, each
// shown when its parts exist, then the device ideas, then "Start from scratch".
const idea = (glyph, color, title, detail, goTo) => M.row_link({glyph, color, title, detail, trail: '', goTo});
const IDEA = {
  bedtime: idea('moon', 'violet', 'Arm at bedtime', 'Every night at a time you choose, after a warning', 'automation-arm'),
  morning: idea('log-out', 'rose', 'Arm Away on weekday mornings', 'After everyone has left, with a warning'),
  remind: idea('bell', 'amber', 'Remind me to arm', 'A chime and a note if it\'s still disarmed at bedtime', 'automation-remind'),
  armed: idea('send', 'blue', 'Tell me if it\'s still armed', 'Like nobody home from school by 4:00 PM', 'automation-still-armed'),
  open: idea('shield-off', 'amber', 'Disarm at opening time', 'For a shop or an office, on weekdays'),
  door: idea('door-open', 'amber', 'Tell me when a door opens at night', 'A notification to your phone, 10:00 PM to 6:00 AM', 'automation-notify'),
  chime: idea('bell-off', 'gray', 'Quiet the door chime at night', 'Off at bedtime; another can turn it on in the morning'),
  output: idea('toggle-right', 'blue', 'Switch an output when an alarm sounds', 'Like the garden lights, for 30 minutes'),
  doorLight: idea('door-open', 'amber', 'Turn on a light when a door opens', 'Like the porch light, in the evening'),
  lock: idea('lock', 'green', 'Lock up when you arm Away', 'Lock a door when everyone has left'),
  alarmLight: idea('siren', 'red', 'Turn on lights when an alarm sounds', 'So you can see, and be seen'),
  leak: idea('droplets', 'blue', 'Close the water when a leak is found', 'Shut the water main'),
  scratch: idea('plus', 'blue', 'Start from scratch', 'Choose when it runs and what it does', 'automation-when'),
};
const EMPTY_PAGE = {title: 'Automations', body: [ADD(),
  M.paragraph('The panel can do things by itself, like arming at bedtime or telling your phone when a door opens at night.')].join('')};
auto({id: 'automation-add', shot: 'automation_ideas', title: 'Adding: ideas to start from, on a new panel',
  notes: 'The master\'s code on a panel with zones but no device and no output yet (BL-224). Every idea here needs only the panel: the arming ones open the time sheet, then the Arm the panel sheet with their level chosen (Night, or Away for the mornings); Remind and "still armed" open the time sheet, then their own sheet; the door idea asks for the door (only doors and windows) and comes with 10:00 PM to 6:00 AM; the chime idea asks only the time (off at 10:00 PM). "Disarm at opening time" shows only while the installer option allows disarming and for a code that may disarm. Once a light, a lock, a valve or an output exists, today\'s device ideas join below these, before "Start from scratch" (automation-add-arm). Nothing is saved until the last pick, and Cancel at any step leaves nothing. The sheet scrolls.',
  page: EMPTY_PAGE,
  over: M.pick_sheet({title: 'Add an automation', fit: true, rows: [IDEA.bedtime, IDEA.morning, IDEA.remind, IDEA.armed,
    IDEA.open, IDEA.door, IDEA.chime, IDEA.scratch].join('')})});

auto({id: 'automation-add-installer', shot: 'automation_ideas_installer', title: 'Adding with the installer code: arming ideas too',
  notes: 'The installer sets up most new panels (the headless panel\'s phone setup signs in as the installer). The installer gets the arming ideas and Arm the panel; each arming automation acts as the system master, checked by the panel at every arming, so it is never more than the master could do (BL-224 N1). No "Disarm at opening time" and no Disarm action: the installer code never disarms (USR-001). A panel with an output for automations adds "Switch an output when an alarm sounds".',
  page: EMPTY_PAGE,
  over: M.pick_sheet({title: 'Add an automation', fit: true, rows: [IDEA.bedtime, IDEA.morning, IDEA.remind, IDEA.armed,
    IDEA.door, IDEA.chime, IDEA.output, IDEA.scratch].join('')})});

const WHEN = [
  {glyph: 'shield', title: 'When the panel is armed', detail: 'Once exit time is over', goTo: 'automation-armed-how'},
  {glyph: 'house', color: 'green', title: 'When the panel is disarmed'},
  {glyph: 'siren', color: 'red', title: 'When an alarm sounds', detail: 'Never for a silent alarm'},
  {glyph: 'flame', color: 'red', title: 'When a fire or CO alarm sounds'},
  {glyph: 'door-open', color: 'amber', title: 'When a door or window opens', detail: 'Or a motion or water sensor', goTo: 'automation-zone'},
  {glyph: 'door-closed', color: 'green', title: 'When a door or window closes', goTo: 'automation-zone-empty'},
  {glyph: 'clock', title: 'At a time of day', detail: 'On the days you pick', goTo: 'automation-time'},
  {glyph: 'triangle-alert', color: 'amber', title: 'When there\'s a trouble', detail: 'Power lost, a low battery, a sensor gone quiet', goTo: 'automation-trouble'},
];
auto({id: 'automation-when', title: 'Adding: when should it run?',
  notes: 'The eight triggers in words a person knows; the sheet scrolls for the last ones. "Armed" means once exit time is over, once per arming. Silent alarms and duress never start anything, so nothing seen on the wall can give a duress code away. "When there\'s a trouble" (BL-224) asks which one next. Changing the trigger of a reminder or "still armed" leaves only "At a time of day"; of a Disarm, never the alarm rows nor a trouble (a power cut never disarms).',
  page: LIST_PAGE,
  over: M.pick_sheet({title: 'When should it run?', note: 'Silent alarms and duress never start an automation.', fit: true,
    rows: WHEN.map((r) => M.row_link({trail: '', ...r})).join('')})});

auto({id: 'automation-trouble', shot: 'automation_trouble_which', title: 'Adding: which trouble?',
  notes: 'BL-224: the few troubles a person acts on, in their words. "Any trouble" is every trouble the panel shows on Troubles. It fires once when the trouble starts, never again while it lasts, and never for the troubles a start-up or a settings change shows for a moment (AUT-003). The sentences: "When there\'s a trouble", "When the power goes off", "When the panel\'s battery is low", "When a sensor stops reporting", "When there\'s a siren problem". It is system-wide: no partition is asked. After it, "What should it do?" has no reminder (a time only) and no Disarm.',
  page: EMPTY_PAGE,
  over: M.pick_sheet({title: 'Which trouble?', fit: true, rows: [
    M.row_link({glyph: 'triangle-alert', color: 'amber', title: 'Any trouble', trail: ''}),
    M.row_link({glyph: 'unplug', color: 'amber', title: 'The power goes off', detail: 'The panel runs on its battery', trail: ''}),
    M.row_link({glyph: 'battery-low', color: 'amber', title: 'The panel\'s battery is low', trail: ''}),
    M.row_link({glyph: 'radio', color: 'amber', title: 'A sensor stops reporting', detail: 'Or its battery is low', trail: ''}),
    M.row_link({glyph: 'siren', color: 'amber', title: 'A siren problem', trail: ''}),
  ].join('')})});

auto({id: 'automation-armed-how', title: 'Adding: armed how?',
  notes: 'After "When the panel is armed": the level, or any of them. The trigger sentence follows ("When the panel is armed Away").',
  page: LIST_PAGE,
  over: M.pick_sheet({title: 'Armed how?', fit: true, rows: [
    M.row_link({glyph: 'log-out', color: 'rose', title: 'Away', trail: ''}),
    M.row_link({glyph: 'house', color: 'blue', title: 'Stay', trail: ''}),
    M.row_link({glyph: 'moon', color: 'violet', title: 'Night', trail: ''}),
    M.row_link({glyph: 'shield', color: 'gray', title: 'Any of them', trail: ''}),
  ].join('')})});

auto({id: 'automation-partition', title: 'Adding: which partition?',
  notes: 'Only on a panel with more than one partition enabled; then the sentences name it ("When Main House is armed Away"). With one partition this sheet never shows.',
  page: LIST_PAGE,
  over: M.pick_sheet({title: 'Which partition?', fit: true, rows: [
    M.row_link({glyph: 'house', title: 'Main House', trail: ''}),
    M.row_link({glyph: 'house', title: 'Studio', trail: ''}),
    M.row_link({glyph: 'shield', color: 'gray', title: 'Any of them', trail: ''}),
  ].join('')})});

auto({id: 'automation-zone', title: 'Adding: which sensor?',
  notes: 'For "opens": doors, windows, motion and water sensors, each with its zone number. Silent, fire, CO and button zones are never listed (the type allowlist); the note says so. For "closes" the title is "Which door or window?" and only doors, windows and garage doors are listed.',
  page: LIST_PAGE,
  over: M.pick_sheet({title: 'Which sensor?', note: 'Silent, fire and button zones aren\'t listed.', fit: true, rows: [
    M.row_link({glyph: 'door-closed', title: 'Front Door', detail: 'Zone 1', trail: ''}),
    M.row_link({glyph: 'door-closed', title: 'Back Door', detail: 'Zone 2', trail: ''}),
    M.row_link({glyph: 'app-window', title: 'Kitchen Window', detail: 'Zone 3', trail: ''}),
    M.row_link({glyph: 'footprints', title: 'Hall Motion', detail: 'Zone 4', trail: ''}),
    M.row_link({glyph: 'droplets', title: 'Basement Leak', detail: 'Zone 5', trail: ''}),
  ].join('')})});

auto({id: 'automation-zone-empty', title: 'Adding: no door or window to choose',
  notes: 'A house with no usable door or window zone: one dimmed row says so instead of an empty sheet.',
  page: LIST_PAGE,
  over: M.pick_sheet({title: 'Which door or window?', note: 'Silent, fire and button zones aren\'t listed.', fit: true,
    rows: M.row_link({glyph: 'door-closed', color: 'gray', title: 'No doors or windows to choose', trail: '', disabled: true})})});

// time_sheet: one time and the days, in the region's week order (Sunday first in the United States)
const WEEK = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];
const days = (on) => WEEK.map((t, i) => ({text: t, on: on.includes(i)}));
const RUN_AT = {hour: ['9', '10', '11', '12', '1'], minute: ['50', '55', '00', '05', '10'], ampm: ['', 'AM', 'PM', '', '']};
auto({id: 'automation-time', title: 'Adding: a time of day',
  notes: 'One Hour, Minute (5-minute steps) and AM/PM set beside seven 64 px day toggles (the chips are 40 px, under the touch target). The line under them is the whole sentence the list will show. It runs once a day at most, and not while the clock isn\'t set.',
  page: LIST_PAGE,
  over: M.time_sheet({title: 'Run at', note: 'It runs once a day at most, and waits while the clock isn\'t set.',
    rollers: RUN_AT, days: days([1, 2, 3, 4, 5]), result: {value: 'Runs on weekdays at 11:00 PM'}})});

auto({id: 'automation-time-no-day', title: 'Adding: a time with no day picked',
  notes: 'With every day off, the line asks for one and OK is dimmed. Every day on reads "Runs every day at 11:00 PM"; Saturday and Sunday "Runs on weekends at 9:00 AM"; others "Runs on Mon, Wed and Fri at 7:30 AM".',
  page: LIST_PAGE,
  over: M.time_sheet({title: 'Run at', note: 'It runs once a day at most, and waits while the clock isn\'t set.',
    rollers: RUN_AT, days: days([]), result: {value: 'Pick at least one day', tone: 'amber'}, okDisabled: true})});

const DEVICES = [
  {glyph: 'thermometer', color: 'orange', title: 'Hallway', detail: 'Downstairs'},
  {glyph: 'lightbulb', color: 'blue', title: 'Hallway Light', detail: 'Downstairs'},
  {glyph: 'lock', color: 'green', title: 'Front Door Lock', detail: 'Entry', goTo: 'automation-device-do-lock'},
  {glyph: 'warehouse', color: 'green', title: 'Garage Door', detail: 'Garage', goTo: 'automation-device-do-garage'},
  {glyph: 'plug', color: 'blue', title: 'Living Room Lamp', detail: 'Living Room'},
  {glyph: 'sparkles', color: 'violet', title: 'Good Night', detail: 'Living Room'},
  {glyph: 'plug', color: 'gray', title: 'Christmas Lights', detail: 'Not reachable now'},
  {glyph: 'lightbulb', color: 'blue', title: 'Porch Light', detail: 'Outside'},
  {glyph: 'droplet', color: 'blue', title: 'Water Main', detail: 'Utility'},
];
const OFF_NOTE = 'Your installer has turned off automations that unlock, open or disarm.';
// A second action (after a device, an output, the chime or a notification): the chime and a
// notification too, each once (BL-224), then the devices.
const SECOND_PANEL = [M.section('The alarm'),
  M.row_link({glyph: 'bell-ring', title: 'Door chime', detail: 'Turn it on or off', trail: '', goTo: 'automation-chime'}),
  M.section('Tell someone'),
  M.row_link({glyph: 'send', title: 'Send a notification', detail: 'To the phones set up in Integrations', trail: '', goTo: 'automation-notify'}),
  M.section('Devices')].join('');
auto({id: 'automation-then', title: 'Adding: what should it do? (a second action)',
  notes: 'The devices by area, as "Add to Favorites", each with what it is or "Not reachable now" (still pickable). Only devices with an allowed action are listed, and never a script (AUT-002). A lock may lock or unlock, a garage door or cover close, open or stop, a valve close or open (D23); the next sheet offers them. A scene goes straight to its confirm. Drawn here as a second action: the door chime and a notification come first (BL-224, each once in an automation), then the outputs for automations, then the devices; Arm, Disarm, a reminder and "still armed" are a first action only (automation-then-arm, automation-then-time).',
  page: LIST_PAGE,
  over: M.pick_sheet({title: 'What should it do?', fit: true, rows: SECOND_PANEL + DEVICES.map((r) => M.row_link({trail: '', ...r})).join('')})});

auto({id: 'automation-then-option-off', title: 'Adding: what should it do, with the installer option off',
  notes: 'While the installer has turned "Allow less secure automations" off (AUT-015), this note is the only change here: the devices are the same, but the next sheet has no Unlock, Open or Stop, so a lock can only lock and a garage door or valve only close (one action: that sheet is skipped). It never says this while the option is on.',
  page: LIST_PAGE,
  over: M.pick_sheet({title: 'What should it do?', note: OFF_NOTE, fit: true, rows: SECOND_PANEL + DEVICES.map((r) => M.row_link({trail: '', ...r, goTo: ''})).join('')})});

// BL-224 N3: "What should it do?" always has something - The alarm, Tell someone, Outputs, then
// Devices; with no device a quiet note in place of the dead row.
const pick = (glyph, color, title, detail, goTo) => M.row_link({glyph, color, title, detail, trail: '', goTo});
const P = {
  arm: pick('shield', 'blue', 'Arm the panel', 'After a warning', 'automation-arm'),
  disarm: pick('shield-off', 'amber', 'Disarm the panel', 'Right away', 'automation-disarm-partition'),
  remind: pick('bell', 'amber', 'Remind to arm', 'If it\'s still disarmed then', 'automation-remind'),
  armed: pick('send', 'blue', 'Tell me if it\'s still armed', 'If nobody has disarmed it by then', 'automation-still-armed'),
  chime: pick('bell-ring', 'blue', 'Door chime', 'Turn it on or off', 'automation-chime'),
  notify: pick('send', 'blue', 'Send a notification', 'To the phones set up in Integrations', 'automation-notify'),
  garden: pick('toggle-right', 'blue', 'Garden Lights', 'Output on GPIO 47', 'automation-output-do'),
};
const NO_DEV = M.paragraph('Lights, locks and other devices show here once one is connected.');
auto({id: 'automation-then-time', shot: 'automation_then_time', title: 'Adding: what should it do, at a time, with no device',
  notes: 'The master\'s code, after "At 11:00 PM every day". The alarm first: Arm (the only action, after a warning), Disarm (master only, with the installer option on), Remind to arm and Tell me if it\'s still armed (after a time only: they check the state at that moment), Door chime. Then Tell someone, then Outputs (only the wired outputs set to "For automations"; none, no heading), then Devices (none here: the quiet note at the end, where "No devices to choose" used to be). With the installer\'s code there is no Disarm row. With two partitions, each alarm action asks which partition next. The sheet scrolls.',
  page: EMPTY_PAGE,
  over: M.pick_sheet({title: 'What should it do?', fit: true, rows: [M.section('The alarm'), P.arm, P.disarm, P.remind, P.armed, P.chime,
    M.section('Tell someone'), P.notify, M.section('Outputs'), P.garden, NO_DEV].join('')})});

auto({id: 'automation-then-door', shot: 'automation_then_door', title: 'Adding: what should it do, after a door opens, with no device',
  notes: 'After "When Front Door opens": no Remind or "still armed" (they belong to a time). Disarm still asks its plain confirm (AUT-016). Send a notification may be a second action beside an output or the chime; Arm stays the only action. No output here yet, so no Outputs heading.',
  page: EMPTY_PAGE,
  over: M.pick_sheet({title: 'What should it do?', fit: true, rows: [M.section('The alarm'), P.arm, P.disarm, P.chime,
    M.section('Tell someone'), P.notify, NO_DEV].join('')})});

auto({id: 'automation-then-empty', title: 'Adding: nothing at all to choose',
  notes: 'Only for an idea that names a device kind whose devices have all gone meanwhile: one dimmed row says where they come from. "What should it do?" from scratch always has The alarm and Tell someone (BL-224).',
  page: LIST_PAGE,
  over: M.pick_sheet({title: 'What should it do?', fit: true,
    rows: M.row_link({glyph: 'plug', color: 'gray', title: 'No devices to choose', detail: 'Devices appear here once one is connected.', trail: '', disabled: true})})});

// ---- an automation's page ----
const USE = (detail, o = {}) => M.row_toggle({glyph: 'zap', title: 'Use this automation', detail, on: true, ...o});
const WHEN_DOOR = M.row_link({glyph: 'door-open', color: 'amber', title: 'When Front Door opens', detail: 'Zone 1', goTo: 'automation-when'});
const BETWEEN = (value) => M.row_link({glyph: 'clock', title: 'Only between', value, goTo: 'automation-window'});
const PORCH_ON = M.row_link({glyph: 'lightbulb', title: 'Turn on Porch Light', detail: 'Outside', goTo: 'automation-device-do'});
const OFF_AFTER = (value) => M.row_link({glyph: 'timer', title: 'Turn it off after', value, goTo: 'automation-off-after'});
const ADD_SECOND = M.row_action({glyph: 'plus', text: 'Add a second action', color: 'blue', goTo: 'automation-then'});
const TRY = M.row_action({glyph: 'play', text: 'Try it now', color: 'blue'});
const DELETE = M.section('') + M.row_action({glyph: 'trash-2', text: 'Delete this automation', color: 'red', goTo: 'automation-delete'});
const pageOf = (body) => ({title: 'Automation', back: 'automations', body: body.join('')});
const PORCH_PAGE = (use = USE('On · ran at 7:42 PM')) => pageOf([use, M.section('When'), WHEN_DOOR, BETWEEN('6:00 PM to 6:00 AM'),
  M.section('Then'), PORCH_ON, OFF_AFTER('10 minutes'), ADD_SECOND, TRY, DELETE]);

auto({id: 'automation', shot: 'automation', title: 'One automation',
  notes: 'One switch on top carries the state in its detail (rule 2), updated at most once a second (UI-023): "On · ran at 7:42 PM", "On · ran on Sep 19", "On · hasn\'t run since the panel started", "On · waits until the clock is set", "On · tried at 9:44 PM", "Off · it won\'t run". When and Then reopen their sheets; every change is kept at once. "Try it now" runs the actions and says how it went in the switch\'s detail, no toast. Delete is last, apart.',
  page: PORCH_PAGE()});

const AWAY_BODY = (use) => [use, M.section('When'),
  M.row_link({glyph: 'shield', title: 'When the panel is armed Away', detail: 'Once exit time is over', goTo: 'automation-when'}),
  BETWEEN('Any time'), M.section('Then'),
  M.row_link({glyph: 'lock', color: 'green', title: 'Lock Front Door', detail: 'Entry'}),
  M.row_link({glyph: 'sparkles', color: 'violet', title: 'Run Good Night', detail: 'Living Room'}),
  TRY, DELETE];
auto({id: 'automation-two', shot: 'automation_two', title: 'An automation with two actions',
  notes: 'Two actions, run one after the other and independently: a lock and a scene. "Add a second action" is gone at two. No "Turn it off after": that row shows only under a Turn on.',
  page: pageOf(AWAY_BODY(USE('On · ran on Sep 19')))});

auto({id: 'automation-failed', shot: 'automation_failed', title: 'An automation whose last run didn\'t work',
  notes: 'The switch\'s detail turns amber and says when and why, in one sentence: "{0} didn\'t answer", "{0} wasn\'t reachable", "{0} said no", "{0} was busy", "Too many ran at once". A command once sent is never sent again (AUT-006); the next trigger tries afresh.',
  page: pageOf(AWAY_BODY(USE('On · didn\'t work at 8:02 AM: Front Door didn\'t answer', {detailColor: 'amber'})))});

auto({id: 'automation-missing', shot: 'automation_missing', title: 'An automation whose device is gone',
  notes: 'The device left the panel (removed, or gone from the integration for more than 30 s). The action row keeps the name it had and says what to do; a tap opens the device picker. The rest still works.',
  page: pageOf([USE('On · didn\'t work at 9:44 PM: Porch Light isn\'t on the panel now', {detailColor: 'amber'}), M.section('When'), WHEN_DOOR,
    BETWEEN('6:00 PM to 6:00 AM'), M.section('Then'),
    M.row_link_alert({glyph: 'lightbulb', color: 'gray', title: 'Turn on Porch Light', detail: 'Missing: it was called Porch Light · pick another device',
      detailColor: 'amber', goTo: 'automation-then'}),
    OFF_AFTER('10 minutes'), ADD_SECOND, TRY, DELETE])});

auto({id: 'automation-newer', shot: 'automation_newer', title: 'An automation made by newer software',
  notes: 'A record this software can\'t fully read (saved by newer software, then rolled back): kept and shown, never run. The switch and rows are dimmed; only Delete works.',
  page: pageOf([USE('Can\'t run: made by newer software', {disabled: true, on: false}), M.section('When'),
    M.row_link({glyph: 'house', color: 'gray', title: 'When the panel is disarmed', trail: '', disabled: true}),
    M.section('Then'),
    M.row_link({glyph: 'zap', color: 'gray', title: 'Something this software can\'t show', trail: '', disabled: true}),
    DELETE])});

auto({id: 'automation-cant-run', shot: 'automation_cant_run', title: 'An automation that can\'t run until it is opened',
  notes: 'The zone was retyped (Back Door is now a fire or button zone) or a device changed kind: the runner refuses it every time and the detail says why. The other "Can\'t run" detail: "Can\'t run: open it and pick the device again". Picking again fixes it.',
  page: pageOf([USE('Can\'t run: Back Door can\'t start an automation now', {detailColor: 'amber'}), M.section('When'),
    M.row_link({glyph: 'door-open', color: 'amber', title: 'When Back Door opens', detail: 'Zone 2', goTo: 'automation-zone'}),
    BETWEEN('Any time'), M.section('Then'),
    M.row_link({glyph: 'lightbulb', title: 'Turn on Hallway Light', detail: 'Downstairs'}), OFF_AFTER('5 minutes'), ADD_SECOND, TRY, DELETE])});

// ---- the sheets an automation's page opens ----
const ALARM_PAGE = pageOf([USE('On · hasn\'t run since the panel started'), M.section('When'),
  M.row_link({glyph: 'siren', color: 'red', title: 'When an alarm sounds', detail: 'Never for a silent alarm'}),
  BETWEEN('Any time'), M.section('Then'),
  M.row_link({glyph: 'lightbulb', title: 'Turn on Porch Light', detail: 'Outside'}), OFF_AFTER('Leave it on'),
  M.row_link({glyph: 'lightbulb', title: 'Turn on Hallway Light', detail: 'Downstairs'}), OFF_AFTER('Leave it on'), TRY, DELETE]);
auto({id: 'automation-device-do', title: 'What should the device do?',
  notes: 'Only the allowed actions (AUT-002): Turn on, Turn off and now Toggle for a light or a switch (D23; for a switch, Turn on and Toggle ask once, D9); Turn on and Turn off for a fan; Turn off for a thermostat. A lock, a garage door or cover and a valve now have this sheet too (the next screens); a scene still skips it. When editing, "Choose another device", and "Remove this action" (red) while there are two.',
  page: ALARM_PAGE,
  over: M.pick_sheet({title: 'What should Hallway Light do?', fit: true, rows: [
    M.row_link({glyph: 'power', color: 'green', title: 'Turn on', trail: 'check', trailColor: 'blue'}),
    M.row_link({glyph: 'power', color: 'gray', title: 'Turn off', trail: ''}),
    M.row_link({glyph: 'refresh-cw', title: 'Toggle', detail: 'On if it is off, off if it is on', trail: ''}),
    M.row_link({glyph: 'arrow-left-right', title: 'Choose another device', trail: ''}),
    M.row_link({glyph: 'trash-2', color: 'red', title: 'Remove this action', trail: ''}),
  ].join('')})});

// ---- D23-D25: unlock, open and stop; the confirm; the installer option ----
const LOCK_PAGE = (use, {when = WHEN_DOOR, between = true, then = M.row_link({glyph: 'lock-open', color: 'amber', title: 'Unlock Front Door Lock',
  detail: 'Entry', goTo: 'automation-device-do-lock'})} = {}) => pageOf([use, M.section('When'), when,
  between ? BETWEEN('Any time') : '', M.section('Then'), then, ADD_SECOND, TRY, DELETE]);
auto({id: 'automation-device-do-lock', title: 'What should a lock do?',
  notes: 'A lock now has this sheet: Lock or Unlock (D23, AUT-002). Unlock is a less secure action: it is offered only while the installer allows less secure automations (AUT-015), and after a door, window, motion or alarm trigger picking it asks the confirm (AUT-016). A valve has the same two rows as Close and Open. No "Turn it off after" for any of them.',
  page: LIST_PAGE,
  over: M.pick_sheet({title: 'What should Front Door Lock do?', fit: true, rows: [
    M.row_link({glyph: 'lock', color: 'green', title: 'Lock', trail: ''}),
    M.row_link({glyph: 'lock-open', color: 'amber', title: 'Unlock', trail: ''}),
  ].join('')})});

auto({id: 'automation-device-do-garage', title: 'What should a garage door do?',
  notes: 'A garage door or a cover: Close, Open or Stop (D23, AUT-002). Open and Stop are less secure (Stop can leave it part-open): offered only while the installer allows them (AUT-015), and confirmed after a door, window, motion or alarm trigger (AUT-016). No position: "Open to 50 %" is for later.',
  page: LIST_PAGE,
  over: M.pick_sheet({title: 'What should Garage Door do?', fit: true, rows: [
    M.row_link({glyph: 'arrow-down', color: 'green', title: 'Close', trail: ''}),
    M.row_link({glyph: 'arrow-up', color: 'amber', title: 'Open', trail: ''}),
    M.row_link({glyph: 'square', color: 'amber', title: 'Stop', trail: ''}),
  ].join('')})});

auto({id: 'automation-confirm-unlock', title: 'Unlocking after a door opens asks first',
  notes: 'After "When Front Door opens", picking Unlock asks this before it is kept (AUT-016): plain (blue, not red), one sentence a homeowner reads once, saying what will happen. Cancel leaves the action as it was. It is asked again whenever the trigger or that action changes (another door, another lock, Open instead of Unlock), and the panel refuses to save it without it, from the touchscreen and the browser alike. A time of day, armed or disarmed trigger never asks: those are a schedule or follow a code. The same confirm says "will open Garage Door" or "will stop Garage Door" for Open and Stop.',
  page: LOCK_PAGE(USE('On · hasn\'t run since the panel started')),
  over: M.confirm_dialog({title: 'Unlock Front Door Lock when Front Door opens?', tone: 'primary', ok: 'Use it', cancel: 'Cancel',
    note: 'Anyone who opens Front Door will unlock Front Door Lock, even when the alarm is on.'})});

const WHEN_DISARMED = M.row_link({glyph: 'house', color: 'green', title: 'When the panel is disarmed', goTo: 'automation-when'});
const CANT_RUN = 'Can\'t run: your installer turned unlocking off';
auto({id: 'automations-option-off', title: 'Automations: an unlock the installer has turned off',
  notes: 'The installer turned "Allow less secure automations" off. A stored unlock, open, stop or disarm is kept exactly as it was (never marked broken) but can\'t run and is never sent (AUT-015); its row says so in amber. Turned back on, it runs again with nothing to redo.',
  page: {title: 'Automations', note: '4 of 16 automations', body: ADD() + rows([ROWS.porch,
    {glyph: 'lock-open', color: 'gray', title: 'When the panel is disarmed', detail: 'Unlock Front Door Lock', sub: CANT_RUN, goTo: 'automation-option-off'},
    ROWS.fire, ROWS.leak])}});

auto({id: 'automation-option-off', title: 'An automation the installer option stops',
  notes: 'Its page says why, and the action row what to do: ask the installer, who alone changes the option (Installer > Delays and arming). The switch keeps its place and the rows can still be edited, but Unlock can\'t be picked again while the option is off ("Try it now" refuses too). Nothing is sent until the option is back on (AUT-015). The same words say "opening" or "disarming" for an open, a stop or a disarm.',
  page: LOCK_PAGE(USE(CANT_RUN, {detailColor: 'amber'}), {when: WHEN_DISARMED,
    then: M.row_link_alert({glyph: 'lock-open', color: 'gray', title: 'Unlock Front Door Lock', detail: 'Ask your installer to allow less secure automations',
      detailColor: 'amber', goTo: 'automation-device-do-lock'})})});


auto({id: 'automation-confirm', title: 'Picking a scene asks once',
  notes: 'The panel can\'t see inside a scene, so picking one asks this once and keeps the answer with the action (D8). Blue, not red: it destroys nothing. Cancel leaves the scene out. Only the note changed: since D23 it says what a scene that unlocks will do, as the design does, instead of "Use one only if it never unlocks or opens anything". The installer option can\'t reach inside a scene.',
  page: pageOf(AWAY_BODY(USE('On · ran on Sep 19'))),
  over: M.confirm_dialog({title: 'Use Good Night in an automation?', tone: 'primary', ok: 'Use it', cancel: 'Cancel',
    note: 'The panel can\'t see what a scene does. If it unlocks or opens anything, it will do so every time this runs.'})});

auto({id: 'automation-confirm-switch', title: 'Turning on a switch asks once',
  notes: 'A switch could front a gate or a garage opener, so "Turn on" or "Toggle" for a switch asks this once (D9, D23). "Turn off" never asks. Scripts are never offered at all.',
  page: LIST_PAGE,
  over: M.confirm_dialog({title: 'Use Christmas Lights in an automation?', tone: 'primary', ok: 'Use it', cancel: 'Cancel',
    note: 'Use it only for a light or a plug, never for a switch that opens a door, a gate or a garage.'})});

auto({id: 'automation-window', title: 'Only between',
  notes: 'From and To are the night sheet\'s rollers (5-minute steps); it may pass midnight. The line says the window as the row will. "Any time" clears it. From and To the same is refused (OK dimmed). Not offered for a time-of-day trigger.',
  page: PORCH_PAGE(),
  over: M.sheet({title: 'Only between', width: 760, body: `
    <div class="row" style="justify-content:space-around;padding-top:var(--space_sm)">
      <div class="col" style="gap:var(--space_sm);align-items:center">${M.text('From', 'label')}${M.time_rollers({hour: ['4', '5', '6', '7', '8'], minute: ['50', '55', '00', '05', '10'], ampm: ['', 'AM', 'PM', '', '']})}</div>
      <div class="col" style="gap:var(--space_sm);align-items:center">${M.text('To', 'label')}${M.time_rollers({hour: ['4', '5', '6', '7', '8'], minute: ['50', '55', '00', '05', '10'], ampm: ['', '', 'AM', 'PM', '']})}</div>
    </div><div class="result_line">${M.text('From 6:00 PM to 6:00 AM', 'label')}${M.button('Any time')}</div>`})});

auto({id: 'automation-off-after', title: 'Turn it off after',
  notes: 'Under a Turn on action only. The timer restarts when the trigger comes again; turning the automation off lets it finish; a restart of the panel loses it and the light stays on.',
  page: PORCH_PAGE(),
  over: M.pick_sheet({title: 'Turn it off after', fit: true, rows: ['Leave it on', '5 minutes', '10 minutes', '15 minutes', '30 minutes', '1 hour']
    .map((t) => M.row_link({title: t, trail: t === '10 minutes' ? 'check' : '', trailColor: 'blue'})).join('')})});

auto({id: 'automation-delete', title: 'Deleting an automation',
  notes: 'The red confirm. After Delete the list shows without it: no toast. A running "Turn it off after" timer is cancelled.',
  page: PORCH_PAGE(),
  over: M.confirm_dialog({title: 'Delete this automation?', note: 'It stops at once. This can\'t be undone.', ok: 'Delete'})});


// ---- Arm the panel and Disarm the panel (BL-170) ----
{
// ---- the list, and adding one ----
const ARM_ROW = {glyph: 'moon', color: 'violet', title: 'At 10:30 PM every day', detail: 'Arm Night', goTo: 'automation-arm-page'};
const ARM_LIST = [
  {glyph: 'lightbulb', title: 'When Front Door opens', detail: 'Turn on Porch Light for 10 minutes'},
  ARM_ROW,
  {glyph: 'droplet', title: 'When Basement Leak detects water', detail: 'Close Water Main'},
];
const ARM_LIST_PAGE = {title: 'Automations', note: '3 of 16 automations', body: [ADD(),
  M.row_link_alert(ARM_LIST[0]), M.row_link_alert(ARM_LIST[1]), M.row_link_alert(ARM_LIST[2])].join('')};

auto({id: 'automations-arm', title: 'Automations: one that arms the panel',
  notes: 'An arming automation is a row like the others: the time as the title, "Arm Night" under it, a moon (a shield for Away and Stay) in the level\'s tone.',
  page: ARM_LIST_PAGE});

auto({id: 'automation-add-arm', title: 'Adding: the ideas in a house with devices',
  notes: 'Harbor House, with its lights, locks and valve: the ideas that need only the panel first (BL-224), then today\'s device ideas, each shown when its parts exist, then "Start from scratch". "Arm at bedtime" asks for the time and days (the time sheet, 10:30 PM every day), then opens the Arm the panel sheet with Night chosen. The sheet scrolls.',
  page: ARM_LIST_PAGE,
  over: M.pick_sheet({title: 'Add an automation', fit: true, rows: [IDEA.bedtime, IDEA.morning, IDEA.remind, IDEA.armed,
    IDEA.open, IDEA.door, IDEA.chime, IDEA.doorLight, IDEA.lock, IDEA.alarmLight, IDEA.leak, IDEA.scratch].join('')})});

const ARM_PICK = M.row_link({glyph: 'shield', title: 'Arm the panel', detail: 'After a warning', trail: '', goTo: 'automation-arm'});
const DISARM_PICK = M.row_link({glyph: 'shield-off', color: 'amber', title: 'Disarm the panel', detail: 'Right away', trail: '', goTo: 'automation-disarm-partition'});
const CHIME_PICK = M.row_link({glyph: 'bell-ring', title: 'Door chime', detail: 'Turn it on or off', trail: '', goTo: 'automation-chime'});
const TELL = M.section('Tell someone') +
  M.row_link({glyph: 'send', title: 'Send a notification', detail: 'To the phones set up in Integrations', trail: '', goTo: 'automation-notify'});
const THEN_DEVICES = [
  M.section('Devices'),
  M.row_link({glyph: 'thermometer', color: 'orange', title: 'Hallway', detail: 'Downstairs', trail: ''}),
  M.row_link({glyph: 'lightbulb', title: 'Hallway Light', detail: 'Downstairs', trail: ''}),
  M.row_link({glyph: 'lock', color: 'green', title: 'Front Door Lock', detail: 'Entry', trail: ''}),
  M.row_link({glyph: 'warehouse', color: 'green', title: 'Garage Door', detail: 'Garage', trail: ''}),
].join('');
auto({id: 'automation-then-arm', shot: 'automation_then_disarm', title: 'Adding: what should it do, after a door opens',
  notes: 'After any trigger (D23; first only a time of day) "The alarm" comes first: here after "When Front Door opens". "Arm the panel" makes it the only action and always warns first; after a trigger other than a time the sheet\'s line reads "Warns for 5 minutes, then arms Night." "Disarm the panel" acts at once as the person saving it and may have one device action with it ("Disarm Home, then Unlock Front Door Lock"). An automation\'s own arm or disarm never starts another arm or disarm (AUT-007). The old note ("It never unlocks, opens or disarms") is gone.',
  page: ARM_LIST_PAGE,
  over: M.pick_sheet({title: 'What should it do?', fit: true, rows: [
    M.section('The alarm'), ARM_PICK, DISARM_PICK, CHIME_PICK, TELL, THEN_DEVICES,
  ].join('')})});

auto({id: 'automation-then-arm-alarm', title: 'Adding: what should it do, after an alarm',
  notes: 'After "When an alarm sounds" or "When a fire or CO alarm sounds", "The alarm" offers Arm but never Disarm: an automation never silences an alarm (D22, AUT-011). The engine also refuses an automation\'s disarm while that partition\'s alarm sounds, whatever started it.',
  page: ARM_LIST_PAGE,
  over: M.pick_sheet({title: 'What should it do?', fit: true, rows: [
    M.section('The alarm'), ARM_PICK, CHIME_PICK, TELL, THEN_DEVICES,
  ].join('')})});

auto({id: 'automation-then-arm-option-off', title: 'Adding: what should it do, with the installer option off',
  notes: 'While the installer has turned "Allow less secure automations" off, there is no Disarm row and the note says why (AUT-015). Arm stays: arming is the master\'s to decide (D4). A stored disarm shows "Can\'t run: your installer turned disarming off" and the engine refuses it.',
  page: ARM_LIST_PAGE,
  over: M.pick_sheet({title: 'What should it do?', note: OFF_NOTE, fit: true, rows: [
    M.section('The alarm'), ARM_PICK, CHIME_PICK, TELL, THEN_DEVICES,
  ].join('')})});

// ---- the Arm the panel sheet ----
// The paragraph no longer says it "arms only if every door and window is closed": with the Bypass
// switch on it can arm over an open one. The switch's own line says which it is (D29).
const ARM_NOTE = 'Fire, CO and 24-hour zones are never bypassed. It does nothing if the panel is already armed. Anyone can put it off with their code.';
const result = (t) => `<div class="result_line">${M.text(t, 'label')}</div>`;
const BYPASS_OFF = 'Off: it won\'t arm while a door or window is open';
const BYPASS_ON = 'Arms anyway; each one is watched again once it closes';
const bypass = (on) => M.row_toggle({glyph: 'circle-slash', color: 'amber', title: 'Bypass open zones', detail: on ? BYPASS_ON : BYPASS_OFF, on});
auto({id: 'automation-arm', shot: 'automation_arm', title: 'Arm the panel: Night',
  notes: 'The level as three 64 px keys (the arming tiles\' glyphs and tones), how long it warns first (1 to 15 minutes), "Bypass open zones" (off by default: the panel then won\'t arm while a door or window is open), and the sentence of what will happen. The paragraph says fire, CO and 24-hour zones are never bypassed (they never block arming and are always on), and that it does nothing over an armed panel. With one partition there is no partition row.',
  page: ARM_LIST_PAGE,
  over: M.sheet({title: 'Arm the panel', width: 760, body: [
    M.arm_levels({active: 2}),
    M.row_number({title: 'Warn first', detail: '1 to 15 minutes', value: '5 minutes'}),
    bypass(false),
    result('Warns at 10:25 PM, arms Night at 10:30 PM.'),
    M.paragraph(ARM_NOTE),
  ].join('')})});

auto({id: 'automation-arm-bypass', shot: 'automation_arm_bypass', title: 'Arm the panel: Night, bypassing open zones',
  notes: 'With the switch on the result line says it bypasses open zones. It bypasses open burglary zones only: a sensor in tamper, loop or missing-sensor trouble still stops the arming, and fire, CO and 24-hour zones are never touched. Each bypass is logged "By automation" and reported; a bypassed zone is watched again once it has been closed for 10 seconds (the Kitchen Window left open all night is the trap this avoids), and every bypass ends at the next disarm. The automation\'s page then reads "Arm Night · bypasses open zones".',
  page: ARM_LIST_PAGE,
  over: M.sheet({title: 'Arm the panel', width: 760, body: [
    M.arm_levels({active: 2}),
    M.row_number({title: 'Warn first', detail: '1 to 15 minutes', value: '5 minutes'}),
    bypass(true),
    result('Warns at 10:25 PM, arms Night at 10:30 PM, bypassing open zones.'),
    M.paragraph(ARM_NOTE),
  ].join('')})});

auto({id: 'automation-arm-away', shot: 'automation_arm_away', title: 'Arm the panel: Away, with two partitions',
  notes: 'Away adds an amber line: inside sensors will be on, so anyone still home sets them off. With more than one partition, the partition row (a pick sheet) and the sentence names it. The Bypass switch is the same row as for Night.',
  page: ARM_LIST_PAGE,
  over: M.sheet({title: 'Arm the panel', width: 760, body: [
    M.arm_levels({active: 0}),
    M.paragraph('Inside sensors will be on.', 'amber'),
    M.row_link({glyph: 'house', title: 'Partition', value: 'Main House'}),
    M.row_number({title: 'Warn first', detail: '1 to 15 minutes', value: '5 minutes'}),
    bypass(false),
    result('Warns at 10:25 PM, arms Main House Away at 10:30 PM.'),
    M.paragraph(ARM_NOTE),
  ].join('')})});

// ---- the automation's page, and what its switch can say ----
// D30: "Arms as Alex" read as if Alex did it. The page says who set it up, not who acts: the engine
// runs it with that person's permission, re-checked each time it runs, and the log says "By automation".
const armPage = (use, then = 'Set up by Alex · warns 5 minutes before') => ({title: 'Automation', back: 'automations-arm', body: [use,
  M.section('When'),
  M.row_link({glyph: 'clock', title: 'At 10:30 PM every day', goTo: 'automation-time'}),
  M.section('Then'),
  M.row_link({glyph: 'moon', color: 'violet', title: 'Arm Night', detail: then, goTo: 'automation-arm'}),
  M.section(''),
  M.row_action({glyph: 'trash-2', text: 'Delete this automation', color: 'red', goTo: 'automation-delete'}),
].join('')});

auto({id: 'automation-arm-page', shot: 'automation_arm_page', title: 'An automation that arms the panel',
  notes: '"Set up by Alex" says who saved it, never that Alex arms (D30): the engine runs it with that person\'s permission, checked again at every arm, and the log says "By automation". With Bypass open zones on the line ends "· bypasses open zones". No "Only between" (it is a time already), no second action and no "Try it now": there is no arming without the warning.',
  page: armPage(USE('On · ran at 10:30 PM'))});

auto({id: 'automation-arm-page-bypass', shot: 'automation_arm_page_bypass', title: 'An automation that arms, bypassing open zones',
  notes: 'With "Bypass open zones" on, the action\'s line ends "· bypasses open zones" (two lines on the wall: the row grows). Otherwise as the page above.',
  page: armPage(USE('On · hasn\'t run since the panel started'), 'Set up by Alex · warns 5 minutes before · bypasses open zones')});

auto({id: 'automation-arm-not-ready', title: 'It didn\'t arm: a window was open',
  notes: 'With Bypass open zones off, an open zone stops it. A tamper, loop or missing-sensor trouble stops it even with the switch on (the line then names that zone). The panel plays its error sound and reports the failed arm, as a keypad arm would (ARM_FAILED, reported now for automations).',
  page: armPage(USE('On · didn\'t arm at 10:30 PM: Kitchen Window was open', {detailColor: 'amber'}))});

auto({id: 'automation-arm-already', title: 'It didn\'t arm: someone already had',
  notes: 'Already armed at any level: nothing happens and nothing is logged. It never re-arms over another level (a family\'s Night never becomes Away). Not a problem, so not amber.',
  page: armPage(USE('On · already armed at 10:30 PM'))});

auto({id: 'automation-arm-put-off', title: 'Put off for an hour',
  notes: 'Someone put it off an hour with their code at the wall: the name is the code\'s owner, which can be anyone with a code. At 11:25 PM the warning starts again with its Put off button (every warning has one), so a party can put it off again; each put-off is logged ("arming put off", by whom).',
  page: armPage(USE('On · put off by Pat until 11:30 PM'))});

auto({id: 'automation-arm-skipped', title: 'Put off: not tonight',
  notes: 'The switch\'s line for "Not tonight": who, and when it arms again. For an arm time between 6 PM and 6 AM it holds until 6:00 AM; for a daytime arm time the choice reads "Not today" and holds until midnight ("Arms again tomorrow at 10:30 AM"). The next trigger arms as usual. Built: "Not tonight" needs a code that may disarm that partition; with an arm-only or guest code the pad says "That code can only put it off an hour".',
  page: armPage(USE('On · not tonight, put off by Pat · arms again tomorrow at 10:30 PM'))});

auto({id: 'automation-arm-no-warning', title: 'It didn\'t arm: no time to warn first',
  notes: 'It never arms without the whole warning: the clock was set at 10:28 PM for a 10:30 PM arm, or the panel restarted during the warning. It tries again the next day.',
  page: armPage(USE('On · didn\'t arm at 10:30 PM: no time to warn first', {detailColor: 'amber'}))});

auto({id: 'automation-arm-cant-run', title: 'It can\'t run: the maker can\'t arm',
  notes: 'Alex was deleted, demoted or lost the partition: the engine refuses (NOT_AUTHORIZED) and the failed arm is reported. Opening it and saving it again with a code that may arm makes that person its maker.',
  page: armPage(USE('Can\'t run: Alex can no longer arm this partition', {detailColor: 'amber'}))});

// ---- disarming (D23) ----
// With one partition the row reads "Disarm the panel" and opens nothing (the partition is fixed); with more it names the partition and opens the picker.
const DISARM_HOME = M.row_link({glyph: 'shield-off', color: 'amber', title: 'Disarm the panel', detail: 'Set up by Alex', trail: ''});
const UNLOCK = M.row_link({glyph: 'lock-open', color: 'amber', title: 'Unlock Front Door Lock', detail: 'Entry'});
const disarmPage = (use, when, then) => ({title: 'Automation', back: 'automations', body: [use, M.section('When'), when,
  M.section('Then'), ...then, DELETE].join('')});

auto({id: 'automation-disarm-partition', title: 'Disarm: which partition?',
  notes: 'With more than one partition, picking "Disarm the panel" asks which one: one partition, never "Any of them" (AUT-011). With one partition this sheet never shows and the action reads "Disarm the panel". Only partitions the person saving it may disarm are listed.',
  page: ARM_LIST_PAGE,
  over: M.pick_sheet({title: 'Which partition?', fit: true, rows: [
    M.row_link({glyph: 'house', title: 'Home', trail: '', goTo: 'automation-disarm-page'}),
    M.row_link({glyph: 'house', title: 'Studio', trail: ''}),
  ].join('')})});
const NUDGE = M.paragraph('Nothing arms it again. Add one that arms in the evening.');
auto({id: 'automation-disarm-page', shot: 'automation_disarm_page', title: 'An automation that disarms, then unlocks',
  notes: 'The example: at 10:00 AM on weekdays, disarm Home for a worker, then unlock the front door. "Set up by Alex" says who saved it, never that Alex disarms (D30): the engine uses that person\'s permission, checked again each time (AUT-011), and the log says "By automation · At 10:00 AM on weekdays". Under a time-of-day Disarm a quiet paragraph says nothing arms it again. No warning: a disarm makes nothing sound. No "Try it now": it would disarm the house. A time trigger needs no confirm (D24). Other details its switch can show: "On · already disarmed at 10:00 AM", "On · didn\'t disarm at 10:00 AM: Home\'s alarm was sounding" (D22), "Can\'t run: Alex can no longer disarm Home", "Can\'t run: your installer turned disarming off".',
  page: disarmPage(M.row_toggle({glyph: 'zap', title: 'Use this automation', detail: 'On · ran at 10:00 AM', on: true}),
    M.row_link({glyph: 'clock', title: 'At 10:00 AM on weekdays', goTo: 'automation-time'}),
    [DISARM_HOME, UNLOCK, NUDGE])});

auto({id: 'automation-disarm-confirm', shot: 'automation_disarm_confirm', title: 'Disarming after a door opens asks first',
  notes: 'After "When Front Door opens", picking Disarm asks this before it is kept (AUT-016): plain (blue, not red), one sentence that says what will happen. It is asked again whenever the trigger or the action changes, and the panel refuses the save without it. A time of day, armed or disarmed trigger never asks. After an alarm trigger Disarm isn\'t offered at all (D22). The page behind says "Set up by Alex" (D30), not that Alex disarms; on a panel with one partition the action reads "Disarm the panel".',
  page: disarmPage(M.row_toggle({glyph: 'zap', title: 'Use this automation', detail: 'On · hasn\'t run since the panel started', on: true}),
    M.row_link({glyph: 'door-open', color: 'amber', title: 'When Front Door opens', detail: 'Zone 1'}),
    [M.row_link({glyph: 'shield-off', color: 'amber', title: 'Disarm the panel', detail: 'Set up by Alex', trail: ''})]),
  over: M.confirm_dialog({title: 'Disarm the panel when Front Door opens?', tone: 'primary', ok: 'Use it', cancel: 'Cancel',
    note: 'Anyone who opens Front Door will disarm the panel.'})});


auto({id: 'automations-put-off', title: 'Put off: Automations says by whom and until when',
  notes: 'In place of a card kept for the whole hour (as first drawn), Automations says it (D20): the row\'s line "Put off by Pat until 11:30 PM", and the automation\'s switch "On · put off by Pat until 11:30 PM" (automation-arm-put-off). Grey, not amber: nothing is wrong. Home keeps its quiet note meanwhile (D27). The warning starts again 5 minutes before the new time, with its Put off button.',
  page: {title: 'Automations', note: '3 of 16 automations', body: [ADD(),
    M.row_link_alert(ARM_LIST[0]),
    M.row_link_alert({...ARM_ROW, sub: 'Put off by Pat until 11:30 PM', subColor: 'text_2', goTo: 'automation-arm-put-off'}),
    M.row_link_alert(ARM_LIST[2])].join('')}});

}

// ---- Automations without devices (BL-224, docs/features/41 DESIGN.md section 15) ----
// The new actions' sheets, their pages and a new panel's list. Built by ui_auto_edit.c (the
// sheets) and ui_automations.c (the words, the pages); walked in walk_automations_no_device.
{
const result = (t) => `<div class="result_line">${M.text(t, 'label')}</div>`;
const GOES_TO = M.row_link({glyph: 'send', title: 'Goes to', value: 'ntfy, Pushover', trail: ''});
const TIME = (t) => M.row_link({glyph: 'clock', title: t, goTo: 'automation-time'});

auto({id: 'automation-remind', shot: 'automation_remind', title: 'Remind to arm',
  notes: 'After a time (the idea "Remind me to arm" comes with 11:00 PM every day). At that time, only if the partition is still disarmed: one chime (never over an alarm sound, D22) and a steady note on Home and the Panel Manager\'s Overview, until it is armed or 6:00 AM. Nobody has to answer it. "Also send a notification" adds "Home isn\'t armed yet" to the phones; it starts on while a phone service is set up. With two partitions a partition row comes first and the line names it.',
  page: EMPTY_PAGE,
  over: M.sheet({title: 'Remind to arm', width: 760, body: [
    M.row_toggle({glyph: 'send', title: 'Also send a notification', detail: 'To the phones set up in Integrations', on: true}),
    result('At 11:00 PM, if the panel isn\'t armed: a chime, a note on Home and a notification.'),
    M.paragraph('It never arms by itself. The note goes once the panel is armed, or at 6:00 AM.'),
  ].join('')})});

auto({id: 'automation-still-armed', shot: 'automation_still_armed', title: 'Tell me if it\'s still armed',
  notes: '"Late to open" and "no-show": nobody disarmed by a time, so nobody came home. After a time; only a notification, and only if the partition is armed at that moment; nothing changes at the panel. The quoted words are the notification\'s own (notify_words.c: "The panel is still armed at 4:00 PM", or the partition\'s name with more than one). Without a phone service the sheet says so as the notification\'s does (automation-notify-none).',
  page: EMPTY_PAGE,
  over: M.sheet({title: 'Tell me if it\'s still armed', width: 760, body: [
    GOES_TO,
    result('At 4:00 PM on weekdays, if the panel is still armed: "The panel is still armed at 4:00 PM".'),
    M.paragraph('Nothing changes at the panel.'),
  ].join('')})});

auto({id: 'automation-chime', shot: 'automation_chime', title: 'Door chime',
  notes: 'The same chime as the status bar\'s Chime button, for that partition (with two, the partition next); the automation acts as the person who saved it, so no code is asked. While one of two actions is changed, "Remove this action" is the last row. The idea "Quiet the door chime at night" saves "Turn the door chime off" at 10:00 PM every day at once; its page then says "Nothing turns it on again. Add one for the morning." while no automation turns it the other way (automation-chime-page).',
  page: EMPTY_PAGE,
  over: M.pick_sheet({title: 'Door chime', fit: true, rows: [
    M.row_link({glyph: 'bell-ring', title: 'Turn the door chime on', trail: ''}),
    M.row_link({glyph: 'bell-off', color: 'gray', title: 'Turn the door chime off', trail: ''}),
  ].join('')})});

auto({id: 'automation-notify', shot: 'automation_notify', title: 'Send a notification',
  notes: 'The words come from the trigger, never typed (AUT-001), and are the notification\'s own (notify_words.c): "Front Door opened", "Kitchen Window closed", "Home was armed Night", "An alarm is sounding", "The power went off", "It\'s 11:00 PM" for a time alone. It goes to every phone service set up in Integrations, even with that kind of notification switched off there (the person asked for it), and the usual limits apply (at most one a minute from one automation). It may be the second action with an output, a device or the chime; while one of two is changed, "Remove this action" is the last row. "Only between" is said only when the automation has one.',
  page: EMPTY_PAGE,
  over: M.sheet({title: 'Send a notification', width: 760, body: [
    GOES_TO,
    result('Sends "Front Door opened".'),
    M.paragraph('Only between 10:00 PM and 6:00 AM, as the automation says.'),
  ].join('')})});

auto({id: 'automation-notify-none', shot: 'automation_notify_none', title: 'Send a notification, with no phone service yet',
  notes: 'A new panel often has none set up. It can still be saved; until a service is set up its page and its row in the list say "Can\'t send: no phone notifications are set up" in amber, and nothing is held back to send later.',
  page: EMPTY_PAGE,
  over: M.sheet({title: 'Send a notification', width: 760, body: [
    M.row_link({glyph: 'send', color: 'amber', title: 'Goes to', value: 'Nothing yet', valueColor: 'amber', trail: ''}),
    M.paragraph('No phone notifications are set up. Set them up in Integrations; until then it can\'t send.', 'amber'),
  ].join('')})});

auto({id: 'automation-output-do', shot: 'automation_output_do', title: 'What should an output do?',
  notes: 'A wired output set "For automations" in Installer > Sirens and outputs (AUTO_ACT_OUTPUT = 3), as a light\'s: Turn on or Turn off. "Turn on" gets the page\'s "Turn it off after" row (5 minutes to 1 hour, or Leave it on), so a timed output is one automation. At least 10 seconds between two automation actions on one output (AUT-007): it can never be made to flash. While one of two actions is changed, "Remove this action" is the last row.',
  page: EMPTY_PAGE,
  over: M.pick_sheet({title: 'What should Garden Lights do?', fit: true, rows: [
    M.row_link({glyph: 'toggle-right', title: 'Turn on', trail: ''}),
    M.row_link({glyph: 'toggle-left', color: 'gray', title: 'Turn off', trail: ''}),
  ].join('')})});

// ---- the pages ----
auto({id: 'automation-installer-arm-page', shot: 'automation_installer_arm_page', title: 'An arming automation the installer set up',
  notes: 'BL-224 N1: "arm Away every night", saved with the installer\'s code. The line says who set it up and whose permission it uses (the system master, checked at every arming); the log still says "By automation" (D30). If Alex can no longer arm this partition, the switch reads "Can\'t run: Alex can no longer arm this partition", as today. Saved with the master\'s code it reads "Set up by Alex · warns 5 minutes before" (automation-arm-page).',
  page: pageOf([USE('On · hasn\'t run since the panel started'), M.section('When'), TIME('At 10:30 PM every day'), M.section('Then'),
    M.row_link({glyph: 'log-out', color: 'rose', title: 'Arm Away', detail: 'Set up by Installer · uses Alex\'s permission · warns 5 minutes before', goTo: 'automation-arm'}),
    DELETE])});

auto({id: 'automation-remind-page', shot: 'automation_remind_page', title: 'An automation that reminds',
  notes: 'Other switch lines: "On · reminded at 11:00 PM", "On · already armed at 11:00 PM" (not amber: nothing was needed). No "Try it now" and no second action: a reminder means something only at its time, alone. With "Also send a notification" the action\'s line reads "A chime and a note on Home · also a notification".',
  page: pageOf([USE('On · already armed at 10:00 PM'), M.section('When'), TIME('At 11:00 PM every day'), M.section('Then'),
    M.row_link({glyph: 'bell', color: 'amber', title: 'Remind to arm', detail: 'A chime and a note on Home', goTo: 'automation-remind'}), DELETE])});

auto({id: 'automation-door-night-page', shot: 'automation_notify_page', title: 'A door, to the phones',
  notes: '"Only between" is today\'s row (the door idea comes with 10:00 PM to 6:00 AM). "Try it now" sends the notification once, marked as a test, so the person sees it arrive; the switch then says "On · tried at 10:42 PM". The action\'s line is what it says and where it goes; with no phone service, in amber "Can\'t send: no phone notifications are set up".',
  page: pageOf([USE('On · tried at 10:42 PM'), M.section('When'),
    M.row_link({glyph: 'door-open', color: 'amber', title: 'When Front Door opens', detail: 'Zone 1', goTo: 'automation-when'}),
    M.row_link({glyph: 'clock', title: 'Only between', value: 'Any time', goTo: 'automation-window'}), M.section('Then'),
    M.row_link({glyph: 'send', title: 'Send a notification', detail: '"Front Door opened" · ntfy, Pushover', goTo: 'automation-notify'}),
    ADD_SECOND, TRY, DELETE])});

auto({id: 'automation-output-page', shot: 'automation_output_page', title: 'An output when an alarm sounds',
  notes: 'The output idea\'s page ("Switch an output when an alarm sounds": on for 30 minutes). An output in an automation reads as a device does, its terminal under it. "Try it now" switches it on for its time. An output that is no longer "For automations" makes it "Can\'t run: open it to fix it".',
  page: pageOf([USE('On · hasn\'t run since the panel started'), M.section('When'),
    M.row_link({glyph: 'siren', color: 'red', title: 'When an alarm sounds', detail: 'Never for a silent alarm', goTo: 'automation-when'}),
    M.row_link({glyph: 'clock', title: 'Only between', value: 'Any time', goTo: 'automation-window'}), M.section('Then'),
    M.row_link({glyph: 'toggle-right', title: 'Turn on Garden Lights', detail: 'Output on GPIO 47', goTo: 'automation-output-do'}),
    OFF_AFTER('30 minutes'), ADD_SECOND, TRY, DELETE])});

auto({id: 'automation-chime-page', shot: 'automation_chime_page', title: 'The door chime off at night',
  notes: 'The chime idea\'s page. Under a time-of-day "Turn the door chime off" with no automation that turns that partition\'s chime on again, a quiet line says so; it goes once one does. Other switch lines: "On · ran at 10:00 PM", "On · the door chime was already off at 10:00 PM".',
  page: pageOf([USE('On · hasn\'t run since the panel started'), M.section('When'), TIME('At 10:00 PM every day'), M.section('Then'),
    M.row_link({glyph: 'bell-off', color: 'gray', title: 'Turn the door chime off', goTo: 'automation-chime'}),
    ADD_SECOND, TRY, M.paragraph('Nothing turns it on again. Add one for the morning.'), DELETE])});

auto({id: 'automations-no-device', shot: 'automations_no_device', title: 'Automations on a panel with no device',
  notes: 'A new panel\'s list with these: rows as today, the trigger sentence, what it does, the chip by the first action. A notification (or "still armed") while no phone service is set up carries the amber line "Can\'t send: no phone notifications are set up" and keeps its tone: it runs, it can\'t send.',
  page: {title: 'Automations', note: '6 of 16 automations', body: ADD() + [
    {glyph: 'send', title: 'When Front Door opens', detail: 'Send a notification', sub: 'Can\'t send: no phone notifications are set up', goTo: 'automation-door-night-page'},
    {glyph: 'bell-off', color: 'gray', title: 'At 10:00 PM every day', detail: 'Turn the door chime off', goTo: 'automation-chime-page'},
    {glyph: 'bell', color: 'amber', title: 'At 11:00 PM every day', detail: 'Remind to arm', goTo: 'automation-remind-page'},
    {glyph: 'send', title: 'At 4:00 PM on weekdays', detail: 'Tell me if it\'s still armed', sub: 'Can\'t send: no phone notifications are set up'},
    {glyph: 'log-out', color: 'rose', title: 'At 10:30 PM every day', detail: 'Arm Away', goTo: 'automation-installer-arm-page'},
    {glyph: 'toggle-right', title: 'When an alarm sounds', detail: 'Turn on Garden Lights for 30 minutes', goTo: 'automation-output-page'},
  ].map((r) => M.row_link_alert(r)).join('')}});
}
