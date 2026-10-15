// BL-131: Settings, reorganised (mockup-next). Eight sections that fit without scrolling, each
// setting in one place, live subtitles, pickers instead of stepper rows for dates and times.

const RAIL = {troubles: 1};
const BAR = {time: '10:50', troubles: 1, wifi: 'wifi'};
// The section list's links go to this proposal's pages.
const GO = {display: 'n-display', users: 'settings-users', 'wifi': 'n-wifi', time: 'n-time', integrations: 'n-integrations',
  walk: 'settings-walk', about: 'n-about', installer: 'n-installer-menu'};
const P = (replaces, why) => ({bl: 'BL-131', status: 'approved', decided: '2026-10-01: approved as drawn: "start working on getting all of the screen changes in place"', replaces, why});
const page = ({id, title, replaces, why, notes, over, ...s}) => M.screen({
  id, area: 'Settings', title, shot: null, proposal: P(replaces, why), notes,
  render: () => M.main({next: true, view: 'settings', rail: RAIL, bar: BAR, content: M.settings({next: true, go: GO, title, ...s}), over: over ? over() : ''}),
});

// ---- Screen and sound ----
page({
  id: 'n-display', title: 'Screen and sound', replaces: 'settings-display', section: 'display',
  why: 'Today\'s page has 12 rows in one scroll, the chime twice (here and on the rail), and the night rows at the bottom. Now: Screen, then Sound, with the night hours on their own page.',
  notes: 'Chime is left to the rail\'s Chime button (it already asks for a code). The screen saver\'s own brightness moves onto the Screen saver page.',
  title: 'Screen and sound', body: [
    M.section('Screen'),
    M.row_level({glyph: 'sun', title: 'Brightness', value: 80}),
    M.changed([
      M.row_link({glyph: 'image', title: 'Screen saver', detail: 'A dimmed photo with the time and the alarm state', value: 'After 1 minute'}),
      M.row_link({glyph: 'monitor-off', title: 'Screen off', detail: 'Dark until a touch wakes it', value: 'Never'}),
      M.row_link({glyph: 'moon', color: 'violet', title: 'Night hours', detail: '10:00 PM to 6:00 AM · dimmer, and dark sooner', value: 'On', goTo: 'n-night'}),
    ].join('')),
    M.section('Sound'),
    M.row_level({glyph: 'volume-2', title: 'Volume', value: 70}),
    M.paragraph('Alarms, fire and CO always sound at full volume.'),
    M.row_toggle({title: 'Key clicks', detail: 'A short click for every touch', on: true}),
  ].join(''),
});

page({
  id: 'n-night', title: 'Night hours', replaces: 'settings-display-night', section: 'display', back: 'n-display',
  why: 'The night rows were the bottom of a long page, and Starts/Ends stepped with - and + (the preview shows 10:00 AM to 12:00 PM, which looks like a wrong default). Now a page of its own, with the hours as one row.',
  title: 'Night hours', note: 'A dimmer screen at night that goes dark sooner', body: [
    M.row_toggle({title: 'Night hours', detail: 'Every night, on this keypad', on: true}),
    M.changed(M.row_link({glyph: 'clock', title: 'Hours', value: '10:00 PM to 6:00 AM', goTo: 'n-night-hours'})),
    M.row_level({glyph: 'moon', title: 'Brightness at night', value: 20}),
    M.row_link({glyph: 'monitor-off', title: 'Screen off at night', value: 'After 1 minute'}),
    M.paragraph('A touch, a change of state or an alarm wakes the screen. An alarm shows at full brightness, at night too.'),
  ].join(''),
});

const hm = (h, m, ap) => `<div class="time_rollers">${M.roller({label: 'Hour', items: h, width: 88})}${M.roller({label: 'Minute', items: m, width: 88})}${M.roller({items: ap, width: 80})}</div>`;
page({
  id: 'n-night-hours', title: 'Choosing the night hours', section: 'display', back: 'n-display',
  why: 'Both ends of the night in one sheet, set with rollers (a flick, not twenty taps).',
  notes: 'lv_roller columns; minutes in steps of 15. "Done" saves, "Cancel" leaves them as they were.',
  title: 'Night hours', note: 'A dimmer screen at night that goes dark sooner', body: '',
  over: () => M.sheet({title: 'Night hours', note: 'From when to when, every night', width: 760, body: M.changed(`
    <div class="row" style="justify-content:space-around;gap:var(--space_xl)">
      <div class="col" style="gap:var(--space_sm);align-items:center">${M.text('Starts', 'label')}${hm(['8', '9', '10', '11', '12'], ['30', '45', '00', '15', '30'], ['', 'AM', 'PM', '', ''])}</div>
      <div class="col" style="gap:var(--space_sm);align-items:center">${M.text('Ends', 'label')}${hm(['4', '5', '6', '7', '8'], ['30', '45', '00', '15', '30'], ['', '', 'AM', 'PM', ''])}</div>
    </div>`) + `<div class="result_line">${M.text('Night lasts', 'body', 'text_2')}${M.text('8 hours · 10:00 PM to 6:00 AM', 'label')}</div>`}),
});

