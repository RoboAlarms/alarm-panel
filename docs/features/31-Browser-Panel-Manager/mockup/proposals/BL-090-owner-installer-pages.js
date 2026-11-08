// BL-090: the Owner (the system master, user 2) gets the Installer pages (docs/features/31 mockup/README.md).
// The owner's decision of 2026-11-08: the system is owned by the Owner and no code outranks it; everything the
// installer can do, the system master can do too (docs/features/03 "Authority levels", 14 CFG-002). On the panel
// the installer's code keeps its name; whether it opens Setup mode becomes the Owner's setting.
// Built, this is mostly one line in panel-pages.js (isInstaller() admits the system master) and the access table
// in panel_web.c and mock-panel.cjs (ACC.installer); the only new words are the Setup mode card.
proposal({
  bl: 'BL-090',
  title: 'The system master gets the Installer pages',
  status: 'draft',
  decided: '',
  role: 'master',
  why: [
    'Signed in as the system master (the Owner), the Panel Manager hides the whole Installer group: Zones, Z-Wave, Zigbee, Partitions and devices, System options, Access and maintenance, and Developer. The Owner can\'t change the system they own without the installer, though no code outranks the Owner (the owner\'s decision of 2026-11-08).',
    'The system master gets the same Installer group as the installer, every page editable in the same way, and Access and maintenance gains a Setup mode card: whether the installer code may open Setup mode, an Owner setting ("Let the Admin code open Setup mode"), on by default, changed only by the system master and only at the panel.',
  ],
  look: [
    ['options', 'System options as Alex, the system master: the Installer group in the menu, every option editable, Review changes', {role: 'master', scenario: 'ready'}],
    ['access', 'Access and maintenance as the system master: remote access and Developer tools, the installer and master codes, the new Setup mode card, backup, restore and factory reset', {role: 'master', scenario: 'ready'}],
    ['access/installer-code-off', 'The Setup mode card once the system master has turned "Let the installer code open Setup mode" off at the panel', {role: 'master', scenario: 'ready'}],
    ['developer', 'Developer as the system master (Developer tools are on): Install a signed file, Enroll a workstation, the development log', {role: 'master', scenario: 'ready'}],
    ['partitions', 'Partitions and devices as the system master: editable, as the installer\'s', {role: 'master', scenario: 'ready'}],
    ['zone-setup', 'Installer > Zones as the system master: open a zone, its sensor, the review', {role: 'master', scenario: 'ready'}],
    ['options', 'Armed Away: as today, the pages read and their changes wait until everything is disarmed', {role: 'master', scenario: 'armed'}],
    ['access', 'As the installer: the same pages; the Setup mode card says only the system master changes it', {role: 'installer', scenario: 'ready'}],
  ],
  replaces: ['options', 'access', 'partitions', 'zone-setup', 'developer'],
  notes: [
    'Follows the owner\'s decision of 2026-11-08 (and 2026-11-02 for Setup mode): the system is truly owned by the Owner, the system master (user 2); no code outranks the Owner; everything the installer can do, the system master can do too. The installer is called the Admin in the server\'s words; on the panel and here it stays the installer, and user 2 stays the System master, so no page gains a new word for either.',
    'Menu: the system master sees the Installer group (Zones, Z-Wave, Zigbee, Partitions and devices, System options, Access and maintenance) and, with Developer tools on, Developer under System, exactly as the installer does. The group keeps its name, Installer.',
    'Pages: nothing on them changes except Access and maintenance\'s new card; what the installer may change there, the system master may change too. Where a page asks isInstaller() today (System options, Partitions and devices, a Z-Wave device\'s Set it up again, a zone\'s Choose words on the panel, MQTT\'s Allow panic alarms, the update server and channel under Software updates), the system master gets the installer\'s controls.',
    'What stays as today: the installer still never arms or disarms (USR-001) and gets no panic buttons; the system master still arms, disarms and has the panic buttons. People and codes keeps its own rules (who may change whose code), unchanged here.',
    'Access and maintenance, new card "Setup mode", after Installer and master codes: "Setup mode (installer mode) opens at the panel while everything is disarmed. The system master’s code always opens it." One read-only row, "Let the installer code open Setup mode", On or Off, with "The installer’s code opens it too." or "Only the system master’s code opens it.", then "Only the system master changes this, at the panel." The installer sees the same card.',
    'That setting is the Owner\'s, named in the decision "Let the Admin code open Setup mode": on by default, changed only by the system master and only at the panel (the touchscreen\'s own proposal draws where); the Panel Manager shows it and never changes it. Here it says "installer code", the panel\'s word for that code; if the touchscreen\'s row says "Admin code", this row says the same (WORD-006).',
    'No factory codes: a new panel ships with no codes, so nothing here mentions a default code.',
    'API (changed): every path the firmware\'s access table (panel_web.c get_access/post_access, mock-panel.cjs ACC.installer) keeps for the installer admits the system master too, and GET /api/v1/status gives the system master access.installer: true. Status gains a read-only `installer_opens_setup` (true by default), set only at the panel. In this mockup the pretend panel stays as it is: the proposal reads status with access.installer on for the system master and, when the pretend panel refuses the system master an installer\'s request, asks it again as the installer would; and today\'s pages run with isInstaller() true for the system master. Both are stand-ins for the change above.',
    'Mockup only: "access/installer-code-off" shows the card with the setting off; the pretend panel doesn\'t keep it.',
  ],
  css: ``,
  apply(mock) {
    // The pretend panel as it will answer: the system master passes wherever the installer does.
    const real = window.fetch;
    window.fetch = (input, init = {}) => ownerFetch(real, mock, input, init);
    // Today's pages, with isInstaller() true for the system master as well (built: isInstaller() itself).
    for (const [id, page] of Object.entries(PAGES))
      if (page.access === 'installer' || ['updates', 'integration'].includes(id)) page.render = ownerToo(page.render);
    const access = PAGES.access.render;
    PAGES.access.render = async arg => {
      const page = await access(arg);
      const at = page.html.indexOf('<h2>' + ht('Backup') + '</h2>');
      const start = at < 0 ? page.html.length : page.html.lastIndexOf('<section', at);
      return {...page, html: page.html.slice(0, start) + setupModeCard(state.installer_opens_setup !== false && arg !== 'installer-code-off') + page.html.slice(start)};
    };
  },
});

