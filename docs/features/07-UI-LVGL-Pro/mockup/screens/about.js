// BL-189: About holds what System details showed, and Installer opens from the bottom of About instead
// of the Settings list (decided 2026-10-07: "take the System Details and move it directly into the
// About Screen under SD card. Then move the Installer Menu to be under that."). The Settings list ends
// with About; the installer's pages count as About's.

const ABOUT_SECTIONS = M.settings_sections({drop: ['installer']}).filter((s) => !s.divider);
const ABOUT_RAIL = {troubles: 1};
const ABOUT_BAR = {time: '10:58', troubles: 1};
const about = ({id, shot, title, notes, body, installer = false}) => M.screen({
  id, area: 'Settings', title, shot, notes,
  render: () => M.main({view: 'settings', tone: installer ? 'blue' : 'green', rail: installer ? {troubles: 1, tone: 'blue'} : ABOUT_RAIL,
    bar: installer ? {banner: 'installer', time: '10:58', troubles: 1} : ABOUT_BAR,
    content: M.settings({section: 'about', sections: ABOUT_SECTIONS, ...body})}),
});

const ABOUT_TOP = [
  M.row_status({glyph: 'shield-check', color: 'green', title: 'RoboAlarms Panel', detail: 'Version 0.6.0 · Panel ID a1b2c3d4e5f6'}),
  M.row_link({glyph: 'refresh-cw', title: 'Software update', value: 'Up to date', goTo: 'settings-update'}),
  M.row_link({glyph: 'hard-drive', color: 'violet', title: 'SD card', detail: '44.7 GB free of 59.6 GB', value: 'Ready'}),
];
const DETAILS_START = [
  M.section('This panel'),
  M.row_value({title: 'Built', value: 'Sep 19, 2026'}),
  M.row_value({title: 'Running for', value: '3 days'}),
  M.section('Network'),
  M.row_value({title: 'Wi-Fi', value: 'Harbor House'}),
  M.row_value({title: 'IP address', value: '192.0.2.42'}),
  M.row_value({title: 'MAC address', value: '02:00:00:1A:2B:3C'}),
  M.section('Set up'),
  M.row_value({title: 'Zones', value: '8 of 128'}),
  M.row_value({title: 'Users', value: '3 of 64'}),
  M.row_value({title: 'Event log', value: '52 events'}),
];
const DETAILS_END = [
  M.section('Hardware'),
  M.row_value({title: 'Processor', value: 'ESP32-P4 v1.0 (v1.x image)'}),
  M.row_value({title: 'Wi-Fi module', value: 'ESP32-C6 2.12.3'}),
  M.row_value({title: 'ESP-IDF', value: 'v6.1'}),
  M.row_value({title: 'Free memory', value: '214 KB internal · 29.1 MB PSRAM'}),
  M.row_value({title: 'Firmware space', value: '2.3 MB of 4.5 MB used'}),
];
const INSTALLER_ROW = M.row_link({glyph: 'wrench', color: 'blue', title: 'Installer', detail: 'Changes how the alarm works · needs the installer code', goTo: 'installer-locked'});
const CREDITS = M.paragraph('Made with LVGL (MIT), the Inter typeface (SIL Open Font License 1.1), Lucide icons (ISC) and parts of Z-Wave JS (MIT).');

about({id: 'settings-about', shot: 'about', title: 'About: System details on the page, under SD card',
  notes: 'The rows System details had, in its order, after SD card: This panel, Network, Set up, Hardware; its Panel ID and Version are left out because the top row has them, and Version\'s build date becomes Built. The words and values are today\'s (ui_settings.c build_system_details). The page scrolls; its end is the next screen. Read-only rows, as before, for anyone who opens About. Software update\'s row says only "Up to date", "Update" or "Updating" to anyone; opening it asks for the installer or system master code (BL-186, BL-188).',
  body: {title: 'About this panel', body: [...ABOUT_TOP, DETAILS_START.join('')].join('')}});

about({id: 'settings-about-end', shot: 'about_end', title: 'About, scrolled to the end: Installer under the details',
  notes: 'Drawn scrolled to the end (Set up is above Hardware). Installer looks like any row and opens the page Settings > Installer opens today: Enter the installer code, then the Installer menu. The licence line stays last. The Settings list ends with About: no divider, no Installer. Who may do what doesn\'t change: the installer code opens the menu.',
  body: {title: 'About this panel', body: [DETAILS_END.join(''), M.section(''), INSTALLER_ROW, CREDITS].join('')}});

about({id: 'installer-locked', shot: 'installer_locked', title: 'Installer, opened from About',
  notes: 'The page itself is BL-187\'s (no factory code on show). Lost the installer code? as today.',
  body: {title: 'Installer', note: 'Installer settings change how the alarm works', back: 'settings-about-end', body: [
    M.row_action({glyph: 'key-round', text: 'Enter the installer code', color: 'amber'}),
    M.row_link({glyph: 'life-buoy', color: 'gray', title: 'Lost the installer code?', goTo: 'installer-lost-code'}),
    M.paragraph('While you change settings nothing can be armed. Fire, CO and panic buttons keep working.'),
  ].join('')}});

