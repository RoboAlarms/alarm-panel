// BL-225: Settings > Integrations > RoboAlarms, and a phone on a person's page (alarm-server's
// panel-link.md "What alarm-panel needs", direct-mode.md "Pairing a phone"). Approved and built: the PC
// preview's pictures 515 to 521 (walk_server_link) are these screens.

const SL_SECTIONS = M.settings_sections({drop: ['installer']}).filter((s) => !s.divider);
const slPage = (meta, notes, page) => M.screen({...meta, area: 'Settings', shot: null, notes,
  proposal: {bl: 'BL-225', status: 'approved',
    decided: '2026-11-02: the owner approved the whole BL-225 build, screens included',
    why: 'A person standing at the panel links it to their alarm-server, and sees who is asking before it is allowed. People get the phone app without opening a port.'},
  render: () => M.main({view: 'settings', tone: 'blue', rail: {tone: 'blue'}, bar: {time: '10:58'},
    content: M.settings({section: 'about', sections: SL_SECTIONS, ...page})})});
const SL = {title: 'RoboAlarms', back: ''};

slPage({id: 'bl-225-connect', title: 'RoboAlarms: not linked'},
  'Behind an Owner or Admin code, beside Home Assistant and MQTT. The server is cloud.roboalarms.com until someone changes it.',
  {...SL, body: M.changed([
    M.paragraph('See and control this panel from the RoboAlarms app and website.'),
    M.row_link({glyph: 'globe', color: 'gray', title: 'Server', detail: 'cloud.roboalarms.com', goTo: ''}),
    M.row_action({glyph: 'plus', text: 'Connect to RoboAlarms', color: 'blue', goTo: 'bl-225-code'}),
  ].join(''))});

slPage({id: 'bl-225-code', title: 'RoboAlarms: the code to type'},
  'Six characters, and the same link as a QR code beside them (the QR part is not in the kit yet). 10 minutes, one use. The countdown changes at most once a second (UI-023).',
  {...SL, body: M.changed([
    M.big_value({label: 'On your phone or computer, enter', value: '7KQ 2MZ'}),
    M.paragraph('cloud.roboalarms.com/link · 9 minutes left'),
    M.row_action({glyph: 'x', text: 'Cancel', color: 'gray', goTo: 'bl-225-connect'}),
  ].join(''))});

slPage({id: 'bl-225-allow', title: 'RoboAlarms: who is asking'},
  'The step that stops a code read over a shoulder being used by someone else: it names the account. The pattern of the Home Assistant pairing and Add a keypad: red on the left.',
  {...SL, body: M.changed([
    M.row_status({glyph: 'user', color: 'blue', title: 'Alex (a•••@example.org)', detail: 'Wants to link this panel as Home'}),
    M.action_pair({glyph: 'circle-x', text: 'Don\'t allow', color: 'red', goTo: 'bl-225-connect'},
                  {glyph: 'circle-check', text: 'Allow', color: 'green', goTo: 'bl-225-linked'}),
  ].join(''))});

slPage({id: 'bl-225-linked', title: 'RoboAlarms: linked, with what the server may do'},
  'The remote ceiling (D6). Only someone at the panel changes these. Off means off for everyone, whatever the server says. Unlink works with no server at all and changes nothing else on the panel.',
  {...SL, body: M.changed([
    M.row_status({glyph: 'circle-check', color: 'green', title: 'Connected', detail: 'cloud.roboalarms.com · Home · heard just now'}),
    M.section('What the app and website may do'),
    M.row_toggle({title: 'Disarm and silence', detail: 'Arming always works', on: true}),
    M.row_toggle({title: 'Open Setup mode', detail: '', on: true}),
    M.row_toggle({title: 'Change people and codes', detail: '', on: true}),
    M.row_toggle({title: 'Ask for the alarm code to disarm', detail: 'A duress code then works from the app', on: false}),
    M.row_action({glyph: 'x', text: 'Unlink', color: 'red', goTo: 'bl-225-connect'}),
  ].join(''))});

slPage({id: 'bl-225-phone-qr', title: 'A person\'s page: add a phone'},
  'Direct mode. Under Settings > Users > the person, opened with their own code or an Owner\'s. 5 minutes, one use, three wrong tries and it closes. The Web server must be on.',
  {title: 'Add a phone', back: '', body: M.changed([
    M.paragraph('Open the RoboAlarms app and scan the code on this screen, on this Wi-Fi.'),
    M.big_value({label: 'QR code here', value: '▣'}),
    M.row_action({glyph: 'x', text: 'Cancel', color: 'gray', goTo: ''}),
  ].join(''))});

slPage({id: 'bl-225-phone-allow', title: 'A person\'s page: which phone'},
  'After the scan. Allow stores it in one write. Its row then sits on the person\'s page with Remove.',
  {title: 'Add a phone', back: '', body: M.changed([
    M.row_status({glyph: 'smartphone', color: 'blue', title: 'Alex\'s iPhone', detail: 'Add this phone for Alex?'}),
    M.action_pair({glyph: 'circle-x', text: 'Don\'t allow', color: 'red', goTo: ''},
                  {glyph: 'circle-check', text: 'Allow', color: 'green', goTo: ''}),
  ].join(''))});
