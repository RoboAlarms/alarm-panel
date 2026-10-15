// BL-186: updates from the RoboAlarms update server (docs/features/13 OTA-017..OTA-025). The update
// source becomes a domain ("roboalarms.com"; the panel asks https://firmware.<domain>), checked before
// it is kept; a server whose signing key the panel doesn't trust yet needs the installer to hold a
// warning that shows the key's fingerprint. Also the channel the panel follows, and a withdrawn version.
// The update server and the channel are the installer's alone, so they live in the Installer menu
// (Updates); Software update in Settings > About is the same page for the installer and the system master.

const UPD_BAR = {time: '10:58', troubles: 1};
const UPD_RAIL = {troubles: 1};
const upd = ({id, shot, title, notes, page, over}) => M.screen({
  id, area: 'Settings', title, shot, notes,
  render: () => M.main({view: 'settings', rail: UPD_RAIL, bar: UPD_BAR, over,
    content: M.settings({section: 'about', ...page})}),
});

const RA_KEY = M.row_value({title: 'RoboAlarms release', value: '3F9A 1C2B 77D0 E415'});
const EX_KEY = M.row_value({title: 'Example Builds', value: '8C41 0E9D 52A7 B3F6'});
// The Installer menu's pages: installer mode (the blue frame and its banner), opened from the bottom of
// About, which stays lit; the Settings list ends with About.
const ABOUT_SECTIONS = M.settings_sections({drop: ['installer']}).filter((s) => !s.divider);
const INST186_RAIL = {troubles: 1, tone: 'blue'};
const INST186_BAR = {banner: 'installer', time: '10:58', troubles: 1};
const inst186 = ({id, shot, title, notes, page, over}) => M.screen({
  id, area: 'Installer', title, shot, notes,
  render: () => M.main({view: 'settings', tone: 'blue', rail: INST186_RAIL, bar: INST186_BAR, over,
    content: M.settings({section: 'about', sections: ABOUT_SECTIONS, ...page})}),
});
const SERVER_PAGE = (rows) => ({title: 'Update server', note: 'Where this panel gets its updates', back: 'installer-menu-updates', body: rows.join('')});
const CHANGE_ROW = M.row_link({glyph: 'pencil', title: 'Change the update server', detail: 'For your own builds or another project\'s', goTo: 'installer-update-server-type'});

// ---- Software update ----

upd({id: 'settings-update', shot: 'about_update_available', title: 'Software update: what is ready, and Install',
  notes: 'The same page for the installer and the system master: no Advanced section (the update source is the installer\'s, in Installer > Updates). Software update opens for the installer and the system master only; for everyone else the About row is the usual locked row. A panel on another project\'s server says nothing about it here: the Installer menu says so.',
  page: {title: 'Software update', back: 'settings-about', tags: {about: {text: 'Update', tone: 'violet'}}, body: [
    M.row_link({glyph: 'sparkles', color: 'violet', title: 'Version 0.6.1 is ready to install', detail: 'You have 0.6.0 · the alarm keeps working while it downloads', trail: ''}),
    M.row_action({glyph: 'download', text: 'Install version 0.6.1', color: 'blue'}),
    M.paragraph('The panel restarts once everything is disarmed. It takes about a minute.'),
    M.row_link({glyph: 'refresh-cw', title: 'Check again', detail: 'Checked 10:58 PM', trail: ''}),
  ].join('')}});