about({id: 'installer-menu', shot: 'installer_menu', title: 'Installer mode: the menu, under About', installer: true,
  notes: 'Back goes to About; installer mode stays on until Leave installer mode or a tap on the banner. Four groups plus Updates, the risky ones last (BL-131 finding 18); the update server and the channel are the installer\'s (OTA-020, OTA-021) and are drawn on their own screens. With Z-Wave built in, a Z-Wave row follows Zones (docs/features/36). The Panel Manager is one switch with the installer\'s own sign-in under it; while it runs, its detail is its address and certificate (WEB-002). Installer mode\'s pages (Zones, Delays and arming ...) keep About lit in the list.',
  body: {title: 'Installer', back: 'settings-about-end', body: [
    M.section('Sensors and devices'),
    M.row_link({glyph: 'door-closed', color: 'green', title: 'Zones', detail: 'Sensors, types, names', value: '8 in use', goTo: 'installer-zones'}),
    M.row_link({glyph: 'warehouse', color: 'violet', title: 'Partitions', detail: 'Areas that arm separately', value: '1 in use'}),
    M.row_link({glyph: 'cpu', title: 'Keypads and devices', detail: 'Supervised keypads and expanders', value: 'None', goTo: 'installer-devices'}),
    M.section('How the alarm works'),
    M.row_link({glyph: 'sliders-horizontal', title: 'Delays and arming', detail: 'Entry and exit times, sirens, panic keys, codes', goTo: 'installer-options'}),
    M.section('This panel'),
    M.row_link({glyph: 'house', color: 'violet', title: 'System name', detail: 'Shown on the screen and in the event log', value: 'Home'}),
    M.row_link({glyph: 'key-round', color: 'amber', title: 'Installer and master codes', detail: 'Factory codes still in use', goTo: 'installer-codes'}),
    M.row_toggle({glyph: 'monitor', title: 'Panel Manager', detail: 'Change settings from a browser on your network'}),
    M.row_toggle({glyph: 'globe', title: 'Browser sign-in', detail: 'User name: Installer · a password, not the keypad code'}),
    M.row_toggle({glyph: 'code-xml', color: 'gray', title: 'Developer mode', detail: 'Enrolled development tools and signed network updates'}),
    M.section('Updates'),
    M.row_link({glyph: 'server', color: 'green', title: 'Update server', detail: 'Where updates come from', value: 'roboalarms.com', goTo: 'installer-update-server'}),
    M.row_link({glyph: 'layers', title: 'Update channel', detail: 'Which releases this panel is offered', value: 'Stable', goTo: 'installer-update-channel'}),
    M.section('Test and start over'),
    M.row_link({glyph: 'footprints', title: 'Walk test', detail: 'Leaves installer mode and starts one'}),
    M.row_link({glyph: 'sparkles', color: 'rose', title: 'Run the setup again', detail: 'Wi-Fi, time zone, codes and name, step by step', goTo: 'setup-welcome'}),
    M.row_link({glyph: 'rotate-ccw', color: 'red', title: 'Factory reset', detail: 'Erase everything', goTo: 'factory-reset'}),
    M.row_action({glyph: 'log-out', text: 'Leave installer mode', color: 'blue'}),
  ].join('')}});

// Installer > Keypads and devices (ui_installer.c build_devices, build_device): the engine's table of
// supervised devices, every one of its 16 numbers a row, and a device's page. No preview picture yet.
about({id: 'installer-devices', shot: null, title: 'Installer: Keypads and devices', installer: true,
  notes: 'A row for each of the 16 device numbers, in order: the name (or "Device 4"), and its partition ("All partitions", the partition\'s name) when in use, else "Not in use" with a gray chip. A fresh panel has none in use, so the menu row says None. The page scrolls; the rows under the fold are Device 6 to Device 16.',
  body: {title: 'Keypads and devices', note: 'A supervised device that stops answering shows a trouble', back: 'installer-menu', body:
    Array.from({length: 16}, (_, i) => M.row_link({glyph: 'cpu', color: 'gray', title: `Device ${i + 1}`, detail: 'Not in use',
      goTo: i === 0 ? 'installer-device' : ''})).join('')}});

about({id: 'installer-device', shot: null, title: 'Installer: one supervised device', installer: true,
  notes: 'Device 1 just switched on. Off, the page has the switch alone. Name opens the keyboard ("Hall keypad, Garage expander..."); Home partition a pick sheet (All partitions, then each partition in use; "The partition a keypad shows and arms"); Supervision a pick sheet (Not supervised, 1, 4 or 12 hours; "Silent this long: a trouble"). Saved when the page is left.',
  body: {title: 'Device 1', note: 'Device 1 · saved when you go back', back: 'installer-devices', body: [
    M.row_toggle({title: 'In use', detail: 'Supervised: the panel expects it to answer', on: true}),
    M.row_link({glyph: 'tag', title: 'Name'}),
    M.row_link({glyph: 'warehouse', color: 'violet', title: 'Home partition', value: 'All partitions'}),
    M.row_link({glyph: 'clock-alert', title: 'Supervision', detail: 'Silent this long: a trouble', value: 'Not supervised'}),
  ].join('')}});
