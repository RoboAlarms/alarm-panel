// BL-188 (ui_update.c): an update's progress from the download to the self-test, and Stop the update.
// A level bar and the size while it downloads, the check of the download, the wait for disarm, the
// restart and the self-test, with Stop until the restart. Software update is the same page for the
// installer and the system master (BL-186). Nothing about an update covers or blocks Home, arming, the
// code pad, Panic or an alarm: the restart is told in the status bar banner, never in a sheet. Stage,
// steps, the banner and Stop are for a release install only; the Wi-Fi module's update and a developer
// file have their own words (settings-update-wifi-module).
// The Panel Manager follows the same run (docs/features/31).

const PROG_BAR = {time: '10:58', troubles: 1};
const PROG_RAIL = {troubles: 1};
const UPDATING = {about: {text: 'Updating', tone: 'blue'}};
// The Settings list as BL-189 has it: Installer opens from About.
const PROG_SECTIONS = M.settings_sections({drop: ['installer']}).filter((s) => !s.divider);
// The status bar's update banner (it comes before the factory-codes one); none once the run has ended.
const BANNER = (text, glyph = 'download') => ({...PROG_BAR, banner: {glyph, tone: 'blue', text}});
const prog = ({id, shot, title, notes, body, over, tags = UPDATING, bar = BANNER('Downloading an update')}) => M.screen({
  id, area: 'Settings', title, shot, notes,
  render: () => M.main({view: 'settings', rail: PROG_RAIL, bar, over,
    content: M.settings({section: 'about', sections: PROG_SECTIONS, title: 'Software update', back: 'about-update-running', tags, body: body.join('')})}),
});
const homeScreen = ({id, shot, title, notes, render}) => M.screen({id, area: 'Home', title, shot, notes,
  render});

// The steps, the one running in blue; done ones are green, the ones to come gray.
const STEPS = [
  ['Download', '2.7 MB'],
  ['Check the download', 'Its signature and checksum'],
  ['Wait until everything is disarmed', 'Right away when it already is'],
  ['Restart with the new version', 'About a minute; the self-test starts at once'],
  ['Self-test', 'Usually a minute, at most three · restarts with 0.6.0 if it fails'],
];
const steps = (now) => [M.section('Steps'), ...STEPS.map(([title, detail], i) => M.row_link({
  glyph: i < now ? 'circle-check' : i === now ? 'circle-dot' : 'circle', color: i < now ? 'green' : i === now ? 'blue' : 'gray',
  title, detail, trail: ''}))].join('');
const STOP = M.row_action({glyph: 'circle-x', text: 'Stop the update', color: 'red', goTo: 'settings-update-stop-question'});
const OFFERED = (status) => [
  status,
  M.row_action({glyph: 'download', text: 'Install version 0.6.1', color: 'blue'}),
  M.paragraph('The panel restarts once everything is disarmed. It takes about a minute.'),
  M.row_link({glyph: 'refresh-cw', title: 'Check again', detail: 'Checked 10:58 PM', trail: ''}),
];

const DOWNLOADING = [
  M.row_status({glyph: 'download', color: 'blue', title: 'Downloading version 0.6.1', detail: '1.1 of 2.7 MB · 42 %', level: 42}),
  STOP,
  M.paragraph('The alarm keeps working while it downloads. You can stop the update until the panel restarts.'),
  steps(0),
];

// ---- before: the question ----

prog({id: 'settings-update-install-question', shot: 'upd_install_question', bar: PROG_BAR, title: 'Install: the question, saying it can be stopped',
  notes: 'Was "The panel downloads it now and restarts once everything is disarmed. It keeps protecting until then." with a red Install. After Install the page shows the download at once, so the toast "Downloading the update" goes (it said what the screen shows). Nothing is ever kept from an earlier run, so the question is always this one. The firmware takes an Install only when nothing runs (IDLE, FAILED, or during a plain check, which it then follows): otherwise the refusal toast "An update is already running", as today. An Install it can\'t start at once (another update holds the slot: a language pack\'s check) waits; after 30 s it ends as "Update failed · Another update is running".',
  tags: {about: {text: 'Update', tone: 'violet'}},
  body: OFFERED(M.row_link({glyph: 'sparkles', color: 'violet', title: 'Version 0.6.1 is ready to install', detail: 'You have 0.6.0 · the alarm keeps working while it downloads', trail: ''})),
  over: M.confirm_dialog({title: 'Install version 0.6.1?', tone: 'primary', ok: 'Install',
    note: 'The panel downloads it now and restarts once everything is disarmed. It keeps protecting while it downloads and waits, and you can stop the update until it restarts.'})});

