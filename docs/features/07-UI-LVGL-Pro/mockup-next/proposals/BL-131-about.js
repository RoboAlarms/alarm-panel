// BL-131: About this panel (mockup-next). The three things a person comes here for: is the
// software current, is the SD card all right, and what to tell someone who is helping them.
// The Storage page and the Internal flash page go: the SD card is a row of About, and the
// panel's own memory is one line in System details (the Panel Manager keeps the full numbers).

const RAIL = {troubles: 1};
const BAR = {time: '10:52', troubles: 1, wifi: 'wifi'};
const GO = {display: 'n-display', users: 'settings-users', 'wifi': 'n-wifi', time: 'n-time', integrations: 'n-integrations',
  walk: 'settings-walk', about: 'n-about', installer: 'n-installer-menu'};
const P = (replaces, why) => ({bl: 'BL-131', status: 'approved', decided: '2026-10-01: approved as drawn: "start working on getting all of the screen changes in place"', replaces, why});
const page = ({id, label, title, replaces, why, notes, over, toast, ...s}) => M.screen({
  id, area: 'About', title: label || title, shot: null, proposal: P(replaces, why), notes,
  render: () => M.main({next: true, view: 'settings', rail: RAIL, bar: BAR, toast,
    content: M.settings({next: true, go: GO, section: 'about', title, ...s}), over: over ? over() : ''}),
});

page({
  id: 'n-about', title: 'About this panel', replaces: 'settings-about',
  why: 'Storage mixed the SD card (something you put in and take out) with the internal flash (partition sizes nobody can change), and every row said what its page is for, never how things are. Now: the panel and its version on top, then Software update, the SD card and System details, each with its state.',
  notes: 'The top row is not a button: it is what support asks for first. Storage and Internal flash are gone; Firmware is no longer listed twice.',
  body: [
    M.changed(M.row_status({glyph: 'shield-check', color: 'green', title: 'RoboAlarms Panel', detail: 'Version 0.6.0 · Panel ID a1b2c3d4e5f6'})),
    M.changed([
      M.row_link({glyph: 'refresh-cw', title: 'Software update', detail: 'Checked today at 3:00 AM', value: 'Up to date', goTo: 'n-update'}),
      M.row_link({glyph: 'hard-drive', color: 'violet', title: 'SD card', detail: '44.7 GB free of 59.6 GB', value: 'Ready', goTo: 'n-sd'}),
      M.row_link({glyph: 'life-buoy', color: 'gray', title: 'System details', detail: 'Versions, network and hardware, for someone helping you', goTo: 'n-system-details'}),
    ].join('')),
  ].join(''),
});

page({
  id: 'n-about-update', label: 'About this panel: an update waiting', title: 'About this panel', replaces: 'settings-about',
  why: 'When a new version is waiting, About says so in the row itself, in violet, and its line in the list on the left carries an Update tag, so nobody has to open the page to find out.',
  tags: {about: {text: 'Update', tone: 'violet'}},
  body: [
    M.row_status({glyph: 'shield-check', color: 'green', title: 'RoboAlarms Panel', detail: 'Version 0.6.0 · Panel ID a1b2c3d4e5f6'}),
    M.changed(M.row_link({glyph: 'sparkles', color: 'violet', title: 'Software update', detail: 'Version 0.6.1 is ready to install', value: 'Install', goTo: 'n-update'})),
    M.row_link({glyph: 'hard-drive', color: 'violet', title: 'SD card', detail: '44.7 GB free of 59.6 GB', value: 'Ready', goTo: 'n-sd'}),
    M.row_link({glyph: 'life-buoy', color: 'gray', title: 'System details', detail: 'Versions, network and hardware, for someone helping you', goTo: 'n-system-details'}),
  ].join(''),
});

page({
  id: 'n-update', title: 'Software update', replaces: 'settings-software', back: 'n-about',
  why: 'The section caption repeated the title, Install and Check for updates were two equal buttons, a toast said "Checking for updates" over it all, and the update source (a web address few should change) sat among them. Now: what is waiting, one Install button, Check again as a plain row with the time it last looked, the source under Advanced.',
  notes: '"Last checked" needs the time of the update service\'s last check. The update source could be installer-only (docs/features/14). While a check runs, the Check again row shows a spinner instead of a toast.',
  body: [
    M.changed(M.row_status({glyph: 'sparkles', color: 'violet', title: 'Version 0.6.1 is ready to install', detail: 'You have 0.6.0 · the alarm keeps working while it downloads'})),
    M.changed(M.row_action({glyph: 'download', text: 'Install version 0.6.1', color: 'blue'})),
    M.paragraph('The panel restarts once everything is disarmed. It takes about a minute.'),
    M.changed(M.row_link({glyph: 'refresh-cw', title: 'Check again', detail: 'Last checked today at 3:00 AM'})),
    M.changed(M.section('Advanced')),
    M.row_link({glyph: 'server', color: 'gray', title: 'Update source', detail: 'Where updates come from', value: 'updates.example.org'}),
  ].join(''),
});

