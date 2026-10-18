// Interactive proposal only: no production API or panel configuration changes.
proposal({
  bl: 'BL-048', title: 'Partitions, devices and keypad settings', status: 'draft', decided: '', role: 'installer',
  why: ['Show configured partitions and devices, with Add and Remove instead of unused slots.',
    'Use the same popup editor for partitions and devices, with Cancel in both.'],
  look: [['partitions', 'Active partitions and devices'], ['keypad-partition/1', 'Keypad editor: switching allowed'],
    ['keypad-partition/never', 'Never disables the seconds field'], ['keypad-partition/2', 'Locked keypad: return options hidden'],
    ['keypad-partition/local', 'This panel'], ['partitions/full', 'At capacity: Add buttons disabled'],
    ['partitions/empty', 'No paired keypads'], ['partitions/armed', 'Armed: changes disabled'],
    ['partitions/error', 'Offline keypad'], ['link-updates', 'Automatic update progress'],
    ['link-updates/failed', 'An update failed']].map(([route, label]) => [route, label, {parts: '2'}]),
  replaces: ['partitions', 'updates'],
  today: {'keypad-partition': 'partitions', 'link-updates': 'updates'},
  notes: ['Draft: owner review required before production implementation (WEB-026).',
    'Only configured entries appear. Add reserves a free slot only after confirmation; Remove frees it. Limits shown are four partitions and sixteen devices.',
    'Keypad row and Edit open the same popup, including Cancel. Switching off hides both return controls while retaining their values; Never disables seconds and visibly dims that row.',
    'Keypad supervision is automatic, so no Check-in supervision field. Expanders and receivers retain their supervision setting.',
    'The built-in panel cannot be removed. A partition with assigned devices or zones must be cleared first; removal never silently reassigns protection.',
    'Production must reconcile device slots and paired keypad identities and enforce capacities, dependencies and authorization in the API. Adding a remote keypad must complete pairing before activating its record.',
    'No extra keypad code for selecting a view. Alarm authorization stays unchanged; audio remains local.',
    'All edits and removals here change fictional mock data only. No production API changes.'],
  css: `.keypad-return-disabled { opacity: .45; }
    .proposal-device-actions { display: flex; flex-wrap: wrap; gap: .5rem; }`,
  apply() {
    const parts = [
      {number: 1, name: 'Home', exit_delay_s: 60, entry_delay1_s: 30, entry_delay2_s: 45},
      {number: 2, name: 'Garage', exit_delay_s: 60, entry_delay1_s: 30, entry_delay2_s: 45},
    ];
    const devices = [
      {id: 'local', name: 'Panel keypad', kind: 'keypad', home: 1, allowed: false, seconds: 30, never: false},
      {id: '1', name: 'Hall keypad', kind: 'keypad', home: 1, allowed: true, seconds: 30, never: false},
      {id: '2', name: 'Workshop keypad', kind: 'keypad', home: 2, allowed: false, seconds: 30, never: false},
    ];
    let nextDevice = 3, full = false;
    const partName = n => parts.find(p => p.number === Number(n))?.name || t('Unassigned');
    const summary = d => d.kind !== 'keypad' ? t('Partition: {0}', partName(d.home)) : !d.allowed ? t('Locked to {0}', partName(d.home)) :
      d.never ? t('Default: {0} · No automatic return', partName(d.home)) : t('Default: {0} · Returns after {1} seconds inactive', partName(d.home), d.seconds);
    const backToList = () => go('partitions');
    function remove(kind, item) {
      const isPart = kind === 'partition';
      review({title: t('Remove {0}?', item.name), intro: isPart ? t('This partition will no longer appear in the list.') : t('This device will be removed from this system.'),
        confirm: t('Remove'), danger: true, done: t('Removed.'), after: backToList,
        run: async () => { const list = isPart ? parts : devices; list.splice(list.indexOf(item), 1); }});
    }
    function editPartition(item, readOnly, isNew = false) {
      const specs = partitionSpecs().filter(s => s.name !== 'enabled');
      editDialog({title: isNew ? t('Add partition') : item.name, specs, values: isNew ? {} : item, initial: item, readOnly,
        reviewOptions: {title: isNew ? t('Add partition') : t('Save partition'), done: t('Partition saved.'), after: backToList,
          run: async patch => { Object.assign(item, patch); if (isNew) parts.push(item); }}});
    }
    function editDevice(item, readOnly, isNew = false) {
      const specs = [
        {name: 'name', label: t('Name'), type: 'text', required: true, maxLength: 24},
        ...(isNew ? [{name: 'kind', label: t('Device type'), type: 'select', options: [['keypad', t('Keypad')], ['expander', t('Expander')], ['receiver', t('Receiver')]]}] : []),
        {name: 'home', label: t('Default partition'), type: 'select', numeric: true, options: parts.map(p => [p.number, p.name])},
        {name: 'allowed', label: t('Allow partition switching'), type: 'switch', help: t('Off locks this keypad to its default partition.'), showIf: v => (v.kind || item.kind) === 'keypad'},
        {name: 'seconds', label: t('Return after inactivity'), type: 'number', min: 1, max: 3600, step: 1, unit: t('seconds'),
          help: t('Default: 30 seconds. Using any screen restarts the inactivity timer.'), showIf: v => (v.kind || item.kind) === 'keypad' && v.allowed},
        {name: 'never', label: t('Never return automatically'), type: 'switch', showIf: v => (v.kind || item.kind) === 'keypad' && v.allowed},
        {name: 'supervision_s', label: t('Check-in supervision'), type: 'number', min: 0, max: 65535, step: 1, unit: t('s'), help: t('0: not supervised.'), showIf: v => (v.kind || item.kind) !== 'keypad'},
      ];
      editDialog({title: isNew ? t('Add device') : item.name, specs, values: isNew ? {} : item, initial: item, readOnly,
        reviewOptions: {title: isNew ? t('Add device') : t('Save keypad settings'), done: t('Device saved.'), after: backToList,
          run: async patch => { Object.assign(item, patch); if (isNew) devices.push(item); }},
        bindExtra(form) {
          const update = () => {
            const never = form.querySelector('[name="never"]').checked;
            const input = form.querySelector('[name="seconds"]');
            input.disabled = readOnly || never;
            input.closest('[data-field]').classList.toggle('keypad-return-disabled', never);
          };
          form.addEventListener('change', update); update();
        }});
    }
    PAGES.partitions.render = async mode => {
      if (mode === 'full' && !full) {
        full = true;
        for (let n = 3; n <= 4; n++) if (!parts.some(p => p.number === n)) parts.push({number: n, name: t('Partition {0}', n), exit_delay_s: 60, entry_delay1_s: 30, entry_delay2_s: 45});
        while (devices.length < 16) { const id = String(nextDevice++); devices.push({id, name: t('Expander {0}', id), kind: 'expander', home: 1, supervision_s: 300}); }
      }
      const readOnly = mode === 'armed' || !!state?.locked;
      const shown = mode === 'empty' ? devices.filter(d => d.id === 'local') : devices;
      const alarm = await loadAlarm();
      const used = p => parts.length === 1 || devices.some(d => Number(d.home) === p.number) || alarm.zones.some(z => z.enabled && z.partition === p.number);
      const actions = (kind, id, blocked, reason = '') => `<div class="proposal-device-actions"><button data-edit-${kind}="${esc(id)}">${ht('Edit')}</button><button class="danger" data-remove-${kind}="${esc(id)}" ${readOnly || blocked ? 'disabled' : ''} ${reason ? `title="${esc(reason)}"` : ''}>${ht('Remove')}</button></div>`;
      return {html: pageHead(t('Partitions and devices'), t('Manage the partitions and devices configured on this system.')) +
        (readOnly ? notice('warn', t('Disarm every partition before making changes.')) : '') +
        `<section class="section"><div class="section-head"><div><h2>${ht('Partitions')}</h2><p>${ht('{0} of {1} configured', parts.length, 4)}</p></div><button id="add-partition" ${readOnly || parts.length >= 4 ? 'disabled' : ''}>${ht('Add partition')}</button></div><div class="card"><ul class="list clickable">` +
        parts.map(p => `<li data-partition-edit="${p.number}" tabindex="0"><div class="main"><strong>${esc(p.name)}</strong><span>${ht('Exit {0} · entry {1} / {2}', fmtDuration(p.exit_delay_s), fmtDuration(p.entry_delay1_s), fmtDuration(p.entry_delay2_s))}</span>${used(p) ? `<span>${ht('Move assigned devices and zones before removing this partition.')}</span>` : ''}</div>${actions('partition', p.number, used(p), t('Partition still assigned or the last partition.'))}</li>`).join('') +
        `</ul></div></section><section class="section"><div class="section-head"><div><h2>${ht('Devices')}</h2><p>${ht('{0} of {1} configured', shown.length, 16)}</p></div><button id="add-device" ${readOnly || devices.length >= 16 ? 'disabled' : ''}>${ht('Add device')}</button></div><div class="card"><ul class="list clickable">` +
        shown.map(d => `<li data-device-edit="${esc(d.id)}" tabindex="0"><div class="main"><strong>${esc(d.name)}</strong><span>${esc(summary(d))}</span>${d.id === 'local' ? `<span>${ht('Built-in keypad')}</span>` : ''}</div>${mode === 'error' && d.id === '2' ? tag(t('Not responding'), 'warn') : ''}${actions('device', d.id, d.id === 'local', d.id === 'local' ? t('The built-in keypad cannot be removed.') : '')}</li>`).join('') +
        `</ul></div></section>`, bind() {
          const wire = (kind, list, edit) => {
            $$(`[data-${kind}-edit]`).forEach(row => {
              const id = row.getAttribute(`data-${kind}-edit`);
              const item = list.find(x => String(kind === 'partition' ? x.number : x.id) === id);
              const open = () => edit(item, readOnly || (mode === 'error' && item.id === '2'));
              row.onclick = event => { if (!event.target.closest('button')) open(); };
              row.onkeydown = event => { if (event.target === row && (event.key === 'Enter' || event.key === ' ')) { event.preventDefault(); open(); }};
              row.querySelector(`[data-edit-${kind}]`).onclick = open;
              row.querySelector(`[data-remove-${kind}]`).onclick = () => remove(kind, item);
            });
          };
          wire('partition', parts, editPartition); wire('device', devices, editDevice);
          $('#add-partition').onclick = () => { const number = [1,2,3,4].find(n => !parts.some(p => p.number === n)); if (number) editPartition({number, name: '', exit_delay_s: 60, entry_delay1_s: 30, entry_delay2_s: 45}, false, true); };
          $('#add-device').onclick = () => { if (devices.length < 16) editDevice({id: String(nextDevice++), name: '', kind: 'keypad', home: parts[0].number, allowed: false, seconds: 30, never: false, supervision_s: 0}, false, true); };
        }};
    };
    PAGES['keypad-partition'] = {title: tx('Keypad settings'), parent: 'partitions', nav: false, access: 'installer', render: async arg => {
      const result = await PAGES.partitions.render('');
      return {...result, bind() { result.bind(); const item = devices.find(d => d.id === arg) || devices[1];
        editDevice(arg === 'never' ? {...item, never: true} : item, !!state?.locked); }};
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