// ---- running ----

prog({id: 'settings-update-downloading', shot: 'upd_downloading', title: 'Downloading: the bar, the size and Stop the update',
  notes: 'The level bar is the list row\'s own (ui_row_status level), stepped to the whole percent at most once a second with no animation (LV_ANIM_OFF), like the SD card\'s; the title doesn\'t change while it downloads, only the detail ("1.1 of 2.7 MB · 42 %", the catalog\'s percent). Before the first bytes the detail says "Starting the download" and the bar is empty; that covers an Install that has to ask the server first. Software update opens for the installer and the system master only (BL-186); Stop needs the same code (ACCESS_SYSTEM). Check again is hidden while an update runs, and Installer > Updates holds the server and the channel (the Installer menu, below). Arming closes Settings as today; the Home banner says the rest.',
  body: DOWNLOADING});

prog({id: 'settings-update-checking', shot: 'upd_checking', bar: BANNER('Checking the download', 'shield-check'), title: 'Checking the download',
  notes: 'A few seconds (the SHA-256 and the signature, esp_ota_end). Stop still works: the firmware reads its stop flag after the check and throws the download away instead of going on to wait.',
  body: [
    M.row_status({glyph: 'shield-check', color: 'blue', title: 'Checking the download', detail: 'Its signature and checksum · a few seconds', level: 100}),
    STOP,
    M.paragraph('The alarm keeps working while it checks. You can stop the update until the panel restarts.'),
    steps(1),
  ]});

prog({id: 'settings-update-waiting', shot: 'upd_waiting', bar: BANNER('Update waiting · installs once disarmed', 'clock'), title: 'Waiting until everything is disarmed',
  notes: 'Named "Waiting", not "Ready": "Version 0.6.1 is ready to install" asks for Install, this one installs by itself. One detail and one paragraph per reason the panel gives: "Installer mode is on" (Installs once installer mode is off / Leave installer mode and the panel restarts to install it.), "The system is armed" (Installs once everything is disarmed / Disarm everything ...), "An alarm is sounding" (Installs once the alarm is silenced / Silence the alarm ...), "A walk test is running" (Installs once the walk test ends / End the walk test ...), "Trying again shortly" (the next screen). Stop here throws the checked download away, as at every step: Install later downloads it again. A restart while it waits (a power cut) forgets the update: the panel keeps 0.6.0 and Software update offers 0.6.1 again.',
  body: [
    M.row_status({glyph: 'clock', color: 'amber', title: 'Waiting to install version 0.6.1', detail: 'Installs once installer mode is off · Installer mode is on'}),
    STOP,
    M.paragraph('Leave installer mode and the panel restarts to install it. It takes about a minute.'),
    steps(2),
  ]});

prog({id: 'settings-update-trying-again', shot: 'upd_trying_again', bar: BANNER('Update waiting · installs once disarmed', 'clock'), title: 'Waiting: the restart was put off, trying again shortly',
  notes: 'The panel gives a restart up when someone arms, an alarm starts or Stop lands in the seconds it holds the restart (OTA-004: it costs the attempt, not the protection). It goes back to waiting with that reason; once everything is quiet again it waits 30 s before the next attempt, with the reason "Trying again shortly". Activity: the firmware adds the appended stage "restart put off" after "Installing an update", so Activity doesn\'t end on an install that didn\'t happen.',
  body: [
    M.row_status({glyph: 'clock', color: 'amber', title: 'Waiting to install version 0.6.1', detail: 'Installs within 30 seconds · Trying again shortly'}),
    STOP,
    M.paragraph('The system got busy as it was about to restart. It tries again within 30 seconds.'),
    steps(2),
  ]});