// The Installer menu (installer.js installer-menu), scrolled to its Updates group. server: the Update server row.
const INSTALLER_MENU = (server, channel = 'Stable') => [
  M.section('This panel'),
  M.row_link({glyph: 'key-round', color: 'amber', title: 'Installer and master codes', detail: 'Factory codes still in use'}),
  M.row_toggle({glyph: 'monitor', title: 'Panel Manager', detail: 'Change settings from a browser on your network'}),
  M.row_toggle({glyph: 'globe', title: 'Browser sign-in', detail: 'User name: Installer · a password, not the keypad code'}),
  M.row_toggle({glyph: 'code-xml', color: 'gray', title: 'Developer mode', detail: 'Enrolled development tools and signed network updates'}),
  M.section('Updates'),
  server,
  M.row_link({glyph: 'layers', title: 'Update channel', detail: 'Which releases this panel is offered', value: channel, goTo: 'installer-update-channel'}),
  M.section('Test and start over'),
  M.row_link({glyph: 'footprints', title: 'Walk test', detail: 'Leaves installer mode and starts one'}),
  M.row_link({glyph: 'sparkles', color: 'rose', title: 'Run the setup again', detail: 'Wi-Fi, time zone, codes and name, step by step'}),
  M.row_link({glyph: 'rotate-ccw', color: 'red', title: 'Factory reset', detail: 'Erase everything'}),
  M.row_action({glyph: 'log-out', text: 'Leave installer mode', color: 'blue'}),
].join('');
const RA_SERVER_ROW = M.row_link({glyph: 'server', color: 'green', title: 'Update server', detail: 'Where updates come from', value: 'roboalarms.com', goTo: 'installer-update-server'});

inst186({id: 'installer-menu-updates', shot: 'fwsrv_menu', title: 'Installer: Updates, with the update server and the channel',
  notes: 'Drawn scrolled to the new group. Updates comes after This panel (System name, the codes, Panel Manager, Browser sign-in, Developer mode). Update server shows the domain only (never the full address). Update channel opens the channel list. The Installer menu opens from the bottom of About.',
  page: {title: 'Installer', body: INSTALLER_MENU(RA_SERVER_ROW)}});

inst186({id: 'installer-menu-other-server', shot: 'fwsrv_menu_other', title: 'Installer: another project\'s server in use',
  notes: 'The row is amber while the server isn\'t the build\'s own (roboalarms.com for RoboAlarms\' builds). Software update itself doesn\'t mention the server, for anyone.',
  page: {title: 'Installer', body: INSTALLER_MENU(M.row_link_alert({glyph: 'server', color: 'amber', title: 'Update server', detail: 'Not the RoboAlarms server', detailColor: 'amber', value: 'example.net', goTo: 'installer-update-server-other'}), 'Beta')}});

upd({id: 'settings-update-withdrawn', shot: 'upd_withdrawn', title: 'Software update: the version it runs was withdrawn',
  notes: 'The server says so at every check. With a newer release offered, Install stays as it is; without one, the row says "Its publisher took it back" and nothing else changes. "An important fix" in the detail when the offered release is marked critical.',
  page: {title: 'Software update', back: 'settings-about', tags: {about: {text: 'Update', tone: 'violet'}}, body: [
    M.row_link({glyph: 'triangle-alert', color: 'amber', title: 'Version 0.6.0 was withdrawn', detail: 'Its publisher took it back · 0.6.1 is an important fix', trail: ''}),
    M.row_action({glyph: 'download', text: 'Install version 0.6.1', color: 'blue'}),
    M.paragraph('The panel restarts once everything is disarmed. It takes about a minute.'),
    M.row_link({glyph: 'refresh-cw', title: 'Check again', detail: 'Checked 10:58 PM', trail: ''}),
  ].join('')}});

inst186({id: 'installer-update-channel', shot: 'fwsrv_channel', title: 'Choosing the update channel',
  notes: 'The installer only (Installer > Updates). A change checks at once. A channel offers its own releases and those of the channels to its right: Alpha also gets Beta and Stable releases; Dev stands apart.',
  page: {title: 'Installer', body: INSTALLER_MENU(RA_SERVER_ROW)},
  over: M.pick_sheet({title: 'Update channel', note: 'Which releases this panel is offered', fit: true, rows: [
    M.row_link({glyph: 'shield-check', color: 'green', title: 'Stable', detail: 'Tested the longest. Right for most homes', trail: 'check'}),
    M.row_link({glyph: 'flask-conical', color: 'blue', title: 'Beta', detail: 'Newer releases, tried by a few people first', trail: ''}),
    M.row_link({glyph: 'flask-conical', color: 'violet', title: 'Alpha', detail: 'The newest releases, still being tested', trail: ''}),
    M.row_link({glyph: 'code-xml', color: 'gray', title: 'Dev', detail: 'Special builds for developers', trail: ''}),
  ].join('')})});

