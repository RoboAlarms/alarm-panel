// BL-131 finding 7, part 0: Activity as the whole event log (mockup-next). It was decided on
// 2026-10-05 that Activity becomes the event log, Settings loses its Event log line, and LOG-008
// stays (docs/features/41-Automations D2). The approved drawing is `n-activity` in
// BL-131-settings.js; these screens draw what it left open: the bottom of a long list, LOG-008's
// private step (silent events only behind a master or installer code), LOG-010's latched loss
// warning, and the private step while armed. The Settings list without Event log (eight lines) is
// already drawn by every page of BL-131-settings.js (the kit's SECTIONS_NEXT).
//
// Open questions for review:
// (1) The private step as drawn: a quiet "Show private events" row at the end of the list, always
//     there (whether or not there are silent events, so it hints at nothing), opening the code pad
//     over Activity; after a master or installer code the silent events join the list marked
//     "Silent" and the row reads "Hide private events". Forgotten on leaving Activity, at the
//     screen saver or idle, and when an arming starts. Is that the step you want?
// (2) Where LOG-010 lives and who sees it: drawn at the top of the list only after the private
//     step (a master or an installer), with "Clear the warning" only for an installer code. The
//     `storage` trouble it holds still shows on Troubles for everyone. Or should the amber rows
//     show to everyone on the public list?
// (3) The private step while armed or in alarm: drawn dimmed, "once the system is disarmed", as
//     UI-022 dims Settings. Or keep it open while armed (it changes nothing, and a master may want
//     the log during an alarm)?
// (4) "Yesterday, 9:01 PM" (Yesterday with its time, LOG-004's full time) and the older format
//     "Sep 19, 10:50 PM" (the region's short date). Today's events keep only the time.
// (5) Does a partition master see other partitions' silent events, or only their own partitions'?
//     (The installer sees all.)

const RAIL = {zones: 1, troubles: 1};
const BAR = {time: '10:48', troubles: 1, wifi: 'wifi'};
const NOTE = 'Everything the panel did, newest first';
const P = (why) => ({bl: 'BL-131', status: 'draft', replaces: 'activity', why});
const FILTERS = `<div style="flex:none"><div class="row">${M.segments(['All', 'Alarms', 'Troubles', 'Arming', 'Bypass', 'System'])}</div></div>`;

// Activity: the header, the filters and the list (rows: HTML); bottom: the list scrolled to its end.
const activity = ({note = NOTE, rows, bottom = false, tone = 'amber', rail = RAIL, bar = BAR, over = ''}) => M.main({
  next: true, view: 'activity', tone, rail, bar, over, content: `<div class="view column">
    <div class="row">${M.view_header({title: 'Activity', note})}</div>${FILTERS}
    <div class="list scroll"${bottom ? ' data-scroll="9999"' : ''}>${rows}</div></div>`,
});
const events = (list) => list.map(M.event_row).join('');

// The list's end: older events, how much the log keeps, the private step.
const OLDER = [
  {glyph: 'lock-open', title: 'Disarmed', detail: 'By Alex', tone: 'green', time: 'Yesterday, 9:01 PM'},
  {glyph: 'circle-check', title: 'Alarm aborted', detail: 'Disarmed before it was reported', tone: 'green', time: 'Yesterday, 8:58 PM'},
  {glyph: 'lock', title: 'Armed Away', detail: 'By Alex', tone: 'rose', time: 'Yesterday, 8:15 AM'},
  {glyph: 'user', title: 'Code changed for Sam', detail: 'By Master', time: 'Sep 19, 10:50 PM'},
];
const more = M.row_action({glyph: 'history', text: 'Show older events', color: 'blue'});
const keeps = M.paragraph('Keeps the last 6080 or more');
const showPrivate = M.row_link({glyph: 'key-round', color: 'gray', title: 'Show private events', trail: '', goTo: 'n-activity-private-ask'});
const hidePrivate = M.row_link({glyph: 'eye-off', color: 'gray', title: 'Hide private events', trail: '', goTo: 'n-activity-long'});

// ---- 1. The bottom of a long list ----
M.screen({
  id: 'n-activity-long', area: 'Views', title: 'Activity: the bottom of a long list', shot: null,
  proposal: P('The approved n-activity shows the top of the list; this is its end, where today\'s Event log pages back with "Show older events" and says how much it keeps.'),
  notes: 'LOG-007 (filters, paging), LOG-004 (the full time). "Show older events" loads the next page, as Settings > Event log does today. '
    + 'The note says how much the log keeps, with no total: a count that included silent events would hint at them (LOG-008). '
    + 'Proposed: an event from yesterday says "Yesterday, 9:01 PM" (Yesterday with its time); older ones the region\'s short date and the time, "Sep 19, 10:50 PM"; today\'s only the time.',
  render: () => activity({bottom: true, rows: events([
    {glyph: 'lock-open', title: 'Disarmed', detail: 'By Pat', tone: 'green', time: '7:30 AM'},
    {glyph: 'lock', title: 'Armed Stay', detail: 'By Pat', tone: 'amber', time: '12:04 AM'},
    ...OLDER,
  ]) + M.changed(more + keeps + showPrivate)}),
});

