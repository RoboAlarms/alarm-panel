// The viewer: a list of the screens, one screen at its real size (or fitted), the PC preview's
// picture of the same state beside or over it, a gallery, and which preview pictures have no
// mockup yet. Links: #<screen-id>, #<screen-id>/side and so on.
(() => {
  const SHOTS = window.MOCKUP_SHOTS || {};   // 'home_ready' -> 'shots/01_home_ready.png'
  const $ = (s) => document.querySelector(s);
  const all = M.screens;
  const byId = new Map();
  const problems = [];
  for (const s of all) {
    if (byId.has(s.id)) problems.push(`Two screens are called "${s.id}".`);
    byId.set(s.id, s);
  }
  for (const s of all) {
    const r = s.proposal && s.proposal.replaces;
    if (r && !byId.has(r)) problems.push(`${s.id} replaces "${r}", which isn't a screen.`);
    if (s.shot && Object.keys(SHOTS).length && !SHOTS[s.shot]) problems.push(`${s.id}: no preview picture called "${s.shot}".`);
  }
  let current = all[0] && all[0].id, mode = 'mockup';

  const statusChip = (p) => (p ? `<span class="v-st ${M.esc(p.status || 'draft')}">${M.esc(p.bl || '')} ${M.esc(p.status || 'draft')}</span>` : '');
  const html = (s) => { try { return s.render(); } catch (e) { return `<div class="v-none">${M.esc(s.id)}: ${M.esc(e.message)}</div>`; } };

  // A screen at a scale, with what goes inside: the mockup, the panel's picture, or both.
  function frame(inner, scale, cap) {
    return `<div class="v-frame">${cap ? `<div class="v-cap">${cap}</div>` : ''}
      <div class="v-scale" style="width:${1024 * scale}px;height:${600 * scale}px"><div class="v-inner" style="transform:scale(${scale})">${inner}</div></div></div>`;
  }
  const shotImg = (s, style = '') => (s.shot && SHOTS[s.shot]
    ? `<img class="v-shot" src="${M.esc(SHOTS[s.shot])}" alt="" style="${style}">`
    : `<div class="v-none" style="${style}">${s.shot ? `No picture "${M.esc(s.shot)}" here: build with --shots after a PC preview run` : 'This state has no PC preview picture yet'}</div>`);
  const fixScroll = (root) => root.querySelectorAll('[data-scroll]').forEach((el) => { el.scrollTop = +el.dataset.scroll || 0; });

  function filtered() {
    const q = $('#v-search').value.trim().toLowerCase();
    const area = $('#v-section').value, kind = $('#v-kind').value;
    return all.filter((s) => (!area || s.area === area) &&
      (!kind || (kind === 'current' ? !s.proposal : kind === 'proposals' ? !!s.proposal : s.proposal?.bl === kind)) &&
      (!q || `${s.id} ${s.title} ${s.area} ${s.proposal?.bl || ''} ${s.proposal?.status || ''}`.toLowerCase().includes(q)));
  }

  function markCurrent() {
    $('#v-list').querySelectorAll('.v-item').forEach((el) => {
      const active = el.dataset.id === current;
      el.classList.toggle('on', active);
      if (active) el.setAttribute('aria-current', 'true');
      else el.removeAttribute('aria-current');
    });
    $('#v-locate').textContent = `Viewing: ${byId.get(current)?.title || ''} · Find in list`;
  }

  function list() {
    const groups = new Map();
    const visible = filtered();
    for (const s of visible) {
      const g = s.proposal ? `Proposals · ${s.proposal.bl || ''}` : s.area;
      if (!groups.has(g)) groups.set(g, []);
      groups.get(g).push(s);
    }
    $('#v-list').innerHTML = [...groups].map(([g, ss]) => `<div class="v-area">${M.esc(g)}</div>` +
      ss.map((s) => `<button class="v-item${s.id === current ? ' on' : ''}" data-id="${M.esc(s.id)}">${M.esc(s.title)}${statusChip(s.proposal)}</button>`).join('')).join('');
    if (!visible.length) $('#v-list').innerHTML = '<p class="v-area">No matching screens</p>';
    $('#v-list').scrollTop = 0;
    $('#v-count').textContent = `${visible.length} of ${all.length}`;
    markCurrent();
  }

  function fitScale(n) {
    const w = $('#v-stage').clientWidth || 1024;
    return Math.min(1, (w - 16 * (n - 1)) / (1024 * n));
  }

  function show() {
    const s = byId.get(current);
    if (!s) return;
    const stage = $('#v-stage');
    const p = s.proposal;
    const base = p && p.replaces ? byId.get(p.replaces) : null;
    let out;
    if (mode === 'mockup' && base) {
      const k = fitScale(2);
      out = frame(html(base), k, `Now: ${M.esc(base.title)}`) + frame(html(s), k, `Proposed (${M.esc(p.bl || '')}, ${M.esc(p.status || 'draft')})`);
    } else if (mode === 'mockup') out = frame(html(s), fitScale(1));
    else if (mode === 'panel') out = frame(shotImg(s), fitScale(1), 'The PC preview');
    else if (mode === 'side') { const k = fitScale(2); out = frame(html(s), k, 'Mockup') + frame(shotImg(s), k, 'The PC preview'); }
    else {
      const o = mode === 'diff' ? 'mix-blend-mode:difference' : `opacity:${$('#v-opa').value / 100}`;
      out = frame(html(s) + shotImg(s, o), fitScale(1), mode === 'diff' ? 'Difference: black where they match' : 'Mockup with the preview over it');
    }
    stage.innerHTML = out;
    fixScroll(stage);
    $('#v-opa-wrap').hidden = mode !== 'overlay';
    const used = [...M.missing];
    $('#v-warn').hidden = !(used.length || problems.length);
    const warnings = [...problems.map(M.esc), used.length ? `Icons: ${used.map(M.esc).join(', ')}` : ''].filter(Boolean);
    $('#v-warn-summary').textContent = `Preview diagnostics (${warnings.length})`;
    $('#v-warn-list').innerHTML = warnings.join('<br>');
    $('#v-info').innerHTML = `<h2>${M.esc(s.title)} ${statusChip(p)}</h2>
      <dl><dt>Id</dt><dd><code>${M.esc(s.id)}</code></dd><dt>Area</dt><dd>${M.esc(s.area)}</dd>
      <dt>Preview picture</dt><dd>${s.shot ? `<code>${M.esc(SHOTS[s.shot] || s.shot)}</code>` : 'none'}</dd>
      ${s.notes ? `<dt>Notes</dt><dd>${M.esc(s.notes)}</dd>` : ''}
      ${p ? `<dt>Proposal</dt><dd>${M.esc(p.bl || '')} · ${M.esc(p.status || 'draft')}${p.replaces ? ` · replaces <code>${M.esc(p.replaces)}</code>` : ' · a new screen'}</dd>
        ${p.why ? `<dt>Why</dt><dd>${M.esc(p.why)}</dd>` : ''}${p.decided ? `<dt>Decided</dt><dd>${M.esc(p.decided)}</dd>` : ''}` : ''}</dl>`;
    markCurrent();
    if (location.hash.slice(1) !== `${current}${mode === 'mockup' ? '' : '/' + mode}`) history.replaceState(null, '', `#${current}${mode === 'mockup' ? '' : '/' + mode}`);
  }

  function gallery() {
    const k = 0.25;
    $('#v-gallery').innerHTML = filtered().map((s) => `<button class="v-card" data-id="${M.esc(s.id)}">${frame(html(s), k)}
      <div class="v-cap">${M.esc(s.title)} ${statusChip(s.proposal)}</div></button>`).join('');
    fixScroll($('#v-gallery'));
  }

  function coverage() {
    const covered = new Map();
    for (const s of all) if (s.shot && !s.proposal) covered.set(s.shot, [...(covered.get(s.shot) || []), s.id]);
    const names = Object.keys(SHOTS).sort((a, b) => parseInt(SHOTS[a].split('/').pop(), 10) - parseInt(SHOTS[b].split('/').pop(), 10));
    const missing = names.filter((n) => !covered.has(n));
    $('#v-coverage').innerHTML = names.length
      ? `<p>${covered.size} of ${names.length} preview pictures have a mockup screen; ${missing.length} have none yet. A screen for a new state is welcome: copy a close one and change it.</p>
        <table class="v-cov"><tr><th>Preview picture</th><th>Mockup screens</th></tr>${names.map((n) => `<tr><td><code>${M.esc(SHOTS[n].split('/').pop())}</code></td>
        <td>${(covered.get(n) || []).map((id) => `<a data-id="${M.esc(id)}">${M.esc(id)}</a>`).join(', ') || '-'}</td></tr>`).join('')}</table>`
      : '<p>This build has no preview pictures. Run the PC preview, then build the mockup with <code>--shots</code>.</p>';
  }

  function tab(t) {
    document.querySelectorAll('.v-tabs button').forEach((b) => b.classList.toggle('on', b.dataset.tab === t));
    document.querySelectorAll('.v-tab').forEach((x) => x.classList.toggle('on', x.id === `tab-${t}`));
    if (t === 'gallery') gallery();
    if (t === 'coverage') coverage();
    if (t === 'screen') show();
  }
  function open(id) { if (byId.has(id)) { current = id; tab('screen'); $('.v-main').scrollTop = 0; } }
  function step(d) {
    const visible = filtered();
    if (!visible.length) return;
    const i = visible.findIndex((s) => s.id === current);
    open(visible[i < 0 ? (d > 0 ? 0 : visible.length - 1) : (i + d + visible.length) % visible.length].id);
  }

  document.addEventListener('click', (e) => {
    const t = e.target.closest('[data-tab]'); if (t) return tab(t.dataset.tab);
    const m = e.target.closest('[data-mode]'); if (m) { mode = m.dataset.mode; document.querySelectorAll('#v-mode button').forEach((b) => b.classList.toggle('on', b === m)); return show(); }
    const i = e.target.closest('.v-item, .v-card, .v-cov a'); if (i) return open(i.dataset.id);
    const g = e.target.closest('.v-stage [data-go]');
    if (g && $('#v-click').checked && byId.has(g.dataset.go)) open(g.dataset.go);
  });
  function filterChanged() { list(); if ($('#tab-gallery').classList.contains('on')) gallery(); }
  $('#v-search').addEventListener('input', filterChanged);
  $('#v-section').addEventListener('change', filterChanged);
  $('#v-kind').addEventListener('change', filterChanged);
  $('#v-clear').addEventListener('click', () => {
    $('#v-search').value = $('#v-section').value = $('#v-kind').value = '';
    filterChanged();
  });
  $('#v-locate').addEventListener('click', () => {
    if (!filtered().some((s) => s.id === current)) $('#v-clear').click();
    const item = [...$('#v-list').querySelectorAll('.v-item')].find((el) => el.dataset.id === current);
    if (item) $('#v-list').scrollTop += item.getBoundingClientRect().top - $('#v-list').getBoundingClientRect().top - 8;
  });
  $('#v-opa').addEventListener('input', show);
  $('#v-prev').addEventListener('click', () => step(-1));
  $('#v-next').addEventListener('click', () => step(1));
  document.addEventListener('keydown', (e) => {
    if (e.target.matches('input, select, textarea, [contenteditable]')) return;
    if (e.key === 'ArrowRight') step(1);
    if (e.key === 'ArrowLeft') step(-1);
  });
  window.addEventListener('resize', () => { if ($('#tab-screen').classList.contains('on')) show(); });
  window.addEventListener('hashchange', () => fromHash());
  function fromHash() {
    const [id, m = 'mockup'] = decodeURIComponent(location.hash.slice(1)).split('/');
    if (['mockup', 'panel', 'side', 'overlay', 'diff'].includes(m)) {
      mode = m; document.querySelectorAll('#v-mode button').forEach((b) => b.classList.toggle('on', b.dataset.mode === m));
    }
    if (byId.has(id)) current = id;
    show();
  }
  // ?bare#<id>: the screen alone at 1024 x 600, for a screenshot (a 1024 x 600 window shows it whole).
  if (new URLSearchParams(location.search).has('bare')) {
    const draw = () => {
      const s = byId.get(decodeURIComponent(location.hash.slice(1)).split('/')[0]) || all[0];
      document.body.innerHTML = html(s);
      document.body.style.cssText = 'margin:0;display:block;background:#000;overflow:hidden';
      fixScroll(document.body);
    };
    window.addEventListener('hashchange', draw);
    draw();
    return;
  }
  const props = all.filter((s) => s.proposal).length;
  for (const area of [...new Set(all.map((s) => s.area))].sort()) $('#v-section').add(new Option(area, area));
  for (const bl of [...new Set(all.filter((s) => s.proposal?.bl).map((s) => s.proposal.bl))].sort()) $('#v-kind').add(new Option(`${bl} proposals`, bl));
  $('#v-sub').textContent = `${all.length - props} screens · ${props} proposal screens · built ${window.MOCKUP_BUILT}`;
  list();
  fromHash();
})();
