// Proposed browser management of paired link keypads; no production API changes yet.
proposal({
  bl: 'BL-048',
  title: 'Assign remote keypads and see their updates',
  status: 'draft',
  decided: '',
  role: 'installer',
  why: ['Paired remote keypads are missing from Partitions and devices.',
    'Give each keypad a named home partition and show how its automatic update is progressing.'],
  look: [['link-keypads', 'Assigned keypads'], ['link-keypads/empty', 'No paired keypads'],
    ['link-keypads/armed', 'Assignment locked while armed'], ['link-keypads/error', 'A keypad is not responding'],
    ['link-updates', 'Automatic update progress'], ['link-updates/failed', 'An update failed']],
  replaces: ['partitions', 'updates'],
  today: {'link-keypads': 'partitions', 'link-updates': 'updates'},
  notes: ['Draft: requires owner approval before production implementation (WEB-026).',
    'Requires authenticated link-keypad list/edit API and firmware-plan status API. Installer edits require a code and all partitions disarmed.',
    'Assignment uses the existing home-partition operation, not the generic device table. Only enabled partitions are offered.',
    'Updates follow the master automatically. No independent version selector or manual install button is added.',
    'Mock data and changes are confined to this proposal.'],
  apply() {
    const rows = [{id: 1, name: 'Hall keypad', home: 1}, {id: 2, name: 'Workshop keypad', home: 2}];
    const names = {1: 'House', 2: 'Workshop'};
    PAGES['link-keypads'] = {title: tx('Remote keypads'), icon: 'monitor', group: tx('Installer'),
      access: 'installer', render: async mode => ({
        html: pageHead(t('Remote keypads'), t('Each keypad controls and announces its home partition.')) +
          (mode === 'armed' ? notice(t('Disarm every partition before changing keypad assignments.')) : '') +
          `<div class="card"><ul class="list">${mode === 'empty' ? `<li class="empty">${ht('No remote keypads paired. Pair a keypad at the alarm panel.')}</li>` : rows.map(k =>
            `<li><div class="main"><strong>${esc(k.name)}</strong><span>${esc(names[k.home])} · ${ht('Checked every 5 minutes')}</span></div>` +
            tag(mode === 'error' && k.id === 2 ? t('Not responding') : t('Online'), mode === 'error' && k.id === 2 ? 'warn' : 'good') +
            `<button data-link-edit="${k.id}" ${mode === 'armed' ? 'disabled' : ''}>${ht('Assign partition')}</button></li>`).join('')}</ul></div>`,
        bind() {
          $$('[data-link-edit]').forEach(button => { button.onclick = () => {
            const k = rows.find(r => r.id === Number(button.dataset.linkEdit));
            editDialog({title: k.name, specs: [{name: 'home', label: t('Home partition'), type: 'select',
              options: Object.entries(names).map(([v, name]) => [Number(v), t(name)])}], values: {home: k.home},
              reviewOptions: {title: t('Assign keypad partition'), password: true, confirm: t('Apply'),
                done: t('Keypad assigned.'), run: async values => { k.home = Number(values.home); }}});
          }; });
        },
      })};
    PAGES['link-updates'] = {title: tx('Keypad updates'), icon: 'download', group: tx('System'),
      render: async mode => ({html: pageHead(t('Keypad updates'), t('Remote keypads update automatically to the master’s version.')) +
        `<div class="card"><ul class="list"><li><div class="main"><strong>${ht('Hall keypad')}</strong><span>${ht('Up to date')}</span></div>${tag('0.7.4', 'good')}</li>` +
        `<li><div class="main"><strong>${ht('Workshop keypad')}</strong><span>${mode === 'failed' ? ht('Update failed. The keypad still runs its previous version.') : ht('Receiving update from the master: 52%')}</span></div>` +
        tag(mode === 'failed' ? t('Needs attention') : t('Updating'), mode === 'failed' ? 'warn' : '') + `</li></ul></div>` +
        `<p class="muted">${ht('The master waits for a safe time to restart. You can continue using the keypads during the download.')}</p>`})};
    for (const [page, target, label] of [['partitions', 'link-keypads', 'Remote keypads'], ['updates', 'link-updates', 'Keypad updates']]) {
      const original = PAGES[page].render;
      PAGES[page].render = async (...args) => {
        const result = await original(...args);
        return {...result, html: result.html + `<section class="section"><a href="#/${target}">${ht(label)}</a></section>`};
      };
    }
  },
});
