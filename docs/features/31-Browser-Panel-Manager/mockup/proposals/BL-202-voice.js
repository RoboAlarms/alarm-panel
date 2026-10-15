// BL-202: the Panel Manager's parts of the voice (docs/features/08-Audio-and-Annunciation/voice-first-build.md
// D06 settings, D14 the Panel Manager). The touchscreen's parts are in
// docs/features/07-UI-LVGL-Pro/mockup/proposals/BL-202-spoken-name.js, in the same words.
// Display and sound gains an Announcements card (four switches, language, availability, pack upload);
// each zone's card on Zones > All zones and its setup card gain a read-only Spoken name. The browser has no word editor in this build.
proposal({
  bl: 'BL-202',
  title: 'Announcements, the voice pack and each zone\'s spoken name',
  status: 'approved',
  decided: 'Approved 2026-10-08, after the changes the review required.',
  role: 'installer',
  why: [
    'The panel is to speak (status messages, zone names). Its switches, the voice pack and the zones\' spoken words can be seen and set only at the keypad.',
    'Display and sound gets an Announcements card: the four switches, the voice language, whether the voice is ready, and for the system master and installer an upload for the signed voice pack. Every zone card shows its Spoken name, read-only; the installer gets a button to choose the words on the panel, through the Remote keypad.',
  ],
  look: [
    ['display', 'Display and sound as the installer: the Announcements card, ready (a draft pack)', {role: 'installer'}],
    ['display', 'Pick "Pretend: announcements off", "voice pack released", "installed, voice unavailable", "no voice pack" or "information could not be loaded": the card says what is true and why a switch can\'t be turned on'],
    ['display', 'Pick an upload answer under "Mockup only", choose any file (or none): too big, bad signature, armed, not allowed, busy'],
    ['display', 'As a system master: the same switches and the upload', {role: 'master'}],
    ['display', 'As a standard user: the switches and Preview, but no upload; the note says who can install a voice pack', {role: 'standard'}],
    ['display', 'Armed: the switches, Preview and the upload wait until everything is disarmed and quiet', {role: 'installer', scenario: 'armed'}],
    ['zones', 'Zones > All zones as a system master: each zone card shows its Spoken name, read-only', {role: 'master'}],
    ['zones', 'Zones > All zones as a standard user: the same line', {role: 'standard'}],
    ['zone-setup/1', 'Zone 1 setup: Spoken name with its words, and Choose words on the panel (installer only)', {role: 'installer'}],
    ['zone-setup/3', 'Zone 3 has Hall Motion and needs review; zone 2 has no words yet (Choose words)', {role: 'installer'}],
  ],
  replaces: ['display', 'zones', 'zone-setup'],
  notes: [
    'Words match the touchscreen: Announcements, Status messages, Zone chime, Walk test, Voice language, Voice pack, Preview; states Choose words, Review spoken name, Announcements are off, Voice pack not installed, Voice is unavailable.',
    'Status messages says "Says arming, armed, disarmed, entry and not ready messages."; Zone chime says "Says the zone’s spoken name after the chime."; the same as the touchscreen.',
    'Readiness, not presence: GET /api/v1/voices gives {installed, locale, name, version, draft, ready, reason, free}. Enabling Announcements and Preview need ready (signed pack, mandatory clips and decoder resources valid), not only installed. No pack: "Voice pack not installed". Installed but not ready: "Voice is unavailable." with the installed language and version kept. The Draft tag shows only when draft is true. Enabling is refused with "Announcements cannot be turned on because the voice pack is not installed." or "Announcements cannot be turned on because voice is unavailable."',
    'If the information can\'t be loaded the card says "Voice information could not be loaded. Try again." and does not invent a pack: enabling and Preview stay off until readiness is known.',
    'API (new): PUT /api/v1/voices takes the signed .robovoice as application/octet-stream and answers {result}: ok, too_big, bad_signature, not_allowed, armed, busy. Only the system master and installer may PUT, and only while disarmed and silent, like the panel; the pretend panel enforces both.',
    'Access: the four switches and Preview follow Display and sound\'s existing access and the disarmed/silent gate, as every other switch on that page; they are not limited to masters and installers. Installing a pack is system-master/installer-only and obeys the maintenance gates. A standard user sees the note "Only the system master or installer can install a voice pack." instead of the upload.',
    'Upload: choosing no file says "Choose a file." in the line beside the button; success shows no toast, the card refreshes to the installed pack; installing a pack keeps the announcement preferences and never turns Announcements on.',
    'API (changed): config/display gains voice_enabled (default off), voice_status, voice_chime, voice_walk. Turning voice_enabled on is refused until the audio is ready ("no_pack", "unavailable"). Here the switches keep their value in the page only.',
    'API (changed): each zone response gains a computed, read-only `spoken` object: {words: [word ids], summary: "Front door", state: "ready"|"empty"|"review"|"off"|"missing", binding_rev, approved_rev, pack_rev}. It is outside the editable field descriptors, and a PUT never takes it as authority to approve; changing a zone\'s name, partition or sensor through the browser clears the approval, as on the panel.',
    'Zones: each All zones card gains a read-only Spoken name line with the retained summary and a tag for the state, for every role that sees those zones (zone 3 keeps Hall Motion and shows Review spoken name). The zone\'s setup card shows the same row; only the installer also sees "Choose words on the panel" with a link to the Remote keypad.',
    'Preview uses the Remote keypad, not a browser audio player. No custom recordings and no system-prompt override in this build; a full word editor in the browser is its own row.',
    'Language: the labels, the "Voice language" label and the locale name "English (United States)" are translated into the page language; the recorded words and the pack\'s own display name (the Voice pack row) are data and are not. Changing the page language does not change the voice.',
    'Mockup only: the dashed box under the card picks what the pretend panel answers; it is not part of the design.',
  ],
  css: `
    .mockup-only { border: 1px dashed var(--line); border-radius: var(--radius, 10px); padding: 10px 14px; margin: 12px 16px; display: flex; gap: 10px; align-items: center; flex-wrap: wrap; }
    .mockup-only small { color: var(--muted, inherit); }
    .spoken-row .what { display: flex; gap: 8px; align-items: center; flex-wrap: wrap; }
    .spoken-line { display: flex; gap: 6px; align-items: center; flex-wrap: wrap; }
  `,
  routes(route) {
    route('GET', /^voices$/, () => voiceDemo.mode === 'error' ? [500, {error: 'unavailable'}] : [200, voiceDemo.get()]);
    route('PUT', /^voices$/, () => {
      // The panel's own refusals come first: who may install, and only while disarmed and silent.
      if (!voiceRoleMay()) return [403, {result: 'not_allowed'}];
      if (state?.locked) return [409, {result: 'armed'}];
      const r = voiceDemo.upload;
      if (r === 'ok') voiceDemo.mode = 'ready'; // the preferences stay as they were; installing never turns Announcements on
      return r === 'ok' ? [200, {result: 'ok'}] : [409, {result: r}];
    });
  },
  apply() {
    const display = PAGES.display.render;
    PAGES.display.render = async () => {
      const page = await display();
      const v = await api('voices').catch(() => null); // null: readiness is not known
      const at = page.html.indexOf('<h2>' + t('This browser'));
      const start = at < 0 ? page.html.length : page.html.lastIndexOf('<section', at);
      return {html: page.html.slice(0, start) + voiceCard(v) + page.html.slice(start), bind() { page.bind?.(); bindVoice(); }};
    };
    const zoneCard = zpZoneCard;
    zpZoneCard = () => {
      const html = zoneCard();
      const marker = '<div class="row"><div><label class="label" for="zp-zone-type"';
      const at = html.indexOf(marker);
      return at < 0 ? html : html.slice(0, at) + spokenRow(zp.zone.number || zp.number) + html.slice(at);
    };
    // Zones > All zones: each card gets a read-only Spoken name under "Zone N · partition".
    const zonesHtml = zonesBody;
    zonesBody = () => zonesHtml().replace(/(<span>Zone (\d+)(?: · [^<]*)?<\/span>)(<\/div>)/g, (_, where, n, end) => where + spokenLine(Number(n)) + end);
  },
});

