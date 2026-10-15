// Settings: the section list beside the open page (screen_main.xml "view_settings"). The pages
// are built in C (ui_settings.c, ui_net.c, ui_integrations.c, ui_notify.c, ui_lang.c,
// ui_walk.c, ui_update.c, ui_storage.c) from the rows in ui_forms.c, which M.row_* mirror.
// A page in the mockup shows all its rows; scroll inside it to see the ones under the fold.

const SET_RAIL = {troubles: 1};
const SET_BAR = {time: '10:50', troubles: 1, wifi: 'wifi-low'};
const set = (meta, page, extra = {}) => M.screen({
  ...meta, area: 'Settings', ...extra,
  render: () => M.main({view: 'settings', rail: SET_RAIL, bar: SET_BAR, content: M.settings(page()), ...(extra.main ? extra.main() : {})}),
});

// How the screen looks (BL-132, built): Appearance opens the list of three, High contrast is a
// switch that works with any of them. Automatic says what it follows under its name.
const LOOK_ROWS = ({appearance = 'Dark', contrast = false, night = true}) => [
  M.row_link({glyph: 'sun-moon', title: 'Appearance', value: appearance, goTo: 'settings-appearance',
    detail: appearance !== 'Automatic' ? '' : night ? 'Dark during the night hours, light by day' : 'Night hours are off: stays light'}),
  M.row_toggle({glyph: 'contrast', title: 'High contrast', detail: 'Stronger colors, every button outlined', on: contrast}),
].join('');
const DISPLAY_PAGE = (look) => () => ({
  section: 'display', title: 'Screen and sound',
  body: [
    M.section('Screen'),
    LOOK_ROWS(look),
    M.row_level({glyph: 'sun', title: 'Brightness', value: 80}),
    M.row_link({glyph: 'image', title: 'Screen saver', detail: 'A dimmed photo with the time and the alarm state', value: 'After 1 minute', goTo: 'settings-display-saver'}),
    M.row_link({glyph: 'monitor-off', title: 'Screen off', detail: 'Dark until a touch wakes it', value: 'Never'}),
    M.row_link({glyph: 'moon', color: 'violet', title: 'Night hours', detail: '10:00 PM to 7:00 AM · dimmer, and dark sooner', value: 'Off', goTo: 'settings-display-night'}),
  ].join(''),
});
set({id: 'settings-display-light', title: 'Screen and sound in Light', shot: 'settings_display_light'}, DISPLAY_PAGE({appearance: 'Light'}),
  {theme: 'light', notes: 'BL-132, built (a-settings-light): Light picked, the page light at once, in one step.'});
set({id: 'settings-display-contrast', title: 'Screen and sound with High contrast on', shot: 'settings_display_contrast'},
  DISPLAY_PAGE({appearance: 'Dark', contrast: true}),
  {theme: 'contrast', notes: 'BL-132, built (a-display-contrast): Dark with High contrast, white on black, every row outlined.'});
set({id: 'settings-display', title: 'Screen and sound', shot: 'settings_display'}, () => ({
  section: 'display', title: 'Screen and sound', note: '',
  body: [
    M.section('Screen'),
    LOOK_ROWS({}),
    M.row_level({glyph: 'sun', title: 'Brightness', value: 80}),
    M.row_link({glyph: 'image', title: 'Screen saver', detail: 'A dimmed photo with the time and the alarm state', value: 'After 1 minute', goTo: 'settings-display-saver'}),
    M.row_link({glyph: 'monitor-off', title: 'Screen off', detail: 'Dark until a touch wakes it', value: 'Never'}),
    M.row_link({glyph: 'moon', color: 'violet', title: 'Night hours', detail: '10:00 PM to 7:00 AM · dimmer, and dark sooner', value: 'Off', goTo: 'settings-display-night'}),
    M.section('Sound'),
    M.row_level({glyph: 'volume-2', title: 'Volume', value: 70}),
    M.paragraph('Alarms, fire and CO always sound at full volume.'),
    M.row_toggle({title: 'Key clicks', detail: 'A short click for every touch', on: true}),
  ].join(''),
}), {notes: 'Screen, then Sound (BL-131 finding 4, built). The appearance on top (BL-132, built): a change applies at once, in one step, and needs no code, like the brightness. No Chime row: the status bar\'s Chime switch is its one place. The slider rows put their icon in a chip, so the titles line up with the rows below. Rows below the fold follow ui_settings.c build_display().'});

