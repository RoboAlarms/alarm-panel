/* The Panel Manager mockup (docs/features/31-Browser-Panel-Manager/mockup) builds, and its app.html works:
 * the pretend panel (mock-panel.cjs, run inside the page by mockup/shim.js) answers the real pages for every
 * role, the shim's SHA-1 and backup stand-in behave, and every proposal's pages render without a script
 * error or "undefined", "NaN" or "[object Object]" in the words. No browser needed. */
const {test} = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const vm = require('node:vm');
const nodeCrypto = require('node:crypto');

const mockup = require('../../../../docs/features/31-Browser-Panel-Manager/mockup/build.cjs');
const out = fs.mkdtempSync(path.join(os.tmpdir(), 'manager-mockup-'));
const built = mockup.build({out});
const app = fs.readFileSync(path.join(out, 'app.html'), 'utf8');
const scripts = [...app.matchAll(/<script>\n([\s\S]*?)\n<\/script>/g)].map(m => m[1].replace(/<\\\/script/g, '</script'));

// Just enough of a browser for app.html's scripts up to boot.js (which starts the page; not run here).
function browser(query) {
  const element = () => ({textContent: '', innerHTML: '', hidden: false, dataset: {}, style: {}, classList: {add() {}, remove() {}, toggle() {}},
    setAttribute() {}, replaceChildren() {}, prepend() {}, append() {}, focus() {}, querySelector: () => null, querySelectorAll: () => [],
    contains: () => false, open: false});
  const elements = new Map();
  const href = `http://mockup.test/app.html?${query}`;
  const context = {
    console, URL, URLSearchParams, Date, Math, Number, JSON, Promise, Error, TypeError, Map, Set, Blob, Headers, Response, AbortSignal,
    TextEncoder, TextDecoder, Intl, DataView, BigInt, atob, btoa, queueMicrotask, setTimeout, clearTimeout, setInterval, clearInterval,
    crypto: nodeCrypto.webcrypto,
    fetch: () => Promise.reject(new TypeError('no network in the test')),
    sessionStorage: {getItem: () => null, setItem() {}, removeItem() {}},
    localStorage: {getItem: () => null, setItem() {}, removeItem() {}},
    location: {href, origin: 'http://mockup.test', search: '?' + query, hash: '', host: 'mockup.test', hostname: 'mockup.test', pathname: '/app.html', protocol: 'http:'},
    history: {replaceState() {}},
    addEventListener() {}, scrollTo() {},
    document: {hidden: false, title: '', body: {classList: {toggle() {}, contains: () => false}}, head: element(), documentElement: element(),
      createElement: element, querySelector: s => elements.get(s) || elements.set(s, element()).get(s), querySelectorAll: () => [], addEventListener() {}},
  };
  context.window = context;
  vm.createContext(context);
  for (const [i, s] of scripts.slice(0, -1).entries()) vm.runInContext(s, context, {filename: `app.html script ${i + 1}`});
  return context;
}
const run = (context, code) => vm.runInContext(code, context);

function checkHtml(html, where) {
  const words = html.replace(/<[^>]*>/g, ' ');
  for (const bad of ['undefined', 'NaN', '[object Object]'])
    assert.ok(!words.includes(bad) && !html.includes(`="${bad}"`), `${where} shows "${bad}"`);
  assert.ok(html.includes('<h1'), `${where} has a heading`);
}
const ARGS = {integration: ['mqtt', 'ntfy', 'pushover', 'home_assistant'], binding: ['1', '4', '50'], zones: ['', 'setup'], 'zone-setup': ['', '1', '5', 'new']};
async function renderAll(context, where, extra = []) {
  const pages = run(context, 'Object.keys(PAGES).filter(id => pageAllowed(PAGES[id]))');
  const routes = [...pages.flatMap(id => (ARGS[id] || ['']).map(arg => [id, arg])), ...extra.filter(([id]) => pages.includes(id))];
  for (const [id, arg] of routes) {
    context.__id = id;
    context.__arg = arg;
    const view = await run(context, 'PAGES[__id].render(__arg)');
    checkHtml(typeof view === 'string' ? view : view.html, `${where}: ${id}/${arg}`);
  }
  return pages;
}

test('the mockup builds both pages, with the pretend panel and the Panel Manager inlined', () => {
  assert.ok(fs.readFileSync(path.join(out, 'index.html'), 'utf8').includes('const PROPOSALS = ['), 'the viewer lists the proposals');
  assert.ok(scripts.length >= 7, 'app.html carries the shim, the pretend panel, the four Panel Manager files and boot.js');
  assert.ok(!scripts.some(s => /\nstartup\(\);\s*$/.test(s)), 'panel.js is held back from starting until the proposals are in');
});

for (const role of ['installer', 'master', 'partition', 'standard']) {
  test(`the ${role}'s pages render against the pretend panel inside the page`, async () => {
    const context = browser(`role=${role}`);
    await run(context, 'refreshStatus()');
    assert.equal(run(context, 'state.authority'), {installer: 'installer', master: 'master', partition: 'partition_master', standard: 'standard'}[role]);
    const pages = await renderAll(context, role);
    assert.ok(pages.includes('overview') && pages.includes('zones'));
  });
}

test('a scenario, two partitions and no Z-Wave reach the pretend panel', async () => {
  const context = browser('role=master&scenario=alarm&parts=2&zwave=0');
  const status = await run(context, 'refreshStatus()');
  assert.equal(status.partitions.length, 2);
  assert.equal(status.partitions[0].alarm, true);
  assert.equal(status.capabilities.zwave, false);
});

test('the shim\'s SHA-1 is SHA-1, and a backup opens only with its password', async () => {
  const context = browser('role=installer');
  for (const text of ['', 'abc', 'x'.repeat(1000)])
    assert.equal(run(context, `MOCKUP.sha1(new TextEncoder().encode(${JSON.stringify(text)})).toString('hex')`),
      nodeCrypto.createHash('sha1').update(text).digest('hex'));
  const answer = await run(context, `(async () => {
    const r = await fetch('/api/v1/backup', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({me_password: 'installer password', password: 'harbor house 1'})});
    const file = await r.blob();
    const put = pw => fetch('/api/v1/backup', {method: 'PUT', headers: {'X-Panel-Password': '696e7374616c6c65722070617373776f7264', // BL-135: the signed-in person's own password, in hex
      'X-Backup-Password': Array.from(new TextEncoder().encode(pw), x => x.toString(16).padStart(2, '0')).join('')}, body: file}).then(x => x.status);
    return [r.status, await put('harbor house 1'), await put('wrong password')];
  })()`);
  assert.deepEqual(Array.from(answer), [200, 200, 400]);
});

for (const p of built.proposals) {
  test(`proposal ${p.id}: its pages render`, async () => {
    const context = browser(`role=${p.role || 'installer'}&p=${[...p.builds, p.id].join(',')}`);
    const spec = run(context, `MOCKUP.proposals.find(x => x.id === ${JSON.stringify(p.id)})`);
    assert.ok(spec, 'the proposal is in app.html');
    context.__spec = spec;
    run(context, '__spec.routes?.(MOCKUP.route, MOCKUP.internals); __spec.apply?.(MOCKUP.internals)');
    await run(context, 'refreshStatus()');
    const look = p.look.map(([route]) => { const [id, ...rest] = route.split('/'); return [id, rest.join('/')]; });
    await renderAll(context, p.id, look);
  });
}