// What the pretend panel says. mode: ready (a draft pack), released (a pack that is not a draft), unavailable
// (installed, not ready), none (no pack), error (the information can't be loaded).
const voiceDemo = {
  mode: 'ready', status: true, chime: true, walk: true, enabled: true, upload: 'ok',
  get() {
    const installed = this.mode !== 'none';
    const ready = this.mode === 'ready' || this.mode === 'released';
    return {installed, locale: installed ? 'en-US' : '', name: installed ? 'English (United States)' : '', version: installed ? 1 : 0,
      draft: this.mode === 'ready' || this.mode === 'unavailable', ready, reason: installed ? (ready ? '' : 'unavailable') : 'no_pack', free: 1.4e6};
  },
};
const voiceRoleMay = () => isInstaller() || state?.authority === 'master';
// The pretend choices: [value, label, mode, announcements on]
const VOICE_PRETEND = [
  ['ready', 'Pretend: draft pack ready, announcements on', 'ready', true],
  ['off', 'Pretend: announcements off', 'ready', false],
  ['released', 'Pretend: released pack ready, announcements on', 'released', true],
  ['unavailable', 'Pretend: pack installed, voice unavailable', 'unavailable', false],
  ['none', 'Pretend: no voice pack', 'none', false],
  ['error', 'Pretend: voice information could not be loaded', 'error', false],
];
const voicePretendValue = () => (VOICE_PRETEND.find(([, , mode, on]) => mode === voiceDemo.mode && on === voiceDemo.enabled) || VOICE_PRETEND.find(([, , mode]) => mode === voiceDemo.mode))[0];
const spokenWords = {1: ['Front', 'Door'], 3: ['Hall', 'Motion'], 4: ['Garage', 'Side', 'Door']};
// A zone's computed `spoken` object, as the zone response would carry it: zones 2 and 5 are empty, 3 keeps Hall Motion and needs review.
function spokenOf(n) {
  const words = spokenWords[n] || [];
  const base = {words, summary: words.join(' '), state: !words.length ? 'empty' : n === 3 ? 'review' : 'ready'};
  if (base.state === 'ready' && voiceDemo.mode === 'none') base.state = 'missing';
  else if (base.state === 'ready' && !voiceDemo.enabled) base.state = 'off';
  return base;
}
function spokenState(s) {
  return {ready: [t('Ready'), 'good'], empty: [t('Choose words'), ''], review: [t('Review spoken name'), 'warn'],
    off: [t('Announcements are off'), ''], missing: [t('Voice pack not installed'), 'warn']}[s.state];
}
function spokenRow(n) {
  const s = spokenOf(n);
  const [word, kind] = spokenState(s);
  const may = isInstaller();
  return `<div class="row spoken-row"><div><span class="label">${ht('Spoken name')}</span>
    <span class="help">${ht('The recorded words the panel says for this zone. Chosen at the keypad.')}</span></div>
    <div class="control"><span class="what"><strong>${esc(s.summary || '-')}</strong>${tag(word, kind)}</span>
    ${may ? `<a class="button small" href="#/keypad" data-k="spoken-keypad">${ht('Choose words on the panel')}</a>` : ''}</div></div>`;
}
// The same, read-only, on a Zones > All zones card (every role that sees the zone).
function spokenLine(n) {
  const s = spokenOf(n);
  const [word, kind] = spokenState(s);
  return `<span class="spoken-line"><small>${ht('Spoken name')}</small> <strong>${esc(s.summary || '-')}</strong>${tag(word, kind)}</span>`;
}
const voiceLocaleName = locale => locale === 'en-US' ? ht('English (United States)') : esc(locale);
function voiceCard(v) {
  const may = voiceRoleMay();
  const gate = !!state?.locked;
  const known = !!v;
  const off = !voiceDemo.enabled;
  const sw = (name, label, help, on, locked) => fieldRow({name, label, type: 'switch', help, locked}, on);
  const status = !known ? [t('Voice information could not be loaded. Try again.'), 'warn']
    : !v.installed ? [t('Voice pack not installed'), 'warn']
    : !v.ready ? [t('Voice is unavailable.'), 'warn'] : [t('Ready'), 'good'];
  const canPreview = known && v.ready && !gate;
  return `<section class="section" id="voice-card"><div class="card"><div class="card-head"><div><h2>${ht('Announcements')}</h2>
    <p>${ht('The panel says what is happening, in words. Off until you turn it on. Alarms, fire and CO always sound at full volume.')}</p></div></div>
    <div class="rows">
      ${sw('voice_enabled', t('Announcements'), known && v.installed ? t('Uses the volume above.') : known ? t('Needs a voice pack.') : '', voiceDemo.enabled, gate || !known)}
      <p class="dialog-error" id="voice-error" role="alert" hidden></p>
      ${sw('voice_status', t('Status messages'), t('Says arming, armed, disarmed, entry and not ready messages.'), voiceDemo.status, off || gate)}
      ${sw('voice_chime', t('Zone chime'), t('Says the zone’s spoken name after the chime.'), voiceDemo.chime, off || gate)}
      ${sw('voice_walk', t('Walk test'), t('Says each zone as it opens'), voiceDemo.walk, off || gate)}
      <div class="row"><div><span class="label">${ht('Voice language')}</span><span class="help">${ht('Changing the page language does not change the voice.')}</span></div>
        <div class="control"><span class="what">${known && v.installed ? voiceLocaleName(v.locale) : '-'}</span></div></div>
      <div class="row"><div><span class="label">${ht('Voice pack')}</span><span class="help">${esc(known && v.installed ? t('Version {0}', v.version) : known ? t('No voice pack is installed.') : '')}</span></div>
        <div class="control"><span class="what">${known && v.installed ? esc(v.name) : ''} ${tag(status[0], status[1])}${known && v.installed && v.draft ? ' ' + tag(t('Draft'), 'warn') : ''}
          ${known ? '' : `<button type="button" class="small" id="voice-retry">${ht('Try again')}</button>`}</span></div></div>
      <div class="row"><div><span class="label">${ht('Preview')}</span><span class="help">${ht('Plays on the panel. Use the Remote keypad.')}</span></div>
        <div class="control">${canPreview ? `<a class="button small" href="#/keypad">${ht('Open the Remote keypad')}</a>`
          : `<button type="button" class="small" disabled>${ht('Open the Remote keypad')}</button>`}</div></div>
    </div>
    ${may ? `<form id="voice-upload-form"><fieldset${gate ? ' disabled' : ''} style="border:0;margin:0;padding:0"><div class="rows"><div class="row"><div><label class="label" for="voice-file">${ht('Voice pack')}</label>
      <span class="help">${ht('A signed .robovoice file. Installing one never turns Announcements on.')}</span></div>
      <div class="control"><input id="voice-file" name="file" type="file" accept=".robovoice"></div></div></div>
      <div class="card-foot"><span class="note" id="voice-upload-note" role="status">${gate ? ht('The panel is armed or an alarm is sounding. Changes wait until everything is disarmed and quiet.') : ''}</span><button class="primary">${ht('Install')}</button></div></fieldset></form>`
      : `<p class="note gap">${ht('Only the system master or installer can install a voice pack.')}</p>`}
    <div class="mockup-only"><strong>Mockup only</strong>
      <select id="voice-pretend" aria-label="Pretend">
        ${VOICE_PRETEND.map(([value, label]) => `<option value="${value}"${voicePretendValue() === value ? ' selected' : ''}>${label}</option>`).join('')}</select>
      <select id="voice-answer" aria-label="Upload answer">
        ${['ok', 'too_big', 'bad_signature', 'armed', 'not_allowed', 'busy'].map(r => `<option value="${r}"${voiceDemo.upload === r ? ' selected' : ''}>Upload answers: ${r}</option>`).join('')}</select>
      <small>The panel answers these itself; this box only picks which.</small></div>
  </div></section>`;
}
const VOICE_RESULTS = () => ({
  too_big: t('The voice pack is too large to install.'),
  bad_signature: t('The voice pack is damaged or was not signed for this panel.'),
  armed: t('The panel is armed or an alarm is sounding. Changes wait until everything is disarmed and quiet.'),
  locked: t('The panel is armed or an alarm is sounding. Changes wait until everything is disarmed and quiet.'),
  not_allowed: t('Only the system master or installer can install a voice pack.'),
  busy: t('The panel is busy. Try again in a moment.'),
});
// Why Announcements can't be turned on, from the pack's readiness.
const voiceRefusal = v => !v ? '' : !v.installed ? t('Announcements cannot be turned on because the voice pack is not installed.')
  : !v.ready ? t('Announcements cannot be turned on because voice is unavailable.') : '';