M.screen({
  id: 'settings-appearance', area: 'Settings', title: 'Choosing the appearance', shot: 'appearance_pick',
  notes: 'BL-132, built (a-appearance): the three appearances, each saying when it suits. Automatic follows the night hours (UI-043): dark during them, light by day, switching once at each end; with the night hours off it says "Night hours are off: stays light". The screen saver keeps the dark colours in every appearance.',
  render: () => M.main({view: 'settings', rail: SET_RAIL, bar: SET_BAR,
    content: M.settings({section: 'display', title: 'Screen and sound', body: [
      M.section('Screen'),
      LOOK_ROWS({appearance: 'Automatic'}),
      M.row_level({glyph: 'sun', title: 'Brightness', value: 80}),
    ].join('')}),
    over: M.pick_sheet({title: 'Appearance', note: 'How the screen looks', fit: true, rows: [
      M.row_link({glyph: 'moon', color: 'violet', title: 'Dark', detail: 'Light words on a dark screen: easy on the eyes at night', trail: ''}),
      M.row_link({glyph: 'sun', color: 'amber', title: 'Light', detail: 'Dark words on a light screen, for a bright room', trail: ''}),
      M.row_link({glyph: 'sun-moon', color: 'blue', title: 'Automatic', detail: 'Dark during the night hours (10:00 AM to 12:00 PM), light by day', trail: 'check', trailColor: 'blue'}),
    ].join('')})}),
});

set({id: 'settings-display-saver', title: 'Screen saver', shot: 'settings_display_saver'}, () => ({
  section: 'display', title: 'Screen saver', back: 'settings-display',
  body: [
    M.row_link({glyph: 'image', title: 'Screen saver', detail: 'A dimmed photo with the time and the alarm state', value: 'After 1 minute'}),
    M.row_level({glyph: 'moon', title: 'Screen saver brightness', value: 50, label: '25%'}),
  ].join(''),
}), {notes: 'When the saver starts and how bright it is: the two rows that were on Screen and sound, moved here. At 0 a line says the screen goes dark.'});

set({id: 'settings-display-night', title: 'Night hours', shot: 'settings_display_night'}, () => ({
  section: 'display', title: 'Night hours', note: 'A dimmer screen at night that goes dark sooner', back: 'settings-display',
  body: [
    M.row_toggle({title: 'Night hours', detail: 'Every night, on this keypad', on: true}),
    M.row_link({glyph: 'clock', title: 'Hours', value: '10:00 AM to 12:00 PM', goTo: 'settings-night-hours'}),
    M.row_level({glyph: 'moon', title: 'Brightness at night', value: 20}),
    M.row_link({glyph: 'monitor-off', title: 'Screen off at night', value: 'After 1 minute'}),
    M.paragraph('A touch, a change of state or an alarm wakes the screen. An alarm shows at the normal brightness, at night too.'),
  ].join(''),
}), {notes: 'The night rows have a page of their own; the hours are one row that opens the sheet (the next screen).'});

const NIGHT_ROLLERS = (h, m, ap) => `<div class="time_rollers">${M.roller({label: 'Hour', items: h, width: 88})}${M.roller({label: 'Minute', items: m, width: 88})}${M.roller({items: ap, width: 80})}</div>`;
M.screen({
  id: 'settings-night-hours', area: 'Settings', title: 'Choosing the night hours', shot: 'settings_night_hours',
  render: () => M.main({view: 'settings', rail: SET_RAIL, bar: SET_BAR,
    content: M.settings({section: 'display', title: 'Night hours', note: 'A dimmer screen at night that goes dark sooner', back: 'settings-display', body: [
      M.row_toggle({title: 'Night hours', detail: 'Every night, on this keypad', on: true}),
      M.row_link({glyph: 'clock', title: 'Hours', value: '10:00 AM to 12:00 PM'}),
      M.row_level({glyph: 'moon', title: 'Brightness at night', value: 20}),
      M.row_link({glyph: 'monitor-off', title: 'Screen off at night', value: 'After 1 minute'}),
    ].join('')}),
    over: M.sheet({title: 'Night hours', note: 'From when to when, every night', width: 760, body: `
      <div class="row" style="justify-content:space-around;padding-top:var(--space_sm)">
        <div class="col" style="gap:var(--space_sm);align-items:center">${M.text('Starts', 'label')}${NIGHT_ROLLERS(['8', '9', '10', '11', '12'], ['30', '45', '00', '15', '30'], ['', '', 'AM', 'PM', ''])}</div>
        <div class="col" style="gap:var(--space_sm);align-items:center">${M.text('Ends', 'label')}${NIGHT_ROLLERS(['10', '11', '12', '1', '2'], ['30', '45', '00', '15', '30'], ['', 'AM', 'PM', '', ''])}</div>
      </div>` + `<div class="result_line">${M.text('Night lasts', 'body', 'text_2')}${M.text('2 hours · 10:00 AM to 12:00 PM', 'label')}</div>`})}),
  notes: 'Starts and Ends are the clock sheet\'s Hour, Minute and AM/PM rollers (minutes in steps of 15; no AM/PM on a 24-hour clock), and under them how long the night lasts. Done saves both ends, Cancel leaves them as they were.',
});