// ---- Date, time and language ----
const timeBody = (clockRow) => [
  clockRow,
  M.row_link({glyph: 'globe', title: 'Time zone', detail: 'New York, Toronto, Miami · UTC-5', value: 'Eastern Time'}),
  M.changed(M.row_link({glyph: 'clock', title: 'Clock', detail: '2:30 PM or 14:30', value: '12-hour'})),
  M.section('Language'),
  M.row_link({glyph: 'languages', title: 'Language', value: 'English'}),
  M.row_link({glyph: 'map-pin', title: 'Region', detail: 'How dates and numbers are written', value: 'United States'}),
  M.row_value({title: 'Looks like', value: 'Wed, Sep 30 · 10:50 PM · 12,345.6'}),
  M.section('Advanced'),
  M.row_link({glyph: 'server', title: 'Time server', detail: 'Where the clock comes from', value: 'pool.ntp.org'}),
].join('');

page({
  id: 'n-time', title: 'Time and language', replaces: 'settings-time', section: 'time',
  why: 'Two pages each set the 12/24-hour clock (a switch here, "Clock" under Language and region) and each was half empty. One page now: what the clock says, the time zone, one Clock choice, then the language.',
  notes: 'Clock offers Automatic (the region\'s), 12-hour and 24-hour. The time server moves under Advanced: few people change it.',
  title: 'Time and language', body: timeBody(M.changed(
    M.row_link({glyph: 'calendar-clock', color: 'green', title: 'Wednesday, September 30 · 10:50 PM', detail: 'Kept right by the internet', trail: ''}))),
});

page({
  id: 'n-time-unset', title: 'Date, time and language: clock not set', replaces: 'settings-time-settable', section: 'time',
  why: 'When the clock has never been set, the first row says so in amber and opens the picker.',
  title: 'Time and language', body: timeBody(M.changed(
    M.row_link({glyph: 'calendar-clock', color: 'amber', title: 'Sunday, September 20 · 12:54 AM', detail: 'Not set: no internet yet. Tap to set it', value: 'Set', goTo: 'n-time-set'}))),
});

page({
  id: 'n-time-set', title: 'Setting the date and time', replaces: 'time-set', section: 'time',
  why: 'Five stepper rows (year, month, day, hour, minute) took many taps and scrolled. Now a calendar and three rollers on one sheet, and the result in words.',
  notes: 'lv_calendar with arrows for the month; tapping "September 2026" opens month and year rollers. Today\'s day has a ring. Set saves; the internet still corrects it later.',
  title: 'Time and language', body: timeBody(M.row_link({glyph: 'calendar-clock', color: 'amber', title: 'Sunday, September 20 · 12:54 AM', detail: 'Not set', value: 'Set'})),
  over: () => M.sheet({title: 'Set the date and time', note: 'The internet corrects it once the panel is online', ok: 'Set', body: M.changed(`
    <div class="row" style="gap:var(--space_xxl);align-items:flex-start;justify-content:center">
      ${M.calendar({sel: 30, today: 20})}
      <div class="col" style="gap:var(--space_sm);align-items:center;padding-top:var(--space_lg)">${hm(['10', '11', '12', '1', '2'], ['52', '53', '54', '55', '56'], ['', '', 'AM', 'PM', ''])}</div>
    </div>`) + `<div class="result_line">${M.text('The clock will say', 'body', 'text_2')}${M.text('Wednesday, September 30, 2026 · 12:54 AM', 'label')}</div>`}),
});