function bindVoice() {
  $('#voice-pretend')?.addEventListener('change', async event => {
    const [, , mode, on] = VOICE_PRETEND.find(([value]) => value === event.target.value);
    voiceDemo.mode = mode;
    voiceDemo.enabled = on;
    await show();
  });
  $('#voice-answer')?.addEventListener('change', event => { voiceDemo.upload = event.target.value; });
  $('#voice-retry')?.addEventListener('click', () => show());
  for (const name of ['voice_enabled', 'voice_status', 'voice_chime', 'voice_walk']) {
    const input = $(`[name="${name}"]`);
    if (!input) continue;
    input.addEventListener('change', async () => {
      const e = $('#voice-error');
      // The panel's own gates: nothing changes while armed, and enabling needs ready audio.
      if (state?.locked) {
        input.checked = !input.checked;
        e.textContent = VOICE_RESULTS().armed;
        e.hidden = false;
        return;
      }
      const v = await api('voices').catch(() => null);
      if (name === 'voice_enabled' && input.checked && (!v || !v.ready)) {
        input.checked = false;
        e.textContent = v ? voiceRefusal(v) : t('Voice information could not be loaded. Try again.');
        e.hidden = false;
        return;
      }
      voiceDemo[{voice_enabled: 'enabled', voice_status: 'status', voice_chime: 'chime', voice_walk: 'walk'}[name]] = input.checked;
      await show();
    });
  }
  const form = $('#voice-upload-form');
  if (form) form.onsubmit = async event => {
    event.preventDefault();
    const file = form.file.files[0], note = $('#voice-upload-note'), button = form.querySelector('button');
    if (!file) { note.textContent = t('Choose a file.'); return; }
    button.disabled = true;
    note.textContent = t('Checking and saving the file…');
    try {
      const response = await fetch('/api/v1/voices', {method: 'PUT', credentials: 'same-origin', cache: 'no-store',
        headers: {'Content-Type': 'application/octet-stream'}, body: file});
      const answer = await response.json().catch(() => ({}));
      if (!response.ok || answer.result !== 'ok') throw new Error(VOICE_RESULTS()[answer.result] || t('The panel answered {0}.', response.status));
      await show(); // the card shows the installed pack; no toast
    } catch (error) {
      note.textContent = error.message;
      button.disabled = false;
    }
  };
}
