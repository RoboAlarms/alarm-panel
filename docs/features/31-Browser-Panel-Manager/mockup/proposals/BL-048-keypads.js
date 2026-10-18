// Proposed browser management of paired link keypads; no production API changes yet.
proposal({
  bl: 'BL-048',
  title: 'Keypad partition settings and updates',
  status: 'draft',
  decided: '',
  role: 'installer',
  why: ['Paired remote keypads are missing from Partitions and devices.',
    'Set each panel’s default partition, switching permission and inactivity return independently.'],
  look: [['partitions', 'Partitions and devices: choose a keypad'], ['keypad-partition/1', 'Switching allowed: 30 seconds idle'],
    ['keypad-partition/never', 'Never: seconds disabled'], ['keypad-partition/2', 'Locked to its default partition'],
    ['keypad-partition/local', 'This panel has its own settings'], ['keypad-partition/armed', 'Settings locked while armed'], ['link-keypads/empty', 'No paired keypads'],
    ['link-keypads/armed', 'Settings locked while armed'], ['link-keypads/error', 'A keypad is not responding'], ['keypad-partition/offline', 'Offline settings are read-only'],
    ['link-updates', 'Automatic update progress'], ['link-updates/failed', 'An update failed']].map(([route, label]) => [route, label, {parts: '2'}]),
  replaces: ['partitions', 'updates'],
  today: {'link-keypads': 'partitions', 'link-updates': 'updates', 'keypad-partition': 'partitions'},
  notes: ['Draft: requires owner approval before production implementation (WEB-026).',
    'Requires authenticated link-keypad list/edit API and firmware-plan status API. Edits use the signed-in installer’s authority and require all partitions disarmed; no extra keypad code.',
    'Assignment uses the existing home-partition operation, not the generic device table. Only enabled partitions are offered.',
    'Updates follow the master automatically. No independent version selector or manual install button is added.',
    'No code to switch the touchscreen view. Alarm actions retain normal authorization. Activity on any page renews the inactivity timer.',
    'Never sits below the seconds field and disables it without losing its value. Switching off disables both return controls.',
    'This panel and paired keypads have independent policies; no audio settings are copied.',
    'Open Partitions and devices, then Partition settings on a keypad row in Devices. The row and its link open the same editor. The local keypad keeps In use, Name and Check-in supervision alongside its partition policy; no extra Installer navigation section.',
    'Mock data and changes are confined to this proposal.'],
  apply() {
    const rows = [
      {id: 'local', name: 'Panel keypad', enabled: true, supervision_s: 0, home: 1, allowed: false, seconds: 30, never: false},
      {id: '1', name: 'Hall keypad', home: 1, allowed: true, seconds: 30, never: false},
      {id: '2', name: 'Workshop keypad', home: 2, allowed: false, seconds: 30, never: false},
    ];
    const names = {1: 'Home', 2: 'Garage'};
    const summary = k => !k.allowed ? t('Locked to {0}', names[k.home]) :
      k.never ? t('Default: {0} · No automatic return', names[k.home]) :
      t('Default: {0} · Returns after {1} seconds inactive', names[k.home], k.seconds);
    const originalPartitions = PAGES.partitions.render;
    PAGES.partitions.render = async mode => {
      const result = await originalPartitions();
      const armed = mode === 'armed' || !!state?.locked;
      const policyLink = k => `<a data-keypad-policy href="#/keypad-partition/${armed ? 'armed' : mode === 'error' && k.id === '2' ? 'offline' : k.id}">${ht('Partition settings')}</a>`;
      const remotes = mode === 'empty' ? '' : rows.slice(1).map(k =>
        `<li tabindex="0"><div class="main"><strong>${esc(k.name)}</strong><span>${esc(summary(k))}</span></div>` +
        tag(mode === 'error' && k.id === '2' ? t('Not responding') : t('Online'), mode === 'error' && k.id === '2' ? 'warn' : 'good') +
        policyLink(k) + `</li>`).join('');
      // Keep other device editors. Each keypad row and its link open one combined editor.
      const html = result.html.replace(/(<li data-device="1"[\s\S]*?)(<\/li>)/,
        (_, row, end) => row + policyLink(rows[0]) + end + remotes);
      return {...result, html, bind() {
        result.bind?.();
        $$('[data-keypad-policy]').forEach(a => {
          const row = a.closest('li');
          const open = () => { location.hash = a.getAttribute('href'); };
          row.onclick = open;
          row.onkeydown = event => {
            if (event.key === 'Enter' || event.key === ' ') { event.preventDefault(); open(); }
          };
          a.onclick = event => { event.preventDefault(); event.stopPropagation(); open(); };
        });
      }};
    };
    // Scenario routes are review shortcuts, never separate menu entries.
    PAGES['link-keypads'] = {title: tx('Partitions and devices'), parent: 'partitions', nav: false,
      access: 'installer', render: mode => PAGES.partitions.render(mode)};
    PAGES['keypad-partition'] = {title: tx('Partition settings'), parent: 'partitions', nav: false,
      access: 'installer', render: async arg => {
        const armed = arg === 'armed' || !!state?.locked;
        const offline = arg === 'offline';
        const original = rows.find(k => k.id === (offline ? '2' : arg)) || rows[1];
        const values = {...original, ...(arg === 'never' ? {never: true} : {})};
        const specs = [
          ...(original.id === 'local' ? [
            {name: 'enabled', label: t('In use'), type: 'switch'},
            {name: 'name', label: t('Name'), type: 'text'},
            {name: 'supervision_s', label: t('Check-in supervision'), type: 'number', min: 0, max: 65535, step: 1, unit: t('s'), help: t('0: not supervised.')},
          ] : []),
          {name: 'home', label: t('Default partition'), type: 'select', numeric: true, options: Object.entries(names).map(([n, name]) => [Number(n), name])},
          {name: 'allowed', label: t('Allow partition switching'), type: 'switch', help: t('Off locks this panel to its default partition.')},
          {name: 'seconds', label: t('Return after inactivity'), type: 'number', min: 1, max: 3600, step: 1, unit: t('seconds'), locked: !values.allowed || values.never,
            help: t('Default: 30 seconds. Using any screen restarts the inactivity timer.')},
          {name: 'never', label: t('Never return automatically'), type: 'switch', locked: !values.allowed},
        ];
        return {html: pageHead(t('{0}: partition settings', original.name), t('These settings apply only to this panel.')) +
          `<p><a href="#/partitions">${ht('Back to Partitions and devices')}</a></p>` +
          (armed ? notice('warn', t('Disarm every partition before changing keypad settings.')) : '') +
          (offline ? notice('warn', t('This keypad is not responding. Try again when it is online.')) : '') +
          settingsCard({id: 'keypad-policy', title: t('Partition settings'), specs, values, readOnly: armed || offline}),
          bind() {
            const form = $('#keypad-policy');
            bindSettingsCard('keypad-policy', specs, values, ({changes, values: patch}) =>
              review({title: t('Save keypad partition settings'), changes, done: t('Keypad settings saved.'),
                run: async () => { Object.assign(original, patch); }}));
            const update = () => {
              const allow = form.querySelector('[name="allowed"]').checked;
              const never = form.querySelector('[name="never"]').checked;
              for (const [name, disabled] of [['seconds', !allow || never], ['never', !allow]]) {
                form.querySelector(`[name="${name}"]`).disabled = armed || offline || disabled;
              }
            };
            form.addEventListener('change', update);
            update();
          },
        };
      }};
    PAGES['link-updates'] = {title: tx('Keypad updates'), icon: 'download', parent: 'updates', nav: false,
      render: async mode => ({html: pageHead(t('Keypad updates'), t('Remote keypads update automatically to the master’s version.')) +
        `<div class="card"><ul class="list"><li><div class="main"><strong>${ht('Hall keypad')}</strong><span>${ht('Up to date')}</span></div>${tag('0.7.4', 'good')}</li>` +
        `<li><div class="main"><strong>${ht('Workshop keypad')}</strong><span>${mode === 'failed' ? ht('Update failed. The keypad still runs its previous version.') : ht('Receiving update from the master: 52%')}</span></div>` +
        tag(mode === 'failed' ? t('Needs attention') : t('Updating'), mode === 'failed' ? 'warn' : '') + `</li></ul></div>` +
        `<p class="muted">${ht('The master waits for a safe time to restart. You can continue using the keypads during the download.')}</p>`})};
    for (const [page, target, label] of [['updates', 'link-updates', 'Keypad updates']]) {
      const original = PAGES[page].render;
      PAGES[page].render = async (...args) => {
        const result = await original(...args);
        return {...result, html: result.html + `<section class="section"><a href="#/${target}">${ht(label)}</a></section>`};
      };
    }
  },
});