prog({id: 'settings-update-stop-question', shot: 'upd_stop_question', title: 'Stop the update?',
  notes: 'Keep updating closes the question (the cancel button, named for what it does: ui_confirm_choice). Stop it: the page says "Stopping the update" until it lands, within a few seconds, or up to a minute when the connection has stalled; the download is thrown away at every step. A Stop sent in the seconds before the restart still lands: the panel gives the restart up. If the run ends while the question is open (stopped from the Panel Manager, failed, or the restart begins), the question closes, as Z-Wave\'s does (UX-033), and the page shows what happened; a Stop that reaches a panel with nothing running gets the toast "It isn\'t running any more". Activity: "Update stopped", by Alex (a new appended firmware stage, "stopped", which MQTT and the Home Assistant link carry; RoboAlarms/ha-addon learns it in the same change).',
  body: DOWNLOADING,
  over: M.confirm_dialog({title: 'Stop the update?', note: 'The panel keeps version 0.6.0. You can install 0.6.1 again later; it downloads it again.', ok: 'Stop it', cancel: 'Keep updating'})});

prog({id: 'settings-update-stopping', shot: 'upd_stopping', bar: BANNER('Stopping the update', 'circle-x'), title: 'Stopping: until the stop lands',
  notes: 'The steps stay where the stop found them; no Stop, no bar. The firmware reads its stop flag between pieces, between the server\'s redirects, after each blocking call and before it calls a stalled read a failure, so a stop never ends as "Update failed". The Home banner says "Stopping the update".',
  body: [
    M.row_status({glyph: 'circle-x', color: 'gray', title: 'Stopping the update', detail: 'Nothing is changed · a few seconds, up to a minute on a slow connection'}),
    steps(0),
  ]});

prog({id: 'settings-update-stopped', shot: 'upd_stopped', bar: PROG_BAR, title: 'Stopped',
  notes: 'The panel keeps the result, so this page and the Panel Manager say the same for the same time: until the next Install or Check again. The page follows the panel once a second: an update started anywhere else replaces this at once with its progress. "By" is the user\'s name, or "the panel" for a stop with no one signed in.',
  tags: {about: {text: 'Update', tone: 'violet'}},
  body: OFFERED(M.row_status({glyph: 'circle-x', color: 'gray', title: 'Update stopped', detail: 'Stopped by Alex · you have 0.6.0, nothing was changed'}))});

prog({id: 'settings-update-failed', shot: 'upd_failed', bar: PROG_BAR, title: 'Failed: the download broke off',
  notes: 'The panel\'s reasons, as today ("The update server can\'t be reached", "The download broke off", "Signature or image check failed" ...). Only a failed install says "Update failed"; a failed check, even after an earlier one found a version, says "Couldn\'t check for updates" with the reason and Check again (today\'s page says "Update failed" whenever a version was found before). It stays until a newer result, as today. Install tries again.',
  tags: {about: {text: 'Update', tone: 'violet'}},
  body: [
    M.row_status({glyph: 'circle-x', color: 'red', title: 'Update failed', detail: 'The download broke off · nothing was changed'}),
    M.row_action({glyph: 'download', text: 'Install version 0.6.1', color: 'blue'}),
    M.row_link({glyph: 'refresh-cw', title: 'Check again', detail: 'Checked 10:58 PM', trail: ''}),
  ]});

M.screen({id: 'installer-menu-update-held', shot: 'fwsrv_held', area: 'Installer', title: 'Installer menu: the server and the channel held while an update runs',
  notes: 'In Installer > Updates (BL-186), Update server and Update channel are dimmed with the detail "Not while an update runs" from Install to the end of the self-test; the Panel Manager holds the same, and the firmware refuses a server or channel change meanwhile. Installer mode makes the update wait, so the installer can be here while it downloads. Drawn scrolled to the group; the update banner gives way to the installer banner.',
  render: () => M.main({view: 'settings', tone: 'blue', rail: {troubles: 1, tone: 'blue'}, bar: {banner: 'installer', time: '10:58', troubles: 1},
    content: M.settings({section: 'about', sections: PROG_SECTIONS, title: 'Installer', back: 'about-update-running', tags: UPDATING, body: [
      M.section('This panel'),
      M.row_link({glyph: 'key-round', color: 'amber', title: 'Installer and master codes'}),
      M.row_toggle({glyph: 'monitor', title: 'Panel Manager', detail: 'Change settings from a browser on your network', on: true}),
      M.section('Updates'),
      M.row_link({glyph: 'server', color: 'green', title: 'Update server', detail: 'Not while an update runs', value: 'roboalarms.com', disabled: true}),
      M.row_link({glyph: 'layers', title: 'Update channel', detail: 'Not while an update runs', value: 'Stable', disabled: true}),
      M.section('Panel Manager'),
      M.row_link({glyph: 'globe', title: 'Allow browser sign-in', detail: 'User name: Installer', value: 'Off'}),
      M.row_action({glyph: 'log-out', text: 'Leave installer mode', color: 'blue'}),
    ].join('')})})});