set({id: 'settings-users-code', title: 'Users: asks for a code first', shot: 'settings_users_code'}, () => ({
  section: 'users', pad: M.code_pad({title: 'Users', tone: 'violet', glyph: 'users', digits: 4, width: 428}),
}), {notes: 'A code-protected section shows the pad in the page\'s place (settings_pad_area).'});

set({id: 'settings-users', title: 'Users', shot: 'settings_users'}, () => ({
  section: 'users', title: 'Users', note: 'Signed in as Master · Codes are never shown',
  body: [
    M.row_action({glyph: 'user-plus', text: 'Add a user', color: 'blue'}),
    M.row_link({glyph: 'user', color: 'amber', title: 'Installer', detail: 'User 01 · Installer', trail: ''}),
    M.row_link({glyph: 'user', color: 'violet', title: 'Master', detail: 'User 02 · System master', value: 'You', goTo: 'user-page'}),
  ].join(''),
}));

set({id: 'user-page', title: 'One user', shot: 'user_page'}, () => ({
  section: 'users', title: 'Sam', note: 'User 03 · Guest', back: 'settings-users',
  body: [
    M.row_link({glyph: 'tag', title: 'Name', value: 'Sam', goTo: 'keyboard'}),
    M.row_link({glyph: 'shield', color: 'gray', title: 'What this code can do', detail: 'Disarms only what this code armed', value: 'Guest', goTo: 'user-authority'}),
    M.row_link({glyph: 'key-round', color: 'amber', title: 'Change the code', detail: '4 to 10 digits'}),
    M.section(''),
    M.row_action({glyph: 'trash-2', text: 'Delete this user', color: 'red'}),
  ].join(''),
}), {notes: 'A Guest has no Panel Manager section (no browser sign-in). Delete is last, apart under a gap; the master has none.'});

set({id: 'user-page-standard', title: 'A user with browser sign-in', shot: 'user_page_standard'}, () => ({
  section: 'users', title: 'Pat', note: 'User 03 · Standard', back: 'settings-users',
  body: [
    M.row_link({glyph: 'tag', title: 'Name', value: 'Pat'}),
    M.row_link({glyph: 'shield', title: 'What this code can do', detail: 'Arms, disarms, bypasses, chime', value: 'Standard'}),
    M.row_link({glyph: 'key-round', color: 'amber', title: 'Change the code', detail: '4 to 10 digits'}),
    M.section('Panel Manager'),
    M.row_toggle({glyph: 'globe', title: 'Browser sign-in', detail: 'User name: Pat · a password, not the keypad code'}),
    M.section(''),
    M.row_action({glyph: 'trash-2', text: 'Delete this user', color: 'red'}),
  ].join(''),
}), {notes: 'Browser sign-in is a switch (off: asks for a password; on: "Change the browser password" appears under it, and turning it off asks "Turn off browser sign-in?"). Delete is last, apart.'});

M.screen({
  id: 'keyboard', area: 'Settings', title: 'Typing a name', shot: 'keyboard',
  notes: 'text_sheet: the keyboard covers the screen; a toast moves to the top while it is open (toast_kb_y).',
  render: () => M.main({view: 'settings', rail: SET_RAIL, bar: SET_BAR,
    content: M.settings({section: 'users', title: 'User 03', body: ''}),
    over: M.text_sheet({title: 'Name', note: 'Shown in the event log', value: 'Sam'}) + M.toast('User added. Give them a name', 'kb')}),
});

M.screen({
  id: 'user-authority', area: 'Settings', title: 'Choosing what a code can do', shot: 'user_authority',
  render: () => M.main({view: 'settings', rail: SET_RAIL, bar: SET_BAR,
    content: M.settings({section: 'users', title: 'Sam', note: 'User 03 · Standard', back: true, body: [
      M.row_link({glyph: 'tag', title: 'Name', value: 'Sam'}),
      M.row_link({glyph: 'shield', color: 'gray', title: 'What this code can do', value: 'Standard'})].join('')}),
    over: M.pick_sheet({fit: true, title: 'What this code can do', rows: [
      M.row_link({glyph: 'user', title: 'Standard', detail: 'Arms, disarms, bypasses, chime', trail: 'check', trailColor: 'blue'}),
      M.row_link({glyph: 'user', color: 'gray', title: 'Arm only', detail: 'Arms only, can\'t disarm', trail: ''}),
      M.row_link({glyph: 'user', color: 'gray', title: 'Guest', detail: 'Disarms only what this code armed', trail: ''}),
      M.row_link({glyph: 'user', color: 'red', title: 'Duress', detail: 'Disarms and silently calls for help', trail: ''}),
      M.row_link({glyph: 'user', color: 'violet', title: 'Partition master', detail: 'Everything, and manages users of its area', trail: ''}),
    ].join('')})}),
});