// ---- 2. Asking for the code ----
M.screen({
  id: 'n-activity-private-ask', area: 'Views', title: 'Activity: asking for a code to show private events', shot: null,
  proposal: P('LOG-008 keeps silent events (duress, silent panic) behind a master or installer code. With Event log gone from Settings, Activity needs a way in.'),
  notes: 'LOG-008. The pad opens over Activity (modal_pad, as the chime does, UI-025): the list stays behind it. '
    + 'Proposed: the "Show private events" row is always at the list\'s end, whether or not there are silent events, so its presence hints at nothing; the pad says nothing about what is private. '
    + 'A user code or a wrong code is refused as any wrong code is (it counts towards the keypad lockout).',
  render: () => activity({bottom: true, rows: events(OLDER) + more + keeps + M.changed(showPrivate),
    over: M.changed(M.modal_pad({title: 'Show private events', tone: 'blue', glyph: 'key-round', hint: 'Enter a master or installer code'}))}),
});

// ---- 3. Private events shown ----
M.screen({
  id: 'n-activity-private', area: 'Views', title: 'Activity: private events shown', shot: null,
  proposal: P('After a master or installer code the list includes the silent events, marked "Silent", as Settings > Event log shows them today.'),
  notes: 'LOG-008. Silent events take their place in time order, with "Silent" first in their detail. '
    + 'Proposed: the note says the private events are shown; the row at the end reads "Hide private events". '
    + 'Proposed: the code is forgotten, and the list is public again, on leaving Activity, at the screen saver or idle, and when an arming starts (as UI-022 forgets a Settings code).',
  render: () => activity({note: 'Newest first · private events shown', rows: events([
    {glyph: 'lock-open', title: 'Disarmed', detail: 'By Pat', tone: 'green', time: '7:30 AM'},
  ]) + M.changed(events([
    {glyph: 'siren', title: 'Silent panic', detail: 'Silent · Bedroom Panic Button', tone: 'red', time: '2:12 AM'},
  ])) + events([
    {glyph: 'lock-open', title: 'Disarmed', detail: 'By Alex', tone: 'green', time: 'Yesterday, 9:01 PM'},
  ]) + M.changed(events([
    {glyph: 'lock-open', title: 'Duress code used', detail: 'Silent · By Alex', tone: 'red', time: 'Yesterday, 9:01 PM'},
  ])) + events(OLDER.slice(1)) + more + keeps + M.changed(hidePrivate)}),
});

// ---- 4. The latched loss warning (LOG-010) ----
const lossRow = M.row_link({glyph: 'triangle-alert', color: 'amber', title: '3 events couldn\'t be saved', detail: '1 alert couldn\'t be kept', trail: ''});
const TOP = events([
  {glyph: 'circle-slash', title: 'Hallway Motion bypassed', detail: 'By Master', time: '10:48 PM'},
  {glyph: 'battery-warning', title: 'Basement Water · Low battery', detail: 'Check the sensor', tone: 'amber', time: '10:31 PM'},
  {glyph: 'siren', title: 'Silent panic', detail: 'Silent · Bedroom Panic Button', tone: 'red', time: '10:12 PM'},
  {glyph: 'lock-open', title: 'Disarmed', detail: 'By Alex', tone: 'green', time: '9:01 PM'},
]);
M.screen({
  id: 'n-activity-loss', area: 'Views', title: 'Activity: the log-loss warning, installer code', shot: null,
  proposal: P('LOG-010\'s latched warning lived at the top of Settings > Event log; it moves to the top of Activity\'s list after the private step.'),
  notes: 'LOG-010: the amber row stays until the installer clears it; a later good write never clears it. The `storage` trouble it holds shows on Troubles for everyone. '
    + 'Proposed: the rows show only after the private step, at the top of the list above the newest event; with an installer code, "Clear the warning".',
  render: () => activity({note: 'Newest first · private events shown', rows: M.changed(lossRow + M.row_action({glyph: 'check', text: 'Clear the warning', color: 'blue'})) + TOP}),
});
M.screen({
  id: 'n-activity-loss-master', area: 'Views', title: 'Activity: the log-loss warning, master code', shot: null,
  proposal: P('The same warning after a master code: it shows, but only the installer clears it (today\'s words).'),
  notes: 'LOG-010. Proposed: a master sees the rows and who clears them, with no button.',
  render: () => activity({note: 'Newest first · private events shown', rows: M.changed(lossRow + M.paragraph('Only the installer code clears this warning.')) + TOP}),
});

// ---- 5. While armed or in alarm ----
M.screen({
  id: 'n-activity-armed', area: 'Views', title: 'Activity: while armed, the private step dimmed', shot: null,
  proposal: P('UI-022 locks Settings while armed or in alarm; with the full log in Activity, the private step could follow it.'),
  notes: 'UI-022 parity, proposed, not decided (question 3): from the exit time on, or while an alarm sounds, the row is dimmed and says why; the public list itself stays open. '
    + 'An arming that starts while private events are shown hides them again.',
  render: () => activity({bottom: true, tone: 'rose', bar: {...BAR, time: '10:52'}, rows: events([
    {glyph: 'lock', title: 'Armed Away', detail: 'By Alex', tone: 'rose', time: '10:52 PM'},
    ...OLDER,
  ]) + more + keeps + M.changed(M.row_link_off({glyph: 'key-round', color: 'gray', title: 'Show private events', detail: 'Once the system is disarmed'}))}),
});