// ---- the restart: a banner, never a sheet ----

homeScreen({id: 'home-update-restarting', shot: 'home_update_restarting', title: 'Restarting: the banner, Home still usable',
  notes: 'No sheet and no backdrop: the arm tiles, the code pad and Panic stay usable. The banner shows only while the panel is about to restart (RESTARTING). The firmware holds that 3 s from when it starts (the log is written meanwhile) and runs the status listeners first, so MQTT and Home Assistant hear it; ui_app\'s 1 s tick reads the update status on every screen, so the banner is up for about 2-3 s. Arming, an alarm or Stop in those seconds puts the restart off: arming in those seconds puts the restart off and the banner goes back to "Update waiting". It doesn\'t light a dark screen and isn\'t brightened over a dimmed one: there the panel just restarts. Then the screen is dark until the new version draws, about a minute in all (to be measured on a panel). On Software update the row says "Restarting with version 0.6.1 · the screen is back in about a minute". Static: the words change once, when the step changes.',
  render: () => M.main({bar: {banner: {glyph: 'rotate-ccw', tone: 'blue', text: 'Restarting to install the update'}, time: '7:12', ampm: 'AM'}, content: M.home(
    {tone: 'green', glyph: 'shield-check', headline: 'Disarmed', detail: 'Ready to arm', note: 'Disarmed 7:12 AM · By Alex'}, M.panel_arm())})});

// ---- after the restart ----

prog({id: 'settings-update-self-test', shot: 'upd_self_test', bar: BANNER('Self-test after the update', 'shield-check'), title: 'After the restart: the self-test',
  notes: 'The panel\'s probation: 60 s at least, 180 s at most when a part comes up late. No Stop: going back is what a failed self-test does by itself, and it doesn\'t wait for disarm (OTA-005: a version that failed its self-test isn\'t kept), so an armed panel restarts and comes back armed (Home, the banner before the second restart). The Home banner says "Self-test after the update" meanwhile.',
  body: [
    M.row_status({glyph: 'shield-check', color: 'blue', title: 'Self-test of version 0.6.1', detail: 'Usually a minute, at most three'}),
    M.paragraph('If its self-test fails, the panel restarts with 0.6.0 by itself, armed or not. The screen is back in about a minute.'),
    steps(4),
  ]});

homeScreen({id: 'home-update-going-back', shot: 'home_update_going_back', title: 'The self-test failed: the banner before the second restart',
  notes: 'The firmware sets the restarting step with "going back" before it writes its log (2 s) and restarts with 0.6.0: it runs the status listeners and sends a new appended event stage, "rolling back", so MQTT and Home Assistant hear it too. It shows only when the screen still works: a crash or a reset during the self-test, or a failed display part, rolls back with no banner. The panel comes back armed if it was (CORE-PWR-002). Steady amber, static.',
  render: () => M.main({tone: 'rose', rail: {locked: true}, bar: {banner: {glyph: 'triangle-alert', tone: 'amber', text: 'Going back to version 0.6.0 · restarting'}, time: '7:13', ampm: 'AM', locked: true},
    content: M.home({tone: 'rose', glyph: 'lock', headline: 'Armed Away', detail: 'The whole house is protected', note: 'Since 7:13 AM · By Alex'},
      M.code_pad({title: 'Disarm', tone: 'green', glyph: 'lock-open', close: false}))})});