// ---- Wi-Fi and network ----
page({
  id: 'n-wifi', title: 'Wi-Fi and network', replaces: 'settings-wifi', section: 'wifi',
  why: 'A big red "Forget this network" sat at the top of the page, one tap from cutting the panel off; the connected network was listed twice; the network settings hid behind a small gear. Now the connected network opens its page, and Forget lives there.',
  title: 'Wi-Fi and network', note: 'The alarm keeps working without the network', body: [
    M.changed(M.row_link({glyph: 'wifi', color: 'green', title: 'Harbor House', detail: 'Connected · good signal · 192.0.2.42', goTo: 'n-network'})),
    M.section('Other networks nearby'),
    M.row_link({glyph: 'wifi-high', title: 'Harbor House 5G', detail: 'Password · good signal', trail: 'lock'}),
    M.row_link({glyph: 'wifi-high', title: 'Maple Street', detail: 'Password · good signal', trail: 'lock'}),
    M.row_link({glyph: 'wifi-low', title: 'Harbor Guest', detail: 'Open · weak signal', trail: ''}),
  ].join(''),
});

// A password can be seen before it is sent (decided 2026-10-01: "like other devices support").
const PASSWORD = {title: 'Password for Maple Street', note: 'Letters and symbols: 123 and #+= switch the keys', value: 'harborlights7', password: true, eye: true};
page({
  id: 'n-wifi-password', title: 'Typing a password: the eye', replaces: 'wifi-password', section: 'wifi',
  why: 'A password shows only as dots, so a slip in a long Wi-Fi password shows up only when the router refuses it. Now the field ends in an eye, as on a phone: a tap shows what was typed, another hides it.',
  notes: 'Every password field gets it, as they share the text sheet (ui_forms.c): Wi-Fi here and in the setup, the browser sign-in password, the MQTT password, ntfy and Pushover tokens. It starts hidden each time the sheet opens, and the choice isn\'t kept, as the panel hangs where others walk past.',
  title: 'Wi-Fi and network', note: 'The alarm keeps working without the network', body: '',
  over: () => M.text_sheet(PASSWORD),
});
page({
  id: 'n-wifi-password-shown', title: 'Typing a password: shown', section: 'wifi',
  why: 'After a tap on the eye: the letters in the clear, and the eye crossed out to hide them again. The keyboard and the cursor stay where they were.',
  title: 'Wi-Fi and network', note: 'The alarm keeps working without the network', body: '',
  over: () => M.text_sheet({...PASSWORD, shown: true}),
});

page({
  id: 'n-network', title: 'The connected network', replaces: 'network-address', section: 'wifi', back: 'n-wifi',
  why: '"How the panel gets its address" and "Name server" read oddly to the people who change them, and the plain answer (Automatic) was hidden as "From the router". Forget moves here, at the bottom.',
  title: 'Harbor House', note: 'Connected · good signal', body: [
    M.row_value({title: 'IP address', value: '192.0.2.42'}),
    M.changed([
      M.row_link({glyph: 'sliders-horizontal', title: 'IP address settings', detail: 'Automatic from the router, or a fixed address', value: 'Automatic'}),
      M.row_link({glyph: 'server', title: 'DNS server', detail: 'Turns names into addresses', value: 'Automatic'}),
    ].join('')),
    M.row_action({glyph: 'heart-pulse', text: 'Check the connection', color: 'blue'}),
    M.section(''),
    M.changed(M.row_action({glyph: 'wifi-off', text: 'Forget this network', color: 'red'})),
  ].join(''),
});

// ---- Integrations ----
page({
  id: 'n-integrations', title: 'Integrations', replaces: 'settings-integrations', section: 'integrations',
  why: 'A paragraph between the sections explained the rows; the rows now say it themselves, and "Notifications" reads as "Alerts to your phone".',
  title: 'Integrations', note: 'Smart-home systems and alerts to your phone', body: [
    M.section('Smart home'),
    M.row_link({glyph: 'house-wifi', color: 'violet', title: 'Home Assistant', detail: 'Connected', goTo: 'integration-ha'}),
    M.changed(M.row_link({glyph: 'server', title: 'MQTT', detail: 'Off · any broker: Node-RED, your own scripts'})),
    M.changed(M.section('Alerts to your phone')),
    M.row_link({glyph: 'bell-ring', color: 'orange', title: 'ntfy', detail: 'Off'}),
    M.row_link({glyph: 'radio-tower', title: 'Pushover', detail: 'Off'}),
  ].join(''),
});

// About this panel: proposals/BL-131-about.js