set({id: 'settings-wifi', title: 'Wi-Fi and network', shot: 'settings_wifi'}, () => ({
  section: 'wifi', title: 'Wi-Fi and network', note: 'The alarm keeps working without the network',
  body: [
    M.row_link({glyph: 'wifi', color: 'green', title: 'Harbor House', detail: 'Connected · good signal · 192.0.2.42'}),
    M.section('Other networks nearby'),
    M.row_link({glyph: 'wifi-high', title: 'Harbor House 5G', detail: 'Password · good signal', trail: 'lock'}),
    M.row_link({glyph: 'wifi-high', title: 'Maple Street', detail: 'Password · good signal', trail: 'lock'}),
    M.row_link({glyph: 'wifi-low', title: 'Harbor Guest', detail: 'Open · weak signal', trail: ''}),
  ].join(''),
}));

set({id: 'wifi-password', title: 'Wi-Fi: typing a password', shot: 'wifi_password'}, () => ({
  section: 'wifi', title: 'Wi-Fi and network', note: 'The panel keeps protecting without the network',
}), {notes: 'The text sheet (ui_forms.c) in password mode: dots, and the eye that shows what was typed (BL-131 finding 27, built).',
  main: () => ({over: M.text_sheet({title: 'Password for Maple Street', note: 'Letters and symbols: 123 and #+= switch the keys', value: 'harborlights7', password: true, eye: true})})});

// Time and language (BL-131 finding 2): what the clock says, the time zone and one Clock choice,
// then the language; the time server under Advanced. Without a master or installer code only the
// clock's rows show (docs/features/14). The clock's row opens the setter only where a time set by
// hand is kept (RTC-008): settings-time-settable.
const CLOCK_NOW = (more = {}) => M.row_link({glyph: 'calendar-clock', color: 'green', title: 'Saturday, September 19 · 10:50 PM', detail: 'Kept right by the internet', trail: '', ...more});
const CLOCK_CHOICE = (value = '12-hour') => M.row_link({glyph: 'clock', title: 'Clock', detail: '2:30 PM or 14:30', value});
const timeBody = ({clock = CLOCK_NOW(), zone = ['New York, Toronto, Miami · UTC-5', 'Eastern Time'], hours = '12-hour', region = 'United States', looks = 'Sep 19, 10:50 PM · 12,345.6'} = {}) => [
  clock,
  M.row_link({glyph: 'globe', title: 'Time zone', detail: zone[0], value: zone[1]}),
  CLOCK_CHOICE(hours),
  M.section('Language'),
  M.row_link({glyph: 'languages', title: 'Language', value: 'English'}),
  M.row_link({glyph: 'map-pin', title: 'Region', detail: 'How dates and numbers are written', value: region}),
  M.row_value({title: 'Looks like', value: looks}),
  M.section('Advanced'),
  M.row_link({glyph: 'server', title: 'Time server', detail: 'Where the clock comes from', value: 'pool.ntp.org'}),
].join('');

set({id: 'settings-time', title: 'Time and language', shot: 'settings_time'}, () => ({
  section: 'time', title: 'Time and language', body: timeBody(),
}), {notes: 'No note under the title: the rows say it. The clock\'s row turns amber while nothing has set the clock ("Not set: it sets itself once Wi-Fi is connected").'});

set({id: 'settings-time-locked', title: 'Time and language without a code', shot: 'language_locked'}, () => ({
  section: 'time', title: 'Time and language', note: 'A master or installer code changes the time zone and the language',
  body: [
    CLOCK_NOW(),
    CLOCK_CHOICE(),
    M.row_action({glyph: 'key', text: 'Enter a master or installer code', color: 'violet'}),
  ].join(''),
}), {notes: 'The clock\'s rows are anyone\'s: the 12/24-hour clock is this keypad\'s.'});

set({id: 'settings-integrations', title: 'Integrations', shot: 'integrations'}, () => ({
  section: 'integrations', title: 'Integrations', note: 'Smart-home systems and alerts to your phone',
  body: [
    M.section('Smart home'),
    M.row_link({glyph: 'house-wifi', color: 'violet', title: 'Home Assistant', detail: 'Off', goTo: 'integration-ha'}),
    M.row_link({glyph: 'server', title: 'MQTT', detail: 'Off · any broker: Node-RED, your own scripts'}),
    M.section('Alerts to your phone'),
    M.row_link({glyph: 'bell-ring', color: 'orange', title: 'ntfy', detail: 'Off'}),
    M.row_link({glyph: 'radio-tower', title: 'Pushover', detail: 'Off'}),
  ].join(''),
}), {notes: 'Integrations are named only here and on their own pages (UI-018). The rows say their state: no paragraph between the groups.'});

