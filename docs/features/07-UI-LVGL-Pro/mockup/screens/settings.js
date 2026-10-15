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
// The sound on top of Screen and sound: it is changed far more often than the screen. Chime, Voice
// and Beeps each have a volume (BL-207); a build without a voice has no Voice volume and no Announcements.
const SOUND_ROWS = ({voice = true} = {}) => [
  M.section('Sound'),
  M.row_level({glyph: 'bell', title: 'Chime volume', value: 70}),
  voice ? M.row_level({glyph: 'megaphone', title: 'Voice volume', value: 70}) : '',
  M.row_level({glyph: 'volume-2', title: 'Beeps volume', value: 70}),
  M.paragraph('Alarms, fire and CO always sound at full volume.'),
  M.row_toggle({title: 'Key clicks', detail: 'A short click for every touch', on: true}),
  voice ? M.row_link({glyph: 'megaphone', title: 'Announcements', value: 'Off', goTo: 'settings-announcements-off'}) : '',
].join('');
const displayPage = (voice) => () => ({
  section: 'display', title: 'Screen and sound', note: '',
  body: [
    SOUND_ROWS({voice}),
    M.section('Screen'),
    LOOK_ROWS({}),
    M.row_level({glyph: 'sun', title: 'Brightness', value: 80}),
    M.row_link({glyph: 'image', title: 'Screen saver', detail: 'A dimmed photo with the time and the alarm state', value: 'After 1 minute', goTo: 'settings-display-saver'}),
    M.row_link({glyph: 'monitor-off', title: 'Screen off', detail: 'Dark until a touch wakes it', value: 'Never'}),
    M.row_link({glyph: 'moon', color: 'violet', title: 'Night hours', detail: '10:00 PM to 7:00 AM · dimmer, and dark sooner', value: 'Off', goTo: 'settings-display-night'}),
  ].join(''),
});
set({id: 'settings-display', title: 'Screen and sound', shot: 'settings_display'}, displayPage(true), {notes: 'Sound, then Screen: the sound is changed far more often (decided 2026-10-05; BL-131 finding 4 had the screen first). Three sliders, Chime, Voice, Beeps (BL-207, AUD-032), each 0-100 % on its own and saved as it moves; no sample sound plays as one moves. Beeps volume is the old Volume: entry and exit, trouble, the walk test, key clicks and the other keypad beeps. Alarms, fire and CO stay at full volume (AUD-003). The appearance first among the screen rows (BL-132, built): a change applies at once, in one step, and needs no code, like the brightness. No Chime row: the status bar\'s Chime switch is its one place. Announcements, under Sound, opens its own page and says On or Off (BL-202, D06; only in a build with a voice). The slider rows put their icon in a chip, so the titles line up with the rows below. Rows below the fold follow ui_settings.c build_display().'});
set({id: 'settings-display-no-voice', title: 'Screen and sound in a build without a voice', shot: null}, displayPage(false), {notes: 'A build without the voice has no announcements, so no Voice volume and no Announcements row: Chime volume and Beeps volume only.'});

// Screen and sound > Announcements (BL-202; docs/features/08 voice-first-build.md D06): a child
// page of Screen and sound, with its access (anyone), saved at once like its other switches.
const ANN_WHY = 'Announcements are off until someone turns them on; the page says what they need and why one can\'t be turned on. No Walk test switch: the walk test always says a zone\'s words when it has them, Announcements off too (WALK-016; decided 2026-10-12). No voice language or pack rows: Time and language sets the language, and the page says only whether the voice works (decided 2026-10-12).';
const annBody = ({on = false, voice = 'ready', refused = ''}) => {
  const ready = voice === 'ready';
  return [
    M.row_toggle({glyph: 'megaphone', title: 'Announcements', detail: ready ? 'The panel says what is happening, in words' : voice === 'none' ? 'Needs a voice pack' : 'Needs working voice', on}),
    refused ? M.paragraph(refused, 'amber') : '',
    M.row_toggle({title: 'Status messages', detail: 'Says arming, armed, disarmed, entry and not ready messages.', on: true, disabled: !on}),
    M.row_toggle({title: 'Zone chime', detail: 'Says the zone’s spoken name after the chime.', on: true, disabled: !on}),
    voice === 'unavailable' ? M.paragraph('Voice is unavailable.', 'amber') : '',
    M.row_action({glyph: 'volume-2', text: 'Preview', color: 'blue', disabled: !ready}),
    M.paragraph('Uses the Voice volume in Screen and sound. Alarms, fire and CO always sound at full volume.'),
  ].join('');
};
const ann = (id, title, shot, opts, notes) => set({id, title, shot}, () => ({
  section: 'display', title: 'Announcements', note: 'Screen and sound', back: 'settings-display', body: annBody(opts),
}), {notes: notes + ' ' + ANN_WHY});
ann('settings-announcements-off', 'Announcements: off, as shipped', 'voice_announcements_off', {},
  'D06. Back returns to Screen and sound, where the Voice volume stays. On by default, as on modern panels (D06, 2026-10-12); turned off, the other two keep their choices but are dimmed. Preview plays a sample name ("Front Door" with the en-US voice) with the switch off too, and changes nothing; it respects volume, power saving and alarms. Zone words stay installer-only (Installer > Zones); installing or removing a pack stays system-master/installer-only (the Panel Manager).');