page({
  id: 'n-sd', title: 'SD card', replaces: 'sd-card', back: 'n-about',
  why: 'Seven value lines and a blank gap, then three equal buttons below the fold, and nothing said what the card is for. Now: its state and free space with a bar, one line on what it is for, Safely remove first, the details under it, and Format last, apart. GB, not GiB.',
  notes: '"Check card" shows only when there is no card or after Safely remove. The FAT12/16/32 sentence moves to the Unsupported card state, the one place it helps. Used and Available are one line ("44.7 GB free of 59.6 GB"). What the card is for is the page\'s note, the same with a card and without.',
  note: 'Optional: nothing is kept on it yet',
  body: [
    M.changed(M.row_status({glyph: 'hard-drive', color: 'green', title: 'Ready', detail: '44.7 GB free of 59.6 GB', level: 25})),
    M.row_action({glyph: 'log-out', text: 'Safely remove', color: 'blue'}),
    M.changed(M.section('Card details')),
    M.row_value({title: 'Format', value: 'FAT32'}),
    M.row_value({title: 'Model', value: 'SD64G'}),
    M.section(''),
    M.changed(M.row_action({glyph: 'trash-2', text: 'Format the card', color: 'red'})),
    M.paragraph('Erases everything on the card. Keep the power on until it finishes.'),
  ].join(''),
});

page({
  id: 'n-sd-none', label: 'SD card: no card', title: 'SD card', replaces: 'sd-no-card', back: 'n-about',
  why: 'With no card, five lines said "Unavailable" and the one useful button was below the fold. Now the state, the button, and nothing else.',
  note: 'Optional: nothing is kept on it yet',
  body: [
    M.changed(M.row_status({glyph: 'hard-drive', color: 'gray', title: 'No card', detail: 'Insert a card, then check'})),
    M.changed(M.row_action({glyph: 'refresh-cw', text: 'Check for a card', color: 'blue'})),
  ].join(''),
});

page({
  id: 'n-system-details', title: 'System details', replaces: 'system-details', back: 'n-about',
  why: 'Grouped by what someone helping you asks first: which panel and version, its network, what is set up, then the hardware. The Internal flash page becomes one line, Firmware space. The "Network" line\'s stray dot (a bug) goes: the Wi-Fi name and the address get a line each.',
  notes: 'Days and hours, not "3 d 4 h 12 min". The Panel ID shows here and on About.',
  title: 'System details', note: 'What to tell someone helping you',
  body: [
    M.changed(M.section('This panel')),
    M.row_value({title: 'Panel ID', value: 'a1b2c3d4e5f6'}),
    M.row_value({title: 'Version', value: '0.6.0 · built Sep 19, 2026'}),
    M.row_value({title: 'Running for', value: '3 days 4 hours'}),
    M.section('Network'),
    M.changed([
      M.row_value({title: 'Wi-Fi', value: 'Harbor House'}),
      M.row_value({title: 'IP address', value: '192.0.2.42'}),
    ].join('')),
    M.row_value({title: 'MAC address', value: '02:00:5E:10:00:42'}),
    M.changed(M.section('Set up')),
    M.row_value({title: 'Zones', value: '8 of 128'}),
    M.row_value({title: 'Users', value: '3 of 64'}),
    M.row_value({title: 'Event log', value: '43 events'}),
    M.section('Hardware'),
    M.row_value({title: 'Processor', value: 'ESP32-P4 v1.0'}),
    M.row_value({title: 'Wi-Fi module', value: 'ESP32-C6 · 2.12.3'}),
    M.row_value({title: 'ESP-IDF', value: 'v6.1'}),
    M.row_value({title: 'Free memory', value: '212 KB internal · 29.4 MB PSRAM'}),
    M.changed(M.row_value({title: 'Firmware space', value: '2.3 MB of 4.5 MB used'})),
    M.paragraph('Made with LVGL (MIT), the Inter typeface (SIL Open Font License 1.1) and Lucide icons (ISC).'),
  ].join(''),
});