// ---- the update server ----

inst186({id: 'installer-update-server', shot: 'fwsrv_server', title: 'Update server: RoboAlarms\'',
  notes: 'The installer only: it opens from Installer > Updates. The server shows as its domain only: the panel adds https://firmware. by itself and nobody needs the full address. The fingerprint is the first 16 hex digits of the key\'s SHA-256, in groups of four, as espsecure and the server\'s Signing keys page show it; the console prints all 64.',
  page: SERVER_PAGE([
    M.row_status({glyph: 'server', color: 'green', title: 'roboalarms.com', detail: 'The RoboAlarms update server'}),
    M.section('Signing key'),
    RA_KEY,
    M.paragraph('Every update must be signed with this key. The panel refuses anything else, wherever it comes from.'),
    M.section(''),
    CHANGE_ROW,
  ])});

inst186({id: 'installer-update-server-type', shot: 'fwsrv_type', title: 'Update server: typing a domain',
  notes: 'Lowercase letters, digits, hyphens and dots, at least two parts ("example.net", "builds.example.co.uk"); "https://" and a trailing "/" are dropped. Check asks the server; nothing is kept until the next screens say so. An empty field and Check: "Type a domain, like roboalarms.com".',
  page: SERVER_PAGE([M.row_status({glyph: 'server', color: 'green', title: 'roboalarms.com', detail: 'The RoboAlarms update server'})]),
  over: M.text_sheet({title: 'Update server', note: 'The domain only, like roboalarms.com', value: 'example.net', ok: 'Check'})});

inst186({id: 'installer-update-server-checking', shot: 'fwsrv_checking', title: 'Update server: checking a new one',
  notes: 'About 2-15 s. Static words, no spinner (display safety). Going back cancels.',
  page: SERVER_PAGE([
    M.row_status({glyph: 'refresh-cw', color: 'blue', title: 'Checking example.net', detail: 'Asking for its signing key and for firmware for this panel'}),
    M.section('Signing key'), RA_KEY,
  ])});

inst186({id: 'installer-update-server-warning', shot: 'fwsrv_warning', title: 'Update server: the warning before another project\'s server',
  notes: 'Opens after a good check of a server that isn\'t roboalarms.com, or whose key the version it runs wasn\'t signed with. Back cancels; nothing changes until the hold completes. Then the panel keeps the domain and this key, logs "Update server changed" by the installer (it reaches the owner\'s notifications like other system events) and checks at once. Refused while the installer code is still the factory one (next screens).',
  page: {title: 'Use example.net for updates?', note: 'Not the RoboAlarms update server', back: 'installer-update-server', body: [
    M.row_status({glyph: 'shield-alert', color: 'amber', title: 'example.net answered', detail: 'It has firmware for this panel'}),
    M.paragraph('Whoever runs this server can install firmware that changes everything this panel does, its alarms too. Continue only if you trust them.', 'amber'),
    M.section('Its signing key'),
    EX_KEY,
    M.paragraph('Check that this matches the key its owner publishes. From now on the panel also accepts updates signed with it.'),
    M.row_hold({text: 'Hold to use this server'}),
  ].join('')}});

inst186({id: 'installer-update-server-other', shot: 'fwsrv_server_other', title: 'Update server: another project\'s, in use',
  notes: 'Go back to roboalarms.com runs the same check; when the version it runs was signed by another project, RoboAlarms\' key is new to it and the same warning (with RoboAlarms\' key) is asked first.',
  page: SERVER_PAGE([
    M.row_status({glyph: 'server', color: 'amber', title: 'example.net', detail: 'Not the RoboAlarms server'}),
    M.section('Signing key'),
    EX_KEY,
    M.paragraph('Every update must be signed with this key or with the key of the version it runs.'),
    M.section(''),
    CHANGE_ROW,
    M.row_action({glyph: 'rotate-ccw', text: 'Go back to roboalarms.com', color: 'blue'}),
  ])});