set({id: 'integration-ha', title: 'Home Assistant', shot: 'ha_integration'}, () => ({
  section: 'integrations', title: 'Home Assistant', note: 'Found on your network and paired on this screen', back: 'settings-integrations',
  body: [
    M.row_toggle({title: 'Home Assistant', detail: 'Off · switch it on so Home Assistant can find the panel'}),
    M.paragraph('Home Assistant finds the panel on your network and you allow it here. Nothing to type, and no Home Assistant password.'),
  ].join(''),
}), {notes: 'One switch, its state in the detail: no status card above it.'});

set({id: 'settings-log', title: 'Event log', shot: 'event_log'}, () => ({
  section: 'log', title: 'Event log', note: '43 events · newest first · Keeps the last 960 or more',
  body: `<div class="row" style="flex:none">${M.segments(['All', 'Alarms', 'Troubles', 'Arming', 'Bypass', 'System'])}</div>` + [
    {glyph: 'user', title: 'Sam changed', detail: 'Now Guest', value: 'Sep 19, 10:50 PM'},
    {glyph: 'user', title: 'Sam changed', detail: 'Now Standard', value: 'Sep 19, 10:50 PM'},
    {glyph: 'user', title: 'Code changed for Sam', detail: 'By Master', value: 'Sep 19, 10:50 PM'},
    {glyph: 'check', title: 'Troubles acknowledged', detail: 'By Master', value: 'Sep 19, 10:49 PM'},
    {glyph: 'check', title: 'Alarm memory cleared', detail: 'By Master', value: 'Sep 19, 10:49 PM'},
  ].map((r) => M.row_link({...r, color: 'gray', trail: ''})).join(''),
}));

set({id: 'settings-walk', title: 'Walk test', shot: 'settings_walk_test'}, () => ({
  section: 'walk', title: 'Walk test', note: 'Open and close every door and window, walk past every motion sensor: each one beeps',
  body: [
    M.row_toggle({title: 'Include panic and 24-hour zones', detail: 'They beep instead of alarming'}),
    M.paragraph('The siren sounds for 1 s at the start · ends after 60 min'),
    M.row_action({glyph: 'footprints', text: 'Start walk test', color: 'blue', goTo: 'home-walk-test'}),
  ].join(''),
}), {main: () => ({bar: {...SET_BAR, troubles: 0, time: '10:41', wifi: 'wifi', battery: {glyph: 'battery-medium', text: '62%'}}})});

set({id: 'walk-test-list', title: 'Walk test: the zones', shot: 'walk_test_list'}, () => ({
  section: 'walk', title: 'Walk test', note: '3 of 5 tested · Ends in 59 min',
  body: [
    M.row_action({glyph: 'clock', text: 'Extend', color: 'blue'}),
    M.row_action({glyph: 'check', text: 'End test', color: 'green', goTo: 'walk-test-end-confirm'}),
    M.section('Not tested'),
    M.row_link({glyph: 'warehouse', color: 'gray', title: 'Garage Door', detail: 'Zone 2 · Not tested', trail: ''}),
    M.row_link({glyph: 'door-closed', color: 'gray', title: 'Back Door', detail: 'Zone 4 · Cover tested', trail: ''}),
    M.section('Tested'),
    M.row_link({glyph: 'check', color: 'green', title: 'Front Door', detail: 'Zone 1 · Tested 10:41 PM · 2 times', trail: ''}),
    M.row_link({glyph: 'check', color: 'green', title: 'Living Room Window', detail: 'Zone 3 · Tested 10:41 PM', trail: ''}),
    M.row_link({glyph: 'check', color: 'green', title: 'Hallway Motion', detail: 'Zone 5 · Tested 10:42 PM', trail: ''}),
  ].join(''),
}), {main: () => ({rail: {zones: 1, troubles: 1}, tone: 'blue', bar: {...SET_BAR, time: '10:42', wifi: 'wifi', battery: {glyph: 'battery-medium', text: '62%'}}})});

set({id: 'settings-about', title: 'About this panel', shot: 'about'}, () => ({
  section: 'about', title: 'About this panel',
  body: [
    M.row_status({glyph: 'shield-check', color: 'green', title: 'RoboAlarms Panel', detail: 'Version 0.6.0 · Panel ID a1b2c3d4e5f6'}),
    M.row_link({glyph: 'refresh-cw', title: 'Software update', value: 'Up to date', goTo: 'settings-software'}),
    M.row_link({glyph: 'hard-drive', color: 'violet', title: 'SD card', detail: '44.7 GB free of 59.6 GB', value: 'Ready', goTo: 'sd-card'}),
    M.row_link({glyph: 'life-buoy', color: 'gray', title: 'System details', detail: 'Versions, network and hardware, for someone helping you', goTo: 'system-details'}),
  ].join(''),
}), {notes: 'The top row is not a button: it is what support asks for first. The SD card is its own row; the panel\'s own memory is one line in System details (BL-131).'});