prog({id: 'settings-update-updated', shot: 'upd_updated', bar: PROG_BAR, title: 'Updated',
  notes: 'For 24 hours by the clock, or until a newer version is offered; then "Up to date · Version 0.6.1". The panel takes it from the event log\'s newest "Firmware updated" record (its time survives a restart) and the version it runs; the time is left out when the clock wasn\'t set. "7:14 AM" today, "Sep 19" on another day. Activity has "Firmware updated" as today; phones get it when System events are on.',
  tags: {},
  body: [
    M.row_status({glyph: 'circle-check', color: 'green', title: 'Updated to version 0.6.1', detail: '7:14 AM · it passed its self-test'}),
    M.row_link({glyph: 'refresh-cw', title: 'Check again', detail: 'Checked 7:17 AM', trail: ''}),
  ]});

prog({id: 'settings-update-rolled-back', shot: 'upd_rolled_back', bar: PROG_BAR, title: 'Rolled back: the self-test failed',
  notes: 'Was "Up to date" with an amber paragraph "Version 0.6.1 failed its self-test: back on 0.6.0." The About row and Settings tag say "Rolled back" in amber, the word Activity uses ("Update rolled back"). It stays as today: until the next install replaces the failed version or a newer one is offered. Install stays offered; a fixed release shows here once it is out. Firmware: the panel reports a rollback only for a slot whose image verifies, so a stopped download in that slot is never reported as a version that failed.',
  tags: {about: {text: 'Rolled back', tone: 'amber'}},
  body: [
    M.row_status({glyph: 'triangle-alert', color: 'amber', title: 'Version 0.6.1 failed its self-test', detail: 'Back on 0.6.0 by itself'}),
    M.row_action({glyph: 'download', text: 'Install version 0.6.1', color: 'blue'}),
    M.row_link({glyph: 'refresh-cw', title: 'Check again', detail: 'Checked 7:17 AM', trail: ''}),
  ]});

// ---- the Wi-Fi module and developer files: their own words ----

prog({id: 'settings-update-wifi-module', shot: 'upd_wifi_module', bar: BANNER('Updating the Wi-Fi module', 'wifi'), title: 'The Wi-Fi module\'s update: no steps, no Stop',
  notes: 'The firmware records the kind of run when it starts it (release, file or Wi-Fi module) and GET updates and ui_fw_status_t report it. Wi-Fi module: this row, the Home banner "Updating the Wi-Fi module", then "Wi-Fi module update ready · installs once disarmed"; its restart checks again that everything is quiet, as a release\'s does. No Stop (it would need a firmware cancel for the module). A developer file (upload, panelctl, the console): "Installing a developer file · 42 %", no Stop; the sender ends it.',
  body: [
    M.row_status({glyph: 'wifi', color: 'blue', title: 'Updating the Wi-Fi module', detail: '42 % · the panel restarts when it\'s done, once everything is disarmed', level: 42}),
    M.paragraph('The alarm keeps working while it sends the update to the module.'),
  ]});

// ---- elsewhere while it runs ----

const ABOUT_DETAILS = [
  M.section('This panel'),
  M.row_value({title: 'Built', value: 'Oct 7, 2026'}),
  M.row_value({title: 'Running for', value: '3 days'}),
];
const about = ({id, shot, title, notes, row}) => M.screen({id, area: 'Settings', title, shot, notes,
  render: () => M.main({view: 'settings', rail: PROG_RAIL, bar: BANNER('Downloading an update'), content: M.settings({section: 'about', sections: PROG_SECTIONS, title: 'About this panel', tags: UPDATING, body: [
    M.row_status({glyph: 'shield-check', color: 'green', title: 'RoboAlarms Panel', detail: 'Version 0.6.0 · Panel ID a1b2c3d4e5f6'}),
    row,
    M.row_link({glyph: 'hard-drive', color: 'violet', title: 'SD card', detail: '44.7 GB free of 59.6 GB', value: 'Ready'}),
    ...ABOUT_DETAILS,
  ].join('')})})});

