// The Panel Manager mockup's last script in app.html (docs/features/31 mockup/README.md): switches on the
// proposals the address asks for, starts the Panel Manager (panel.js's startup(), which build.cjs held
// back), and tells the viewer around it where it is. Not part of the product.
'use strict';
(function () {
  const q = MOCKUP.query, m = MOCKUP.internals;
  const theme = q.get('theme');
  if (theme === 'light' || theme === 'dark') document.documentElement.dataset.theme = theme;

  // ---- the proposals asked for: their CSS, their answers, then their changes to the pages ---------------
  const wanted = (q.get('p') || '').split(',').filter(Boolean);
  MOCKUP.on = MOCKUP.proposals.filter(p => wanted.includes(p.id));
  for (const p of MOCKUP.on) {
    if (p.css) {
      const style = document.createElement('style');
      style.dataset.proposal = p.id;
      style.textContent = p.css;
      document.head.append(style);
    }
    p.routes?.(MOCKUP.route, m);
    p.apply?.(m);
  }
  if (MOCKUP.on.length) document.documentElement.dataset.proposal = MOCKUP.on.map(p => p.id).join(' ');

  // ---- a sensor opens or closes, as a real one would tell the panel -----------------------------------------
  const ENTRY = [1, 2, 10], FOLLOWER = 4;
  MOCKUP.sensor = (zone, open) => {
    const z = m.zoneCfg(zone);
    if (!z || !z.type) return;
    if (!open) { m.live.open.delete(zone); return; }
    m.live.open.add(zone);
    const p = m.parts[z.partition - 1], w = m.walks[p.number - 1];
    if (w.running) return m.walkMark(p, zone, 'fault');
    if (p.bypass.has(zone) || !m.armedIn(z, p.level)) return;
    if (p.state === 'armed' && !p.alarm) {
      if (ENTRY.includes(z.type) && !p.no_entry_delay) m.beginEntry(p, zone); else m.tripZone(p, zone);
    } else if (p.state === 'entry_delay' && !ENTRY.includes(z.type) && z.type !== FOLLOWER) m.tripZone(p, zone);
    else if (p.alarm) m.tripZone(p, zone);
  };

  // ---- the viewer: where this frame is, the pages it can open, the zones it can trip ------------------
  const tell = () => {
    if (parent === window) return;
    const pages = Object.entries(PAGES).map(([id, page]) => ({id, title: t(page.title), group: page.group ? t(page.group) : '',
      nav: page.nav !== false, allowed: !!state && pageAllowed(page)}));
    const zones = m.alarm.zones.filter(z => z.type).map(z => ({number: z.number, name: z.name, open: m.live.open.has(z.number)}));
    parent.postMessage({mockup: 'route', frame: q.get('frame') || '', hash: location.hash, title: document.title, signedIn: !!state,
      pages, zones, proposals: MOCKUP.on.map(p => p.id)}, '*');
  };
  const realShow = show;
  window.show = async function () { const out = await realShow.apply(this, arguments); tell(); return out; };
  addEventListener('hashchange', tell);
  addEventListener('message', event => {
    const d = event.data || {};
    if (d.mockup === 'go' && d.hash !== location.hash) location.hash = d.hash;
    if (d.mockup === 'sensor') { MOCKUP.sensor(d.zone, d.open); setTimeout(tell, 50); }
    if (d.mockup === 'tell') tell();
  });
  startup().then(tell, tell);
})();