// ---- added with mockup-next, so its proposals have today's screen beside them ----
set({id: 'settings-time-settable', title: 'Time and language: the clock can be set', shot: 'time_settable'}, () => ({
  section: 'time', title: 'Time and language',
  body: timeBody({clock: CLOCK_NOW({title: 'Sunday, September 20 · 12:54 AM', trail: 'chevron-right', goTo: 'time-set'}),
    zone: ['Chicago, Dallas, Winnipeg · UTC-6', 'Central Time'], looks: 'Sep 20, 12:54 AM · 12,345.6'}),
}), {notes: 'Where the panel keeps a time set by hand (RTC-008), the clock\'s row opens the setter; while nothing has set the clock it is amber and says "Set".'});

// Set the date and time (BL-131 finding 3, built): a sheet over Time and language.
const ROLLERS = (w, ampm) => `<div class="time_rollers">${M.roller({label: 'Hour', items: ['10', '11', '12', '1', '2'], width: w})}${M.roller({label: 'Minute', items: ['52', '53', '54', '55', '56'], width: w})}${M.roller({items: ['', '', 'AM', 'PM', ''], width: ampm})}</div>`;
M.screen({
  id: 'time-set', area: 'Settings', title: 'Setting the date and time', shot: 'clock_set',
  render: () => M.main({view: 'settings', rail: SET_RAIL, bar: SET_BAR,
    content: M.settings({section: 'time', title: 'Time and language',
      body: timeBody({clock: CLOCK_NOW({title: 'Sunday, September 20 · 12:54 AM', trail: 'chevron-right'}),
        zone: ['Chicago, Dallas, Winnipeg · UTC-6', 'Central Time'], looks: 'Sep 20, 12:54 AM · 12,345.6'})}),
    over: M.sheet({title: 'Set the date and time', note: 'The internet corrects it once the panel is online', ok: 'Set', body: `
      <div class="row" style="gap:var(--space_xxl);align-items:flex-start;justify-content:center">
        ${M.calendar({sel: 30, today: 20})}
        <div class="col" style="gap:var(--space_sm);align-items:center;padding-top:var(--space_lg)">${ROLLERS(88, 80)}</div>
      </div>` + `<div class="result_line">${M.text('The clock will say', 'body', 'text_2')}${M.text('Wednesday, September 30, 2026 · 12:54 AM', 'label')}</div>`})}),
  notes: 'A calendar (arrows for the month; today ringed, the chosen day filled; "September 2026" opens month and year rollers where the days were, shot clock_set_month) beside Hour, Minute and AM/PM rollers, which wrap around and snap where the finger leaves them. On a 24-hour clock two rollers, 0-23 and 00-59. The calendar keeps six rows of days, so it keeps its size from month to month. Set sets the clock (the row then says "Set by hand"); Cancel leaves it as it was. Without a code the row asks for one first.',
});

M.screen({
  id: 'clock-pick', area: 'Settings', title: 'Choosing the clock', shot: 'clock_pick',
  render: () => M.main({view: 'settings', rail: SET_RAIL, bar: SET_BAR,
    content: M.settings({section: 'time', title: 'Time and language', body: timeBody({hours: '24-hour', region: 'Germany', looks: '19 Sep, 22:50 · 12.345,6'})}),
    over: M.pick_sheet({fit: true, title: 'Clock', note: 'On this keypad', rows: [
      M.row_link({title: 'Automatic', detail: '14:30, from the region', trail: 'check', trailColor: 'blue'}),
      M.row_link({title: '12-hour', detail: '2:30 PM', trail: ''}),
      M.row_link({title: '24-hour', detail: '14:30', trail: ''})].join('')})}),
  notes: 'The one place the 12/24-hour clock is chosen (BL-131 finding 2, built); Automatic says what the region gives. The sheet is as tall as its choices (finding 20, built).',
});

set({id: 'network-address', title: 'The connected network', shot: 'network_address'}, () => ({
  section: 'wifi', back: 'settings-wifi', title: 'Harbor House', note: 'Connected · good signal',
  body: [
    M.row_value({title: 'IP address', value: '192.0.2.42'}),
    M.row_link({glyph: 'sliders-horizontal', title: 'IP address settings', detail: 'Automatic from the router, or a fixed address', value: 'Automatic'}),
    M.row_link({glyph: 'server', title: 'DNS server', detail: 'Turns names into addresses', value: 'Automatic'}),
    M.row_action({glyph: 'heart-pulse', text: 'Check the connection', color: 'blue'}),
    M.section(''),
    M.row_action({glyph: 'wifi-off', text: 'Forget this network', color: 'red'}),
  ].join(''),
}));