// ---- Installer ----
M.screen({
  id: 'n-installer-menu', area: 'Installer', title: 'Installer mode: the menu, grouped', shot: null,
  proposal: P('installer-menu', 'Eleven rows in no order: everyday work (Zones) beside one-offs (System name) and the dangerous ones (Factory reset) mid-list, and the Panel Manager in two places ("Web server" under Remote access, the installer\'s sign-in under Panel Manager). Now four groups, the risky ones last, the Panel Manager in one place.'),
  notes: '"System options" becomes "Delays and arming", the words on its page. With Z-Wave built in, Z-Wave follows Zones. No page note: the banner above already says installer mode is on and arming is off. The switches carry an icon so every row of a group starts at the same place (rule 8).',
  render: () => M.main({next: true, view: 'settings', tone: 'blue', rail: {troubles: 1, tone: 'blue'}, bar: {banner: 'installer', time: '10:52', troubles: 1},
    content: M.settings({next: true, go: GO, section: 'installer', title: 'Installer', body: [
      M.changed(M.section('Sensors and devices')),
      M.row_link({glyph: 'door-closed', color: 'green', title: 'Zones', detail: 'Sensors, types, names', value: '8 in use', goTo: 'installer-zones'}),
      M.row_link({glyph: 'warehouse', color: 'violet', title: 'Partitions', detail: 'Areas that arm separately', value: '1 in use'}),
      M.row_link({glyph: 'cpu', title: 'Keypads and devices', detail: 'Supervised keypads and expanders', value: 'None'}),
      M.changed(M.section('How the alarm works')),
      M.changed(M.row_link({glyph: 'sliders-horizontal', title: 'Delays and arming', detail: 'Entry and exit times, sirens, panic keys, codes', goTo: 'installer-options'})),
      M.changed(M.section('This panel')),
      M.row_link({glyph: 'house', color: 'violet', title: 'System name', detail: 'Shown on the screen and in the event log', value: 'Home'}),
      M.row_link({glyph: 'key-round', color: 'amber', title: 'Installer and master codes', detail: 'Factory codes still in use'}),
      M.changed([
        M.row_toggle({glyph: 'monitor', title: 'Panel Manager', detail: 'Change settings from a browser on your network', on: true}),
        M.row_link({glyph: 'globe', title: 'Your browser sign-in', detail: 'User name: Installer', value: 'Off'}),
      ].join('')),
      M.row_toggle({glyph: 'code-xml', color: 'gray', title: 'Developer mode', detail: 'Enrolled development tools and signed network updates'}),
      M.changed(M.section('Test and start over')),
      M.row_link({glyph: 'footprints', title: 'Walk test', detail: 'Leaves installer mode and starts one'}),
      M.row_link({glyph: 'sparkles', color: 'rose', title: 'Run the setup again', detail: 'Wi-Fi, time zone, codes and name, step by step', goTo: 'n-setup-wifi'}),
      M.row_link({glyph: 'rotate-ccw', color: 'red', title: 'Factory reset', detail: 'Erase everything', goTo: 'factory-reset'}),
      M.row_action({glyph: 'log-out', text: 'Leave installer mode', color: 'blue'}),
    ].join('')})}),
});

// ---- Activity: the whole event log ----
M.screen({
  id: 'n-activity', area: 'Views', title: 'Activity: the whole event log', shot: null,
  proposal: P('activity', 'The log lived twice: Activity on the rail (recent) and Event log in Settings (all of it, with filters). Activity now is the log, with the filters, and Settings loses a section.'),
  notes: 'Times say when ("10:50 PM", "Yesterday"), not only "Just now". The log\'s note (how many it keeps) moves to the bottom of the list.',
  render: () => M.main({next: true, view: 'activity', tone: 'amber', rail: {zones: 1, troubles: 1}, bar: {time: '10:48', troubles: 1, wifi: 'wifi'}, content: `<div class="view column">
    <div class="row">${M.view_header({title: 'Activity', note: 'Everything the panel did, newest first'})}</div>
    <div style="flex:none">${M.changed(`<div class="row">${M.segments(['All', 'Alarms', 'Troubles', 'Arming', 'Bypass', 'System'])}</div>`)}</div>
    <div class="list scroll">${[
      {glyph: 'circle-slash', title: 'Hallway Motion bypassed', detail: 'By Master', time: '10:48 PM'},
      {glyph: 'battery-warning', title: 'Basement Water · Low battery', detail: 'Check the sensor', tone: 'amber', time: '10:31 PM'},
      {glyph: 'check', title: 'Alarm memory cleared', detail: 'By Master', time: '9:02 PM'},
      {glyph: 'lock-open', title: 'Disarmed', detail: 'By Master', tone: 'green', time: '9:01 PM'},
      {glyph: 'circle-check', title: 'Alarm aborted', detail: 'Disarmed before it was reported', tone: 'green', time: 'Yesterday'},
      {glyph: 'lock', title: 'Armed Away', detail: 'By Master', tone: 'rose', time: 'Yesterday'},
    ].map(M.event_row).join('')}</div></div>`}),
});