ann('settings-announcements-on', 'Announcements: on', 'voice_announcements_on', {on: true},
  'The switches are live. Voice volume zero stays silent; Preview plays at the Voice volume set in Screen and sound.');
ann('settings-announcements-refused', 'Announcements: no voice pack', 'voice_announcements_refused',
  {voice: 'none', refused: 'Announcements cannot be turned on because the voice pack is not installed.'},
  'Turning on is refused with the reason in a line, not a toast; the line stays until the page is left. Installing a pack never turns them on.');
ann('settings-announcements-unavailable', 'Announcements: pack installed, voice unavailable', 'voice_announcements_unavailable',
  {voice: 'unavailable', refused: 'Announcements cannot be turned on because voice is unavailable.'},
  'A pack is present but not ready (still being checked at start-up, or not usable): the signed pack, the mandatory clips and the decoder must all be valid before Announcements can be turned on. The page says "Voice is unavailable."');

M.screen({
  id: 'settings-appearance', area: 'Settings', title: 'Choosing the appearance', shot: 'appearance_pick',
  notes: 'BL-132, built: the three appearances, each saying when it suits. Automatic follows the night hours (UI-043): dark during them, light by day, switching once at each end; with the night hours off it says "Night hours are off: stays light". The screen saver keeps the dark colours in every appearance.',
  render: () => M.main({view: 'settings', rail: SET_RAIL, bar: SET_BAR,
    content: M.settings({section: 'display', title: 'Screen and sound', body: [
      SOUND_ROWS,
      M.section('Screen'),
      LOOK_ROWS({appearance: 'Automatic'}),
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
  id: 'keyboard-without-letters', area: 'Settings', title: 'Typing a name, without the letters pack', shot: 'keyboard_without_letters',
  notes: 'BL-124 (docs/features/39 asset-packs.md, OPT-014): without the letters pack the panel cannot draw the accent ' +
    'pages\' letters, so the letters have no àé key and the space bar takes its place; 123, the symbols and ' +
    'everything else stay. The key comes back once the letters pack is loaded (installed by the Flasher, fetched ' +
    'after an update, or put on by hand), the next time a keyboard opens. Names typed elsewhere (the Panel ' +
    'Manager, a backup) with a letter the panel lacks show that letter as a box until then (the preview\'s ' +
    'users_without_letters). No message on the keyboard: the pack is normally there.',
  render: () => M.main({view: 'settings', rail: SET_RAIL, bar: SET_BAR,
    content: M.settings({section: 'users', title: 'User 03', body: ''}),
    over: M.text_sheet({title: 'Name', note: 'Shown in the event log', value: 'Sam', page: 'plain'})}),
});

M.screen({
  id: 'keyboard-accents', area: 'Settings', title: 'The accent page, with the letters pack', shot: 'keyboard_accents',
  notes: 'àé on the letters opens this page (I18N-019), čł the second one (Polish, Czech, Turkish, Hungarian), abc ' +
    'goes back, shift gives the capitals. The preview\'s picture shows the second page\'s capitals.',
  render: () => M.main({view: 'settings', rail: SET_RAIL, bar: SET_BAR,
    content: M.settings({section: 'users', title: 'User 03', body: ''}),
    over: M.text_sheet({title: 'Name', note: 'Shown in the event log', value: 'Zo', page: 'accent'})}),
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

// A password can be seen before it is sent (BL-131): every password field shares the text sheet.
const PASSWORD = {title: 'Password for Maple Street', note: 'Letters and symbols: 123 and #+= switch the keys', value: 'correct horse', password: true};
set({id: 'wifi-password', title: 'Wi-Fi: typing a password', shot: 'wifi_password'}, () => ({
  section: 'wifi', title: 'Wi-Fi and network', note: 'The panel keeps protecting without the network',
}), {notes: 'The text sheet (ui_forms.c) in password mode: dots, and an eye at the end of the field. Every password field has it (Wi-Fi here and in the setup, the browser sign-in, MQTT, the ntfy and Pushover tokens), and each sheet opens with it hidden: the choice isn\'t kept.',
  main: () => ({over: M.text_sheet(PASSWORD)})});

set({id: 'wifi-password-shown', title: 'Wi-Fi: the password shown', shot: 'wifi_password_shown'}, () => ({
  section: 'wifi', title: 'Wi-Fi and network', note: 'The panel keeps protecting without the network',
}), {notes: 'After a tap on the eye: the letters in the clear and the eye crossed out, to hide them again. The cursor and the keyboard stay where they were.',
  main: () => ({over: M.text_sheet({...PASSWORD, shown: true})})});

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