set({id: 'settings-software', title: 'Software update: one available', shot: 'about_update_available'}, () => ({
  section: 'about', back: 'settings-about', title: 'Software update',
  body: [
    M.row_status({glyph: 'sparkles', color: 'violet', title: 'Version 0.6.1 is ready to install', detail: 'You have 0.6.0 · the alarm keeps working while it downloads'}),
    M.row_action({glyph: 'download', text: 'Install version 0.6.1', color: 'blue'}),
    M.paragraph('The panel restarts once everything is disarmed. It takes about a minute.'),
    M.row_link({glyph: 'refresh-cw', title: 'Check again', trail: ''}),
    M.section('Advanced'),
    M.row_link({glyph: 'server', color: 'gray', title: 'Update source', detail: 'Where updates come from', value: 'updates.example.org'}),
  ].join(''),
}), {notes: 'What is waiting, one Install button, Check again as a row (no toast while it looks: the row says "Checking now"), the update source under Advanced. The panel has no time of the last check yet, so Check again has no line under it.'});

set({id: 'sd-card', title: 'SD card', shot: 'sd_card'}, () => ({
  section: 'about', back: 'settings-about', title: 'SD card', note: 'Optional: nothing is kept on it yet',
  body: [
    M.row_status({glyph: 'hard-drive', color: 'green', title: 'Ready', detail: '44.7 GB free of 59.6 GB', level: 25}),
    M.row_action({glyph: 'log-out', text: 'Safely remove', color: 'blue'}),
    M.section('Card details'),
    M.row_value({title: 'Format', value: 'FAT32'}),
    M.row_value({title: 'Model', value: 'SD64G'}),
    M.section(''),
    M.row_action({glyph: 'trash-2', text: 'Format the card', color: 'red'}),
    M.paragraph('Erases everything on the card. Keep the power on until it finishes.'),
  ].join(''),
}), {notes: 'Its state and free space with a bar, Safely remove first, the details under it, Format last and apart. Without a master or installer code the buttons are one row, "Enter the master or installer code".'});

set({id: 'sd-no-card', title: 'SD card: none', shot: 'sd_no_card'}, () => ({
  section: 'about', back: 'settings-about', title: 'SD card', note: 'Optional: nothing is kept on it yet',
  body: [
    M.row_status({glyph: 'hard-drive', color: 'gray', title: 'No readable card', detail: 'Insert a card, then choose Check card.'}),
    M.row_action({glyph: 'refresh-cw', text: 'Check for a card', color: 'blue'}),
  ].join(''),
}), {notes: 'The state and the one button, nothing else.'});


set({id: 'system-details', title: 'System details', shot: 'system_details'}, () => ({
  section: 'about', back: 'settings-about', title: 'System details', note: 'What to tell someone helping you',
  body: [
    M.section('This panel'),
    M.row_value({title: 'Panel ID', value: 'a1b2c3d4e5f6'}),
    M.row_value({title: 'Version', value: '0.6.0 · built Sep 19, 2026'}),
    M.row_value({title: 'Running for', value: '3 days 4 hours'}),
    M.section('Network'),
    M.row_value({title: 'Wi-Fi', value: 'Harbor House'}),
    M.row_value({title: 'IP address', value: '192.0.2.42'}),
    M.row_value({title: 'MAC address', value: '02:00:5E:10:00:42'}),
    M.section('Set up'),
    M.row_value({title: 'Zones', value: '8 of 128'}),
    M.row_value({title: 'Users', value: '3 of 64'}),
    M.row_value({title: 'Event log', value: '43 events'}),
    M.section('Hardware'),
    M.row_value({title: 'Processor', value: 'ESP32-P4 v1.0 (v1.x image)'}),
    M.row_value({title: 'Wi-Fi module', value: 'ESP32-C6 2.12.3'}),
    M.row_value({title: 'ESP-IDF', value: 'v6.1'}),
    M.row_value({title: 'Free memory', value: '212 KB internal · 29.4 MB PSRAM'}),
    M.row_value({title: 'Firmware space', value: '2.3 MB of 4.5 MB used'}),
    M.paragraph('Made with LVGL (MIT), the Inter typeface (SIL Open Font License 1.1) and Lucide icons (ISC).'),
  ].join(''),
}), {notes: 'Grouped by what someone helping you asks first: which panel, its network, what is set up, the hardware. The Internal flash page is the one line Firmware space.'});

M.screen({
  id: 'language-pick', area: 'Settings', title: 'Choosing the language (one installed)', shot: 'language_pick',
  render: () => M.main({view: 'settings', rail: SET_RAIL, bar: SET_BAR,
    content: M.settings({section: 'time', title: 'Time and language', body: timeBody()}),
    over: M.pick_sheet({fit: true, title: 'Language', note: 'The words on this screen. Names stay as they were typed.', rows: [
      M.row_link({title: 'English', trail: 'check', trailColor: 'blue'})].join('')})}),
  notes: 'A sheet as tall as its choices (up to 520 px, then it scrolls): BL-131 finding 20, built.',
});

