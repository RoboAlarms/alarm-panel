// Proposed browser management of paired link keypads; no production API changes yet.
proposal({
  bl: 'BL-048',
  title: 'Keypad partition settings and updates',
  status: 'draft',
  decided: '',
  role: 'installer',
  why: ['Paired remote keypads are missing from Partitions and devices.',
    'Set each panel’s default partition, switching permission and inactivity return independently.'],
  look: [['link-keypads', 'Start here: choose a panel'], ['keypad-partition/1', 'Switching allowed: 30 seconds idle'],
    ['keypad-partition/never', 'Never: seconds disabled'], ['keypad-partition/2', 'Locked to its default partition'],
    ['keypad-partition/local', 'This panel has its own settings'], ['keypad-partition/armed', 'Settings locked while armed'], ['link-keypads/empty', 'No paired keypads'],
    ['link-keypads/armed', 'Settings locked while armed'], ['link-keypads/error', 'A keypad is not responding'], ['keypad-partition/offline', 'Offline settings are read-only'],
    ['link-updates', 'Automatic update progress'], ['link-updates/failed', 'An update failed']],
  replaces: ['partitions', 'updates'],
  today: {'link-keypads': 'partitions', 'link-updates': 'updates', 'keypad-partition': 'partitions'},
  notes: ['Draft: requires owner approval before production implementation (WEB-026).',
    'Requires authenticated link-keypad list/edit API and firmware-plan status API. Edits use the signed-in installer’s authority and require all partitions disarmed; no extra keypad code.',
    'Assignment uses the existing home-partition operation, not the generic device table. Only enabled partitions are offered.',
    'Updates follow the master automatically. No independent version selector or manual install button is added.',
    'No code to switch the touchscreen view. Alarm actions retain normal authorization. Activity on any page renews the inactivity timer.',
    'Never sits below the seconds field and disables it without losing its value. Switching off disables both return controls.',
    'This panel and paired keypads have independent policies; no audio settings are copied.',
    'Mock data and changes are confined to this proposal.'],
  apply() {
    const rows = [
      {id: 'local', name: 'This panel', home: 1, allowed: false, seconds: 30, never: false},
      {id: '1', name: 'Hall keypad', home: 1, allowed: true, seconds: 30, never: false},
      {id: '2', name: 'Workshop keypad', home: 2, allowed: false, seconds: 30, never: false},
    ];
    const names = {1: 'House', 2: 'Workshop'};
    const summary = k => !k.allowed ? t('Locked to {0}', names[k.home]) :
      k.never ? t('Default: {0} · No automatic return', names[k.home]) :
      t('Default: {0} · Returns after {1} seconds inactive', names[k.home], k.seconds);
    PAGES['link-keypads'] = {title: tx('Keypad partition settings'), icon: 'monitor', group: tx('Installer'),
      access: 'installer', render: async mode => ({
        html: pageHead(t('Keypad partition settings'), t('Choose a panel to set its default partition and whether it can switch views.')) +
          (mode === 'armed' ? notice('warn', t('Disarm every partition before changing keypad settings.')) : '') +
          `<div class="card"><ul class="list">${rows.filter(k => mode !== 'empty' || k.id === 'local').map(k =>
            `<li><div class="main"><strong>${esc(k.name)}</strong><span>${esc(summary(k))}</span></div>` +
            (k.id === 'local' ? '' : tag(mode === 'error' && k.id === '2' ? t('Not responding') : t('Online'), mode === 'error' && k.id === '2' ? 'warn' : 'good')) +
            `<a class="button" href="#/keypad-partition/${mode === 'armed' ? 'armed' : mode === 'error' && k.id === '2' ? 'offline' : k.id}">${ht('Partition settings')}</a></li>`).join('')}
            ${mode === 'empty' ? `<li class="empty">${ht('No remote keypads paired. Pair a keypad at the alarm panel.')}</li>` : ''}</ul></div>`,
      })};
    PAGES['keypad-partition'] = {title: tx('Partition settings'), parent: 'link-keypads', nav: false,
      access: 'installer', render: async arg => {
        const armed = arg === 'armed' || !!state?.locked;
        const offline = arg === 'offline';
        const original = rows.find(k => k.id === (offline ? '2' : arg)) || rows[1];
        const values = {...original, ...(arg === 'never' ? {never: true} : {})};
        const specs = [
          {name: 'home', label: t('Default partition'), type: 'select', numeric: true, options: Object.entries(names).map(([n, name]) => [Number(n), name])},
          {name: 'allowed', label: t('Allow partition switching'), type: 'switch', help: t('Off locks this panel to its default partition.')},
          {name: 'seconds', label: t('Return after inactivity'), type: 'number', min: 1, max: 3600, step: 1, unit: t('seconds'), locked: !values.allowed || values.never,
            help: t('Default: 30 seconds. Using any screen restarts the inactivity timer.')},
          {name: 'never', label: t('Never return automatically'), type: 'switch', locked: !values.allowed},
        ];
        return {html: pageHead(t('{0}: partition settings', original.name), t('These settings apply only to this panel.')) +
          `<p><a href="#/link-keypads">${ht('Back to keypads')}</a></p>` +
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
    PAGES['link-updates'] = {title: tx('Keypad updates'), icon: 'download', group: tx('System'),
      render: async mode => ({html: pageHead(t('Keypad updates'), t('Remote keypads update automatically to the master’s version.')) +
        `<div class="card"><ul class="list"><li><div class="main"><strong>${ht('Hall keypad')}</strong><span>${ht('Up to date')}</span></div>${tag('0.7.4', 'good')}</li>` +
        `<li><div class="main"><strong>${ht('Workshop keypad')}</strong><span>${mode === 'failed' ? ht('Update failed. The keypad still runs its previous version.') : ht('Receiving update from the master: 52%')}</span></div>` +
        tag(mode === 'failed' ? t('Needs attention') : t('Updating'), mode === 'failed' ? 'warn' : '') + `</li></ul></div>` +
        `<p class="muted">${ht('The master waits for a safe time to restart. You can continue using the keypads during the download.')}</p>`})};
    for (const [page, target, label] of [['partitions', 'link-keypads', 'Keypad partition settings'], ['updates', 'link-updates', 'Keypad updates']]) {
      const original = PAGES[page].render;
      PAGES[page].render = async (...args) => {
        const result = await original(...args);
        return {...result, html: result.html + `<section class="section"><a href="#/${target}">${ht(label)}</a></section>`};
      };
    }
  },
});
