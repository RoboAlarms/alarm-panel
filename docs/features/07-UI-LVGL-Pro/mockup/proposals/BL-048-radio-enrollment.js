// BL-048 draft: nearby enrollment first; network credentials only after mutual confirmation.
(() => {
  const id = name => 'bl-048-radio-' + name;
  const action = (text, to, glyph = 'chevron-right') => M.row_action({text, glyph, color: 'blue', goTo: id(to)});
  const state = (title, detail, glyph = 'wifi', color = 'blue') => M.row_status({title, detail, glyph, color});
  function screen(name, title, note, body, next, target, back, notes = '') {
    M.screen({id: id(name), area: 'Setup', title: 'Nearby enrollment: ' + title, shot: null,
      proposal: {bl: 'BL-048', status: 'draft', replaces: 'setup-keypad-wifi',
        why: 'Enroll without typing Wi-Fi credentials when nearby; keep manual Wi-Fi available when out of range.'},
      notes: 'Entry: first-start setup > A keypad > Next. ' + notes +
        ' Draft only. Language, display and audio stay local; date/time follow the master. No credentials before both confirmations. Scenario links simulate events, not firmware actions.',
      render: () => {
        let html = M.wizard({step: name === 'done' ? 4 : name.startsWith('wifi') || name === 'sharing' ? 3 : 2,
          steps: 4, glyph: 'keyboard', title, note, body: M.changed(body), next: next || 'Next', nextDisabled: !target, back: !!back});
        if (target) html = html.replace(M.button(next, 'primary', null, 'chevron-right'),
          M.button(next, 'primary', null, 'chevron-right').replace('<div ', '<div data-go="' + id(target) + '" '));
        if (back) html = html.replace(M.button('Back', '', 'chevron-left'),
          M.button('Back', '', 'chevron-left').replace('<div ', '<div data-go="' + id(back) + '" '));
        return html;
      }});
  }
  screen('start', 'Connect to your alarm panel', 'Start pairing on the alarm panel or in Panel Manager.',
    state('Find a nearby alarm panel', 'Wi-Fi settings arrive after you confirm the pairing code.', 'search') +
    action('Select Wi-Fi', 'wifi') + M.paragraph('Use Wi-Fi if the alarm panel is too far away.'),
    'Find panel', 'search', null, 'Panel Manager: Partitions and devices > Add device > Keypad. Works with a normal or headless master.');
  screen('search', 'Looking for your alarm panel', 'Keep pairing open on the alarm panel.',
    state('Searching nearby', 'No Wi-Fi connection needed', 'search') + action('Select Wi-Fi', 'wifi'),
    null, null, 'start', 'Automatic discovery uses ESP-NOW. Review next states: Found = radio-code; no result = radio-missing. No animated spinner.');
  screen('missing', 'No alarm panel found', 'It may be out of range, or pairing may not be open.',
    state('Check that pairing is open', 'Start pairing in Panel Manager or on the alarm panel.', 'search', 'amber') +
    action('Select Wi-Fi', 'wifi'), 'Try again', 'search', 'start');
  screen('code', 'Compare the codes', 'Check the code on the alarm panel or in Panel Manager.',
    state('Harbor House', 'Pairing code', 'shield-check') + M.text('482 619', 'digits') +
    M.paragraph('Confirm on both devices only if the codes match.') + action('Codes do not match', 'rejected', 'x'),
    'Confirm', 'waiting', 'start', 'Shown only after discovering a peer. Cancel/Back ends the attempt; it never sends Wi-Fi settings.');
  screen('waiting', 'Confirm on the alarm panel', 'You can also confirm in Panel Manager.',
    state('Confirmed on this keypad', 'Waiting for the other confirmation', 'shield-check'),
    null, null, 'start', 'After both confirmations, automatically go to radio-sharing. Timeout goes to radio-expired.');
  screen('sharing', 'Getting Wi-Fi settings', 'Your keypad is enrolled with Harbor House.',
    state('Receiving settings securely', 'The alarm panel shares its Wi-Fi connection settings.', 'wifi') +
    M.paragraph('Keep the keypad within range until this finishes.'), null, null, null,
    'Uses the authenticated encrypted ESP-NOW session. After receipt, connect automatically. Success = radio-done; failure or master without credentials = radio-wifi-failed. Enrollment is retained.');
  screen('wifi-failed', 'Wi-Fi is not connected', 'Your keypad is still enrolled.',
    state('Could not join Wi-Fi', 'Shared settings may be unavailable, or the network may be out of range.', 'wifi-off', 'amber') +
    action('Select Wi-Fi', 'wifi-enrolled') + action('Continue without Wi-Fi', 'done-radio'),
    'Try again', 'sharing', null, 'Continue without Wi-Fi is available only while the authenticated nearby link is online. Otherwise show radio-lost.');
  const networks = to => M.section('Networks nearby') +
    M.row_link({glyph: 'wifi', title: 'Harbor House', detail: 'Password · strong signal', trail: 'lock', goTo: id(to)}) +
    M.row_link({glyph: 'wifi', title: 'Other network', detail: 'Enter a network name', goTo: id('wifi-hidden')});
  screen('wifi', 'Select Wi-Fi', 'Join the same network as the alarm panel.',
    networks('wifi-password'), null, null, 'start', 'Existing network scan and password keyboard. Successful connection resumes discovery over LAN; comparison is still required.');
  screen('wifi-hidden', 'Other network', 'Enter the network name, then its password.',
    M.row_link({glyph: 'wifi', title: 'Network name', value: 'Harbor House'}) + M.paragraph('Use the existing network-name keyboard.'),
    'Next', 'wifi-password', 'wifi');
  screen('wifi-password', 'Join Harbor House', 'Enter this network’s Wi-Fi password.',
    M.row_link({glyph: 'lock', title: 'Password', value: '••••••••', goTo: 'wifi-password'}) +
    M.paragraph('Use the existing Wi-Fi password keyboard.'), 'Connect', 'lan', 'wifi',
    'Password is illustrative, never a real credential. Connection failure stays on network selection with the existing error.');
  screen('lan', 'Looking on Wi-Fi', 'Keep pairing open on the alarm panel.',
    state('Connected to Harbor House', 'Looking for your alarm panel on this network', 'wifi') + action('Select Wi-Fi', 'wifi'),
    null, null, 'wifi', 'Found goes to the same code comparison. After both confirmations, keep manually entered Wi-Fi settings, skip transfer, and go to radio-done.');
  screen('wifi-enrolled', 'Select Wi-Fi', 'Your enrollment stays saved.',
    networks('wifi-enrolled-password'), null, null, 'wifi-failed');
  screen('wifi-enrolled-password', 'Join Harbor House', 'Your enrollment stays saved.',
    M.row_link({glyph: 'lock', title: 'Password', value: '••••••••', goTo: 'wifi-password'}),
    'Connect', 'done', 'wifi-enrolled', 'Existing password editor; success reconnects to the enrolled master, never starts a new pairing.');
  screen('done', 'Keypad ready', 'Connected to Harbor House.',
    state('Wi-Fi connected', 'The nearby link remains available as a fallback.', 'wifi', 'green') +
    M.paragraph('Language, display and audio are set on this keypad.'), 'Done', 'start', null,
    'Done finishes setup and opens keypad Home; the mock loops to the start for review.');
  screen('done-radio', 'Keypad ready', 'Connected to Harbor House through the nearby link.',
    state('Wi-Fi not configured', 'You can select Wi-Fi later in Settings.', 'wifi-off') +
    M.paragraph('Language, display and audio are set on this keypad.'), 'Done', 'start', null);
  screen('lost', 'Connection lost', 'Your enrollment stays saved.',
    state('Move closer or select Wi-Fi', 'The keypad needs a connection to the alarm panel.', 'wifi-off', 'amber') +
    action('Select Wi-Fi', 'wifi-enrolled'), 'Try again', 'sharing', null);
  screen('rejected', 'Pairing canceled', 'No Wi-Fi settings were shared.',
    state('The codes did not match', 'Start a new pairing attempt.', 'shield', 'amber'), 'Try again', 'start', null);
  screen('expired', 'Pairing expired', 'No Wi-Fi settings were shared.',
    state('Open pairing again', 'Confirm on both devices within two minutes.', 'clock', 'amber') + action('Select Wi-Fi', 'wifi'),
    'Try again', 'start', null);
})();