const REFUSED = (meta, title, detail, why) => inst186({...meta, title: `Update server: ${title}`,
  notes: why + ' The row stays until the page is left; the server in use doesn\'t change.',
  page: SERVER_PAGE([
    M.row_status({glyph: 'circle-x', color: 'red', title, detail}),
    M.section('Signing key'), RA_KEY, M.section(''), CHANGE_ROW,
  ])});
REFUSED({id: 'installer-update-server-unreachable', shot: 'fwsrv_unreachable'},
  'example.net can\'t be reached', 'Check the domain and that the panel is online',
  'No answer, a name that doesn\'t exist or a certificate the panel refuses: nothing changes.');
REFUSED({id: 'installer-update-server-not-one', shot: 'fwsrv_not_a_server'},
  'example.net isn\'t an update server', 'It didn\'t answer like one. Check the domain',
  'A web site that isn\'t an update server (no signing keys, no check answer): nothing changes.');
REFUSED({id: 'installer-update-server-no-firmware', shot: 'fwsrv_no_firmware'},
  'example.net has no firmware for this panel', 'It doesn\'t serve this model of panel',
  'The server answered, but its catalogue doesn\'t have this board, chip or build (unknown_device): nothing changes.');

inst186({id: 'installer-update-server-factory-code', shot: 'fwsrv_refused_factory', title: 'Update server: the factory installer code is still in use',
  notes: 'The row is dimmed until the installer code is changed (Installer > Installer and master codes). Going back to roboalarms.com, with a key the panel already trusts, stays allowed. The USB service console isn\'t held back: whoever has the cable can flash the panel anyway.',
  page: SERVER_PAGE([
    M.row_status({glyph: 'server', color: 'green', title: 'roboalarms.com', detail: 'The RoboAlarms update server'}),
    M.section('Signing key'), RA_KEY, M.section(''),
    M.row_link({glyph: 'pencil', color: 'gray', title: 'Change the update server', detail: 'Change the installer code first: the factory one is public', disabled: true}),
  ])});

// BL-187: the installer code never arms or disarms (docs/features/03 USR-001), and the screen never
// shows the factory installer code's digits.

M.screen({
  id: 'installer-codes', shot: 'installer_codes', area: 'Installer', title: 'Installer and master codes: no digits on show',
  notes: 'The detail is the setup\'s words, without the code\'s digits ("Still the factory code"). The paragraph under the rows now says what each code is for: the installer code programs the panel but never arms or disarms it (USR-001). Arming with the installer code at the keypad gets today\'s refusal, "This code can\'t do that", with the error sound and no event. The installer still ends a walk test and silences a fire, CO or emergency alarm (neither disarms). Automations: an arming or disarming one acts as its maker, so the installer isn\'t offered one (the arming idea and the Arm and Disarm actions are left out), and one the installer made before fails at its time with today\'s failed line.',
  render: () => M.main({view: 'settings', tone: 'blue', rail: {troubles: 1, tone: 'blue'}, bar: {banner: 'installer', time: '10:52', troubles: 1},
    content: M.settings({section: 'about', sections: ABOUT_SECTIONS, back: 'installer-menu', title: 'Installer and master codes', note: 'The two codes that can\'t be deleted', body: [
      M.row_link({glyph: 'key-round', color: 'amber', title: 'Installer code', detail: 'Still the factory code'}),
      M.row_link({glyph: 'key-round', color: 'amber', title: 'Master code', detail: 'Still the factory code'}),
      M.paragraph('The installer code programs the panel; it never arms or disarms. The factory codes are public: anyone can look them up. Everyone else\'s code is managed in Settings > Users with the master code.'),
    ].join('')})}),
});