about({id: 'about-update-running', shot: 'about_update_running', title: 'About: an update runs, for anyone',
  notes: 'Settings opens with no code while disarmed, so About doesn\'t know who looks: the row says "Updating" (blue), no version, no percent, and the Settings list\'s tag says "Updating" from Install to the end of the self-test. Opening it asks for the installer or system master code (BL-186; today the page opens for anyone). The value follows the run once a second (today it is a snapshot from when About opened).',
  row: M.row_link_alert({glyph: 'download', title: 'Software update', value: 'Updating', valueColor: 'blue', goTo: 'settings-update-downloading'})});

about({id: 'about-update-running-master', shot: 'about_update_running_master', title: 'About: the update\'s progress, after the code',
  notes: 'Until Settings closes, as any unlocked Settings page. The percent at most once a second.',
  row: M.row_link({glyph: 'download', title: 'Software update', detail: 'Downloading version 0.6.1', value: '42 %', goTo: 'settings-update-downloading'})});

homeScreen({id: 'home-update-banner', shot: 'home_update_banner', title: 'Home: the banner while an update runs',
  notes: 'A status bar banner, steady and blue, from Install to the end of the self-test, its words changing only at a new step (reviewed against UI-020 and WCAG 2.3.1; no percent, which stays on Software update): "Downloading an update", "Checking the download", "Update waiting · installs once disarmed", "Restarting to install the update", "Self-test after the update", "Stopping the update"; amber "Going back to version 0.6.0 · restarting". Order: remote keypad, installer, walk test, arming, update, factory codes. Not while an alarm sounds (burglary, fire, CO, medical, or the siren timed out with the alarm still on Home): the alarm has the screen, as with the arming and factory-code banners, and it comes back once the alarm is silenced. Armed and in entry time it shows, so whoever disarms knows the panel will restart. A tap opens Software update (with the code, as always).',
  render: () => M.main({bar: {banner: {glyph: 'download', tone: 'blue', text: 'Downloading an update', goTo: 'about-update-running'}, time: '10:58'},
    content: M.home({tone: 'green', glyph: 'shield-check', headline: 'Disarmed', detail: 'Ready to arm', note: 'All zones closed'}, M.panel_arm())})});

homeScreen({id: 'home-update-given-up', shot: 'home_update_given_up', title: 'Arming in those seconds: the restart is put off',
  notes: 'Exit time started while the banner said "Restarting to install the update": the panel gave the restart up and the banner went straight back to "Update waiting · installs once disarmed", with no toast. Arming wins: whoever arms keeps the protection. An alarm instead: the banner gives way to it (home-update-banner). The next attempt comes once everything is quiet again, 30 s at the soonest.',
  render: () => M.main({tone: 'rose', rail: {locked: true}, bar: {banner: {glyph: 'clock', tone: 'blue', text: 'Update waiting · installs once disarmed'}, time: '7:12', ampm: 'AM', locked: true},
    content: M.home({tone: 'rose', countdown: 58, fill: 0.97, headline: 'Arming Away', detail: 'Leave now through the exit door', note: 'Exit time'},
      M.code_pad({title: 'Cancel arming', tone: 'green', glyph: 'lock-open', close: false}))})});

homeScreen({id: 'home-update-armed', shot: 'home_update_armed', title: 'Home, armed: an update waiting for disarm',
  notes: 'While armed the banner only tells (Settings is closed). The system master can stop it from the Panel Manager: the firmware lets POST updates {action: "cancel"} through while armed (Install and checks stay refused, as today). On disarm the panel restarts within about 3-5 s, with the "Restarting to install the update" banner; arming again in those seconds puts the restart off. During an alarm the banner gives way. A power cut while it waits forgets the update (it is kept in memory only): the banner goes and Software update offers 0.6.1 again.',
  render: () => M.main({tone: 'rose', rail: {locked: true}, bar: {banner: {glyph: 'clock', tone: 'blue', text: 'Update waiting · installs once disarmed'}, time: '11:20', locked: true},
    content: M.home({tone: 'rose', glyph: 'lock', headline: 'Armed Away', detail: 'The whole house is protected', note: 'Since 11:04 PM · By Alex'},
      M.code_pad({title: 'Disarm', tone: 'green', glyph: 'lock-open', close: false}))})});