set({id: 'ha-pairing-code', title: 'Home Assistant: comparing the code', shot: 'ha_pairing_code'}, () => ({
  section: 'integrations', back: 'settings-integrations', title: 'Home Assistant', note: 'Found on your network and paired on this screen',
  body: [
    M.row_toggle({title: 'Home Assistant', detail: 'On · pairing', on: true}),
    M.big_value({label: 'Does Home Assistant show this code?', value: '588 460'}),
    M.action_pair({glyph: 'circle-x', text: 'No, reject', color: 'red'}, {glyph: 'circle-check', text: 'Yes, allow', color: 'green'}),
    M.paragraph('If the codes differ, something on your network is in between: reject it.', 'amber'),
  ].join(''),
}), {notes: 'The six digits big, with the question above and the two answers side by side: red on the left, green where Next usually is.'});

set({id: 'ha-paired', title: 'Home Assistant: paired', shot: 'ha_paired'}, () => ({
  section: 'integrations', back: 'settings-integrations', title: 'Home Assistant', note: 'Found on your network and paired on this screen',
  body: [
    M.row_toggle({title: 'Home Assistant', detail: 'On · connected and paired', on: true}),
    M.paragraph('Find the panel in Home Assistant under Settings > Devices & services.'),
    M.section(''),
    M.row_hold ? M.row_hold({text: 'Unpair'}) : M.row_action({glyph: 'unlink', text: 'Unpair', color: 'red'}),
    M.paragraph('Removes the panel from Home Assistant until it pairs again.'),
  ].join(''),
}), {notes: 'The library\'s name and version are not shown here. Unpair stands apart at the bottom and is held for two seconds.'});

set({id: 'ntfy-page', title: 'ntfy', shot: 'ntfy_page'}, () => ({
  section: 'integrations', back: 'settings-integrations', title: 'ntfy', note: 'An alert on your phone, with no account to make',
  body: [
    M.row_toggle({title: 'Send notifications', detail: 'Off · alerts go to every phone on this topic'}),
    M.section('Your phones'),
    M.row_link({glyph: 'qr-code', color: 'violet', title: 'Add a phone', detail: 'Install ntfy, then scan a QR code or type the topic'}),
    M.row_action({glyph: 'send', text: 'Send a test notification', color: 'blue'}),
    M.section('What to send'),
    M.row_toggle({title: 'Alarms', detail: 'Burglary, fire, carbon monoxide, panic', on: true}),
    M.row_toggle({title: 'Arming and disarming', detail: 'And who did it', on: true}),
    M.row_toggle({title: 'Troubles', detail: 'Mains power, batteries, tamper, a sensor gone quiet', on: true}),
    M.row_toggle({title: 'Zone activity', detail: 'Bypassed zones and the door chime'}),
    M.row_toggle({title: 'System and admin', detail: 'Restarts, updates, code and setting changes'}),
    M.section('Where they go'),
    M.row_link({glyph: 'globe', title: 'Server', detail: 'The free hosted service', value: 'ntfy.sh'}),
    M.row_link({glyph: 'tag', title: 'Topic', detail: 'Keep it private: anyone with it can read your alerts', value: 'roboalarms-a6efk9p7'}),
    M.row_link({glyph: 'key-round', title: 'Access token', detail: 'Only if your server asks for one', value: 'None'}),
    M.row_link({glyph: 'refresh-cw', color: 'gray', title: 'New topic', detail: 'Phones on the old topic stop getting alerts'}),
  ].join(''),
}), {notes: 'The switch with its state, adding a phone and testing it, what to send, and where the alerts go last. "Add a phone" opens the QR code with its sentence. Pushover follows the same order (switch, test, what to send, alarms, the account); MQTT: the switch, then the broker.'});

set({id: 'user-master', title: 'The master user, with browser sign-in', shot: 'web_login_master'}, () => ({
  section: 'users', back: 'settings-users', title: 'Master', note: 'User 02 · System master',
  body: [
    M.row_link({glyph: 'tag', title: 'Name', value: 'Master'}),
    M.row_link({glyph: 'shield', color: 'violet', title: 'What this code can do', detail: 'Everything, including managing users', value: 'System master', trail: ''}),
    M.row_link({glyph: 'key-round', color: 'amber', title: 'Change the code', detail: '4 to 10 digits'}),
    M.section('Panel Manager'),
    M.row_link({glyph: 'globe', title: 'Change the browser password', detail: 'User name: Master', value: 'On'}),
    M.row_action({glyph: 'log-out', text: 'Turn off browser sign-in', color: 'red'}),
  ].join(''),
}), {notes: '"Change the browser password" with "On" beside it reads as two things in one row. For other users "Delete this user" sits above this section.'});