const isSystemMaster = () => state?.authority === 'master';

// Runs a page as it will be built: whatever asks isInstaller() while it draws sees the system master as the installer.
function ownerToo(render) {
  return async arg => {
    if (!isSystemMaster()) return render(arg);
    const real = state;
    const seen = new Proxy(real, {get: (o, k) => k === 'authority' ? 'installer' : Reflect.get(o, k)});
    state = seen;
    try { return await render(arg); } finally { if (state === seen) state = real; }
  };
}

// Access and maintenance's new card: who may open Setup mode at the panel. Read-only here, for everyone.
function setupModeCard(on) {
  return `<section class="card gap"><div class="card-head"><div><h2>${ht('Setup mode')}</h2>
    <p>${ht('Setup mode (installer mode) opens at the panel while everything is disarmed. The system master’s code always opens it.')}</p></div></div>
    <div class="rows"><div class="row"><div><span class="label">${ht('Let the installer code open Setup mode')}</span>
      <span class="help">${on ? ht('The installer’s code opens it too.') : ht('Only the system master’s code opens it.')}
        ${ht('Only the system master changes this, at the panel.')}</span></div>
      <div class="control">${on ? tag(t('On'), 'good') : tag(t('Off'))}</div></div></div></section>`;
}

// The pretend panel's stand-in for the changed access table. A request the system master is refused only because
// it is the installer's is asked again with user 2 seen as the installer, alone: no other request runs meanwhile,
// so none sees the swap. Arming, the panic buttons and People keep their own rules, so they are never asked again.
const OWNER_KEEPS = ['status', 'control', 'partition', 'people', 'login', 'logout', 'password'];
const OWNER_REFUSED = ['not_allowed', 'Installer authority required', 'The installer signed in is required'];
let ownerGate = null, ownerBusy = 0, ownerIdle = null;
async function ownerFetch(real, mock, input, init) {
  const raw = typeof input === 'string' ? input : input.url;
  const path = (/\/api\/v1\/([^?]*)/.exec(raw) || [])[1];
  if (path === undefined) return real(input, init);
  while (ownerGate) await ownerGate;
  ownerBusy++;
  let answer;
  try { answer = await real(input, init); } finally { if (!--ownerBusy && ownerIdle) ownerIdle(); }
  const master = mock.users.find(u => u.number === state?.user && u.authority === 2);
  if (path === 'status' && answer.ok) {
    const body = await answer.json();
    if (body.authority === 'master') body.access = {...body.access, installer: true};
    if (!('installer_opens_setup' in body)) body.installer_opens_setup = true;
    return new Response(JSON.stringify(body), {status: answer.status, headers: answer.headers});
  }
  if (!master || OWNER_KEEPS.includes(path) || ![403, 409].includes(answer.status)) return answer;
  const why = await answer.clone().json().catch(() => ({}));
  if (!OWNER_REFUSED.includes(why.error)) return answer;
  while (ownerGate) await ownerGate;
  let open;
  ownerGate = new Promise(resolve => { open = resolve; });
  if (ownerBusy) await new Promise(resolve => { ownerIdle = resolve; });
  ownerIdle = null;
  master.authority = 1;
  try { return await real(input, init); } finally { master.authority = 2; ownerGate = null; open(); }
}
