/* Panel Manager pages render against the mock panel (components/services/panel_web/web/mock-panel.cjs):
 * every page each role may open (installer, system master, partition master, standard user),
 * without a script error and without "undefined", "NaN" or "[object Object]" leaking into
 * the words; and the pages a role may not open are refused by the panel itself, as the
 * panel's own table says (docs/features/14 "Who can change what"). No browser needed. */
const {test, before, after} = require('node:test');
const assert = require('node:assert/strict');
const {readFileSync} = require('node:fs');
const {fork} = require('node:child_process');
const vm = require('node:vm');
const path = require('node:path');

const dir = path.join(__dirname, '../web');
// PANEL_WEB_DIR: a folder made by minify_asset.py --all, to run these tests against the shipped files (BL-122).
const shipped = process.env.PANEL_WEB_DIR || dir;
const read = name => readFileSync(path.join(shipped, name), 'utf8');
const servers = {};

function start(who, extra = {}) {
  return new Promise((resolve, reject) => {
    const child = fork(path.join(dir, 'mock-panel.cjs'), ['0'], {env: {...process.env, MOCK_AUTOLOGIN: who, ...extra}, silent: true});
    child.stdout.on('data', data => {
      const match = /http:\/\/127\.0\.0\.1:(\d+)/.exec(String(data));
      if (match) resolve({child, base: `http://127.0.0.1:${match[1]}`});
    });
    child.on('error', reject);
  });
}
const ROLES = ['installer', 'master', 'partition', 'standard'];
// WEB-016, BL-135: a protected change asks for the signed-in person's password unless it was typed in the last
// 10 minutes. The mock's autologin sessions start unconfirmed; this ages (or refreshes) one for a test.
const confirmation = (who, recent) => fetch(servers[who].base + '/api/v1/mock/confirmation', {method: 'POST',
  headers: {'Content-Type': 'application/json', 'X-Panel-CSRF': 'auto'}, body: JSON.stringify({recent})});
before(async () => {
  for (const who of ROLES) servers[who] = await start(who);
});
after(() => Object.values(servers).forEach(s => s.child.kill()));

// Just enough of a browser for the page functions: they build HTML strings. One element per selector
// (a live page's redraw writes its innerHTML there); __posts records what the page POSTs.
function browser(base) {
  const element = () => ({textContent: '', innerHTML: '', hidden: false, classList: {add() {}, remove() {}, toggle() {}},
    setAttribute() {}, replaceChildren() {}, prepend() {}, focus() {}, querySelector: () => null, querySelectorAll: () => [],
    contains: () => false, open: false, addEventListener() {}, scrollIntoView() {}});
  const elements = new Map();
  const context = {
    console, URLSearchParams, Date, Math, Number, JSON, Promise, Error, Map, Set, Blob, AbortSignal, TextEncoder, Intl,
    crypto: require('node:crypto').webcrypto,
    setTimeout: () => 0, clearTimeout() {},
    __posts: [],
    fetch: (url, options) => {
      if (options?.method === 'POST' && typeof options.body === 'string') context.__posts.push([url, JSON.parse(options.body)]);
      return fetch(base + url, options);
    },
    sessionStorage: {getItem: () => null, setItem() {}, removeItem() {}},
    location: {hash: '', host: 'panel.test', hostname: 'panel.test', pathname: '/'},
    history: {replaceState() {}},
    window: {addEventListener() {}, scrollTo() {}},
    document: {hidden: false, title: '', body: {classList: {toggle() {}, contains: () => false}},
      querySelector: s => elements.get(s) || elements.set(s, element()).get(s), querySelectorAll: () => [], addEventListener() {}},
  };
  vm.createContext(context);
  const core = read('panel.js');
  vm.runInContext(read('panel-words.js') + '\n' + read('panel-keypad.js') + '\n' + read('panel-pages.js') + '\n' +
    core.slice(0, core.lastIndexOf('startup();')), context);
  return context;
}

const ARGS = {integration: ['mqtt', 'ntfy', 'pushover', 'home_assistant'], binding: ['1', '4', '50'], zones: ['', 'setup', '1'],
  'zone-setup': ['', '1', '3', '4', '5', '50', 'new'], 'zigbee-device': ['1', '4', '5', '6', '7', '8', '99'],
  automations: ['', 'new', 'new/door-light', 'new/lock-away', 'new/alarm-lights', 'new/leak-water', 'new/scratch', 'new/arm-night', 'new/nothing', '1', '2', '3', '4', '9']};
// What each role finds in the menu: the panel's table, page by page.
const EXPECTED = {
  installer: ['people', 'integrations', 'automations', 'network', 'walk', 'zone-setup', 'partitions', 'zwave', 'zwaveradio', 'zigbee', 'zigbee-device', 'options', 'access', 'updates', 'developer'],
  master: ['people', 'integrations', 'automations', 'network', 'walk', 'updates'],
  partition: ['people', 'network', 'walk'],
  standard: [],
};
const RESTRICTED = ['updates', 'people', 'integrations', 'automations', 'network', 'walk', 'zone-setup', 'partitions', 'zwave', 'zwaveradio', 'zigbee', 'zigbee-device', 'options', 'access', 'developer'];
for (const who of ROLES) {
  test(`every page the ${who} may open renders, and only those are in the menu`, async () => {
    const context = browser(servers[who].base);
    await vm.runInContext('refreshStatus()', context);
    const pages = vm.runInContext('Object.keys(PAGES).filter(id => pageAllowed(PAGES[id]))', context);
    for (const id of RESTRICTED)
      assert.equal(pages.includes(id), EXPECTED[who].includes(id), `${who}: ${id} ${EXPECTED[who].includes(id) ? 'missing' : 'offered'}`);
    // BL-113: everyone has the Overview, Zones and Troubles, Troubles right after Zones
    for (const id of ['overview', 'zones', 'troubles']) assert.ok(pages.includes(id), `${who}: ${id} missing`);
    assert.equal(pages.indexOf('troubles'), pages.indexOf('zones') + 1, `${who}: Troubles is not after Zones`);
    for (const id of pages) {
      for (const arg of ARGS[id] || ['']) {
        context.__id = id;
        context.__arg = arg;
        const out = await vm.runInContext('PAGES[__id].render(__arg)', context);
        const html = typeof out === 'string' ? out : out.html;
        const words = html.replace(/<[^>]*>/g, ' ');
        for (const bad of ['undefined', 'NaN', '[object Object]'])
          assert.ok(!words.includes(bad) && !html.includes(`="${bad}"`), `${id}/${arg} shows "${bad}"`);
        assert.ok(html.includes('<h1>'), `${id}/${arg} has a heading`);
      }
    }
  });
}

// Review feedback, 2026-09-23: "The master or normal user can access and see stuff that they cannot see
// on the panel". The panel refuses them itself; hiding a page is only the courtesy on top.
test('the panel refuses what a role cannot open at the panel', async () => {
  const get = (who, p) => fetch(servers[who].base + '/api/v1/' + p);
  for (const p of ['alarm', 'sources', 'binding/1', 'developer/logs']) {
    assert.equal((await get('master', p)).status, 403, `the master reads ${p}`);
    assert.equal((await get('standard', p)).status, 403, `a standard user reads ${p}`);
    assert.equal((await get('installer', p)).status, 200, `the installer can't read ${p}`);
  }
  for (const p of ['people', 'config/network', 'config/mqtt', 'home_assistant'])
    assert.equal((await get('standard', p)).status, 403, `a standard user reads ${p}`);
  assert.equal((await get('partition', 'config/mqtt')).status, 403, 'a partition master reads the integrations');
  for (const p of ['status', 'history', 'zone-status', 'config/display', 'time', 'storage'])
    assert.equal((await get('standard', p)).status, 200, `a standard user can't read ${p}, which anyone sees at the panel`);
  // The zone list shows the panel's own words, and only the viewer's partitions.
  const zones = await (await get('standard', 'zone-status')).json();
  assert.ok(zones.zones.length > 0 && zones.zones.every(z => z.partition === 1 && z.words && z.name !== undefined));
  // Changes: the remote switches and enrolment need the installer, and (when due) their own password (WEB-016, BL-135).
  const post = (who, p, body) => fetch(servers[who].base + '/api/v1/' + p, {method: 'POST',
    headers: {'Content-Type': 'application/json', 'X-Panel-CSRF': 'auto'}, body: JSON.stringify(body)});
  assert.equal((await post('master', 'remote', {web: true, developer: true, me_password: 'correct horse battery'})).status, 403);
  await confirmation('installer', false);
  const noPassword = await post('installer', 'remote', {web: true, developer: true});
  assert.equal(noPassword.status, 403, 'no password, no change');
  assert.equal((await noPassword.json()).error, 'password_required');
  const wrong = await post('installer', 'remote', {web: true, developer: true, me_password: 'correct horse battery'});
  assert.equal(wrong.status, 403, 'someone else’s password');
  assert.equal((await wrong.json()).error, 'wrong_password');
  assert.equal((await post('installer', 'remote', {web: true, developer: true, me_password: 'installer password'})).status, 200);
  assert.equal((await post('standard', 'time', {timezone: 'UTC0', ntp: 'pool.ntp.org'})).status, 403);
  // The clock is anyone's, the language and the region a master's or the installer's (docs/features/14,
  // docs/features/37 I18N-013); a language the panel doesn't have is refused.
  assert.equal((await post('standard', 'config/display', {values: {clock_mode: 2}})).status, 200);
  assert.equal((await post('standard', 'config/display', {values: {lang: 'en-US'}})).status, 403);
  assert.equal((await post('standard', 'config/display', {values: {region: 'DE'}})).status, 403);
  assert.equal((await post('master', 'config/display', {values: {region: 'DE', clock_mode: 0}})).status, 200);
  assert.equal((await post('master', 'config/display', {values: {lang: 'xx-XX'}})).status, 400);
  assert.equal((await post('master', 'config/display', {values: {region: 'ZZ'}})).status, 400);
  const display = await (await get('master', 'config/display')).json();
  assert.equal(display.values.region, 'DE');
  assert.deepEqual(display.fields.find(f => f.name === 'lang').choices, [{value: 'en-US', label: 'English'}]);
  assert.equal(display.fields.find(f => f.name === 'region').choices.find(c => c.value === 'DE').label, 'Germany');
  // OTA-017: the Software updates page is the installer's and the system master's only
  assert.equal((await get('partition', 'updates')).status, 403);
  assert.equal((await get('standard', 'updates')).status, 403);
  assert.equal((await post('partition', 'updates', {action: 'check'})).status, 403);
  assert.equal((await post('master', 'updates', {action: 'channel', channel: 'beta'})).status, 403, 'the channel is the installer’s');
  assert.equal((await post('master', 'updates', {action: 'server_check', domain: 'example.net'})).status, 403);
});

// docs/features/36 screens.md §11 (ZW-024): the Z-Wave page is the installer's; a code is checked
// by the panel, and a registered sensor's PIN (its key's first group) never crosses the wire.
test('Z-Wave: the installer registers sensors by their code; their PIN stays on the panel', async () => {
  const get = who => fetch(servers[who].base + '/api/v1/zwave');
  const post = (who, body) => fetch(servers[who].base + '/api/v1/zwave', {method: 'POST',
    headers: {'Content-Type': 'application/json', 'X-Panel-CSRF': 'auto'}, body: JSON.stringify(body)});
  for (const who of ['master', 'partition', 'standard']) {
    assert.equal((await get(who)).status, 403, `the ${who} reads the Z-Wave page`);
    assert.equal((await post(who, {action: 'register', code: '12345-23456-34567-45678-56789-01234-12345-23456'})).status, 403);
  }
  const result = async body => (await (await post('installer', body)).json()).result;
  assert.equal(await result({action: 'register', code: '12345'}), 'bad_code');
  assert.equal(await result({action: 'register',
    code: '900132783003515253545541424344453132333435212223242500100435301537022065520001000000300578'}), 'checksum');
  assert.equal(await result({action: 'register', code: '65536-00000-00000-00000-00000-00000-00000-00000'}), 'bad_code');
  assert.equal(await result({action: 'register', code: '54321 11111 22222 33333 44444 55555 00001 00002', name: 'Porch'}), 'added');
  assert.equal(await result({action: 'register', code: '5432111111222223333344444555550000100002'}), 'updated');
  const text = await (await get('installer')).text();
  assert.ok(!text.includes('54321'), 'a registered sensor’s PIN crossed the wire');
  const z = JSON.parse(text);
  const porch = z.smartstart.find(e => e.name === 'Porch');
  assert.equal(porch.key, 'xxxxx-11111-22222-33333-44444-55555-00001-00002');
  assert.equal(porch.state, 'waiting');
  assert.equal(await result({action: 'off', id: porch.id}), 'updated');
  assert.equal(await result({action: 'grant', id: porch.id, keys: 1}), 'updated');
  assert.equal(await result({action: 'forget', id: porch.id}), 'updated');
  assert.equal(await result({action: 'forget', id: porch.id}), 'not_found');
});

// docs/features/36 screens.md §11 (ZW-024): adding and removing from the browser, the same steps
// as the touchscreen, the PIN typed on the computer; other roles can't; one operation at a time.
test('Z-Wave: the installer adds a device with its PIN typed in the browser, and removes one', async () => {
  const get = async () => (await (await fetch(servers.installer.base + '/api/v1/zwave')).json());
  const post = (who, body) => fetch(servers[who].base + '/api/v1/zwave', {method: 'POST',
    headers: {'Content-Type': 'application/json', 'X-Panel-CSRF': 'auto'}, body: JSON.stringify(body)});
  const result = async body => (await (await post('installer', body)).json()).result;
  const until = async (step, ms = 9000) => {
    for (const end = Date.now() + ms; Date.now() < end; await new Promise(r => setTimeout(r, 200))) {
      const z = await get();
      if (z.progress.step === step) return z;
    }
    assert.fail(`never reached ${step}`);
  };
  assert.equal((await post('master', {action: 'add'})).status, 403, 'the master started an add');
  const before = (await get()).devices.length;
  assert.equal(await result({action: 'add'}), 'ok');
  assert.equal(await result({action: 'remove'}), 'busy', 'two operations at once');
  assert.equal((await until('listening')).progress.op, 'add');
  const ask = await until('ask_allow');
  assert.ok(ask.progress.requested & 4, 'it asks for access control');
  assert.equal(await result({action: 'allow', keys: 0}), 'bad_request');
  assert.equal(await result({action: 'allow', keys: 6}), 'ok');
  const pin = await until('ask_pin');
  assert.match(pin.progress.dsk, /^xxxxx-/, 'the PIN group crossed the wire');
  assert.equal(await result({action: 'pin', pin: 12345}), 'ok');
  const done = await until('done');
  assert.equal(done.progress.result, 'added');
  assert.equal(done.devices.length, before + 1);
  assert.equal(await result({action: 'dismiss'}), 'ok');
  assert.equal((await get()).progress.op, 'none');
  // a device page's action: a failed device removed, one that answers kept
  assert.equal(await result({action: 'remove_failed', node: 3}), 'bad_request', 'no generation');
  assert.equal(await result({action: 'remove_failed', node: 3, generation: 2}), 'ok');
  assert.equal((await until('done')).progress.result, 'still_answers');
  assert.equal(await result({action: 'dismiss'}), 'ok');
  // the radio's diagnostics are the installer's page's too
  assert.ok((await get()).diag.home_id);
});

// docs/features/36 screens.md §9 (ZW-024): a SmartStart row is named by its model when it has no name
// and the panel knows the product (else today's words); a joined one links to its device's row.
test('Z-Wave: a SmartStart row shows its model name and a joined one links to its device', async () => {
  const context = browser(servers.installer.base);
  await vm.runInContext('refreshStatus()', context);
  const html = (await vm.runInContext('PAGES.zwave.render("")', context)).html;
  assert.match(html, /<strong>Zooz ZSE41 open\/close sensor<\/strong>/, 'the model names a sensor without a name');
  assert.match(html, /<strong>Hall light<\/strong>/, 'a name given stays the name');
  assert.match(html, /data-zwfocus="7"/, 'a joined sensor links to its device');
  assert.match(html, /<tr id="zwdev-7">/, 'the device row it links to');
  assert.equal(vm.runInContext('zwName({name: "", key: "xxxxx-11111-22222-33333-44444-55555-00001-00002", generic: 0})', context), 'Sensor 11111',
    'no name and no model: today\'s words');
});

// docs/features/36 screens.md §3 and §11 (ZW-019): a device row says what state it is in, in the touchscreen's
// words, and the added card says what to do next: a zone for a sensor, its page for a switch or a keypad.
test('Z-Wave: each device row says its setup state, and the added card says what comes next', async () => {
  const context = browser(servers.installer.base);
  await vm.runInContext('refreshStatus()', context);
  const html = (await vm.runInContext('PAGES.zwave.render("")', context)).html;
  const row = node => {
    const at = html.indexOf(`device ${node}</td>`);
    assert.ok(at > 0, `device ${node} has a row`);
    return html.slice(at, html.indexOf('</tr>', at));
  };
  const words = {1: ['Being set up', false], 2: ['Wake it to finish setting it up', true], 3: ['It can’t report to the panel', true],
    4: ['Another controller: remove it', true], 5: ['Setup stopped', true], 6: [undefined], 7: [undefined]};
  const nodes = {7: 1, 8: 2, 9: 3, 10: 4, 5: 5, 11: 6};
  for (const [node, state] of Object.entries(nodes)) {
    const [word, warn] = words[state];
    if (word === undefined) { assert.match(row(node), /Being added: its question waits on Add a device/); continue; }
    assert.match(row(node), new RegExp(word));
    assert.equal(/class="tag warn"|tag warn/.test(row(node)), warn, `${word} is ${warn ? '' : 'not '}a problem tag`);
  }
  assert.match(row(11), /tag[^>]*warn/);
  // BL-152 (ZW-037): a re-read (every step done) says the touchscreen's calm words, no warning tag; a first setup at 15 of 16 keeps its words
  const st = d => vm.runInContext(`zwStatus(${JSON.stringify(d)})`, context);
  const base = {setup_done: 16, setup_total: 16, unreachable: false, battery_low: false};
  assert.deepEqual([...st({...base, state: 1})], ['Being read again', '']);
  assert.deepEqual([...st({...base, state: 2})], ['Read again at its next wake-up', '']);
  assert.deepEqual([...st({...base, state: 5})], ['Read again stopped: works as before', '']);
  assert.deepEqual([...st({...base, state: 1, setup_done: 15})], ['Being set up', '']);
  assert.deepEqual([...st({...base, state: 2, setup_done: 15})], ['Wake it to finish setting it up', 'warn']);
  assert.deepEqual([...st({...base, state: 5, setup_done: 15})], ['Setup stopped', 'warn']);
  assert.deepEqual([...st({...base, state: 5, unreachable: true})], ['Not heard since the panel started', 'warn'], 'a stopped re-read of a silent device says it is silent');
  assert.match(st({...base, state: 5, battery_low: true, heard_s: 30})[0], /ago|min|s/, 'a low battery does not hide behind the calm words');
  assert.match(row(12), /Not heard for 2 h/);
  assert.match(row(12), /tag[^>]*warn/);
  assert.doesNotMatch(row(2), /Not heard|Being set up|Setup stopped/, 'a healthy device looks healthy');
  assert.match(row(5), /Try its setup again/);
  // BL-152 (ZW-019): every listening device can be checked now, and a finished one set up again
  // (installer only), a sleeper's said to wait for its wake-up; never one still being set up
  assert.match(row(3), /data-zwdev="ping"/);
  assert.match(row(3), /data-zwdev="reread" data-sleeps="0"[^>]*>Set it up again</);
  assert.match(row(12), /data-zwdev="reread" data-sleeps="1"/);
  assert.match(row(9), /data-zwdev="reread"/);
  for (const node of [5, 7, 8, 10, 11]) assert.doesNotMatch(row(node), /data-zwdev="reread"/, `device ${node}`);
  const reread = await vm.runInContext("api('zwave', {action: 'setup', node: 3, generation: 2})", context);
  assert.equal(reread.result, 'ok');

  const card = async (node, generation, security = 4) => {
    context.__g = {op: 'add', step: 'done', result: 'added', node, generation, security, model: 'Test device', zones: []};
    const z = await vm.runInContext("api('zwave')", context);
    context.__devices = z.devices;
    return vm.runInContext('zwOpCard(__g, __devices).html', context);
  };
  const sensor = await card(2, 1);
  assert.match(sensor, /Make it a zone under Installer &gt; Zones: open a zone, then Its sensor/);
  assert.match(sensor, /data-zwgo="zone-setup"/);
  assert.doesNotMatch(sensor, /Sensor sources/);
  const sw = await card(3, 2);
  assert.match(sw, /Switch it on its page at the panel/);
  assert.doesNotMatch(sw, /data-zwgo="zone-setup"|Sensor sources|zone/i);
  const keypad = await card(5, 4);
  assert.match(keypad, /its page at the panel/);
  assert.doesNotMatch(keypad, /data-zwgo="zone-setup"|Sensor sources/);
});

// docs/features/36 screens.md §11 (ZW-040): the devices table has a Signal column in the touchscreen's words,
// empty for a device whose frames were never measured.
test('Z-Wave: the devices table has a Signal column (ZW-040)', async () => {
  const context = browser(servers.installer.base);
  await vm.runInContext('refreshStatus()', context);
  const html = (await vm.runInContext('PAGES.zwave.render("")', context)).html;
  assert.match(html, /<th>Last heard<\/th><th>Signal<\/th><th>Zones<\/th>/);
  const cells = node => {
    const at = html.indexOf(`device ${node}</td>`);
    return html.slice(at, html.indexOf('</tr>', at)).split('</td>').map(c => c.replace(/<[^>]*>/g, '').trim());
  };
  assert.equal(cells(2)[4], 'Good');
  assert.equal(cells(8)[4], 'Fair');
  assert.equal(cells(9)[4], 'Weak');
  assert.equal(cells(3)[4], '', 'nothing measured, nothing said');
});

// docs/features/36 screens.md §6 (ZW-044): one set of names for the S2 classes; Access control and With its PIN
// read differently, and a capable but unencrypted device keeps "(it supports encryption)".
test('Z-Wave: the security words tell the classes apart (ZW-044)', async () => {
  const context = browser(servers.installer.base);
  const word = key => vm.runInContext(`zwWord(ZW_SECURITY, ${key})`, context);
  assert.equal(word(5), 'Access control');
  assert.equal(word(4), 'With its PIN');
  assert.equal(word(3), 'Without a PIN');
  assert.equal(word(1), 'Not encrypted');
  assert.equal(word(7), 'Not encrypted (it supports encryption)');
  assert.notEqual(word(5), word(4));
  const keys = vm.runInContext('ZW_KEYS.map(k => zwWord({0: k[1]}, 0))', context);
  assert.deepEqual(Array.from(keys), ['Access control', 'With its PIN', 'Without a PIN']);
});

// ZW-039 (docs/features/20 devices.md "The NVS budget"): every network request result the panel
// can answer has words on the page, the C side (web_zwave.c) and the page in step; a refused Add,
// SmartStart or Replace for want of storage says so in Zigbee's words.
test('Z-Wave: every request result has words, and FULL says the storage is full (ZW-039)', async () => {
  const ZW_C = readFileSync(path.join(__dirname, '../src/web_zwave.c'), 'utf8');
  const from = ZW_C.indexOf('static const char *req_word(ui_zw_req_result_t r)');
  const words = [...ZW_C.slice(from, ZW_C.indexOf('\n}\n', from)).matchAll(/return "([a-z_]+)"/g)].map(m => m[1]);
  assert.deepEqual(words, ['ok', 'not_allowed', 'busy', 'no_radio', 'unsupported', 'low_battery', 'full', 'invalid'],
    'ui_zw_req_result_t’s words, then the default');
  await withPage('installer', {}, async context => {
    for (const code of new Set(words.filter(c => c !== 'ok'))) {
      context.__code = code;
      const said = vm.runInContext('zwRefusal(__code)', context);
      assert.ok(said !== code && said.length > 10 && !said.includes('{'), `no words for ${code}`);
    }
    assert.equal(vm.runInContext("zwRefusal('full')", context), 'No room for another device: the panel’s storage is full.');
    assert.match(vm.runInContext("zgRefusal('full')", context), /storage is full/, 'Zigbee says the same');
  });
  const UI = readFileSync(path.join(__dirname, '../../../ui/alarm_ui/src/ui_zwave.c'), 'utf8');
  const UZB = readFileSync(path.join(__dirname, '../../../ui/alarm_ui/src/ui_zigbee.c'), 'utf8');
  const toast = "No room for another device: the panel's storage is full";
  assert.ok(UI.includes(`case UI_ZW_REQ_FULL: return TR("${toast}")`) && UZB.includes(`TR("${toast}")`),
    'the touchscreen says it in Zigbee’s words');
});

// ZW-039: with the storage down to what a new device needs, the radio card says so (status
// storage_full, web_zwave.c) and Add is refused FULL in Zigbee's words; with room, nothing is said.
test('Z-Wave: a full storage is shown on the radio card and Add is refused (ZW-039)', async () => {
  const C = readFileSync(path.join(__dirname, '../src/web_zwave.c'), 'utf8');
  assert.ok(C.includes('cJSON_AddBoolToObject(radio, "storage_full", st.storage_full)'), 'the C side sends it');
  const said = 'The panel’s storage is full: no new device can be added until space is freed.';
  await withPage('installer', {}, async (context, html, call) => {
    const page = await html('zwave');
    clean(page, 'Z-Wave');
    assert.ok(!words(page).includes(said), 'room: nothing said');
    assert.equal((await call.get('zwave')).radio.storage_full, false);
  });
  await withPage('installer', {MOCK_ZWAVE: 'full'}, async (context, html, call) => {
    const page = await html('zwave');
    clean(page, 'Z-Wave, storage full');
    assert.ok(words(page).includes(said), 'full: the card says why');
    assert.ok(words(page).includes('Devices on the SmartStart list join once there is room.'));
    assert.equal((await (await call.post('zwave', {action: 'add'})).json()).result, 'full');
  });
});

// docs/features/36 network.md §14 (ZW-025): the radio's own software - checked with the update
// service as Z-Wave JS checks a controller, installed from it or from the installer's own .gbl.
test('Z-Wave: the radio is checked for an update and updated, or updated from its maker’s file', async () => {
  const get = async () => (await (await fetch(servers.installer.base + '/api/v1/zwave')).json());
  const post = body => fetch(servers.installer.base + '/api/v1/zwave', {method: 'POST',
    headers: {'Content-Type': 'application/json', 'X-Panel-CSRF': 'auto'}, body: JSON.stringify(body)});
  const put = (who, bytes) => fetch(servers[who].base + '/api/v1/zwave-radio', {method: 'PUT',
    headers: {'Content-Type': 'application/octet-stream', 'X-Panel-CSRF': 'auto'}, body: bytes});
  const wait = async (test, ms = 12000) => {
    for (const end = Date.now() + ms; Date.now() < end; await new Promise(r => setTimeout(r, 200))) {
      const z = await get();
      if (test(z)) return z;
    }
    assert.fail('it never happened');
  };
  assert.equal((await put('master', Buffer.from('x'))).status, 403, 'the master uploaded radio firmware');
  assert.equal((await (await put('installer', Buffer.from('not a gbl file at all'))).json()).result, 'invalid');
  assert.equal((await (await post({action: 'radio_check'})).json()).result, 'ok');
  const offered = await wait(z => z.radio.software.check === 'available');
  assert.equal(offered.radio.software.version, '2.30');
  assert.equal((await (await post({action: 'radio_install'})).json()).result, 'ok');
  const running = await wait(z => z.progress.op === 'radio_update' && z.progress.step === 'working');
  assert.equal(running.radio.state, 'updating');
  const done = await wait(z => z.progress.step === 'done');
  assert.equal(done.progress.result, 'fw_updated');
  assert.equal(done.radio.firmware, '2.30');
  assert.equal((await (await post({action: 'dismiss'})).json()).result, 'ok');
  // the installer's own file, refused by the bootloader with its code
  const gbl = Buffer.alloc(64);
  gbl.writeUInt32LE(0x03A617EB, 0);
  gbl.write('refuse', 20);
  assert.equal((await (await put('installer', gbl)).json()).result, 'ok');
  const failed = await wait(z => z.progress.op === 'radio_update' && z.progress.step === 'done');
  assert.equal(failed.progress.result, 'fw_failed');
  assert.equal(failed.progress.fw_error, 0x18);
  assert.equal((await (await post({action: 'dismiss'})).json()).result, 'ok');
});

test('who manages whom: the installer everyone, a master all but the installer', async () => {
  const people = async who => (await (await fetch(servers[who].base + '/api/v1/people')).json()).users;
  const byNumber = list => Object.fromEntries(list.map(u => [u.number, u]));
  const installer = byNumber(await people('installer')), master = byNumber(await people('master'));
  const partition = byNumber(await people('partition'));
  for (const n of [1, 2, 3, 4, 5]) assert.equal(installer[n].manage, true, `the installer can't manage user ${n}`);
  assert.equal(master[1].manage, false, 'the master manages the installer');
  for (const n of [2, 3, 4, 5]) assert.equal(master[n].manage, true, `the master can't manage user ${n}`);
  assert.equal(partition[1]?.manage, false);
  assert.equal(partition[2]?.manage, false);
  assert.equal(partition[3].manage, true, 'a partition master manages a standard user of its partition');
  assert.equal(installer[4].web_login_possible, false, 'a guest code is offered a browser sign-in');
  // A password set for somebody else is theirs to replace at the first sign-in; your own is not.
  const post = (who, body) => fetch(servers[who].base + '/api/v1/people', {method: 'POST',
    headers: {'Content-Type': 'application/json', 'X-Panel-CSRF': 'auto'}, body: JSON.stringify(body)});
  // BL-135: no keypad code in the request; the role decides whom you may manage.
  assert.equal((await post('master', {number: 1, action: 'web-password', password: 'a new installer pw'})).status, 409,
    'the master set the installer’s password');
  assert.equal((await post('master', {number: 3, action: 'web-password', password: 'sams new password'})).status, 200);
  assert.equal(byNumber(await people('master'))[3].web_login_temporary, true);
  assert.equal((await post('installer', {number: 2, action: 'save', name: 'Alex', authority: 2, partitions: 15, new_code: '8642'})).status, 200,
    'the installer can’t change the master’s code');
  assert.equal((await post('installer', {number: 4, action: 'web-password', password: 'a guest password'})).status, 400,
    'a guest code was given a browser sign-in');
});

// Found by the review of 2026-09-24: the status and update answers carried more than the panel
// shows these people.
test('status lists your own partitions; the update server is the installer\'s', async () => {
  const two = {MOCK_PARTITIONS: '2'};
  const standard = await start('standard', two), master = await start('master', two);
  try {
    const parts = async s => (await (await fetch(s.base + '/api/v1/status')).json()).partitions.map(p => p.number);
    assert.deepEqual(await parts(standard), [1], 'a standard user of partition 1 sees the garage');
    assert.deepEqual(await parts(master), [1, 2], 'the system master misses a partition');
  } finally {
    standard.child.kill();
    master.child.kill();
  }
  // OTA-017: the server, the channel and the check are in the installer's answer only
  for (const who of ['installer', 'master']) {
    const updates = await (await fetch(servers[who].base + '/api/v1/updates')).json();
    assert.ok(!('source' in updates), 'source is gone');
    for (const k of ['server', 'channel', 'check', 'factory_code'])
      assert.equal(k in updates, who === 'installer', `${who}: ${k}`);
    for (const k of ['withdrawn', 'critical', 'installed_channel']) assert.ok(k in updates, `${who}: ${k}`);
  }
});

// docs/features/35 RTC-008: the date and time by hand, only where the panel can keep it (a clock
// module); the hardware is never named on the page.
test('the date and time are set by hand only where the panel can keep them', async () => {
  const fitted = await start('master', {MOCK_CLOCK: 'ok'});
  const standard = await start('standard', {MOCK_CLOCK: 'ok'});
  try {
    const page = async s => {
      const context = browser(s.base);
      await vm.runInContext('refreshStatus()', context);
      return (await vm.runInContext('PAGES.time.render("")', context)).html;
    };
    const none = await page(servers.master);
    assert.ok(!none.includes('clock-form'), 'a panel that can\'t keep the time offers to set it');
    const withModule = await page(fitted);
    assert.ok(withModule.includes('id="clock-form"'), 'a master cannot set the time');
    assert.ok(withModule.indexOf('clock-form') < withModule.indexOf('time-form'), 'the date and time are not above the time zone');
    assert.ok(!/module|DS3231/i.test(withModule.replace(/<[^>]*>/g, ' ')), 'the page names the hardware');
    const seen = await page(standard);
    assert.ok(!seen.includes('clock-form'), 'a standard user may set the time');
    const post = (s, body) => fetch(s.base + '/api/v1/time', {method: 'POST',
      headers: {'Content-Type': 'application/json', 'X-Panel-CSRF': 'auto'}, body: JSON.stringify(body)});
    const at = Date.UTC(2026, 11, 25, 8, 0);
    assert.equal((await post(fitted, {epoch_ms: at})).status, 200);
    const time = await (await fetch(fitted.base + '/api/v1/time')).json();
    assert.ok(Math.abs(time.now_ms - at) < 5000, 'the panel\'s clock was not set');
    assert.equal(time.clock.source, 'manual');
    assert.equal((await post(fitted, {epoch_ms: Date.UTC(2024, 0, 1)})).status, 409, 'a date before 2025 was taken');
    assert.equal((await post(servers.master, {epoch_ms: at})).status, 409, 'set without a clock module');
    assert.equal((await post(standard, {epoch_ms: at})).status, 403, 'a standard user set the clock');
  } finally {
    fitted.child.kill();
    standard.child.kill();
  }
});

test('nothing changes while the panel is armed, but the remote keypad still works', async () => {
  const armed = await start('installer', {MOCK_LOCKED: '1'});
  try {
    const post = (p, body) => fetch(armed.base + '/api/v1/' + p, {method: 'POST',
      headers: {'Content-Type': 'application/json', 'X-Panel-CSRF': 'auto'}, body: JSON.stringify(body)});
    const refused = await post('config/display', {values: {brightness: 40}});
    assert.equal(refused.status, 409);
    assert.equal((await refused.json()).error, 'locked');
    assert.equal((await post('control', {action: 'release'})).status, 200, 'the remote keypad is refused while armed');
    assert.equal((await (await fetch(armed.base + '/api/v1/status')).json()).locked, true);
  } finally {
    armed.child.kill();
  }
});

// Content blockers match first-party URLs too: EasyPrivacy's "/api/v1/activity|" rule (on by
// default in uBlock Origin, AdGuard and Brave) stopped the Activity page with nothing but
// "Failed to fetch" (docs/bugs/B007). Paths known to be caught must not come back.
test('no API path is one a common content blocker stops', () => {
  const source = ['panel.js', 'panel-pages.js', 'panel-keypad.js', 'panel-words.js'].map(read).join('\n');
  const paths = [...source.matchAll(/api\('([^']+)'/g)].map(m => '/api/v1/' + m[1]);
  assert.ok(paths.includes('/api/v1/history'), 'the event history is read from /api/v1/history');
  const blocked = [/\/api\/v1\/activity$/, /\/event-log\?/, /\/track(ing)?\b/, /\/analytics\b/, /\/beacon\b/, /\/telemetry\b/];
  for (const p of paths)
    for (const rule of blocked) assert.ok(!rule.test(p), `${p} matches a content-blocker rule (${rule})`);
});

test('sign-in takes a user name and a password, not a keypad code', async () => {
  const {child, base} = await start('');
  try {
    const login = body => fetch(base + '/api/v1/login', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(body)});
    // B010: a keypad code is refused as one, without counting as a wrong sign-in.
    const code = await login({username: 'Installer', password: '2468013'});
    assert.equal(code.status, 400);
    assert.equal((await code.json()).error, 'password_too_short');
    assert.equal((await login({code: '1234'})).status, 400);
    assert.equal((await login({username: 'Alex', password: 'wrong password'})).status, 401);
    const ok = await login({username: ' Alex ', password: 'correct horse battery'});
    assert.equal(ok.status, 200);
    assert.match((await ok.json()).csrf, /^[0-9a-f]{64}$/);
    // The installer signs in with their name, like everyone else (2026-09-29; it was the panel ID).
    assert.equal((await login({username: 'Installer', password: 'installer password'})).status, 200);
    assert.equal((await login({username: '30eda0123456', password: 'installer password'})).status, 401);
  } finally {
    child.kill();
  }
});

test('a temporary password opens only the change-password page', async () => {
  const temp = await new Promise((resolve, reject) => {
    const c = fork(path.join(dir, 'mock-panel.cjs'), ['0'], {env: {...process.env, MOCK_AUTOLOGIN: '', MOCK_TEMPORARY: '1'}, silent: true});
    c.stdout.on('data', d => { const m = /127\.0\.0\.1:(\d+)/.exec(String(d)); if (m) resolve({c, base: `http://127.0.0.1:${m[1]}`}); });
    c.on('error', reject);
  });
  try {
    const login = await fetch(temp.base + '/api/v1/login', {method: 'POST', headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({username: 'Alex', password: 'correct horse battery'})});
    const {csrf, must_change} = await login.json();
    assert.equal(must_change, true);
    const cookie = login.headers.get('set-cookie').split(';')[0];
    const get = p => fetch(temp.base + '/api/v1/' + p, {headers: {cookie}});
    assert.equal((await get('people')).status, 403, 'other pages refuse a temporary password');
    assert.equal((await (await get('status')).json()).password_change_required, true);
    const change = body => fetch(temp.base + '/api/v1/password', {method: 'POST',
      headers: {'Content-Type': 'application/json', cookie, 'X-Panel-CSRF': csrf}, body: JSON.stringify(body)});
    assert.equal((await change({current: 'wrong', password: 'a brand new passphrase'})).status, 400);
    assert.equal((await change({current: 'correct horse battery', password: 'a brand new passphrase'})).status, 200);
    assert.equal((await get('people')).status, 200, 'a changed password opens the pages');
    // The page itself: the password page renders and is where a temporary sign-in lands.
    const context = browser(temp.base);
    assert.match(vm.runInContext('state = {password_change_required: true}; PAGES.password.render().html', context), /Change your password|Choose your password/);
  } finally {
    temp.c.kill();
  }
});

// docs/features/14 CFG-005, CFG-006, WEB-016, BL-135: the backup file is the installer's, with their own password when due; it
// comes back as an encrypted .rabk that only its password opens, and a restore answers with what
// the file held before the panel restarts. The mock seals and opens it with Node's crypto as the
// panel does with PSA (the format: components/core/backup/include/backup/backup.h).
test('backup: the installer downloads an encrypted file and restores it', async () => {
  const base = who => servers[who].base + '/api/v1/backup';
  const post = (who, body) => fetch(base(who), {method: 'POST',
    headers: {'Content-Type': 'application/json', 'X-Panel-CSRF': 'auto'}, body: JSON.stringify(body)});
  await confirmation('installer', false);
  assert.equal((await post('master', {password: 'Harbor House 2026', me_password: 'correct horse battery'})).status, 403, 'the master downloads a backup');
  assert.equal((await post('installer', {password: 'Harbor House 2026'})).status, 403, 'no password');
  assert.equal((await post('installer', {password: 'Harbor House 2026', me_password: 'correct horse battery'})).status, 403, 'someone else’s password');
  assert.equal((await post('installer', {password: 'short', me_password: 'installer password'})).status, 400, 'a password under 8 characters');
  const made = await post('installer', {password: 'Harbor House 2026'}); // the right password above confirmed it for 10 minutes
  assert.equal(made.status, 200);
  assert.equal(made.headers.get('content-type'), 'application/octet-stream');
  assert.match(made.headers.get('content-disposition'), /attachment; filename="panel-backup-\d{4}-\d\d-\d\d\.rabk"/);
  const file = Buffer.from(await made.arrayBuffer());
  assert.equal(file.toString('latin1', 0, 4), 'RABK');
  assert.ok(!file.includes(Buffer.from('Harbor House')), 'the Wi-Fi name is in the file in the clear');

  const hex = s => Buffer.from(s).toString('hex');
  // X-Panel-Password: the person's own password, hex of its UTF-8 bytes as X-Backup-Password is
  const put = (who, body, password, mePassword) => fetch(base(who), {method: 'PUT', body,
    headers: {'Content-Type': 'application/octet-stream', 'X-Panel-CSRF': 'auto', 'X-Backup-Password': hex(password),
      ...(mePassword ? {'X-Panel-Password': hex(mePassword)} : {})}});
  assert.equal((await put('master', file, 'Harbor House 2026', 'correct horse battery')).status, 403, 'the master restores');
  await confirmation('installer', false);
  const bare = await put('installer', file, 'Harbor House 2026');
  assert.equal(bare.status, 403);
  assert.equal((await bare.json()).error, 'password_required');
  const someone = await put('installer', file, 'Harbor House 2026', 'correct horse battery');
  assert.equal((await someone.json()).error, 'wrong_password', 'someone else’s password');
  const wrong = await put('installer', file, 'Harbor House 2025', 'installer password');
  assert.equal(wrong.status, 400);
  assert.equal((await wrong.json()).error, 'backup_decrypt');
  const changed = Buffer.from(file);
  changed[changed.length - 20] ^= 1;
  assert.equal((await (await put('installer', changed, 'Harbor House 2026')).json()).error, 'backup_decrypt');
  assert.equal((await (await put('installer', Buffer.from('not a backup'), 'Harbor House 2026')).json()).error, 'backup_format');
  const restored = await put('installer', file, 'Harbor House 2026');
  assert.equal(restored.status, 200);
  const answer = await restored.json();
  assert.equal(answer.restarting, true);
  assert.equal(answer.firmware, '0.6.0');
  assert.ok(answer.zones >= 0 && answer.users >= 0 && answer.created > 0);

  // The page itself: the installer's Access and maintenance offers both, in words.
  const context = browser(servers.installer.base);
  await vm.runInContext('refreshStatus()', context);
  const html = vm.runInContext('PAGES.access.render().html', context);
  assert.match(html, /Download a backup…/);
  assert.match(html, /Restore…/);
  assert.match(vm.runInContext('friendly("backup_board")', context), /another kind of panel/);
});


// WEB-016, BL-135: the sign-in decides, with no keypad code in any browser request. Programming saves with
// nothing more; the few changes that let a computer in or wipe the panel ask for the person's own password,
// only when the panel's 10 minutes have run out (status.reauth_s), and never for managing people.
test('BL-135: programming, people and the installer code take no keypad code and no password', async () => {
  await confirmation('installer', false);
  const send = (who, p, body) => fetch(servers[who].base + '/api/v1/' + p, {method: 'POST',
    headers: {'Content-Type': 'application/json', 'X-Panel-CSRF': 'auto'}, body: JSON.stringify(body)});
  const revision = (await (await fetch(servers.installer.base + '/api/v1/alarm')).json()).revision;
  assert.equal((await send('installer', 'alarm', {changes: {options: {quick_exit: false}}, revision})).status, 200, 'programming saves with no code');
  assert.equal((await send('master', 'alarm', {changes: {options: {quick_exit: false}}, revision})).status, 403);
  assert.equal((await send('installer', 'binding/6', {action: 'unbind'})).status, 200, 'a sensor link, no code');
  assert.equal((await send('installer', 'installer-code', {number: 1, new_code: '4321'})).status, 200, 'the installer code asks for nothing');
  assert.equal((await send('master', 'installer-code', {number: 2, new_code: '1234'})).status, 403);
  assert.equal((await send('installer', 'people', {number: 3, action: 'web-off'})).status, 200, 'people ask for nothing');
  assert.equal((await (await fetch(servers.installer.base + '/api/v1/status')).json()).reauth_s, 0, 'still no password in the last 10 minutes');
  const source = read('panel-pages.js') + read('panel.js');
  assert.ok(!/code: 'installer'|code: 'yours'|codeField|X-Panel-Code/.test(source), 'a page still asks for a keypad code');
});

// The stand-in browser, with a dialog's markup kept and a password box that is there only when the markup has it.
async function passwordDialog(who) {
  const context = browser(servers[who].base);
  context.__html = '';
  context.__box = {value: '', focus() {}, select() {}};
  const original = context.document.querySelector;
  context.document.querySelector = selector => selector === '#me-password' ? (context.__html.includes('id="me-password"') ? context.__box : null)
    : selector === '#dialog-content .dialog-body' ? {insertAdjacentHTML: (where, html) => { context.__html += html; }} : original(selector);
  vm.runInContext(`openDialog = html => { globalThis.__html = html; }; closeDialog = () => {}; toast = () => {}; show = async () => {};`, context);
  const run = code => vm.runInContext(code, context);
  await run('refreshStatus()');
  const submit = async password => {
    context.__box.value = password;
    context.__error = context.document.querySelector('#dialog-error');
    await context.document.querySelector('#review-form').onsubmit({preventDefault() {}});
    return context.__error.textContent;
  };
  return {context, run, submit};
}

test('BL-135 WEB-016: a protected review asks for the password only when due, and sends it as me_password', async () => {
  await confirmation('installer', false);
  const {context, run, submit} = await passwordDialog('installer');
  assert.equal(run('passwordDue()'), true, 'a new grant or autologin session starts unconfirmed');
  run(`review({title: 'Change remote access?', password: true, done: '',
    run: mePassword => api('remote', {web: true, developer: true, ...withPassword(mePassword)})})`);
  assert.match(context.__html, /name="me_password"[^>]*autocomplete="current-password"/);
  assert.match(context.__html, /Your password/);
  assert.match(context.__html, /The one you sign in with, to confirm it’s you\. Not asked again for 10 minutes\./);
  assert.equal(await submit(''), 'Enter your password.', 'an empty box is not sent');
  assert.equal(context.__posts.length, 0);
  assert.equal(await submit('installer password'), '');
  assert.equal(context.__posts[0][1].me_password, 'installer password');
  assert.equal(run('passwordDue()'), false, 'confirmed for 10 minutes');
  run(`review({title: 'Change remote access?', password: true, done: '',
    run: mePassword => api('remote', {web: true, developer: true, ...withPassword(mePassword)})})`);
  assert.ok(!context.__html.includes('me_password'), 'no box inside the window');
  await submit('');
  assert.ok(!('me_password' in context.__posts[1][1]), 'nothing sent when not asked');
  // programming never asks
  await confirmation('installer', false);
  await run('refreshStatus()');
  run(`review({title: 'Save zone 1', run: () => api('alarm', {changes: {}, revision: 0})})`);
  assert.ok(!context.__html.includes('me_password'), 'programming has no password box');
});

test('BL-135 WEB-016: password_required and wrong_password put the box in the dialog, in words', async () => {
  const {context, run, submit} = await passwordDialog('installer');
  await confirmation('installer', true);
  await run('refreshStatus()');
  assert.equal(run('passwordDue()'), false);
  run(`review({title: 'Change remote access?', password: true, done: '',
    run: mePassword => api('remote', {web: true, developer: true, ...withPassword(mePassword)})})`);
  assert.ok(!context.__html.includes('me_password'));
  await confirmation('installer', false); // the window ran out while the dialog was open
  assert.equal(await submit(''), 'Enter your password.');
  assert.match(context.__html, /name="me_password"/, 'the box appears in the same dialog');
  assert.equal(await submit('correct horse battery'), 'That password isn’t right.');
  assert.equal(await submit('installer password'), '');
  assert.equal(run('passwordDue()'), false);
  assert.equal(run('friendly("password_required")'), 'Enter your password.');
  assert.equal(run('friendly("wrong_password")'), 'That password isn’t right.');
});

test('BL-135 WEB-016: Access and maintenance dialogs ask from one box when due, and carry no code', async () => {
  const {context, run} = await passwordDialog('installer');
  const bound = {};
  const lookup = context.document.querySelector;
  context.document.querySelector = selector => /^#(change-code|backup-download|backup-restore|factory-reset)$/.test(selector)
    ? (bound[selector] ||= {onclick: null}) : Object.assign(lookup(selector), {addEventListener() {}});
  const shown = async (name, recent) => {
    await confirmation('installer', recent);
    await run('refreshStatus()');
    const page = await run('PAGES.access.render()');
    page.bind();
    await bound[name].onclick();
    return context.__html;
  };
  assert.ok(!(await shown('#change-code', false)).includes('me_password'), 'a code change asks for nothing');
  assert.ok(context.__html.includes('4 to 10 digits'), 'the panel\'s shortest allowed code');
  assert.ok(!/name="code"|confirm-code/.test(context.__html), 'no installer code field');
  assert.match(await shown('#backup-download', false), /name="me_password"/);
  assert.match(await shown('#factory-reset', false), /name="me_password"/);
  for (const name of ['#backup-download', '#factory-reset']) assert.ok(!(await shown(name, true)).includes('me_password'), `${name} inside the window`);
});

// docs/features/37: the Panel Manager's words - t(), tc(), tn(), ht() over a catalog as the panel
// sends it (i18n_web_json: keyed by the FNV-1a hash of the English, as i18n_hash in C), values
// filled in, plural forms by the language's own rule, English without one.
function wordsContext() {
  const context = {Intl, String, Number, Array, Object, TextEncoder, Math};
  vm.createContext(context);
  vm.runInContext(read('panel-words.js'), context);
  return code => vm.runInContext(code, context);
}
test('words: a catalog translates, with values and plurals; English needs none', () => {
  const run = wordsContext();
  // the hash is C's i18n_hash (components/core/i18n/test/test_i18n.c test_hash_vectors)
  assert.equal(run("wordKey(null, '')"), '811c9dc5');
  assert.equal(run("wordKey(null, 'a')"), 'e40c292c');
  assert.equal(run("wordKey(null, 'foobar')"), 'bf9cf968');
  assert.equal(run("wordKey('verb', 'Open')"), run("wordKey(null, 'verb\\u0004Open')"));
  assert.equal(run("t('{0} zones open', 3)"), '3 zones open');
  assert.equal(run("tn('{0} zone', '{0} zones', 1)"), '1 zone');
  assert.equal(run("tn('{0} zone', '{0} zones', 2)"), '2 zones');
  assert.equal(run("t('{{literal}} {0}', 'x')"), '{literal} x');
  assert.equal(run("ht('Tom & <Jerry> {0}', '\"x\"')"), 'Tom &amp; &lt;Jerry&gt; &quot;x&quot;');
  assert.equal(run("hx('Open {0}.', '<a href=\"#\">it</a>')"), 'Open <a href="#">it</a>.');
  run(`Object.assign(words, {locale: 'fr-FR', plural: ['one', 'many', 'other'], entries: {
    [wordKey(null, 'Overview')]: 'Aperçu', [wordKey('zone filter', 'Open')]: 'Ouvertes',
    [wordKey(null, '{0} zone')]: ['{0} zone', '{0} de zones', '{0} zones']}})`);
  assert.equal(run("t('Overview')"), 'Aperçu');
  assert.equal(run("t('Not in it')"), 'Not in it');
  assert.equal(run("tc('zone filter', 'Open')"), 'Ouvertes');
  assert.equal(run("t('Open')"), 'Open'); /* the context keeps meanings apart */
  assert.equal(run("tn('{0} zone', '{0} zones', 1)"), '1 zone');
  assert.equal(run("tn('{0} zone', '{0} zones', 2)"), '2 zones'); /* French: "other" */
  assert.equal(run("tx('Overview')"), 'Overview'); /* only a mark */
  assert.equal(run("nt('Zigbee')"), 'Zigbee'); /* never translated */
});

// I18N-031: every word the pages put on screen goes through the catalog - rendered in the test
// language en-XA (every entry bracketed), no English sentence is left outside the brackets.
test('words: every page is worded from the catalog', async () => {
  const pseudo = path.join(__dirname, '..', '..', '..', '..', 'build', 'i18n', 'pseudo', 'en-XA.json');
  const {existsSync, readFileSync} = require('node:fs');
  if (!existsSync(pseudo)) return; /* tools/i18n.py pseudo --build makes it (CI does) */
  const doc = JSON.parse(readFileSync(pseudo, 'utf8'));
  const context = browser(servers.installer.base);
  vm.runInContext(`Object.assign(words, ${JSON.stringify({locale: doc.locale, plural: doc.plural, entries: doc.entries})})`, context);
  await vm.runInContext('refreshStatus()', context);
  // What the (pretend) panel itself sent - its event sentences, the household's names - isn't the
  // page's to word.
  const served = [];
  for (const e of (await vm.runInContext("api('history')", context)).events) served.push(e.title, e.body);
  for (const z of (await vm.runInContext("api('alarm')", context)).zones) served.push(z.name);
  // A Zigbee device's model and how to wake it are the model table's (devices.md §9), not the page's words.
  for (const d of (await vm.runInContext("api('zigbee')", context)).devices || []) {
    context.__zb = d.id;
    const page = (await vm.runInContext("api('zigbee?device=' + __zb)", context)).device;
    served.push(d.model, d.name, page.wake_words, page.note, ...page.inputs.flatMap(x => [x.words, x.state]));
  }
  // So is a Z-Wave device's model: the panel sends it as the device reported it.
  for (const d of (await vm.runInContext("api('zwave')", context)).devices || []) served.push(d.model);
  // The household's devices and their areas, as the panel names them (docs/features/41 DESIGN.md section 10).
  for (const d of (await vm.runInContext("api('automations/devices')", context)).devices) served.push(d.name, d.area);
  const theirs = served.filter(s => s).sort((a, b) => b.length - a.length);
  const pages = vm.runInContext('Object.keys(PAGES).filter(id => pageAllowed(PAGES[id]))', context);
  for (const id of pages) {
    for (const arg of ARGS[id] || ['']) {
      context.__id = id;
      context.__arg = arg;
      const out = await vm.runInContext('PAGES[__id].render(__arg)', context);
      const html = typeof out === 'string' ? out : out.html;
      // the text between tags, without what is bracketed: an English sentence there wasn't looked up
      let text = html.replace(/<[^>]*>/g, '\n').replace(/\[[^\]]*\]/g, ' ');
      for (const s of theirs) text = text.split(s).join(' ');
      for (const line of text.split('\n')) {
        const bare = line.trim();
        assert.ok(!/[A-Z][a-z]+ [a-z]+ [a-z]+/.test(bare), `${id}/${arg} shows unmarked "${bare}"`);
      }
    }
  }
});

test('words: the mock serves a catalog and offers its language', async () => {
  const {mkdtempSync, writeFileSync} = require('node:fs');
  const os = require('node:os');
  const run = wordsContext();
  const folder = mkdtempSync(path.join(os.tmpdir(), 'panel-i18n-'));
  writeFileSync(path.join(folder, 'fr-FR.json'), JSON.stringify({locale: 'fr-FR', plural: ['one', 'many', 'other'],
    entries: {[run("wordKey('this language', 'English')")]: 'Français', [run("wordKey(null, 'Overview')")]: 'Aperçu'}}));
  const server = await start('master', {MOCK_I18N: folder});
  try {
    const catalog = await fetch(server.base + '/i18n/fr-FR.json');
    assert.equal(catalog.status, 200);
    assert.equal((await catalog.json()).entries[run("wordKey(null, 'Overview')")], 'Aperçu');
    // I18N-031: a browser without a language of its own opens in the panel's, asked before sign-in
    assert.deepEqual(await (await fetch(server.base + '/i18n/panel')).json(), {locale: 'en-US'});
    assert.equal((await fetch(server.base + '/i18n/xx-YY.json')).status, 404);
    assert.equal((await fetch(server.base + '/i18n/..%2Fpanel.js.json')).status, 404);
    const display = await (await fetch(server.base + '/api/v1/config/display')).json();
    assert.deepEqual(display.fields.find(f => f.name === 'lang').choices.map(c => c.label), ['English', 'Français']);
    const changed = await fetch(server.base + '/api/v1/config/display', {method: 'POST',
      headers: {'Content-Type': 'application/json', 'X-Panel-CSRF': 'auto'}, body: JSON.stringify({values: {lang: 'fr-FR'}})});
    assert.equal(changed.status, 200);
    assert.deepEqual(await (await fetch(server.base + '/i18n/panel')).json(), {locale: 'fr-FR'});
  } finally {
    server.child.kill();
  }
});

// BL-113: the Overview arms and disarms as the person signed in; the mock imitates the engine's rules
// (components/core/alarm_core) closely enough to try the page and the API against.
const api = base => ({
  get: async p => (await fetch(base + '/api/v1/' + p)).json(),
  post: (p, body) => fetch(base + '/api/v1/' + p, {method: 'POST', headers: {'Content-Type': 'application/json', 'X-Panel-CSRF': 'auto'}, body: JSON.stringify(body)}),
});
async function withMock(who, extra, run) {
  const s = await start(who, {MOCK_FAST: '1', ...extra});
  try { await run(api(s.base)); } finally { s.child.kill(); }
}
const partitionOne = async a => (await a.get('status')).partitions.find(p => p.number === 1);
const until = async (f, what) => {
  for (let i = 0; i < 100; i++) { const v = await f(); if (v) return v; await new Promise(r => setTimeout(r, 50)); }
  assert.fail(what);
};
const recent = async (a, kind) => (await a.get('history')).events.filter(e => e.event === kind && e.epoch_ms > Date.now() - 5 * 60e3);

test('a standard user arms and disarms their own partition, and is refused on another', async () => {
  await withMock('standard', {MOCK_PARTITIONS: '2'}, async a => {
    assert.equal((await a.post('partition', {number: 1, action: 'arm', level: 'away'})).status, 200);
    const p = await partitionOne(a);
    assert.deepEqual([p.state, p.level, p.armed_by, p.armed_by_name], ['exit_delay', 'away', 3, 'Sam']);
    assert.ok(p.delay_left_ms > 0, 'no exit time is running');
    assert.equal((await a.post('partition', {number: 1, action: 'arm', level: 'stay'})).status, 409, 'armed twice');
    assert.equal((await a.post('partition', {number: 1, action: 'disarm'})).status, 200);
    const d = await partitionOne(a);
    assert.deepEqual([d.state, d.level, d.armed_by, d.armed_by_name], ['disarmed', 'none', 0, '']);
    for (const action of ['arm', 'disarm']) {
      const other = await a.post('partition', {number: 2, action, level: 'away'});
      assert.equal(other.status, 403, `${action} on a partition that is not theirs`);
    }
    assert.equal((await a.post('partition', {number: 1, action: 'arm', level: 'sideways'})).status, 400);
    assert.equal((await a.post('partition', {number: 1, action: 'dance'})).status, 400);
    assert.equal((await a.post('partition', {action: 'disarm'})).status, 400);
  });
});

test('arming with a zone open is refused with the zone, and goes through when told to bypass it', async () => {
  await withMock('master', {MOCK_SCENARIO: 'notready'}, async a => {
    const p0 = await partitionOne(a);
    assert.equal(p0.ready, false);
    assert.equal(p0.trouble_unack, true);
    const refused = await a.post('partition', {number: 1, action: 'arm', level: 'away'});
    assert.equal(refused.status, 409);
    assert.deepEqual(await refused.json(), {error: 'not_ready', zone: 5});
    assert.equal((await partitionOne(a)).state, 'disarmed');
    assert.equal((await a.post('partition', {number: 1, action: 'arm', level: 'away', bypass_open: true})).status, 200);
    const p = await partitionOne(a);
    assert.equal(p.state, 'exit_delay');
    assert.equal(p.bypassed, 1);
    const zones = (await a.get('zone-status')).zones;
    assert.equal(zones.find(z => z.zone === 5).flags & 2, 2, 'the bypassed zone is not marked');
  });
});

test('bypass, chime, clear and acknowledge follow the engine\'s rules', async () => {
  await withMock('master', {MOCK_SCENARIO: 'notready'}, async a => {
    const act = async body => { const r = await a.post('partition', {number: 1, ...body}); return [r.status, await r.json()]; };
    assert.deepEqual(await act({action: 'bypass', zone: 9}), [409, {error: 'not_bypassable', zone: 9}]);
    assert.deepEqual(await act({action: 'bypass', zone: 5}), [200, {ok: true}]);
    assert.equal((await partitionOne(a)).bypassed, 1);
    assert.equal((await act({action: 'unbypass', zone: 5}))[0], 200);
    assert.equal((await partitionOne(a)).bypassed, 0);
    assert.equal((await partitionOne(a)).chime, false);
    await act({action: 'chime'});
    assert.equal((await partitionOne(a)).chime, true);
    await act({action: 'acknowledge'});
    assert.equal((await partitionOne(a)).trouble_unack, false);
    assert.equal((await act({action: 'arm', level: 'stay'}))[1].error, 'not_ready', 'the back door is a perimeter zone');
    assert.deepEqual(await act({action: 'arm', level: 'stay', bypass_open: true}), [200, {ok: true}]);
    assert.equal((await act({action: 'chime'}))[1].error, 'bad_state', 'chime while armed');
    assert.equal((await act({action: 'bypass', zone: 5}))[1].error, 'bad_state', 'bypass while armed');
    assert.equal((await act({action: 'clear'}))[1].error, 'bad_state', 'clear while armed');
  });
});

test('a guest and the installer disarm only what they armed themselves', async () => {
  await withMock('guest', {MOCK_SCENARIO: 'armed'}, async a => {
    const refused = await a.post('partition', {number: 1, action: 'disarm'});
    assert.equal(refused.status, 409);
    assert.deepEqual(await refused.json(), {error: 'not_authorized', zone: 0});
    assert.equal((await partitionOne(a)).state, 'armed');
  });
  await withMock('guest', {}, async a => {
    assert.equal((await a.post('partition', {number: 1, action: 'arm', level: 'stay'})).status, 200);
    assert.equal((await a.post('partition', {number: 1, action: 'disarm'})).status, 200, 'a guest disarms what they armed');
    assert.equal((await a.post('partition', {number: 1, action: 'bypass', zone: 5})).status, 409, 'a guest bypasses');
  });
  await withMock('installer', {MOCK_SCENARIO: 'armed'}, async a => {
    assert.equal((await a.post('partition', {number: 1, action: 'disarm'})).status, 409, 'the installer disarmed what Alex armed');
  });
  await withMock('master', {MOCK_SCENARIO: 'armed'}, async a => {
    assert.equal((await a.post('partition', {number: 1, action: 'disarm'})).status, 200, 'a master disarms anything');
  });
});

test('disarming works while the panel is locked, and nothing else changes', async () => {
  await withMock('master', {MOCK_LOCKED: '1'}, async a => {
    assert.equal((await partitionOne(a)).state, 'armed', 'a locked panel is armed');
    assert.equal((await a.post('config/display', {values: {brightness: 40}})).status, 409);
    assert.equal((await a.post('partition', {number: 1, action: 'disarm'})).status, 200);
    assert.equal((await partitionOne(a)).state, 'disarmed');
    assert.equal((await a.post('config/display', {values: {brightness: 40}})).status, 409, 'still locked');
  });
});

test('status and zone-status carry the Overview\'s fields', async () => {
  await withMock('installer', {MOCK_SCENARIO: 'motion'}, async a => {
    const p = await partitionOne(a);
    for (const key of ['level', 'no_entry_delay', 'silent_exit', 'armed_by', 'armed_by_name', 'delay_left_ms', 'exit_delay_s', 'entry_delay_s',
      'ready_levels', 'chime', 'alarm', 'siren', 'memory', 'canceled', 'bypassed', 'trouble_unack'])
      assert.ok(key in p, `status lacks ${key}`);
    assert.deepEqual(p.entry_delay_s, [30, 45]);
    assert.equal(p.exit_delay_s, 60);
    assert.deepEqual(p.ready_levels, ['stay', 'night'], 'motion blocks Away only');
    const zones = (await a.get('zone-status')).zones;
    const by = n => zones.find(z => z.zone === n);
    for (const z of zones) for (const key of ['levels', 'always', 'entry_s', 'bypassable']) assert.ok(key in z, `zone ${z.zone} lacks ${key}`);
    assert.deepEqual(by(1).levels, ['away', 'stay', 'night']);
    assert.equal(by(1).entry_s, 30);
    assert.equal(by(2).entry_s, 45);
    assert.deepEqual(by(4).levels, ['away']);
    assert.deepEqual(by(6).levels, ['away', 'night'], 'the interior zone flagged Night');
    assert.deepEqual([by(9).levels, by(9).always, by(9).bypassable], [[], true, false]);
    assert.deepEqual([by(17).always, by(17).bypassable], [true, false]);
    assert.equal(by(5).bypassable, true);
  });
});

test('the timed states run, and the Panel Manager\'s own events say so', async () => {
  await withMock('standard', {}, async a => {
    await a.post('partition', {number: 1, action: 'arm', level: 'away'});
    const armedEvent = (await a.get('history')).events.find(e => e.event === 'armed');
    assert.equal(armedEvent.body, 'By Sam · Panel Manager');
    assert.equal(armedEvent.title, 'Armed Away');
    assert.equal(armedEvent.user, 3);
    assert.equal(armedEvent.partition, 1);
    assert.ok(armedEvent.epoch_ms > 0);
    await until(async () => (await partitionOne(a)).state === 'armed', 'the exit time never ended');
    assert.equal((await partitionOne(a)).delay_left_ms, 0);
    await a.post('partition', {number: 1, action: 'disarm'});
    const disarmed = (await a.get('history')).events.find(e => e.event === 'disarmed');
    assert.equal(disarmed.body, 'By Sam · Panel Manager');
    assert.equal(disarmed.title, 'Disarmed');
    // Silent exit doubles the exit time (MOCK_FAST: 3 s, then 6 s), and only for Away.
    await a.post('partition', {number: 1, action: 'arm', level: 'away', silent_exit: true});
    const silent = await partitionOne(a);
    assert.equal(silent.silent_exit, true);
    assert.ok(silent.delay_left_ms > 3300 && silent.delay_left_ms <= 6000, `silent exit time ${silent.delay_left_ms}`);
    await a.post('partition', {number: 1, action: 'disarm'});
    await a.post('partition', {number: 1, action: 'arm', level: 'stay', silent_exit: true});
    assert.equal((await partitionOne(a)).silent_exit, false, 'silent exit on Stay');
  });
  await withMock('master', {MOCK_SCENARIO: 'entry'}, async a => {
    assert.equal((await partitionOne(a)).state, 'entry_delay');
    await until(async () => (await partitionOne(a)).alarm, 'the entry time never ended in an alarm');
  });
});

test('an alarm keeps its memory after the disarm, until it is cleared', async () => {
  await withMock('master', {MOCK_SCENARIO: 'alarm'}, async a => {
    const before = await partitionOne(a);
    // "memory" is the after-an-alarm state: only once disarmed, never while the alarm is still on
    assert.deepEqual([before.state, before.alarm, before.siren, before.memory], ['armed', true, true, false]);
    await until(async () => (await recent(a, 'alarm')).length >= 3, 'the third zone never tripped');
    const alarms = (await recent(a, 'alarm')).reverse();
    assert.deepEqual(alarms.map(e => e.zone), [1, 4, 3], 'the alarms are not in order');
    assert.equal(Math.round((alarms[1].epoch_ms - alarms[0].epoch_ms) / 1000), 19);
    assert.equal((await a.post('partition', {number: 1, action: 'disarm'})).status, 200);
    const after = await partitionOne(a);
    // The alarm began a minute ago, past the abort window: it occurred, it wasn't canceled (the
    // engine sets ALARM_MEM_CANCELED only when a disarm dropped the pending reports).
    assert.deepEqual([after.state, after.alarm, after.siren, after.memory, after.canceled], ['disarmed', false, false, true, false]);
    assert.equal((await a.post('partition', {number: 1, action: 'clear'})).status, 200);
    const cleared = await partitionOne(a);
    assert.deepEqual([cleared.memory, cleared.canceled], [false, false]);
  });
  await withMock('master', {MOCK_SCENARIO: 'memory'}, async a => {
    const p = await partitionOne(a);
    assert.deepEqual([p.state, p.memory, p.alarm, p.canceled], ['disarmed', true, false, false]);
    assert.equal((await recent(a, 'alarm')).length, 3);
  });
});

// BL-113: the pages themselves, rendered against the mock in each of its states.
async function withPage(who, extra, run) {
  const s = await start(who, extra);
  try {
    const context = browser(s.base);
    const html = async (id, arg = '') => {
      context.__id = id;
      context.__arg = arg;
      const out = await vm.runInContext('PAGES[__id].render(__arg)', context);
      return typeof out === 'string' ? out : out.html;
    };
    await run(context, html, api(s.base));
  } finally {
    s.child.kill();
  }
}
const words = html => html.replace(/<[^>]*>/g, ' ').replace(/\s+/g, ' ');
function clean(html, what) {
  for (const bad of ['undefined', 'NaN', '[object Object]'])
    assert.ok(!words(html).includes(bad) && !html.includes(`="${bad}"`), `${what} shows "${bad}"`);
  // CSP: style-src 'self' - no inline styles (the time bar is a <progress>)
  assert.ok(!/\sstyle="/.test(html), `${what} has an inline style`);
}
// What each state's card says (docs/features/38 "What it settled"; WORD-001, WORD-006: the touchscreen's words)
const SCENARIOS = {
  ready: ['Ready to arm', 'Everyone is leaving', 'Someone is home', 'Going to bed', 'Chime'],
  notready: ['Not ready', 'Back door is open'],
  motion: ['Ready for Stay and Night', 'Away: Hallway motion is active'],
  exit: ['Arming Away', 'Leave now through the exit door', 'Cancel arming', '<progress'],
  armed: ['Armed Away', 'Watching', 'Disarm'],
  stay: ['Armed Stay', 'Disarm'],
  entry: ['Disarm now', 'Front door opened', '<progress'],
  alarm: ['Burglary alarm', 'Front door, then ', 'Siren on', 'Disarm and silence', 'What happened', 'First'],
  memory: ['Alarm occurred', 'What happened', '3 sensors', 'Clear alarm'],
};
for (const who of ['master', 'standard']) {
  test(`the Overview renders every state for the ${who} (WORD-001, WORD-006)`, async () => {
    await Promise.all(Object.entries(SCENARIOS).map(([scenario, expected]) => withPage(who, {MOCK_SCENARIO: scenario}, async (context, html) => {
      const page = await html('overview');
      clean(page, `${who}/${scenario}`);
      for (const text of expected) assert.ok(page.includes(text), `${who}/${scenario} lacks "${text}"`);
      assert.ok(!page.includes('Open remote keypad'), 'the keypad button is gone from the Overview');
      // The page is always "Overview"; the card names its partition, also when it is the only one
      assert.ok(page.includes('<h1>Overview</h1>') && page.includes('<span class="part-name">Home</span>'), `${scenario}: the names`);
      assert.ok(page.includes('href="#/zones"') && page.includes('href="#/troubles"') && page.includes('href="#/health"'), `${scenario}: a tile's link`);
      // Connection opens Network for whoever may change it, else System health
      assert.equal(page.includes('href="#/network"'), who === 'master', `${who}: the Connection tile`);
      if (scenario === 'memory') assert.ok(!page.includes('Alarm canceled') && !page.includes('No alert was sent'), 'the alarm wasn\'t canceled');
      // Five sensors show, the rest behind Show all: the mock's alarm has three, so no button
      assert.ok(!page.includes('data-show-all'), `${scenario}: Show all with three sensors`);
    })));
    await withPage(who, {MOCK_PARTITIONS: '2'}, async (context, html) => {
      const page = await html('overview');
      clean(page, `${who}/two partitions`);
      const cards = (page.match(/class="card command /g) || []).length;
      assert.equal(cards, who === 'master' ? 2 : 1, `${who}: one card per partition they have`);
      assert.equal(page.includes('Garage'), who === 'master');
    });
  });
}

// Contract section 4: a fire, CO or auxiliary alarm is active while disarmed too; its card silences
// (acknowledge) instead of disarming, and its headline is the touchscreen's for that kind.
test('an alarm while disarmed offers Silence, named by its kind (WORD-005, WORD-006)', async () => {
  await withPage('master', {}, async (context, html) => {
    await html('overview');
    const card = kind => vm.runInContext(`Object.assign(state.partitions[0], {alarm: true, siren: true, alarm_kind: '${kind}'}); overviewBody()`, context);
    for (const [kind, words] of [['fire', 'Fire alarm'], ['co', 'Carbon monoxide'], ['aux', 'Emergency']]) {
      const page = card(kind);
      clean(page, kind);
      assert.ok(page.includes(words) && page.includes('data-silence="1"') && page.includes('>Silence<'), `${kind}: the card`);
      assert.ok(!page.includes('data-disarm=') && !page.includes('Disarm and silence'), `${kind}: disarming a disarmed partition`);
    }
    const armed = vm.runInContext(`Object.assign(state.partitions[0], {state: 'armed', level: 'away', alarm_kind: 'burglary'}); overviewBody()`, context);
    assert.ok(armed.includes('Burglary alarm') && armed.includes('Disarm and silence') && !armed.includes('data-silence'));
  });
});

test('the arming step offers only that level\'s options (WORD-003)', async () => {
  await withPage('master', {MOCK_SCENARIO: 'notready'}, async (context, html) => {
    await html('overview');
    const step = (level, extra = '') => vm.runInContext(`live.sheet = {p: 1, level: '${level}', no_entry_delay: false, silent_exit: false, bypass_open: false${extra}}; overviewBody()`, context);
    const away = step('away'), stay = step('stay'), night = step('night');
    clean(away + stay + night, 'the arming step');
    assert.ok(away.includes('Silent exit') && away.includes('No beeps · 2:00 to leave'), 'Away offers Silent exit');
    assert.ok(!stay.includes('Silent exit') && !night.includes('Silent exit'), 'Stay or Night offers Silent exit');
    for (const s of [away, stay, night]) {
      assert.ok(s.includes('No entry delay') && s.includes('Bypass Back door'), 'the options every level has here');
      assert.match(s, /data-go-arm="1"[^>]*disabled/, 'Arm is possible with the back door open');
      assert.ok(s.includes('Back door is open'));
    }
    assert.ok(stay.includes('Arm Stay') && stay.includes('Doors and windows') && stay.includes('1:00 to leave · 30–45 s to get in'));
    // Bypassing it makes Arm possible and counts one zone fewer; silent exit doubles the time to leave
    const bypassed = step('away', ', bypass_open: true, bypass_for: \'5\', blockers: \'5\', silent_exit: true');
    assert.doesNotMatch(bypassed, /data-go-arm="1"[^>]*disabled/);
    assert.ok(bypassed.includes('2:00 to leave'));
    assert.ok(!vm.runInContext(`live.sheet = {p: 1, level: 'stay', no_entry_delay: true}; overviewBody()`, context).includes('to get in'));
  });
  await withPage('master', {MOCK_SCENARIO: 'motion'}, async (context, html) => {
    await html('overview');
    const stay = vm.runInContext(`live.sheet = {p: 1, level: 'stay'}; overviewBody()`, context);
    assert.ok(!stay.includes('Bypass Hallway motion'), 'motion inside does not hold up Stay');
    const night = vm.runInContext(`live.sheet = {p: 1, level: 'night'}; overviewBody()`, context);
    assert.ok(night.includes('Doors, windows and Living room motion'), 'Night names the motion it adds');
  });
});

// WORD-005: a dialog only before disarming an armed partition; never for Cancel arming, entry time or an alarm.
test('disarming asks first only when the partition is armed (WORD-005)', async () => {
  const click = (context, dataset) => {
    context.__dataset = dataset;
    return vm.runInContext(`globalThis.__opened = 0; openDialog = h => { globalThis.__opened++; globalThis.__dialog = h; };
      overviewClick({target: {closest: () => ({disabled: false, dataset: __dataset})}})`, context);
  };
  for (const scenario of ['exit', 'entry', 'alarm']) {
    await withPage('master', {MOCK_SCENARIO: scenario}, async (context, html, a) => {
      await html('overview');
      await click(context, {disarm: '1'});
      assert.equal(context.__opened, 0, `${scenario}: a dialog before disarming`);
      assert.equal((await partitionOne(a)).state, 'disarmed', `${scenario}: not disarmed`);
    });
  }
  await withPage('master', {MOCK_SCENARIO: 'armed'}, async (context, html, a) => {
    await html('overview');
    await click(context, {disarm: '1'});
    assert.equal(context.__opened, 1, 'no dialog before disarming an armed partition');
    assert.match(context.__dialog, /Disarm Home\?/);
    assert.match(context.__dialog, /Stops watching \d+ zones\. Smoke, CO and water stay on\./);
    assert.ok(context.__dialog.indexOf('Keep armed') < context.__dialog.indexOf('>Disarm<'), 'Keep armed comes first and takes the focus');
    assert.equal((await partitionOne(a)).state, 'armed', 'disarmed without the confirmation');
  });
});

test('a refusal reads as the touchscreen says it (WORD-006)', async () => {
  await withPage('master', {MOCK_SCENARIO: 'notready'}, async context => {
    await vm.runInContext('refreshStatus().then(loadZones)', context);
    const say = (code, zone = 0) => vm.runInContext(`refusalWords({status: 409, code: '${code}', detail: {zone: ${zone}}})`, context);
    assert.equal(say('not_ready', 5), 'Back door is open');
    assert.equal(say('not_ready'), 'Not ready: a zone is open');
    assert.equal(say('not_bypassable', 9), "That zone can't be bypassed");
    assert.equal(say('bad_state'), 'Not possible right now');
    assert.equal(say('held'), 'Wait: a sensor radio is updating');
    assert.equal(say('disabled'), 'Not enabled');
    assert.equal(say('not_authorized'), "You can't do that here");
    assert.equal(say('bad_arg'), 'Not possible');
    assert.equal(vm.runInContext(`refusalWords({status: 403, message: 'Refused'})`, context), 'Refused', 'only a 409 is the engine\'s');
  });
});

test('Zones and Troubles show what needs attention to every role (WORD-008)', async () => {
  for (const who of ROLES) {
    await withPage(who, {MOCK_SCENARIO: 'notready'}, async (context, html) => {
      const zones = await html('zones');
      clean(zones, `${who}/zones`);
      assert.ok(zones.includes('Needs attention') && zones.includes('Back door · Open') && zones.includes('What these words mean'));
      assert.ok(zones.includes('data-bypass="5"'), `${who}: Bypass for the open back door while disarmed`);
      assert.ok(!zones.includes('data-bypass="9"'), `${who}: Bypass for a smoke detector`);
      assert.ok(!zones.includes('data-arg="setup"') && !zones.includes('Set up zones'), `${who}: no Set up zones button (BL-134)`);
      const troubles = await html('troubles');
      clean(troubles, `${who}/troubles`);
      assert.ok(troubles.includes('Water heater leak · Low battery') && troubles.includes('Check the sensor'), `${who}: the zone's trouble`);
      assert.ok(troubles.includes('id="ack"'), `${who}: Acknowledge while the trouble beeps`);
      assert.ok(troubles.includes('What the panel watches'));
      assert.equal(vm.runInContext('troubleBadge()', context), 1, `${who}: the menu's count`);
    });
    await withPage(who, {MOCK_SCENARIO: 'armed'}, async (context, html) => {
      const zones = await html('zones');
      assert.ok(!/data-(un)?bypass=/.test(zones) && zones.includes('Bypass while disarmed.'), `${who}: bypass while armed`);
      const troubles = await html('troubles');
      assert.ok(troubles.includes('No troubles') && !troubles.includes('id="ack"'), `${who}: troubles when there are none`);
    });
  }
});

test('a system trouble names the touchscreen\'s line and who fixes it (WORD-008, BL-137)', async () => {
  const items = context => JSON.parse(vm.runInContext(
    'JSON.stringify(trItems().filter(x => x.family === "system").map(x => [x.title, x.line, x.page, trFixer(x)]))', context));
  await withPage('standard', {MOCK_SCENARIO: 'troubles'}, async context => {
    await vm.runInContext('refreshStatus().then(loadZones).then(trLoad)', context);
    assert.deepEqual(items(context), [['Reports not getting through', "A connected service can't be reached", 'integrations', 'A system master fixes this'],
      ['Siren problem', 'Check the siren wiring', 'health', 'Your installer fixes this'], ['Panel tamper', 'Restored · acknowledge to clear', 'health', 'Your installer fixes this']],
      'a standard user can\'t open Integrations: who does, instead of a button');
  });
  await withPage('master', {MOCK_SCENARIO: 'troubles'}, async context => {
    await vm.runInContext('refreshStatus().then(loadZones).then(trLoad)', context);
    assert.equal(items(context)[0][3], '', 'a master opens Integrations');
  });
});

// The Overview's controls, driven as a person would: a click, a switch, the polls in between.
const press = (context, dataset) => {
  context.__dataset = dataset;
  return vm.runInContext('overviewClick({target: {closest: () => ({disabled: false, dataset: __dataset})}})', context);
};
const flip = (context, sopt, checked) => {
  context.__change = {dataset: {sopt}, checked};
  return vm.runInContext('overviewChange({target: __change})', context);
};
const posted = context => context.__posts.filter(([url]) => url.endsWith('/partition')).map(([, body]) => body);
const shown = context => context.document.querySelector('#overview').innerHTML;
const armStep = level => ({number: 1, action: 'arm', level, no_entry_delay: false, silent_exit: false});

// Found by the review of U3b: the panel's bypass_open bypasses whatever blocks when the command runs,
// so a switch turned on for one open zone must never bypass another.
test('Arm sends Silent exit only for Away and Bypass only for the zones it was turned on for (WEB-024, WORD-003)', async () => {
  await withPage('master', {MOCK_SCENARIO: 'notready'}, async (context, html, a) => {
    await html('overview');
    vm.runInContext('live.redraw = redrawOverview; globalThis.__load = loadZones', context);
    const frontDoorOpens = () => vm.runInContext('loadZones = async () => { await __load(); live.zones.find(z => z.zone === 1).flags |= 1; }', context);
    const frontDoorCloses = () => vm.runInContext('loadZones = __load', context);
    await press(context, {arm: 'stay', p: '1'});
    vm.runInContext('live.sheet.silent_exit = true', context); /* left on from somewhere: Stay has no silent exit */
    await flip(context, 'bypass_open', true);
    assert.match(shown(context), /data-sopt="bypass_open"[^>]*checked/);
    assert.doesNotMatch(shown(context), /data-go-arm="1"[^>]*disabled/);
    // A poll finds the front door open too: the switch goes back off, and names both
    frontDoorOpens();
    await vm.runInContext('loadZones().then(() => redrawOverview())', context);
    assert.doesNotMatch(shown(context), /data-sopt="bypass_open"[^>]*checked/, 'the switch stayed on for another zone');
    assert.ok(shown(context).includes('Bypass Front door and Back door'));
    // The front door closes again; the switch stays off, and Arm goes out without bypass_open: refused
    frontDoorCloses();
    await press(context, {goArm: '1'});
    assert.deepEqual(posted(context).at(-1), {...armStep('stay'), bypass_open: false});
    assert.equal((await partitionOne(a)).state, 'disarmed', 'the back door was bypassed without its switch');
    // Turned on for the back door; the front door opens between the last poll and Arm (the page reads
    // the zones again just before): no bypass_open
    await flip(context, 'bypass_open', true);
    frontDoorOpens();
    await press(context, {goArm: '1'});
    assert.deepEqual(posted(context).at(-1), {...armStep('stay'), bypass_open: false});
    assert.equal((await partitionOne(a)).state, 'disarmed');
    // The blockers are the ones it was turned on for: armed, the back door bypassed, silent exit off
    frontDoorCloses();
    await vm.runInContext('loadZones().then(() => redrawOverview())', context);
    await flip(context, 'bypass_open', true);
    await press(context, {goArm: '1'});
    assert.deepEqual(posted(context).at(-1), {...armStep('stay'), bypass_open: true});
    const p = await partitionOne(a);
    assert.deepEqual([p.state, p.level, p.silent_exit, p.bypassed], ['exit_delay', 'stay', false, 1]);
    assert.equal(vm.runInContext('live.sheet', context), null, 'the step stayed open after arming');
    assert.equal(posted(context).length, 3);
  });
  // Away keeps its silent exit
  await withPage('master', {}, async (context, html) => {
    await html('overview');
    await press(context, {arm: 'away', p: '1'});
    await flip(context, 'silent_exit', true);
    await press(context, {goArm: '1'});
    assert.deepEqual(posted(context), [{...armStep('away'), silent_exit: true, bypass_open: false}]);
  });
});

test('the open arming step and its switches survive the polls\' redraws (WORD-003)', async () => {
  await withPage('master', {MOCK_SCENARIO: 'notready'}, async (context, html) => {
    await html('overview');
    vm.runInContext('live.redraw = redrawOverview', context);
    await press(context, {arm: 'away', p: '1'});
    for (const k of ['no_entry_delay', 'silent_exit', 'bypass_open']) await flip(context, k, true);
    // the Overview's three polls (status, zones, history), then a forced redraw
    await vm.runInContext(`(async () => {
      await refreshStatus(); noteDeadlines(); redrawOverview();
      await loadZones(); redrawOverview();
      await loadHistory(); redrawOverview(); redrawOverview(true); })()`, context);
    const page = shown(context);
    assert.ok(page.includes('<div class="sheet away">'), 'the arming step closed');
    for (const k of ['no_entry_delay', 'silent_exit', 'bypass_open'])
      assert.match(page, new RegExp(`data-sopt="${k}"[^>]*checked`), `${k} went off`);
    assert.ok(page.includes('2:00 to leave · no entry delay · 5 zones watched'));
    assert.doesNotMatch(page, /data-go-arm="1"[^>]*disabled/);
  });
});

test('one command at a time: a partition\'s buttons wait for the answer (WEB-024)', async () => {
  await withPage('master', {MOCK_SCENARIO: 'exit'}, async (context, html, a) => {
    await html('overview');
    vm.runInContext('live.redraw = redrawOverview', context);
    const first = press(context, {disarm: '1'});
    // on its way: drawn disabled, and a second click sends nothing
    assert.match(shown(context), /data-disarm="1"[^>]*disabled/);
    await press(context, {disarm: '1'});
    await first;
    assert.equal(posted(context).length, 1, 'the second click was sent too');
    assert.equal((await partitionOne(a)).state, 'disarmed');
    assert.equal(vm.runInContext('live.busy.size', context), 0);
    assert.ok(shown(context).includes('data-arm="away"') && !/data-arm="away"[^>]*disabled/.test(shown(context)), 'the buttons stayed disabled');
  });
});

// A safety gap closed with U3b's repair: "Cancel arming" in the exit time's last second would reach a
// partition the panel has armed meanwhile - a disarm without its confirmation.
test('Cancel arming waits in the exit time\'s last second; then Disarm asks first (WORD-005)', async () => {
  await withPage('master', {MOCK_SCENARIO: 'exit', MOCK_FAST: '1'}, async (context, html, a) => {
    await html('overview');
    vm.runInContext('globalThis.__opened = 0; openDialog = () => { globalThis.__opened++; }; live.deadlines[1].end = Date.now() + 400', context);
    assert.match(vm.runInContext('overviewBody()', context), /data-cancel="1"[^>]*disabled/);
    await press(context, {disarm: '1'});
    assert.equal(posted(context).length, 0, 'Cancel arming was sent at the last moment');
    await until(async () => (await partitionOne(a)).state === 'armed', 'the exit time never ended');
    await vm.runInContext('refreshStatus().then(noteDeadlines)', context);
    const page = vm.runInContext('overviewBody()', context);
    assert.ok(page.includes('Armed Away') && page.includes('data-disarm="1"') && !page.includes('data-cancel'), 'the armed card');
    await press(context, {disarm: '1'});
    assert.equal(context.__opened, 1, 'no confirmation before disarming the armed partition');
    assert.equal(posted(context).length, 0);
  });
});

test('What happened shows five sensors, the rest behind Show all (WORD-004)', async () => {
  await withPage('master', {MOCK_SCENARIO: 'memory'}, async (context, html) => {
    await html('overview');
    vm.runInContext(`live.history.splice(1, 0, ...[[6, 125e3], [5, 130e3], [2, 135e3]].map(([zone, ago]) => ({event: 'alarm',
      title: 'Burglary alarm', body: '', partition: 1, zone, user: 0, boot: 12, uptime_s: 3600, epoch_ms: Date.now() - ago})))`, context);
    const rows = page => (page.match(/<span class="n">/g) || []).length;
    const before = vm.runInContext('overviewBody()', context);
    clean(before, 'six sensors');
    assert.ok(before.includes('6 sensors') && before.includes('Show all 6') && before.includes('data-show-all="1"'));
    assert.equal(rows(before), 5);
    await press(context, {showAll: '1'});
    const after = vm.runInContext('overviewBody()', context);
    assert.equal(rows(after), 6);
    assert.ok(!after.includes('data-show-all'), 'Show all after showing all');
    assert.match(words(after), /\+\d+ s/, 'the time after the first');
  });
});

// Found by the review of U3b: "acknowledge" is ALARM_CMD_SILENCE, which silences a fire, CO or
// auxiliary alarm as well - Troubles' Acknowledge leaves a partition in alarm to its Overview card.
// The garage, disarmed with nothing to see first, gets code + OFF, as at the touchscreen (BL-137).
test('Troubles\' Acknowledge skips a partition in alarm (WEB-024, WORD-008)', async () => {
  await withPage('master', {MOCK_SCENARIO: 'fire', MOCK_PARTITIONS: '2'}, async (context, html, a) => {
    const page = await html('troubles');
    assert.ok(page.includes('id="ack"'), 'the garage\'s trouble beeps are not offered');
    await vm.runInContext('acknowledgeTroubles()', context);
    assert.deepEqual(posted(context), [{number: 2, action: 'clear'}]);
    const status = (await a.get('status')).partitions;
    assert.deepEqual([status[0].alarm, status[0].siren, status[0].trouble_unack], [true, true, true], 'the fire was silenced');
    assert.equal(status[1].trouble_unack, false);
    assert.ok(!(await html('troubles')).includes('id="ack"'), 'Acknowledge offered for the partition in alarm');
  });
  await withPage('standard', {MOCK_SCENARIO: 'fire', MOCK_PARTITIONS: '2'}, async (context, html) => {
    assert.ok(!(await html('troubles')).includes('id="ack"'), 'only a partition in alarm, and Acknowledge offered');
  });
});

// BL-137 (WORD-008): GET troubles is what the touchscreen lists, and the page says what to do about each.
test('GET troubles and the status count: the shape, and since from the history (BL-137, WORD-008)', async () => {
  await withMock('master', {MOCK_SCENARIO: 'troubles'}, async a => {
    const list = (await a.get('troubles')).troubles, status = await a.get('status');
    assert.equal(status.trouble_count, list.length, 'the menu\'s count: active and restored');
    assert.deepEqual(list.map(x => [x.family, x.kind, x.number, x.restored]),
      [['system', 2, 0, false], ['system', 7, 0, false], ['system', 8, 0, true], ['keypad', 240, 2, false], ['zone', 3, 6, false], ['zone', 2, 17, false]]);
    const ago = x => (Date.now() - x.since_ms) / 60e3;
    assert.ok(Math.abs(ago(list[0]) - 40) < 2 && Math.abs(ago(list[3]) - 55) < 2 && Math.abs(ago(list[2]) - 19 * 60) < 2, 'since_ms: when it began or ended');
  });
  await withMock('master', {MOCK_SCENARIO: 'notready'}, async a => {
    const list = (await a.get('troubles')).troubles;
    assert.deepEqual(list.map(x => [x.family, x.kind, x.number, x.since_ms]), [['zone', 2, 17, 0]], 'no matching event: 0');
    assert.equal((await a.get('status')).trouble_count, 1);
  });
  await withMock('master', {MOCK_SCENARIO: 'ready'}, async a => {
    assert.deepEqual([(await a.get('troubles')).troubles, (await a.get('status')).trouble_count], [[], 0]);
  });
});

test('Troubles: groups, order, since, and what to do (BL-137, WORD-008)', async () => {
  await withPage('master', {MOCK_SCENARIO: 'troubles'}, async (context, html) => {
    const page = await html('troubles');
    clean(page, 'troubles');
    const text = words(page), at = x => { const i = text.indexOf(x); assert.ok(i >= 0, `"${x}" missing`); return i; };
    assert.ok(at('Affects protection') < at('Fix soon') && at('Fix soon') < at('Restored ·') && at('Reports not getting through') < at('Siren problem'));
    assert.ok(at('Fix soon') < at('Keypad 2 not responding') && at('Keypad 2 not responding') < at('Panel tamper'), 'restored last');
    assert.ok(text.includes('5 active · 1 restored, waiting for acknowledgement'));
    assert.ok(text.includes('Check its power and signal') && text.includes('Living room motion · Not reporting') && /Since \d/.test(text));
    assert.ok(text.includes('Ended ') && text.includes('The panel beeps for these troubles'), 'Ended, and the beeps line');
    assert.ok(/Acknowledge stops the beeps and clears the restored ones/.test(text));
    assert.ok(page.includes('<details class="tr-checks"') && !page.includes('tr-checks" open'), 'the checks fold while there are troubles');
    assert.ok(!/<details class="tr-item[^>]* open/.test(page), 'with several, none starts open');
    assert.equal(vm.runInContext('troubleBadge()', context), 6, 'the menu counts the restored one too');
    const overview = await html('overview');
    assert.ok(overview.includes('6 troubles'), 'the Overview counts the same');
  });
  await withPage('master', {MOCK_SCENARIO: 'notready'}, async (context, html) => {
    const page = await html('troubles');
    assert.ok(/<details class="tr-item[^"]*" data-tr="[^"]*" open>/.test(page), 'one trouble starts open');
    assert.ok(!words(page).includes('Since'), 'no time known, no Since line');
  });
  await withPage('master', {MOCK_SCENARIO: 'ready'}, async (context, html) => {
    const page = await html('troubles');
    assert.ok(page.includes('No troubles') && words(page).includes('Everything is working') && !page.includes('tr-checks'));
    assert.ok(page.includes('What the panel watches'), 'open when nothing is wrong');
  });
});

test('Troubles reload when the list\'s fingerprint moves, though the count stays, and after a failed first load (BL-137)', async () => {
  await withPage('master', {MOCK_SCENARIO: 'troubles'}, async (context, html, a) => {
    const run = code => vm.runInContext(code, context);
    const status = await a.get('status'), got = await a.get('troubles');
    assert.equal(status.trouble_sig, got.sig, 'status and GET troubles fingerprint the same list');
    assert.equal(run('trStale()'), false, 'nothing shown yet, nothing loaded with the status');
    await html('troubles');
    assert.equal(run('trData.length'), 6);
    assert.equal(run('trStale()'), false);
    // a line restored keeps the count: only the fingerprint says the copy is old
    run('trData = trData.map(x => ({...x, restored: false})); trSig = trSig + 1');
    await run('refreshStatus()');
    assert.equal(run('trData.filter(x => x.restored).length'), 1, 'reloaded on the next status poll');
    // a first load that failed tries again on the next poll, not a blank page for good
    run('trData = null; trSig = null');
    await run('refreshStatus()');
    assert.equal(run('trData.length'), 6);
    await run('acknowledgeTroubles()');
    assert.notEqual((await a.get('status')).trouble_sig, status.trouble_sig, 'the restored one cleared: a new fingerprint');
  });
});

test('Troubles: the installer\'s fixes are buttons, a standard user reads who fixes it (BL-137, WORD-008)', async () => {
  await withPage('installer', {MOCK_SCENARIO: 'troubles'}, async (context, html) => {
    const page = await html('troubles');
    assert.ok(page.includes('data-go="integrations"') && page.includes('Open Integrations') && page.includes('Run a walk test'));
    assert.ok(!words(page).includes('Your installer fixes this'));
  });
  await withPage('standard', {MOCK_SCENARIO: 'troubles'}, async (context, html) => {
    const page = await html('troubles');
    assert.ok(!page.includes('data-go="integrations"') && words(page).includes('A system master fixes this') && words(page).includes('Your installer fixes this'));
    assert.ok(page.includes('Replace its battery'), 'a battery is for anyone');
  });
});

test('Troubles: Acknowledge clears the restored ones where code + OFF would, else only the beeps (BL-137, WORD-008)', async () => {
  await withPage('master', {MOCK_SCENARIO: 'troubles'}, async (context, html, a) => {
    await html('troubles');
    await vm.runInContext('acknowledgeTroubles()', context);
    assert.deepEqual(posted(context), [{number: 1, action: 'clear'}], 'disarmed, no memory: clear');
    const list = (await a.get('troubles')).troubles;
    assert.ok(!list.some(x => x.restored) && list.length === 5, 'the restored one is gone');
    assert.equal((await a.get('status')).trouble_count, 5);
  });
  await withPage('master', {MOCK_SCENARIO: 'troubles', MOCK_FAST: '1'}, async (context, html, a) => {
    await a.post('partition', {number: 1, action: 'arm', level: 'away', no_entry_delay: false, silent_exit: false});
    await html('troubles');
    context.__posts.length = 0;
    await vm.runInContext('acknowledgeTroubles()', context);
    assert.deepEqual(posted(context), [{number: 1, action: 'acknowledge'}], 'armed: the beeps only');
    assert.equal((await a.get('troubles')).troubles.filter(x => x.restored).length, 1, 'the restored one waits for the next disarm');
  });
  await withPage('master', {MOCK_SCENARIO: 'fire'}, async (context, html) => {
    await html('troubles');
    context.__posts.length = 0;
    await vm.runInContext('acknowledgeTroubles()', context);
    assert.deepEqual(posted(context), [], 'a partition in alarm is never touched');
  });
});

// I18N-046: the languages card (docs/features/37 language-packs.md "The Panel Manager"): each state in
// words, controls only for who may change the language, Remove asks first, an upload's and a
// download's answers become words, and a locked (armed) panel says so.
const putFile = (fetcher, body) => fetcher('/api/v1/languages', {method: 'PUT', body,
  headers: {'Content-Type': 'application/octet-stream', 'X-Panel-CSRF': 'auto'}});
const pack = (size = 8192) => { const b = Buffer.alloc(size); b.write('RLNG', 0, 'latin1'); b.write('nl-NL', 4, 'latin1'); return b; };
// The page functions with the DOM reduced to what bindLanguages() looks up.
function languageDom(context, fakes) {
  context.document.querySelector = s => fakes[s] || {textContent: '', className: '', hidden: false, disabled: false};
  context.document.querySelectorAll = s => fakes[s] || [];
  context.__reviews = [];
  context.__toasts = [];
  vm.runInContext("review = o => { __reviews.push(o); }; toast = (m, k) => { __toasts.push([m, k || '']); }; show = async () => {}", context);
}

test('Display and sound: the screen going dark and the night hours, as at the panel (UI-043)', async () => {
  await withPage('standard', {}, async (context, html, a) => {
    await vm.runInContext('refreshStatus()', context);
    const page = await html('display');
    clean(page, 'Display and sound');
    const text = words(page);
    for (const phrase of ['Screen off', 'Night hours', 'Night starts', 'Night ends', 'Brightness at night', 'Screen off at night'])
      assert.ok(text.includes(phrase), `"${phrase}" missing`);
    // times of day a quarter hour apart, the panel's 22:00 chosen; the delays the saver's
    assert.match(page, /<option value="1320" selected>10:00\sPM<\/option>/);
    assert.equal((page.match(/<option value="\d+"[^>]*>\d{1,2}:\d\d\s[AP]M<\/option>/g) || []).length, 2 * 96);
    assert.match(page, /name="off_after_ms"[^>]*>(?:(?!<\/select>).)*<option value="0" selected>Never</s);
    // anyone at the keypad may change them (docs/features/14), within the panel's limits
    const night = {night_on: true, night_start: 1350, night_end: 405, night_brightness: 15, night_off_after_ms: 30000, off_after_ms: 600000};
    assert.equal((await a.post('config/display', {values: night})).status, 200);
    const stored = (await a.get('config/display')).values;
    for (const [k, v] of Object.entries(night)) assert.equal(stored[k], v, k);
    assert.equal((await a.post('config/display', {values: {night_start: 1440}})).status, 400);
    assert.equal((await a.post('config/display', {values: {night_brightness: 5}})).status, 400);
  });
});

test('Display and sound: the appearance and high contrast, as at the panel (UI-010, BL-132)', async () => {
  await withPage('standard', {}, async (context, html, a) => {
    await vm.runInContext('refreshStatus()', context);
    const page = await html('display');
    clean(page, 'Display and sound');
    const text = words(page);
    for (const phrase of ['Appearance', 'High contrast', 'Stronger colors, every button outlined.'])
      assert.ok(text.includes(phrase), `"${phrase}" missing`);
    // the panel's three choices, Dark (the default) chosen, the switch off
    assert.match(page, /name="appearance"[^>]*>(?:(?!<\/select>).)*<option value="0" selected>Dark</s);
    for (const choice of ['Light', 'Automatic'])
      assert.match(page, new RegExp(`name="appearance"[^>]*>(?:(?!</select>).)*<option value="\\d">${choice}<`, 's'));
    // saved like the night hours; a choice the panel doesn't have is refused
    const look = {appearance: 2, high_contrast: true};
    assert.equal((await a.post('config/display', {values: look})).status, 200);
    const stored = (await a.get('config/display')).values;
    for (const [k, v] of Object.entries(look)) assert.equal(stored[k], v, k);
    assert.equal((await a.post('config/display', {values: {appearance: 3}})).status, 400);
  });
});

test('Display and sound: firmware without the appearance shows no appearance rows (BL-132)', async () => {
  await withPage('standard', {}, async (context, html) => {
    await vm.runInContext('refreshStatus()', context);
    const page = await html('display');
    assert.match(page, /name="appearance"/); // shown when the panel has the field ...
    vm.runInContext(`(() => { const w = withSchema; withSchema = (specs, schema, x) =>
      w(specs, schema && schema.filter(f => f.name !== 'appearance' && f.name !== 'high_contrast'), x); })()`, context);
    const old = await html('display');
    assert.doesNotMatch(old, /name="appearance"|name="high_contrast"/); // ... and not otherwise
  });
});

test('the languages card words each state and the controls follow the role (I18N-046)', async () => {
  for (const who of ROLES) {
    await withPage(who, {}, async (context, html) => {
      await vm.runInContext('refreshStatus()', context);
      const page = await html('display');
      clean(page, `${who}: Display and sound`);
      const text = words(page);
      assert.ok(text.includes('Languages on this panel'));
      for (const phrase of ['Français (Canada)', 'Made for version 0.6.0; some phrases may show in English', 'Can’t be used here', 'Damaged',
        'Ready; 3 phrases show in English', 'Draft', 'Available to download'])
        assert.ok(text.includes(phrase), `${who}: "${phrase}" missing`);
      assert.ok(/>Ready</.test(page), 'a ready pack with nothing missing says just Ready');
      // es-US is installed at this firmware's version: only the update and the new one are offered
      assert.deepEqual([...page.matchAll(/data-lang-download="([^"]+)"/g)].map(m => m[1]), who === 'standard' ? [] : ['fr-CA', 'pt-BR']);
      const controls = who !== 'standard';
      assert.equal(page.includes('data-lang-remove'), controls, `${who}: Remove`);
      assert.equal(page.includes('accept=".robolang"'), controls, `${who}: file picker`);
    });
  }
});

test('Remove asks first, then removes; the list follows (I18N-046)', async () => {
  await withPage('master', {}, async (context, html, a) => {
    const button = {dataset: {langRemove: 'fr-CA'}};
    languageDom(context, {'[data-lang-remove]': [button]});
    context.__d = await a.get('languages');
    vm.runInContext('bindLanguages(__d, true)', context);
    button.onclick();
    assert.equal(context.__reviews.length, 1, 'no question asked');
    assert.equal(context.__reviews[0].danger, true);
    assert.ok(context.__reviews[0].title.includes('Français (Canada)'));
    assert.ok((await a.get('languages')).installed.some(l => l.locale === 'fr-CA'), 'removed before the answer');
    await context.__reviews[0].run();
    assert.ok(!(await a.get('languages')).installed.some(l => l.locale === 'fr-CA'));
    await assert.rejects(context.__reviews[0].run(), /That language is not installed\./);
  });
});

test('an upload\'s answers become words (I18N-046)', async () => {
  await withPage('installer', {}, async (context, html, a) => {
    const form = {file: {files: []}, querySelector: () => ({disabled: false}), onsubmit: null};
    const note = {textContent: ''};
    languageDom(context, {'#lang-upload-form': form, '#lang-upload-note': note});
    context.__d = await a.get('languages');
    vm.runInContext('bindLanguages(__d, true)', context);
    const submit = async blob => { form.file.files = [blob]; await form.onsubmit({preventDefault() {}}); };
    await submit(new Blob([Buffer.alloc(100)]));
    assert.equal(note.textContent, 'That file is not a language file for this panel (its size is wrong).');
    await submit(new Blob([Buffer.alloc(8192)]));
    assert.equal(note.textContent, 'That is not a language file, or it is damaged.');
    assert.equal(context.__toasts.length, 0);
    await submit(new Blob([pack()]));
    assert.equal(JSON.stringify(context.__toasts), JSON.stringify([['The language was installed.', '']]));
    assert.ok((await a.get('languages')).installed.some(l => l.locale === 'nl-NL'), 'not installed');
  });
});

test('a download is polled until it is done (I18N-046)', async () => {
  await withPage('master', {MOCK_LANG_MS: '2000'}, async (context, html, a) => {
    await vm.runInContext('refreshStatus()', context);
    const button = {dataset: {langDownload: 'pt-BR'}, disabled: false};
    languageDom(context, {'[data-lang-download]': [button]});
    context.__d = await a.get('languages');
    vm.runInContext('bindLanguages(__d, true)', context);
    await button.onclick();
    assert.equal(context.__toasts[0][0], 'Downloading pt-BR…');
    const now = await a.get('languages');
    assert.equal(now.fetch.phase, 'downloading');
    assert.ok(words(await html('display')).includes('Downloading pt-BR…'));
    // the page's poll: quiet while it runs, the result and a redraw when it stops
    let tick;
    context.poll = fn => { tick = fn; };
    context.__d = now;
    vm.runInContext('bindLanguages(__d, true)', context);
    context.__toasts.length = 0;
    await tick();
    assert.equal(context.__toasts.length, 0, 'spoke while still downloading');
    await until(async () => (await a.get('languages')).fetch.phase === 'done', 'the download never finished');
    await tick();
    assert.equal(JSON.stringify(context.__toasts), JSON.stringify([['pt-BR was downloaded.', '']]));
    assert.ok((await a.get('languages')).installed.some(l => l.locale === 'pt-BR' && l.state === 'ready'));
  });
});

test('the languages API follows the access table and the lock (I18N-046)', async () => {
  for (const who of ROLES) {
    const base = servers[who].base, may = who !== 'standard';
    assert.equal((await fetch(base + '/api/v1/languages')).status, 200, `${who} reads the languages`);
    assert.equal((await api(base).post('languages', {action: 'remove', locale: 'zz'})).status, may ? 400 : 403, `${who} removes`);
    assert.equal((await putFile((url, o) => fetch(base + url, o), Buffer.alloc(100))).status, may ? 400 : 403, `${who} installs`);
  }
});

test('the languages API refuses what it should, and a locked panel says so (I18N-046)', async () => {
  await withMock('master', {}, async a => {
    assert.equal((await a.post('languages', {action: 'explode', locale: 'fr-CA'})).status, 400);
    assert.equal((await a.post('languages', {action: 'remove', locale: 'not a tag'})).status, 400);
    assert.equal((await a.post('languages', {action: 'remove', locale: 'de-DE'})).status, 200);
  });
  await withPage('master', {MOCK_LOCKED: '1'}, async (context, html, a) => {
    const put = await putFile(context.fetch, pack());
    assert.equal(put.status, 409);
    assert.deepEqual(await put.json(), {result: 'locked'});
    assert.equal((await a.post('languages', {action: 'remove', locale: 'fr-CA'})).status, 409);
    const form = {file: {files: [new Blob([pack()])]}, querySelector: () => ({disabled: false})};
    const note = {textContent: ''};
    languageDom(context, {'#lang-upload-form': form, '#lang-upload-note': note});
    context.__d = await a.get('languages');
    vm.runInContext('bindLanguages(__d, true)', context);
    await form.onsubmit({preventDefault() {}});
    assert.match(note.textContent, /armed or an alarm is sounding/);
  });
});

/* ---- The voice (docs/features/08 voice-first-build.md D06, D14; BL-202) -----------------------------
 * The proposal docs/features/31-Browser-Panel-Manager/mockup/proposals/BL-202-voice.js, built: Display and
 * sound's Announcements card, PUT/GET voices, the switches in config/display, and each zone's read-only
 * Spoken name. The mock answers as web_voice.c does (MOCK_VOICE). */
const voiceFile = (size = 8192, magic = 'RVOC') => { const b = Buffer.alloc(size); b.write(magic, 0, 'latin1'); return b; };
const putVoice = (fetcher, body) => fetcher('/api/v1/voices', {method: 'PUT', body,
  headers: {'Content-Type': 'application/octet-stream', 'X-Panel-CSRF': 'auto'}});
const mockVoice = (a, body) => a.post('mock/voice', body);
const voiceCardHtml = page => { const at = page.indexOf('id="voice-card"'); return at < 0 ? '' : page.slice(at, page.indexOf('</section>', at)); };
// The card's controls: [name] -> disabled?
const switchLocked = (card, name) => new RegExp(`name="${name}"[^>]*disabled`).test(card);

test('AUD-029: only the system master and the installer install a voice pack, and every answer is a result', async () => {
  for (const who of ROLES) {
    const base = servers[who].base, may = ['installer', 'master'].includes(who);
    const get = await fetch(base + '/api/v1/voices');
    assert.equal(get.status, 200, `${who} reads the voice`); // D14: everyone sees the card
    const v = await get.json();
    for (const k of ['installed', 'ready', 'locale', 'name', 'version', 'draft', 'reason', 'free']) assert.ok(k in v, `${who}: ${k} missing`);
    const put = await putVoice((url, o) => fetch(base + url, o), voiceFile(100));
    assert.equal(put.status, may ? 400 : 403, `${who} installs`);
    assert.deepEqual(await put.json(), {result: may ? 'too_big' : 'not_allowed'});
  }
});

test('AUD-029: the upload refuses a wrong size before anything else, a bad signature, an armed panel; keeps the preferences', async () => {
  const s = await start('master', {MOCK_FAST: '1'});
  try {
    const a = api(s.base);
    const put = async body => { const r = await putVoice((url, o) => fetch(s.base + url, o), body); return [r.status, (await r.json()).result]; };
    assert.deepEqual(await put(voiceFile(4096)), [400, 'too_big'], 'under 8192');
    assert.deepEqual(await put(voiceFile(8192 + 100)), [400, 'too_big'], 'not whole 4096 blocks');
    assert.deepEqual(await put(voiceFile(970752 + 4096)), [400, 'too_big'], 'over 970752');
    assert.deepEqual(await put(voiceFile(8192, 'XXXX')), [400, 'bad_signature']);
    for (const [answer, code] of [['busy', 409], ['no_space', 400], ['flash', 503], ['off', 400]]) {
      await mockVoice(a, {upload: answer});
      assert.deepEqual(await put(voiceFile()), [code, answer], answer);
    }
    await mockVoice(a, {upload: 'ok'});
    // installing never turns Announcements on, and keeps every switch (D06, D12)
    assert.equal((await a.post('config/display', {values: {voice_enabled: false, voice_chime: false}})).status, 200);
    const before = (await a.get('voices')).version;
    assert.deepEqual(await put(voiceFile(970752)), [200, 'ok']);
    const after = await a.get('voices'), values = (await a.get('config/display')).values;
    assert.equal(after.version, before + 1);
    assert.equal(after.ready, true);
    assert.deepEqual([values.voice_enabled, values.voice_status, values.voice_chime], [false, true, false]);
  } finally { s.child.kill(); }
  await withPage('installer', {MOCK_LOCKED: '1'}, async context => {
    const put = await putVoice(context.fetch, voiceFile());
    assert.equal(put.status, 409);
    assert.deepEqual(await put.json(), {result: 'armed'});
  });
});

test('AUD-015: the switches are anyone\'s, and Announcements turn on only with a ready voice', async () => {
  await withMock('standard', {MOCK_VOICE: 'none'}, async a => {
    const values = (await a.get('config/display')).values;
    assert.deepEqual([values.voice_enabled, values.voice_status, values.voice_chime], [true, true, true],
      'defaults: on, as modern panels (D06)');
    assert.ok(!('voice_walk' in values), 'the walk test has no switch: it always speaks (WALK-016)');
    assert.equal((await a.post('config/display', {values: {voice_status: false}})).status, 200, 'a standard user sets a switch');
    assert.equal((await a.post('config/display', {values: {voice_enabled: false}})).status, 200, 'turning off needs no voice');
    const refused = await a.post('config/display', {values: {voice_enabled: true}});
    assert.equal(refused.status, 400);
    assert.equal((await refused.json()).error, 'no_pack');
    await mockVoice(a, {mode: 'unavailable'});
    const unavailable = await a.post('config/display', {values: {voice_enabled: true, brightness: 60}});
    assert.equal((await unavailable.json()).error, 'unavailable');
    assert.equal((await a.get('config/display')).values.brightness, 55, 'a refused request changes nothing');
    await mockVoice(a, {mode: 'ready'});
    assert.equal((await a.post('config/display', {values: {voice_enabled: true}})).status, 200);
    // one already on stays on: saving another setting never fails because the pack went away
    await mockVoice(a, {mode: 'none'});
    assert.equal((await a.post('config/display', {values: {voice_enabled: true, brightness: 60}})).status, 200);
    assert.equal((await a.post('config/display', {values: {voice_status: 'yes'}})).status, 400, 'not a boolean');
  });
  await withMock('standard', {MOCK_LOCKED: '1'}, async a => {
    assert.equal((await a.post('config/display', {values: {voice_status: false}})).status, 409, 'nothing changes while armed');
  });
});

test('D14: the Announcements card, ready with a draft pack, as the installer and the system master', async () => {
  for (const who of ['installer', 'master']) {
    await withPage(who, {}, async (context, html) => {
      await vm.runInContext('refreshStatus()', context);
      const page = await html('display');
      clean(page, `${who}: Display and sound`);
      const card = voiceCardHtml(page), text = words(card);
      for (const phrase of ['Announcements', 'Status messages', 'Says arming, armed, disarmed, entry and not ready messages.', 'Zone chime',
        'Says the zone’s spoken name after the chime.', 'Voice language', 'English (United States)', 'Voice pack', 'Version 1',
        'qwen3-designed_female', 'Ready', 'Draft', 'Preview', 'Open the Remote keypad', 'Uses the Voice volume above.',
        'A signed .robovoice file. Installing one never turns Announcements on.', 'Install'])
        assert.ok(text.includes(phrase), `${who}: "${phrase}" missing`);
      assert.match(card, /accept="\.robovoice"/);
      assert.match(card, /<a class="button small" href="#\/keypad">/, 'Preview opens the Remote keypad');
      for (const name of ['voice_enabled', 'voice_status', 'voice_chime']) assert.ok(!switchLocked(card, name), `${who}: ${name} locked`);
      assert.ok(!/name="voice_walk"/.test(card) && !text.includes('Says each zone as it opens'), `${who}: the walk test has no switch (WALK-016)`);
      assert.ok(!text.includes('Mockup only'), 'the proposal\'s pretend box is not part of the page');
      // the switches are on their own card, not as unknown fields of the main one
      assert.ok(!/Voice enabled|Voice status/.test(words(page)));
      assert.equal((page.match(/name="voice_enabled"/g) || []).length, 1);
      // before "This browser", as the proposal places it
      assert.ok(page.indexOf('id="voice-card"') < page.indexOf('This browser'));
    });
  }
});

test('D14: a standard user has the switches and Preview, and the note instead of the upload', async () => {
  await withPage('standard', {}, async (context, html) => {
    await vm.runInContext('refreshStatus()', context);
    const card = voiceCardHtml(await html('display'));
    assert.ok(words(card).includes('Only the system master or installer can install a voice pack.'));
    assert.doesNotMatch(card, /voice-upload-form|accept="\.robovoice"/);
    assert.ok(!switchLocked(card, 'voice_status'));
    assert.match(card, /href="#\/keypad"/);
  });
});

test('D14: armed, the switches, Preview and the upload wait until everything is disarmed and quiet', async () => {
  await withPage('installer', {MOCK_LOCKED: '1'}, async (context, html) => {
    await vm.runInContext('refreshStatus()', context);
    const card = voiceCardHtml(await html('display'));
    for (const name of ['voice_enabled', 'voice_status', 'voice_chime']) assert.ok(switchLocked(card, name), `${name} not locked`);
    assert.match(card, /<fieldset disabled>/);
    assert.match(card, /<button type="button" class="small" disabled>Open the Remote keypad/);
    assert.ok(words(card).includes('The panel is armed or an alarm is sounding. Changes wait until everything is disarmed and quiet.'));
  });
});

test('D06, D14: each state of the voice says what is true and why Announcements can\'t be turned on', async () => {
  const cases = {
    released: [['Ready', 'Version 1'], ['Draft', 'Voice is unavailable.']],
    unavailable: [['Voice is unavailable.', 'English (United States)', 'Version 1', 'Draft'], ['Voice pack not installed']],
    none: [['Voice pack not installed', 'No voice pack is installed.', 'Needs a voice pack.'], ['Version', 'Draft', 'English (United States)']],
    error: [['Voice information could not be loaded. Try again.', 'Try again'], ['Version', 'Draft']],
  };
  for (const [mode, [has, hasNot]] of Object.entries(cases)) {
    await withPage('installer', {MOCK_VOICE: mode}, async (context, html, a) => {
      await vm.runInContext('refreshStatus()', context);
      const page = await html('display');
      clean(page, `${mode}: Display and sound`);
      const card = voiceCardHtml(page), text = words(card);
      for (const phrase of has) assert.ok(text.includes(phrase), `${mode}: "${phrase}" missing`);
      for (const phrase of hasNot) assert.ok(!text.includes(phrase), `${mode}: "${phrase}" shown`);
      const ready = mode === 'released';
      assert.equal(/<a class="button small" href="#\/keypad">/.test(card), ready, `${mode}: Preview`);
      assert.equal(switchLocked(card, 'voice_enabled'), mode === 'error', `${mode}: Announcements locked`);
      // Announcements start on (D06, as modern panels), so the kinds are free in every state
      assert.equal(switchLocked(card, 'voice_status'), false, `${mode}: the kinds follow the main switch`);
      if (mode === 'none') {
        await a.post('config/display', {values: {voice_enabled: false}});
        await vm.runInContext('refreshStatus()', context);
        assert.ok(switchLocked(voiceCardHtml(await html('display')), 'voice_status'), 'off: the kinds follow the main switch');
      }
    });
  }
  await withPage('installer', {MOCK_VOICE: 'absent'}, async (context, html) => {
    await vm.runInContext('refreshStatus()', context);
    assert.equal(voiceCardHtml(await html('display')), '', 'a firmware without the voice has no card');
  });
});

// The card's controls, with the DOM reduced to what bindVoice() looks up.
function voiceDom(context, extra = {}) {
  const inputs = {}, error = {textContent: '', hidden: true};
  for (const name of ['voice_enabled', 'voice_status', 'voice_chime'])
    inputs[name] = {checked: false, handlers: {}, addEventListener(kind, fn) { this.handlers[kind] = fn; }};
  const fakes = {'#voice-error': error, '#voice-retry': {addEventListener() {}}, ...extra};
  for (const [name, input] of Object.entries(inputs)) fakes[`#voice-card [name="${name}"]`] = input;
  languageDom(context, fakes);
  vm.runInContext('bindVoice()', context);
  return {inputs, error};
}

test('AUD-015: turning Announcements on is refused in words when the voice is not ready; armed, nothing changes', async () => {
  for (const [mode, sentence] of [['none', 'Announcements cannot be turned on because the voice pack is not installed.'],
    ['unavailable', 'Announcements cannot be turned on because voice is unavailable.'],
    ['error', 'Voice information could not be loaded. Try again.']]) {
    await withPage('standard', {MOCK_VOICE: mode}, async (context, html, a) => {
      await a.post('config/display', {values: {voice_enabled: false}});
      await vm.runInContext('refreshStatus()', context);
      const {inputs, error} = voiceDom(context);
      inputs.voice_enabled.checked = true;
      await inputs.voice_enabled.handlers.change();
      assert.equal(inputs.voice_enabled.checked, false, `${mode}: the switch went back`);
      assert.equal(error.textContent, sentence);
      assert.equal(error.hidden, false);
      assert.equal(context.__toasts.length, 0);
      assert.equal((await a.get('config/display')).values.voice_enabled, false);
    });
  }
  await withPage('standard', {MOCK_VOICE: 'ready'}, async (context, html, a) => {
    await vm.runInContext('refreshStatus()', context);
    const {inputs} = voiceDom(context);
    inputs.voice_chime.checked = false;
    await inputs.voice_chime.handlers.change();
    assert.equal((await a.get('config/display')).values.voice_chime, false, 'a standard user turns a kind off');
    assert.deepEqual(context.__posts.at(-1), ['/api/v1/config/display', {values: {voice_chime: false}}]);
    // the panel's own refusal, in the same words, when the page thought the voice ready
    await mockVoice(a, {mode: 'none'});
    vm.runInContext('api = (path, data) => data === undefined && path === "voices" ? Promise.resolve({installed: true, ready: true}) : apiReal(path, data)',
      Object.assign(context, {apiReal: context.api}));
    await a.post('config/display', {values: {voice_enabled: false}});
    const again = voiceDom(context);
    again.inputs.voice_enabled.checked = true;
    await again.inputs.voice_enabled.handlers.change();
    assert.equal(again.error.textContent, 'Announcements cannot be turned on because the voice pack is not installed.');
  });
  await withPage('installer', {MOCK_LOCKED: '1'}, async context => {
    await vm.runInContext('refreshStatus()', context);
    const {inputs, error} = voiceDom(context);
    inputs.voice_chime.checked = false;
    await inputs.voice_chime.handlers.change();
    assert.equal(inputs.voice_chime.checked, true);
    assert.equal(error.textContent, 'The panel is armed or an alarm is sounding. Changes wait until everything is disarmed and quiet.');
    assert.deepEqual(context.__posts, []);
  });
});

test('AUD-029: the upload says "Choose a file." and each answer in words, with no toast', async () => {
  await withPage('installer', {}, async (context, html, a) => {
    await vm.runInContext('refreshStatus()', context);
    const form = {file: {files: []}, querySelector: () => ({disabled: false}), onsubmit: null};
    const note = {textContent: ''};
    voiceDom(context, {'#voice-upload-form': form, '#voice-upload-note': note});
    const submit = async blob => { form.file.files = blob ? [blob] : []; await form.onsubmit({preventDefault() {}}); };
    await submit(null);
    assert.equal(note.textContent, 'Choose a file.');
    await submit(new Blob([Buffer.alloc(100)]));
    assert.equal(note.textContent, 'The voice pack is too large to install.');
    await submit(new Blob([voiceFile(8192, 'XXXX')]));
    assert.equal(note.textContent, 'The voice pack is damaged or was not signed for this panel.');
    for (const [answer, sentence] of [['busy', 'The panel is busy. Try again in a moment.'],
      ['no_space', 'There is no room for it. Remove a language first.'], ['flash', 'The panel could not save it. Try again.'],
      ['off', 'Voice is unavailable.']]) {
      await mockVoice(a, {upload: answer});
      await submit(new Blob([voiceFile()]));
      assert.equal(note.textContent, sentence, answer);
    }
    await mockVoice(a, {upload: 'ok'});
    let shown = 0;
    context.show = async () => { shown++; };
    await submit(new Blob([voiceFile()]));
    assert.equal(shown, 1, 'the card shows the installed pack');
    assert.equal(context.__toasts.length, 0, 'no toast for what the page shows');
  });
  await withPage('installer', {MOCK_LOCKED: '1'}, async context => {
    const form = {file: {files: [new Blob([voiceFile()])]}, querySelector: () => ({disabled: false})};
    const note = {textContent: ''};
    voiceDom(context, {'#voice-upload-form': form, '#voice-upload-note': note});
    await form.onsubmit({preventDefault() {}});
    assert.equal(note.textContent, 'The panel is armed or an alarm is sounding. Changes wait until everything is disarmed and quiet.');
  });
  // the panel's refusal of a role the page wouldn't offer the form to
  await withPage('standard', {}, async context => {
    const form = {file: {files: [new Blob([voiceFile()])]}, querySelector: () => ({disabled: false})};
    const note = {textContent: ''};
    voiceDom(context, {'#voice-upload-form': form, '#voice-upload-note': note});
    await form.onsubmit({preventDefault() {}});
    assert.equal(note.textContent, 'Only the system master or installer can install a voice pack.');
  });
});

test('D14: every zone card shows its Spoken name, read-only, for every role that sees it', async () => {
  for (const who of ['master', 'standard']) {
    await withPage(who, {}, async (context, html, a) => {
      await vm.runInContext('refreshStatus()', context);
      const page = await html('zones');
      clean(page, `${who}: Zones`);
      const cards = page.split('class="card zone-card').slice(1);
      const card = n => cards.find(c => c.includes(`>Zone ${n}<`) || new RegExp(`>Zone ${n} ·`).test(c)) || '';
      assert.match(words(card(1)), /Spoken name Front Door Ready/);
      assert.match(words(card(3)), /Spoken name Hall Motion Review spoken name/);
      assert.match(words(card(2)), /Spoken name - Choose words/);
      assert.ok(!page.includes('Choose words on the panel'), `${who}: the editor's link on All zones`);
      // the computed object is never a field a write takes
      const z1 = (await a.get('zone-status')).zones.find(z => z.zone === 1);
      assert.deepEqual(Object.keys(z1.spoken).sort(), ['approved_rev', 'binding_rev', 'pack_rev', 'state', 'summary', 'words']);
    });
  }
  // announcements off, and no pack: the saved words stay, the state says why they aren't said
  await withPage('standard', {MOCK_VOICE: 'released'}, async (context, html, a) => {
    await a.post('config/display', {values: {voice_enabled: false}});
    await vm.runInContext('refreshStatus()', context);
    assert.match(words(await html('zones')), /Spoken name Front Door Announcements are off/);
  });
  await withPage('standard', {MOCK_VOICE: 'none'}, async (context, html) => {
    await vm.runInContext('refreshStatus()', context);
    assert.match(words(await html('zones')), /Spoken name - Voice pack not installed/);
  });
  await withPage('standard', {MOCK_VOICE: 'absent'}, async (context, html) => {
    await vm.runInContext('refreshStatus()', context);
    assert.ok(!words(await html('zones')).includes('Spoken name'), 'a firmware without the voice');
  });
});

test('D14: the zone\'s setup card has the Spoken name and, for the installer, Choose words on the panel', async () => {
  await withPage('installer', {}, async (context, html) => {
    await vm.runInContext('refreshStatus()', context);
    for (const [n, says] of [[1, /Spoken name .*Front Door Ready/], [3, /Spoken name .*Hall Motion Review spoken name/], [2, /Spoken name .*- Choose words/]]) {
      const page = await html('zone-setup', String(n));
      clean(page, `zone ${n}`);
      assert.match(words(page), says, `zone ${n}`);
      assert.match(page, /<a class="button small" href="#\/keypad" data-k="spoken-keypad">Choose words on the panel<\/a>/);
      assert.ok(page.indexOf('Spoken name') < page.indexOf('for="zp-zone-type"'), 'under the name, before the type');
    }
    assert.ok(!words(await html('zone-setup', 'new')).includes('Spoken name'), 'a new zone has none yet');
  });
});

/* ---- The walk test (docs/features/40) ----------------------------------------------------------
 * GET walk-test, the walk test actions of POST partition and the walk test fields of status, as
 * panel_web.c answers them. Who may do what is the panel's table (WALK-001, WALK-025). The mock's
 * pretend tester stays home unless a test asks for it (MOCK_WALK_STEP_MS). */
async function withWalkMock(who, extra, run) {
  const s = await start(who, {MOCK_FAST: '1', MOCK_WALK_STEP_MS: '0', ...extra});
  const a = {
    get: async p => { const r = await fetch(s.base + '/api/v1/' + p); return [r.status, await r.json()]; },
    act: async body => {
      const r = await fetch(s.base + '/api/v1/partition', {method: 'POST', headers: {'Content-Type': 'application/json', 'X-Panel-CSRF': 'auto'},
        body: JSON.stringify({number: 1, ...body})});
      return [r.status, await r.json()];
    },
  };
  a.status = async (n = 1) => (await a.get('status'))[1].partitions.find(p => p.number === n);
  a.walk = async (n = 1) => (await a.get('walk-test'))[1].partitions.find(p => p.number === n);
  try { await run(a); } finally { s.child.kill(); }
}
const WALK_OK = [200, {ok: true}];
const WALK_BAD_STATE = [409, {error: 'bad_state', zone: 0}];
const WALK_NOT_ALLOWED = [403, {error: 'not_allowed'}];

for (const who of ['installer', 'master', 'partition']) {
  test(`the ${who === 'partition' ? 'partition master' : who} starts, extends and ends a walk test, each only in the state it is for (WALK-001, WALK-002, WALK-004)`, async () => {
    await withWalkMock(who, {}, async a => {
      assert.deepEqual(await a.act({action: 'walk_test_end'}), WALK_BAD_STATE, 'an end with none running');
      assert.deepEqual(await a.act({action: 'walk_test_extend'}), WALK_BAD_STATE, 'an extend with none running');
      assert.deepEqual(await a.act({action: 'walk_test_start'}), WALK_OK);
      const p = await a.status();
      assert.equal(p.walk_test, true);
      assert.equal(p.walk_started_from, 'panel_manager');
      assert.equal(p.walk_started_by_name, {installer: 'Installer', master: 'Alex', partition: 'Pat'}[who]);
      assert.deepEqual([p.walk_tested, p.walk_total], [0, 7]);
      assert.ok(p.walk_remaining_s > 0);
      assert.deepEqual(await a.act({action: 'walk_test_start'}), WALK_BAD_STATE, 'a second start does nothing and says so');
      assert.deepEqual(await a.act({action: 'walk_test_extend'}), WALK_OK);
      // WALK-005: nothing arms it during a walk test; the installer never arms at all (USR-001)
      assert.deepEqual(await a.act({action: 'arm', level: 'away'}), who === 'installer' ? [409, {error: 'not_authorized', zone: 0}] : WALK_BAD_STATE, 'WALK-005: nothing arms it during a walk test');
      assert.deepEqual(await a.act({action: 'walk_test_end'}), WALK_OK);
      assert.equal((await a.status()).walk_test, false);
      const w = await a.walk();
      assert.deepEqual([w.running, w.end_how, w.ended_by_name], [false, 'command', p.walk_started_by_name], 'the summary stays');
      assert.deepEqual(await a.act({action: 'walk_test_end'}), WALK_BAD_STATE, 'a retried end never undoes itself');
    });
  });
}

test('a standard user and a guest can\'t start or extend one, nor read the list (WALK-001)', async () => {
  for (const who of ['standard', 'guest']) {
    await withWalkMock(who, {}, async a => {
      assert.deepEqual(await a.act({action: 'walk_test_start'}), WALK_NOT_ALLOWED, `${who}: start`);
      assert.deepEqual(await a.act({action: 'walk_test_start', life24: true}), WALK_NOT_ALLOWED, `${who}: start with 24-hour zones`);
      assert.deepEqual(await a.act({action: 'walk_test_extend'}), WALK_NOT_ALLOWED, `${who}: extend`);
      assert.deepEqual(await a.get('walk-test'), WALK_NOT_ALLOWED, `${who}: the list`);
      assert.deepEqual(await a.get('walk-test?key=0'), WALK_NOT_ALLOWED, `${who}: the poll`);
      assert.equal((await a.status()).walk_test, false, `${who}: nothing started`);
    });
  }
});

test('only the viewer\'s partitions: a partition master is refused on another and never sees it (WALK-018)', async () => {
  await withWalkMock('partition', {MOCK_PARTITIONS: '2'}, async a => {
    assert.deepEqual(await a.act({number: 2, action: 'walk_test_start'}), WALK_NOT_ALLOWED);
    assert.deepEqual(await a.act({number: 2, action: 'walk_test_end'}), WALK_NOT_ALLOWED);
    const [code, list] = await a.get('walk-test');
    assert.equal(code, 200);
    assert.deepEqual(list.partitions.map(p => p.number), [1]);
  });
  await withWalkMock('master', {MOCK_PARTITIONS: '2'}, async a => {
    assert.deepEqual((await a.get('walk-test'))[1].partitions.map(p => [p.number, p.name]), [[1, 'Home'], [2, 'Garage']]);
    assert.deepEqual(await a.act({number: 2, action: 'walk_test_start'}), WALK_OK);
    assert.equal((await a.walk(2)).running, true);
    assert.equal((await a.walk(1)).running, false, 'a start is the partition\'s alone');
  });
});

test('a test started at the panel shows on the Overview, and a standard user who may disarm ends it (WALK-024, WALK-025)', async () => {
  await withWalkMock('standard', {MOCK_WALK: '1'}, async a => {
    const p = await a.status();
    assert.deepEqual([p.walk_test, p.walk_started_by, p.walk_started_by_name, p.walk_started_from, p.walk_tested, p.walk_total],
      [true, 1, 'Installer', 'panel', 0, 7]);
    assert.ok(p.walk_remaining_s > 0);
    assert.deepEqual(await a.act({action: 'walk_test_extend'}), WALK_NOT_ALLOWED, 'extend stays with those who may start one');
    assert.deepEqual(await a.act({action: 'walk_test_end'}), WALK_OK);
    const q = await a.status();
    assert.deepEqual([q.walk_test, q.walk_tested, q.walk_total, q.walk_remaining_s, q.walk_started_by_name, q.walk_started_from],
      [false, 0, 0, 0, '', '']);
    assert.deepEqual(await a.act({action: 'walk_test_end'}), WALK_BAD_STATE);
  });
  await withWalkMock('guest', {MOCK_WALK: '1'}, async a => {
    assert.deepEqual(await a.act({action: 'walk_test_end'}), WALK_OK, 'a guest may disarm a disarmed partition, so ends its test');
  });
  await withWalkMock('master', {MOCK_WALK: '1'}, async a => {
    assert.deepEqual(await a.act({action: 'clear'}), WALK_OK, 'code + OFF while disarmed');
    const w = await a.walk();
    assert.deepEqual([w.running, w.end_how, w.ended_by_name, w.started_from], [false, 'disarm', 'Alex', 'panel']);
  });
});

test('the list follows each trip, and a poll with nothing new is small (WALK-017, WALK-007, WALK-009)', async () => {
  await withWalkMock('master', {MOCK_WALK_STEP_MS: '2000'}, async a => {
    assert.deepEqual(await a.act({action: 'walk_test_start'}), WALK_OK);
    // The pretend tester: doors, the hallway twice, the keyfob, the back door's cover, the CO detector's own test.
    let w;
    for (let i = 0; i < 100 && (!w || w.trips < 8); i++) {
      await new Promise(r => setTimeout(r, 50));
      w = await a.walk();
    }
    assert.equal(w.trips, 8, 'every trip counted, repeats too');
    assert.deepEqual([w.running, w.tested, w.total, w.life24, w.started_from], [true, 5, 7, false, 'panel_manager']);
    const zone = n => w.zones.find(z => z.number === n);
    assert.deepEqual(w.zones.map(z => z.number), [1, 2, 3, 4, 5, 6, 18], 'the zones it counts');
    assert.deepEqual(w.live.map(z => [z.number, z.name]), [[9, 'Hallway smoke'], [10, 'Basement CO'], [17, 'Water heater leak']],
      'fire, CO and 24-hour zones stay live, by name');
    assert.deepEqual([zone(4).tested, zone(4).how, zone(4).trips, zone(4).name], [true, 'fault', 2, 'Hallway motion']);
    assert.deepEqual([zone(18).tested, zone(18).how], [true, 'button']);
    assert.deepEqual([zone(5).tested, zone(5).cover, zone(5).how], [false, true, 'cover'], 'a cover test alone is not a test');
    assert.deepEqual([zone(6).tested, zone(6).cover, zone(6).how, zone(6).trips], [false, false, '', 0]);
    for (const z of w.zones) for (const key of ['first_s', 'open', 'check']) assert.ok(key in z, `zone ${z.number} lacks ${key}`);
    const [code, all] = await a.get('walk-test');
    assert.equal(code, 200);
    const [, same] = await a.get(`walk-test?key=${all.key}`);
    assert.equal(same.unchanged, true);
    assert.deepEqual(same.partitions.map(p => [p.number, p.running]), [[1, true]]);
    assert.ok(same.partitions[0].remaining_s > 0);
    assert.equal(same.partitions[0].zones, undefined, 'no list in an unchanged answer');
    for (const query of [`key=${all.key + 1}`, `since=${all.seq}&trips=${all.trips}`, 'key=x', 'key='])
      assert.equal((await a.get('walk-test?' + query))[1].unchanged, undefined, `${query}: the whole list`);
  });
});

test('"Include panic and 24-hour zones" counts them; anything but true or false is refused (WALK-007)', async () => {
  await withWalkMock('installer', {}, async a => {
    assert.equal((await a.act({action: 'walk_test_start', life24: 'yes'}))[0], 400);
    assert.deepEqual(await a.act({action: 'walk_test_start', life24: true}), WALK_OK);
    const w = await a.walk();
    assert.deepEqual([w.life24, w.total], [true, 8]);
    assert.ok(w.zones.some(z => z.number === 17), 'the 24-hour zone is counted');
    assert.deepEqual(w.live.map(z => z.number), [9, 10]);
  });
});

test('while another partition is armed, a start is refused, an extend and an end are not (WALK-025)', async () => {
  await withWalkMock('master', {MOCK_PARTITIONS: '2'}, async a => {
    assert.deepEqual(await a.act({action: 'walk_test_start'}), WALK_OK);
    assert.deepEqual(await a.act({number: 2, action: 'arm', level: 'away'}), WALK_OK);
    assert.deepEqual(await a.act({action: 'walk_test_extend'}), WALK_OK);
    assert.deepEqual(await a.act({number: 1, action: 'walk_test_end'}), WALK_OK);
    assert.deepEqual(await a.act({action: 'walk_test_start'}), [409, {error: 'locked'}]);
  });
});

/* ---- The walk test page, the Overview's card and the nav's count (docs/features/40, BL-128 W3) ---------
 * The page's functions run against the mock in the same stand-in browser as the other pages. */
async function walkPageContext(who, extra = {}) {
  const s = await start(who, {MOCK_FAST: '1', MOCK_WALK_STEP_MS: '0', ...extra});
  const context = browser(s.base);
  context.document.querySelector('#walk-live').addEventListener = () => {};
  context.document.querySelector('#dialog').showModal = () => {};
  context.document.documentElement = {lang: 'en-US'};
  const run = code => vm.runInContext(code, context);
  await run('refreshStatus()');
  return {s, context, run, done: () => s.child.kill()};
}
// Everything the pretend tester does takes a few seconds: wait for its last trip (8 trips, WALK_ROUTE)
const walkTested = async (base, trips = 8) => until(async () => {
  const w = (await (await fetch(base + '/api/v1/walk-test')).json()).partitions[0];
  return w.trips >= trips ? w : null;
}, 'the pretend tester never finished');
const liOf = (html, cls) => [...html.matchAll(new RegExp(`<li class="${cls}[ "]`, 'g'))].length;
const walkClickOf = dataset => `walkClick({target: {closest: () => ({disabled: false, dataset: ${JSON.stringify(dataset)}})}})`;

test('the Walk test page is in the System group right after System health, for those who may start one (WALK-001, WALK-018)', async () => {
  for (const who of ROLES) {
    const context = browser(servers[who].base);
    await vm.runInContext('refreshStatus()', context);
    const ids = vm.runInContext("Object.keys(PAGES).filter(id => pageAllowed(PAGES[id]) && PAGES[id].group === 'System')", context);
    assert.equal(ids.includes('walk'), who !== 'standard', `${who}: the Walk test page`);
    if (who !== 'standard') assert.equal(ids.indexOf('walk'), ids.indexOf('health') + 1, `${who}: right after System health`);
  }
});

test('before a test: ready, the zones to test and what stays on; the panic option for everyone who may start one (WALK-007, WALK-014, WALK-018)', async () => {
  for (const [who, option] of [['installer', true], ['master', true], ['partition', true]]) {
    const {run, done} = await walkPageContext(who);
    try {
      const html = (await run('PAGES.walk.render()')).html;
      assert.match(html, /Ready to test/);
      assert.match(html, /7 zones · smoke and CO stay on/);
      assert.match(html, /data-wact="start"/);
      assert.equal(/data-wpanic/.test(html), option, `${who}: Include panic and 24-hour zones`);
      assert.ok(!/data-wpanic[^>]*checked/.test(html), 'off by default');
      assert.match(html, /Stay on during the test/);
      assert.match(html, /Hallway smoke/);
      for (const label of ['Beep', 'Vibrate', 'Say zone names', 'Keep the screen on']) assert.match(html, new RegExp(label), label);
      assert.ok(!/animation|@keyframes/.test(html));
    } finally { done(); }
  }
});

test('Start posts the partition and the panic option, which is off again after it; the panel\'s refusal is worded (WALK-002, WALK-007)', async () => {
  const {context, run, done} = await walkPageContext('installer');
  try {
    await run('PAGES.walk.render()');
    run('walk.panic[1] = true');
    await run(walkClickOf({wact: 'start'}));
    assert.deepEqual(context.__posts.at(-1), ['/api/v1/partition', {number: 1, action: 'walk_test_start', life24: true}]);
    assert.equal(run('walk.panic[1]'), false, 'the option is per start');
    const running = run('walkBody()');
    assert.match(running, /Testing zones/);
    assert.match(running, /Started in the Panel Manager by Installer/);
    assert.match(running, /0<small>\/8<\/small>/, 'the panic zones count');
    assert.match(running, /Panic and 24-hour zones only beep/);
    await run(walkClickOf({wact: 'start'}));
    assert.equal(context.document.querySelector('#toast').textContent, 'Not possible right now', 'a second start says so');
    await run(walkClickOf({wact: 'extend'}));
    assert.deepEqual(context.__posts.at(-1), ['/api/v1/partition', {number: 1, action: 'walk_test_extend'}]);
    await run(walkClickOf({wact: 'end'}));
    assert.deepEqual(context.__posts.at(-1), ['/api/v1/partition', {number: 1, action: 'walk_test_end'}]);
    assert.match(run('walkBody()'), /Ended by Installer/);
  } finally { done(); }
});

test('running: the untested first, the last tested zone as a card, the count and the time left (WALK-009, WALK-013, WALK-017)', async () => {
  const {s, context, run, done} = await walkPageContext('installer', {MOCK_WALK: '1', MOCK_WALK_STEP_MS: '300'});
  try {
    const w = await walkTested(s.base);
    await run('walkRefresh(true)');
    const html = run('walkBody()');
    assert.match(html, /Started at the panel by Installer/);
    assert.match(html, /5<small>\/7<\/small>/);
    assert.match(html, /<progress max="7" value="5"/);
    assert.match(html, /Ends in \d+ min/, 'the time left, in whole minutes');
    // the zone tested last (by the seconds after the start): the card above the list
    const newest = w.zones.filter(z => z.tested).sort((a, b) => b.first_s - a.first_s)[0];
    assert.match(html, new RegExp(`class="wt-last"><span class="wt-chip">[^]*?<strong>${newest.name}</strong>`));
    // the default list is the untested: the one with only its cover tested and the one nobody tripped
    assert.equal(liOf(html, 'untested'), 2);
    assert.equal(liOf(html, 'tested'), 0);
    assert.match(html, /Cover tested/);
    // "All": the untested first, then the tested, newest first
    run("walk.filter = 'all'");
    const all = run('walkBody()');
    assert.ok(all.indexOf('<li class="untested') < all.indexOf('<li class="tested'));
    assert.equal(liOf(all, 'tested'), 5);
    assert.match(all, /2 times/, 'the hallway twice');
    assert.match(all, /Stay on during the test/);
    // the page reads again with the key: nothing moved, so it says unchanged and draws nothing new
    const urls = [];
    const fetched = context.fetch;
    context.fetch = (url, options) => { urls.push(url); return fetched(url, options); };
    const key = run('walk.key');
    await run('walkRefresh(false)');
    assert.deepEqual(urls, [`/api/v1/walk-test?key=${key}`]);
    assert.equal(run('walkBody()'), all);
  } finally { done(); }
});

test('the page reads the panel every second and nothing in the walk test styles moves (WALK-017, WALK-013)', async () => {
  const {context, run, done} = await walkPageContext('master', {MOCK_WALK: '1'});
  try {
    const ms = [];
    context.setTimeout = (fn, wait) => { ms.push(wait); return 0; };
    const view = await run('PAGES.walk.render()');
    view.bind();
    assert.ok(ms.length >= 1 && ms.every(wait => wait === 1000), `the polls: ${ms}`);
  } finally { done(); }
  // The section is found by its comment, which the shipped (minified) file no longer has: read the source.
  const css = readFileSync(path.join(dir, 'panel.css'), 'utf8');
  const walkCss = css.slice(css.indexOf('The walk test (docs/features/40'));
  assert.ok(walkCss.length > 1000);
  assert.ok(!/animation|@keyframes|transition/.test(walkCss), 'the walk test only changes colour steadily');
});

test('ended: the summary says N of M, the untested by name and who ended it and how; Done goes back to Ready (WALK-010, WALK-018)', async () => {
  const {s, run, done} = await walkPageContext('master', {MOCK_WALK: '1', MOCK_WALK_STEP_MS: '300'});
  try {
    const w = await walkTested(s.base);
    await run('PAGES.walk.render()');
    // code + OFF at the panel while disarmed ends it as a disarm (WALK-025)
    await run("api('partition', {number: 1, action: 'clear'})");
    await run('walkRefresh(false)');
    const html = run('walkBody()');
    assert.match(html, /5 of 7 tested/);
    assert.match(html, /Ended by Alex with a disarm/);
    const untested = w.zones.filter(z => !z.tested).map(z => z.name);
    assert.equal(untested.length, 2);
    assert.match(html, /Not tested · 2/);
    for (const name of untested) assert.ok(html.includes(name), `${name} is named`);
    assert.match(html, /Test the untested again/);
    assert.match(html, /data-wact="report"/);
    assert.match(html, /Tested<\/summary>|Tested · 5/);
    // Save report: a file with the zones and their results
    const csv = run('walkCsv(walkReportRows(walk.parts[1]))');
    assert.match(csv, /^Walk test report\r\nPartition,Home\r\n/);
    assert.match(csv, /\r\n\r\nZone,Name,Result,Times tripped,Cover tested,Seconds after the start\r\n/);
    assert.equal(csv.trim().split('\r\n').filter(line => /^\d+,/.test(line)).length, 7);
    assert.match(csv, /,Hallway motion,Tested,2,No,\d+\r\n/);
    assert.match(csv, /,Not tested,0,No,\r\n/);
    await run(walkClickOf({wact: 'done'}));
    assert.match(run('walkBody()'), /Ready to test[^]*Last test: 5 of 7/, 'the last test stays on the Ready card');
    await run(walkClickOf({wact: 'view'}));
    assert.match(run('walkBody()'), /5 of 7 tested/, 'View opens the summary again');
  } finally { done(); }
});

test('the summary says how a test ended (WALK-004)', async () => {
  const {run, done} = await walkPageContext('installer');
  try {
    assert.equal(run("walkEndedWords({end_how: 'time_limit'})"), 'The time ran out');
    assert.equal(run("walkEndedWords({end_how: 'command', ended_by: 1, ended_by_name: 'Installer'})"), 'Ended by Installer');
    assert.equal(run("walkEndedWords({end_how: 'disarm', ended_by: 0, ended_by_name: ''})"), 'Ended by Someone with a disarm');
  } finally { done(); }
});

test('the Overview shows a test started at the panel: a link for those who may start one, the count and End for those who may not (WALK-024, WALK-025)', async () => {
  const {run, done} = await walkPageContext('master', {MOCK_WALK: '1'});
  try {
    await run('PAGES.overview.render()');
    const html = run('overviewBody()');
    assert.match(html, /Walk test<\/span>/);
    assert.match(html, /Started at the panel by Installer/);
    assert.match(html, /0<small>\/7<\/small>/);
    assert.match(html, /data-go="walk"/);
    assert.ok(!/data-walk-end|data-arm=/.test(html), 'no End and no arming while it runs');
  } finally { done(); }
  const standard = await walkPageContext('standard', {MOCK_WALK: '1'});
  try {
    await standard.run('PAGES.overview.render()');
    const html = standard.run('overviewBody()');
    assert.match(html, /Walk test<\/span>/);
    assert.match(html, /0<small>\/7<\/small>/, 'the count');
    assert.match(html, /data-walk-end="1"/, 'End');
    assert.ok(!/data-go="walk"/.test(html), 'no link to a page they can\'t open');
    // End asks first, then ends it as the person signed in
    await standard.run("overviewClick({target: {closest: () => ({disabled: false, dataset: {walkEnd: '1'}})}})");
    assert.match(standard.context.document.querySelector('#dialog-content').innerHTML, /End the walk test on Home\?/);
    assert.ok(!standard.context.__posts.some(([, body]) => body.action === 'walk_test_end'), 'asked first');
    await standard.context.document.querySelector('#do-end-walk').onclick();
    assert.deepEqual(standard.context.__posts.at(-1), ['/api/v1/partition', {number: 1, action: 'walk_test_end'}]);
    assert.ok(!/Walk test<\/span>/.test(standard.run('overviewBody()')), 'the card is gone once it ended');
  } finally { standard.done(); }
});

test('the nav shows the count while a test runs, wherever it was started (WALK-024)', async () => {
  const {s, context, run, done} = await walkPageContext('installer', {MOCK_WALK: '1', MOCK_WALK_STEP_MS: '300'});
  try {
    run("page = 'zones'; renderNav()");
    assert.match(context.document.querySelector('#nav').innerHTML, /<span>Walk test<\/span><span class="badge info">\d\/7<\/span>/);
    await walkTested(s.base);
    await run('refreshStatus()');
    assert.equal(run('walkBadge()'), '5/7');
    run('renderNav()');
    assert.match(context.document.querySelector('#nav').innerHTML, /badge info">5\/7</);
    await run("api('partition', {number: 1, action: 'walk_test_end'})");
    await run('refreshStatus()');
    assert.equal(run('walkBadge()'), '');
    run('renderNav()');
    assert.ok(!/badge info/.test(context.document.querySelector('#nav').innerHTML));
  } finally { done(); }
});

test('speech never piles up: one name waits, more become a count (WALK-015)', async () => {
  const {run, context, done} = await walkPageContext('installer');
  try {
    context.said = [];
    context.dones = [];
    run('speaker = makeSpeaker((text, done) => { said.push(text); dones.push(done); })');
    run("for (const name of ['Back door', 'Garage entry', 'Gate', 'Shed', 'Kitchen window']) speaker.say(name)");
    assert.deepEqual([...context.said], ['Back door'], 'one is said, one waits');
    run('dones.shift()()');
    assert.deepEqual([...context.said], ['Back door', 'Garage entry']);
    run('dones.shift()()');
    assert.deepEqual([...context.said], ['Back door', 'Garage entry', '3 more zones tested']);
    run('dones.shift()()');
    assert.equal(context.said.length, 3, 'nothing is left');
    run("speaker.say('Gate'); speaker.say('Shed'); speaker.say('Fence')");
    run('dones.shift()()');
    run('dones.shift()()');
    assert.equal([...context.said].at(-1), '1 more zone tested');
  } finally { done(); }
});

test('what the phone does for each new answer: names and a beep for a first test, a tick for a repeat, a count after being away (WALK-014, WALK-015)', async () => {
  const {run, context, done} = await walkPageContext('installer');
  try {
    const diff = (seen, zones, trips, gap) => run(`walkDiff(${seen}, ${JSON.stringify(zones)}, ${trips}, ${gap})`);
    const z = (number, tested, first_s = 0) => ({number, name: `Zone ${number}`, tested, first_s});
    const seen = '{tested: new Set([1]), trips: 3}';
    const names = d => [...d.fresh.map(x => x.number)];
    assert.deepEqual(names(diff('null', [z(1, true)], 3, 0)), [], 'the first answer is not announced');
    assert.deepEqual(names(diff(seen, [z(1, true), z(2, true, 9), z(3, true, 5)], 5, 1000)), [3, 2], 'in the order they were tested');
    const away = diff(seen, [z(1, true), z(2, true), z(3, true), z(4, true)], 6, 9000);
    assert.deepEqual([away.fresh.length, away.away], [0, 3], 'after being away: how many, never the names');
    assert.deepEqual([diff(seen, [z(1, true)], 4, 1000).repeat, diff(seen, [z(1, true)], 3, 1000).repeat], [true, false], 'a repeat trip raises only the trips');
    // the same through walkApply: the phone's beep, vibration and voice
    Object.assign(context, {beeps: [], buzzes: [], spoken: [], navigator: {vibrate: p => context.buzzes.push(p)},
      speechSynthesis: {speak: u => context.spoken.push(u.text), cancel() {}}, SpeechSynthesisUtterance: class { constructor(text) { this.text = text; } }});
    run('beep = repeat => beeps.push(repeat)');
    const answer = zones => ({key: 1, partitions: [{number: 1, name: 'Home', running: true, remaining_s: 3000, started_s_ago: 20, started_by: 1, started_by_name: 'Installer',
      started_from: 'panel', tested: zones.filter(x => x.tested).length, total: 4, life24: false, ended_by: 0, ended_by_name: '', end_how: '', trips: zones.reduce((n, x) => n + x.trips, 0),
      zones: zones.map(x => ({cover: false, how: 'fault', open: false, check: false, ...x})), live: []}]});
    const one = (n, tested, trips) => ({number: n, name: `Zone ${n}`, tested, first_s: n, trips});
    const apply = (zones, gap) => run(`walkApply(${JSON.stringify(answer(zones))}, ${gap})`);
    run('walk.fb = {beep: true, vibrate: true, names: true, awake: true}');
    apply([one(1, false, 0), one(2, false, 0)], 0);
    assert.deepEqual([context.beeps.length, context.spoken.length], [0, 0], 'the first answer is the baseline');
    apply([one(1, false, 0), one(2, false, 0)], 1000);
    apply([one(1, true, 1), one(2, false, 0)], 1000);
    assert.deepEqual([...context.beeps], [false], 'a beep for a first test');
    assert.deepEqual([...context.spoken], ['Zone 1'], 'and its name');
    assert.deepEqual([...context.buzzes.at(-1)], [70, 60, 70, 60, 70], 'and a buzz where the browser can');
    apply([one(1, true, 2), one(2, false, 0)], 1000);
    assert.deepEqual([...context.beeps], [false, true], 'a repeat gives a short tick only');
    assert.deepEqual([...context.spoken], ['Zone 1'], 'and says nothing');
    assert.equal(context.buzzes.at(-1), 30);
    // off the Wi-Fi for ten seconds, back with two more tested: a count, no names, one beep
    run('walk.speaker = null');
    context.spoken.length = 0;
    apply([one(1, true, 2), one(2, true, 1), one(3, true, 1)], 10000);
    assert.deepEqual([...context.spoken], ['2 zones tested while you were away']);
    assert.equal(context.beeps.length, 3);
    // "Say zone names" off keeps the voice quiet
    run('walk.fb.names = false; walk.speaker = null');
    context.spoken.length = 0;
    apply([one(1, true, 2), one(2, true, 1), one(3, true, 1), one(4, true, 1)], 1000);
    assert.equal(context.spoken.length, 0);
  } finally { done(); }
});

test('the switches are remembered in this browser, and speech uses the page\'s language (WALK-014)', async () => {
  const {run, context, done} = await walkPageContext('installer');
  try {
    const prefs = () => ({...run('walkPrefs()')});
    assert.deepEqual(prefs(), {beep: true, vibrate: true, names: true, awake: true}, 'no storage: the defaults');
    const store = {};
    context.localStorage = {getItem: k => store[k] ?? null, setItem: (k, v) => { store[k] = v; }};
    run('walk.fb = {...walk.fb, vibrate: false}; walkSavePrefs()');
    assert.deepEqual(prefs(), {beep: true, vibrate: false, names: true, awake: true});
    context.localStorage = {getItem() { throw new Error('blocked'); }, setItem() { throw new Error('blocked'); }};
    run('walkSavePrefs()');
    assert.equal(run('walkPrefs().beep'), true, 'a blocked storage never breaks the page');
    context.document.documentElement = {lang: 'es-US'};
    Object.assign(context, {uttered: [], speechSynthesis: {speak: u => context.uttered.push(u)}, SpeechSynthesisUtterance: class { constructor(text) { this.text = text; } }});
    run("speakNow('Puerta trasera')");
    assert.deepEqual([context.uttered[0].text, context.uttered[0].lang], ['Puerta trasera', 'es-US']);
  } finally { done(); }
});

test('off the Wi-Fi the page says so and the test goes on; back, it catches up (WALK-015)', async () => {
  const {s, context, run, done} = await walkPageContext('installer', {MOCK_WALK_STEP_MS: '4000'}) // MOCK_FAST runs it 20 times faster: a step every 200 ms;
  try {
    await run('PAGES.walk.render()');
    await run(walkClickOf({wact: 'start'})); // from this page, so the phone's list starts empty
    const fetched = context.fetch;
    context.fetch = () => Promise.reject(new TypeError('Failed to fetch'));
    await run('walkPoll()');
    const off = run('walkBody()');
    assert.match(off, /Not connected to the panel/);
    assert.match(off, /Last update/);
    assert.match(off, /data-wact="end"[^>]*disabled/, 'End waits for the network');
    context.fetch = fetched;
    await walkTested(s.base);
    run('walk.lastOk -= 60000');
    context.spoken = [];
    Object.assign(context, {speechSynthesis: {speak: u => context.spoken.push(u.text), cancel() {}}, SpeechSynthesisUtterance: class { constructor(text) { this.text = text; } }});
    await run('walkPoll()');
    assert.ok(!/Not connected to the panel/.test(run('walkBody()')));
    assert.deepEqual([...context.spoken], ['5 zones tested while you were away'], 'how many were tested meanwhile, no backlog of names');
  } finally { done(); }
});

test('words: the walk test\'s states are worded from the catalog (I18N-031)', async () => {
  const pseudo = path.join(__dirname, '..', '..', '..', '..', 'build', 'i18n', 'pseudo', 'en-XA.json');
  const {existsSync} = require('node:fs');
  if (!existsSync(pseudo)) return;
  const doc = JSON.parse(readFileSync(pseudo, 'utf8'));
  const {s, run, done} = await walkPageContext('installer', {MOCK_WALK: '1', MOCK_WALK_STEP_MS: '300'});
  try {
    run(`Object.assign(words, ${JSON.stringify({locale: doc.locale, plural: doc.plural, entries: doc.entries})})`);
    await walkTested(s.base);
    await run('PAGES.walk.render()');
    const pages = [run('walkBody()')];
    run("walk.filter = 'all'");
    pages.push(run('walkBody()'));
    await run('PAGES.overview.render()');
    pages.push(run('overviewBody()'));
    run('walk.offline = true');
    pages.push(run('walkBody()'));
    run('walk.offline = false');
    await run("api('partition', {number: 1, action: 'walk_test_end'})");
    await run('walkRefresh(true)');
    pages.push(run('walkBody()'));
    run("walk.view[1] = 'ready'");
    pages.push(run('walkBody()'));
    const listed = (await (await fetch(s.base + '/api/v1/walk-test')).json()).partitions[0];
    const names = [...listed.zones, ...listed.live].map(z => z.name);
    for (const html of pages) {
      let text = html.replace(/<[^>]*>/g, '\n').replace(/\[[^\]]*\]/g, ' ');
      for (const name of names) text = text.split(name).join(' ');
      for (const line of text.split('\n'))
        assert.ok(!/[A-Z][a-z]+ [a-z]+ [a-z]+/.test(line.trim()), `unmarked "${line.trim()}"`);
    }
  } finally { done(); }
});

test('an alarm or its memory comes before the walk test\'s card on the Overview (WALK-007)', async () => {
  const {run, done} = await walkPageContext('master', {MOCK_WALK: '1'});
  try {
    await run('PAGES.overview.render()');
    assert.match(run('overviewBody()'), /data-go="walk"/, 'no alarm: the walk test\'s card');
    const fire = run("Object.assign(state.partitions[0], {alarm: true, siren: true, alarm_kind: 'fire'}); overviewBody()");
    assert.ok(fire.includes('Fire alarm') && fire.includes('data-silence="1"'), 'the fire alarm card, with Silence');
    assert.ok(!fire.includes('data-go="walk"'), 'not the walk test card');
    const memory = run("Object.assign(state.partitions[0], {alarm: false, siren: false, state: 'disarmed', memory: true}); overviewBody()");
    assert.ok(memory.includes('data-clear="1"') && !memory.includes('data-go="walk"'), 'the alarm memory, with Clear');
  } finally { done(); }
});

test('the speaker counts only zone names; a status phrase takes the waiting place and never adds to the count (WALK-015)', async () => {
  const {run, context, done} = await walkPageContext('installer');
  try {
    context.said = [];
    context.dones = [];
    run('speaker = makeSpeaker((text, done) => { said.push(text); dones.push(done); })');
    run("speaker.say('Back door'); speaker.say('Walk test ended. 1 of 7 tested.', true); speaker.say('Gate', true)");
    run('dones.shift()()');
    assert.deepEqual([...context.said], ['Back door', 'Gate'], 'the latest status wins the waiting place');
    run('dones.shift()()');
    assert.equal(context.said.length, 2, 'a status phrase is no zone: no "1 more zone tested"');
    run("speaker.say('A'); speaker.say('B'); speaker.say('Walk test ended.', true)");
    run('dones.shift()()');
    assert.deepEqual([...context.said].slice(2), ['A', 'Walk test ended.'], 'a status displaces the waiting name, which is counted');
    run('dones.shift()()');
    assert.equal([...context.said].at(-1), '2 more zones tested'.replace('2', '1').replace('zones', 'zone'));
    // stop: nothing is said after the page is left
    run("speaker.say('X'); speaker.say('Y'); speaker.say('Z'); speaker.stop()");
    const n = context.said.length;
    run('while (dones.length) dones.shift()()');
    run("speaker.say('W')");
    assert.equal(context.said.length, n, 'stopped for good');
  } finally { done(); }
});

test('leaving the page stops the voice, the wake lock and a poll in flight (WALK-014, WALK-015)', async () => {
  const {run, context, done} = await walkPageContext('installer', {MOCK_WALK: '1'});
  try {
    const state = {released: 0, resolve: null};
    Object.assign(context, {beeps: [], spoken: [], navigator: {wakeLock: {request: () => new Promise(r => { state.resolve = r; })}},
      speechSynthesis: {speak: u => context.spoken.push(u.text), cancel() {}}, SpeechSynthesisUtterance: class { constructor(text) { this.text = text; } }});
    run('beep = repeat => beeps.push(repeat)');
    await run('PAGES.walk.render()');
    run("page = 'walk'");
    // a wake lock still being granted when the page is left is let go at once
    const pending = run('walkSyncLock()');
    run("page = 'overview'; walkLeave()");
    state.resolve({release: async () => { state.released++; }, addEventListener() {}});
    await pending;
    assert.equal(state.released, 1, 'released');
    assert.equal(run('walk.lock'), null);
    // a poll that answers after the page was left says and beeps nothing
    run("walk.left = false; walk.fb = {beep: true, vibrate: false, names: true, awake: false}");
    const answer = {key: 99, partitions: [{number: 1, name: 'Home', running: true, remaining_s: 3000, started_s_ago: 20, started_by: 1, started_by_name: 'Installer',
      started_from: 'panel', tested: 1, total: 2, life24: false, ended_by: 0, ended_by_name: '', end_how: '', trips: 1,
      zones: [{number: 1, name: 'Zone 1', tested: true, first_s: 1, trips: 1, cover: false, how: 'fault', open: false, check: false}], live: []}]};
    context.__answer = answer;
    run("walk.seen[1] = {tested: new Set(), trips: 0}; walk.parts[1] = {running: true}");
    const fetched = context.fetch;
    let release;
    context.fetch = () => new Promise(r => { release = () => r({ok: true, status: 200, json: async () => answer, headers: {get: () => 'application/json'}, text: async () => JSON.stringify(answer)}); });
    const poll = run('walkPoll()');
    run('walkLeave()');
    release();
    await poll;
    context.fetch = fetched;
    assert.deepEqual([[...context.beeps], [...context.spoken]], [[], []], 'nothing after leaving');
    // and the speaker that was talking says no more
    run('walk.left = false; walk.speaker = null; walkSay("One"); walkSay("Two")');
    const first = run('walk.speaker');
    run('walkLeave()');
    assert.equal(first.stopped, true);
  } finally { done(); }
});

test('the report\'s cells are quoted: commas, quotes, line breaks, and a formula is made text (WALK-010)', async () => {
  const {run, context, done} = await walkPageContext('installer');
  try {
    context.__row = [['a,b', 'say "hi"', 'two\nlines', 'cr\rhere', 7, null]];
    assert.equal(run('walkCsv(__row)'), '"a,b","say ""hi""","two\nlines","cr\rhere",7,\r\n');
    for (const f of ['=SUM(A1)', '+1', '-1', '@x'])
      assert.equal(run(`walkCsv([[${JSON.stringify(f)}]])`), `'${f}\r\n`, `${f} is text`);
    assert.equal(run('walkCsv([["=1,2"]])'), '"\'=1,2"\r\n');
    assert.equal(run('walkCsv([["a"], ["b"]])'), 'a\r\nb\r\n');
  } finally { done(); }
});

// ---- BL-134: Installer > Zones, one page per zone with its sensor on it (docs/features/31) ----

// What the page is editing, from the browser's side: run page code in the context, click as a person would.
const zrun = (context, code) => vm.runInContext(code, context);
const zclick = (context, dataset) => {
  context.__dataset = dataset;
  return zrun(context, 'zpClick({target: {closest: () => ({disabled: false, dataset: __dataset})}})');
};
const zinput = (context, f, value, type = 'change', k = 'f-' + f) => {
  context.__target = {dataset: {f, k}, value, checked: value === true, type};
  return zrun(context, 'zpInput({target: __target})');
};
// The review the page's save opens, with what it would send once confirmed (the page's own review() is the panel's dialog).
const zreview = (context, call) => {
  zrun(context, 'review = options => { __review = options; }');
  call();
  return context.__review;
};
const sentTo = (context, name) => context.__posts.filter(([url]) => url.endsWith('/' + name)).map(([, body]) => body);

test('Installer > Zones is first in its group, Z-Wave next; the old addresses open the new page (BL-134, WORD-006)', async () => {
  await withPage('installer', {}, async (context, html) => {
    await zrun(context, 'refreshStatus()');
    const group = zrun(context, `Object.entries(PAGES).filter(([id, p]) => p.group === 'Installer' && p.nav !== false && pageAllowed(p)).map(([id]) => id)`);
    assert.deepEqual(Array.from(group), ['zone-setup', 'zwave', 'zigbee', 'partitions', 'options', 'access'], 'Sensor sources is no longer an entry; Zigbee after Z-Wave (BL-145)');
    assert.equal(zrun(context, `PAGES['zone-setup'].title`), zrun(context, `PAGES.zones.title`), 'two entries are called Zones, as drawn');
    assert.equal(zrun(context, `PAGES['zone-setup'].icon`), 'door');
    context.history.replaceState = (a, b, hash) => { context.__hash = hash; };
    for (const [id, arg, hash] of [['zones', 'setup', '#/zone-setup'], ['zones', '5', '#/zone-setup/5'], ['binding', '5', '#/zone-setup/5'],
      ['sensors', '', '#/zone-setup'], ['zones', '', null]]) {
      context.__id = id;
      context.__arg = arg;
      context.__hash = null;
      const out = await zrun(context, 'PAGES[__id].render(__arg)');
      await out.bind?.();
      assert.equal(context.__hash, hash, `#/${id}/${arg}`);
      assert.equal(out.html.includes('id="zones-live"'), hash === null, `#/${id}/${arg} opens ${hash ? 'the zone pages' : 'the everyday Zones'}`);
      if (hash) assert.equal(zrun(context, 'page'), 'zone-setup', 'the menu marks Installer > Zones');
    }
  });
  // a master has no Installer menu, and #/zones/5 stays the everyday page for them
  await withPage('master', {}, async (context, html) => {
    await zrun(context, 'refreshStatus()');
    assert.ok((await html('zones', '5')).includes('id="zones-live"'));
    assert.ok(!zrun(context, `pageAllowed(PAGES['zone-setup'])`));
  });
});

test('the zone list names the zones with no sensor first and ends with the sensor sources (BL-134)', async () => {
  await withPage('installer', {}, async (context, html) => {
    await zrun(context, 'refreshStatus()');
    const page = await html('zone-setup');
    clean(page, 'zone-setup');
    const none = page.indexOf('5 zones have no sensor'), list = page.indexOf('class="card"><ul class="list zp-list"');
    assert.ok(none > 0 && none < list, 'the zones with no sensor are named above the list');
    const named = words(page.slice(none, list));
    for (const name of ['Back door', 'Living room motion', 'Basement CO', 'Water heater leak', 'Keyfob']) assert.ok(named.includes(name), name);
    assert.ok(!named.includes('Front door'), 'a zone with a sensor is not named');
    assert.ok(page.includes('href="#/zone-setup/1"') && words(page).includes('Wired contact · Input 1'), 'a row opens the zone page and says where its sensor is');
    assert.ok(page.includes('data-arg="new"') && page.includes('Add a zone'));
    assert.ok(!page.includes('data-zone='), 'no table, no dialog');
    const sources = page.indexOf('<h2>Sensor sources</h2>');
    assert.ok(sources > list, 'the sources are the last section');
    assert.ok(words(page.slice(sources)).includes('Working') && words(page.slice(sources)).includes('Not on this panel:'));
    assert.ok(!page.includes('Link to a zone') && !page.includes('Sensor link'));
  });
});

test('the zone page has the touchscreen\'s groups for a contact, a motion sensor and a Home Assistant sensor (BL-134, WORD-006)', async () => {
  await withPage('installer', {}, async (context, html) => {
    await zrun(context, 'refreshStatus()');
    const groups = ['Right now', 'The zone', 'Its sensor', 'What it does', 'Advanced', 'Remove the sensor, keep the zone', 'Remove this zone'];
    const inOrder = (page, list) => list.reduce((at, text) => {
      const i = page.indexOf(text, at);
      assert.ok(i >= at, `"${text}" is missing or out of order`);
      return i;
    }, 0);
    const contact = await html('zone-setup', '1');
    clean(contact, 'zone 1');
    inOrder(contact, groups);
    assert.ok(contact.includes('href="#/zone-setup"'), 'back leads to the zone list');
    assert.ok(words(contact).includes('Wired contact') && contact.includes('data-f="contact"') && words(contact).includes('Normally closed'), 'a contact says how it is wired');
    assert.ok(!contact.includes('data-f="read"') && !contact.includes('Reverse open and closed'), 'a wired contact has no Open when');
    for (const text of ['Response time', 'When it goes quiet', 'Supervision', 'Unreachable after', 'Other inputs', 'Tamper input', 'Low-battery input'])
      assert.ok(words(contact).includes(text), `contact: ${text}`);
    assert.ok(contact.includes('<option value="350"') && contact.includes('<option value="3600"') && !/ type="number"[^>]*map-response/.test(contact), 'choices, not typed seconds');
    assert.ok(!contact.includes('type="password"'), 'no keypad code on the page');
    const motion = await html('zone-setup', '4');
    clean(motion, 'zone 4');
    inOrder(motion, groups);
    assert.ok(words(motion).includes('Zigbee sensor') && motion.includes('data-f="read"') && words(motion).includes('Waiting for the sensor'), 'a motion sensor the panel waits for');
    assert.ok(motion.includes('data-f="invert"'), 'Reverse open and closed on an on/off reading');
    const ha = await html('zone-setup', '3');
    clean(ha, 'zone 3');
    inOrder(ha, groups);
    assert.ok(words(ha).includes('Home Assistant sensor') && words(ha).includes('binary_sensor.kitchen_window') && ha.includes('data-f="read"'), 'a Home Assistant sensor');
    const none = await html('zone-setup', '5');
    clean(none, 'zone 5');
    assert.ok(words(none).includes('No sensor') && words(none).includes('Pick its sensor') && !none.includes('When it goes quiet'), 'no sensor yet: pick one first');
    assert.ok(!none.includes('Remove the sensor, keep the zone'), 'nothing to remove');
    const added = await html('zone-setup', 'new');
    clean(added, 'new zone');
    assert.ok(words(added).includes('Where is the sensor?') && words(added).includes('Add it without a sensor for now'), 'a new zone starts with its sensor');
    assert.ok(!(await html('zone-setup', '50')).includes('data-zp'), 'a zone that is not in use has no editor');
  });
});

test('What it does shows only the options the zone\'s type has (BL-134, WORD-006)', async () => {
  await withPage('installer', {}, async (context, html) => {
    await zrun(context, 'refreshStatus()');
    const does = async n => { const page = await html('zone-setup', String(n)); return words(page.slice(page.indexOf('What it does'), page.indexOf('Advanced'))); };
    const entry = await does(1);   // an entry/exit zone: Chime, no Night, not a 24-hour zone
    assert.ok(entry.includes('Chime') && entry.includes('Report this zone') && !entry.includes('Night zone') && !entry.includes('Can be bypassed'), entry);
    const interior = await does(4); // an interior follower: Night and Chime
    assert.ok(interior.includes('Night zone') && interior.includes('Chime') && !interior.includes('Can be bypassed'), interior);
    const aux = await does(17);     // a 24-hour zone: Can be bypassed, no Chime
    assert.ok(aux.includes('Can be bypassed') && !aux.includes('Chime') && !aux.includes('Night zone') && aux.includes('Report this zone'), aux);
    const smoke = await does(9);    // fire: only whether it reports
    assert.ok(!smoke.includes('Chime') && !smoke.includes('Night zone') && !smoke.includes('Can be bypassed') && smoke.includes('Report this zone'), smoke);
    const advanced = (await html('zone-setup', '1'));
    for (const text of ['Cross-zone partner', 'Alarm limit', 'System setting', 'Never']) assert.ok(words(advanced).includes(text), text);
    assert.ok(advanced.includes('<option value="99"'), 'Never is one choice of the alarm limit');
  });
});

test('the picker shows what each source sees, marks a sensor another zone uses and puts a just-changed one first (BL-134)', async () => {
  await withPage('installer', {}, async (context, html) => {
    await zrun(context, 'refreshStatus()');
    await html('zone-setup', '5');
    await zclick(context, {zp: 'pick', role: '0'});
    const kinds = zrun(context, 'zpBody()');
    assert.ok(words(kinds).includes('Where is the sensor?') && words(kinds).includes('Wired contact') && words(kinds).includes('Home Assistant sensor'), 'the sources the panel has');
    assert.ok(!words(kinds).includes('Zigbee sensor'), 'a source this panel can\'t use is not offered');
    await zclick(context, {zp: 'kind', kind: '2'});
    assert.equal(zrun(context, 'zp.cands.length'), 8, 'the wired terminals');
    let body = zrun(context, 'zpCandsBody()');
    for (const n of [1, 2, 5]) assert.ok(new RegExp(`data-key="2/${n}/0/"[^>]*disabled`).test(body), `input ${n} belongs to another zone`);
    assert.ok(words(body).includes('In use: Front door') && !/data-key="2\/7\/0\/"[^>]*disabled/.test(body), 'a free terminal can be picked');
    // a terminal that changes while the picker is open goes first, marked
    zrun(context, `zp.seen.set('2/7/0/', 'open')`);
    await zrun(context, 'zpSearch()');
    body = zrun(context, 'zpCandsBody()');
    assert.ok(body.indexOf('data-key="2/7/0/"') < body.indexOf('data-key="2/8/0/"') && body.indexOf('data-key="2/7/0/"') < body.indexOf('data-key="2/1/0/"'), 'the changed one is first');
    assert.ok(words(body.slice(body.indexOf('data-key="2/7/0/"'), body.indexOf('data-key="2/8/0/"'))).includes('Just opened or closed'));
    // Home Assistant: a search
    await zclick(context, {zp: 'kinds'});
    await zclick(context, {zp: 'kind', kind: '4'});
    zrun(context, `zp.query = 'door'`);
    await zrun(context, 'zpSearch()');
    body = zrun(context, 'zpCandsBody()');
    assert.ok(body.includes('Back door contact') && body.includes('Shed door') && !body.includes('Porch motion'), 'the search filters');
    assert.ok(body.includes('data-key="4/0/0/binary_sensor.back_door"') && !/data-key="4\/0\/0\/binary_sensor.back_door"[^>]*disabled/.test(body), 'the back door sensor is free: zone 5 has none');
    // picking is a change; typing an address by hand is one tap deeper
    assert.ok(zrun(context, 'zpBody()').includes('Type its address instead'));
    await zclick(context, {zp: 'cand', key: '4/0/0/binary_sensor.shed_door'});
    assert.equal(zrun(context, 'zp.inputs[0].address.ref'), 'binary_sensor.shed_door');
    assert.equal(zrun(context, 'zp.picking'), null);
  });
});

// Code review RF24 (RF-011, RF-021): a 433 MHz candidate's state is when it was last heard, and the source says
// which one was tested (changed, best) and which code to keep away from (unusable). The picker reads those as the
// touchscreen's does (ui_sources.c rf_status, fill, fill_codes), with its words; a wired contact keeps Open/Closed.
test('the 433 MHz picker reads the source\'s flags: age, not Open/Closed; the one it is sure of (RF-011, RF24)', async () => {
  await withPage('installer', {}, async (context, html) => {
    await zrun(context, 'refreshStatus()');
    await html('zone-setup', '5');
    await zclick(context, {zp: 'pick', role: '0'});
    zrun(context, 'zp.picking.kind = ZP_RF_KIND; zp.seen = new Map(); zp.moved = new Set(); zp.cands = []');
    const id = (device, proto = 1) => String(proto * 2 ** 32 + device);
    const key = (device, code = 3) => `5/${code + 1}/${id(device)}/`;
    const row = (body, k) => { const at = body.indexOf(`data-k="cand-${k}"`); return at < 0 ? '' : body.slice(body.lastIndexOf('<li', at), body.indexOf('</li>', at)); };
    const body = () => zrun(context, 'zpCandsBody()');

    // a unique best, through the mock panel's API (its shape is the firmware's: changed, best, unusable)
    await zrun(context, 'zpSearch()');
    let b = body();
    clean(b, 'rf best');
    assert.ok(words(b).includes('Found it: Sensor A2B3C answered each time.'), 'the sensor it is sure of is named first');
    assert.ok(b.includes(`data-key="${key(0xA2B3C)}" data-k="use-best"`) && words(b).includes('Use Sensor A2B3C'), 'one tap to use it');
    assert.ok(b.indexOf('Found it') < b.indexOf(`data-k="cand-${key(0xA2B3C)}"`), 'above the list');
    assert.ok(/class="zp-cand fresh"/.test(row(b, key(0xA2B3C))), 'the tested one is marked');
    assert.ok(b.indexOf(`data-k="cand-${key(0xA2B3C)}"`) < b.indexOf('Already transmitting before you started') &&
      b.indexOf('Already transmitting before you started') < b.indexOf(`data-k="cand-${key(0x51D07)}"`), 'the source\'s order, the earlier ones under their heading');
    assert.ok(words(row(b, key(0xA2B3C))).includes('Just now') && words(row(b, key(0x51D07))).includes('40 s ago'), 'when each was last heard');
    assert.ok(!words(b).includes('Closed') && !words(b).includes('Open ') && !words(b).includes('Just opened or closed'), 'an age is not a contact state');
    await zclick(context, {zp: 'cand', key: key(0xA2B3C)});
    assert.equal(zrun(context, 'zp.inputs[0].address.id'), id(0xA2B3C), 'Use picks it');
    assert.equal(zrun(context, 'zp.picking'), null);

    // the rest from API-shaped answers the page reads as it would the panel's
    zrun(context, 'zp.inputs[0].address = {...ZP_NONE}'); // none of them is this zone's own
    await zclick(context, {zp: 'pick', role: '0'});
    zrun(context, 'zp.picking.kind = ZP_RF_KIND; zp.seen = new Map(); zp.moved = new Set(); zp.cands = []');
    zrun(context, 'api = async (p, body) => p === "sources/search" ? {candidates: __rf} : {}');
    const cand = (device, extra) => ({address: {kind: 5, input: 4, id: id(device), ref: ''}, name: `Sensor ${device.toString(16).toUpperCase()}`,
      detail: 'Code 3', state: 'just now', zone: 0, device_class: 0, changed: false, best: false, unusable: false, ...extra});
    const answer = async list => { context.__rf = list; await zrun(context, 'zpSearch()'); return body(); };

    // heard only before learning began: nothing tested yet, nothing marked, each can still be picked
    b = await answer([cand(0x51D07, {state: '40 s ago'})]);
    assert.ok(words(b).includes('Listening: nothing heard yet.') && words(b).includes('Already transmitting before you started'));
    assert.ok(!/fresh/.test(row(b, key(0x51D07))) && !/disabled/.test(row(b, key(0x51D07))), 'not marked, not refused');

    // ageing with no new packet: the state text changes, nothing else does - no "just opened", no Closed
    b = await answer([cand(0x51D07, {state: 'just now'})]);
    assert.ok(words(row(b, key(0x51D07))).includes('Just now') && !words(b).includes('Closed'), 'Just now is an age');
    b = await answer([cand(0x51D07, {state: '5 s ago'})]);
    assert.equal(zrun(context, 'zp.moved.size'), 0, 'an age that moves is not a change');
    assert.ok(words(row(b, key(0x51D07))).includes('5 s ago') && !words(b).includes('Just opened or closed') && !words(b).includes('Closed'));
    assert.ok(!/fresh/.test(row(b, key(0x51D07))));

    // one heard while learning, then two that tie: no best, the touchscreen's words for each
    b = await answer([cand(0xA2B3C, {changed: true, detail: 'Codes 3 and 5, heard twice'}), cand(0x51D07, {state: '40 s ago'})]);
    assert.ok(words(b).includes('Heard Sensor A2B3C. Close the door (or press the button again) to be sure.') && !words(b).includes('Found it'));
    assert.ok(/fresh/.test(row(b, key(0xA2B3C))) && words(row(b, key(0xA2B3C))).includes('Codes 3 and 5, heard twice'), 'its codes, not "Just opened or closed"');
    b = await answer([cand(0xA2B3C, {changed: true}), cand(0x6E0F1, {changed: true})]);
    assert.ok(words(b).includes('Heard 2 sensors. Open and close the door once more: the one that answers is picked.'));
    assert.ok(!words(b).includes('Found it') && !b.includes('data-k="use-best"') && !words(b).includes('Already transmitting'), 'a tie names no one');

    // the one it is sure of already feeds another zone: said, and no button to take it
    b = await answer([cand(0xA2B3C, {changed: true, best: true, zone: 2})]);
    assert.ok(words(b).includes('Sensor A2B3C answered, but it already feeds Garage door.') && !b.includes('data-k="use-best"'));
    assert.ok(/disabled/.test(row(b, key(0xA2B3C))) && words(row(b, key(0xA2B3C))).includes('In use: Garage door'));
  });
});

// Code review RF24 repair (RF-021, bug B012): a 433 MHz zone's tamper or low-battery input is a code of the zone's own
// sensor. The touchscreen asks the source for that sensor's codes ("codes <protocol> <device>", zsrc_rf_codes_query) and
// shows them as fill_codes does; the sensors around, "Found it" and its one-tap "Use" never show there, so the door's own
// code can't become its tamper code by a tap, and the code the source says closes the zone shows why and can't be picked.
test('a 433 MHz tamper input asks for its sensor\'s codes; the door\'s code is never offered, the closing code is refused (RF-021, B012, RF24)', async () => {
  await withPage('installer', {}, async (context, html) => {
    await zrun(context, 'refreshStatus()');
    await html('zone-setup', '5');
    const id = String(2 ** 32 + 0xA2B3C), key = code => `5/${code + 1}/${id}/`;
    const row = (body, k) => { const at = body.indexOf(`data-k="cand-${k}"`); return at < 0 ? '' : body.slice(body.lastIndexOf('<li', at), body.indexOf('</li>', at)); };
    const body = () => zrun(context, 'zpCandsBody()');
    context.__main = id; // the zone's own sensor: A2B3C, its opening code 3
    zrun(context, 'zp.inputs[0].address = {kind: 5, input: 4, id: __main, ref: ""}');
    zrun(context, 'var __asked = [], __api0 = api; api = async (p, b) => { if (p === "sources/search") __asked.push(b.query); return __api0(p, b); }');

    await zclick(context, {zp: 'pick', role: '1'});
    await zclick(context, {zp: 'kind', kind: '5'});
    assert.equal(zrun(context, 'JSON.stringify([...new Set(__asked)])'), '["codes 1 A2B3C"]', 'the touchscreen\'s question, not the sensors around');
    const head = words(zrun(context, 'zpPicker()'));
    assert.ok(head.includes('Find the tamper code') && head.includes('Open and close the door, then take the cover off') &&
      head.includes('Pick the code that appeared as the cover came off.') && !head.includes('Find the sensor'), 'the touchscreen\'s title and words');
    let b = body();
    clean(b, 'rf codes');
    assert.ok(!words(b).includes('Found it') && !b.includes('data-k="use-best"') && !words(b).includes('Already transmitting') && !words(b).includes('Sensor 51D07'),
      'no "Found it", no sensors around');
    const own = row(b, key(3)), closer = row(b, key(5)), cover = row(b, key(7));
    assert.ok(/disabled/.test(own) && words(own).includes('Sent first, twice · The zone&#39;s own code'), 'the door\'s own code: said, refused');
    assert.ok(/disabled/.test(closer) && words(closer).includes('Closes the zone, twice') && !/fresh/.test(closer), 'the source\'s reason, refused');
    assert.ok(!/disabled/.test(cover) && /fresh/.test(cover) && words(cover).includes('Sent third, once') && words(cover).includes('Just now'));
    assert.ok(!words(b).includes('Only its door codes so far'), 'one can be picked');
    for (const code of [3, 5]) {
      await zclick(context, {zp: 'cand', key: key(code)});
      assert.equal(zrun(context, 'zp.inputs[1].address.kind'), 0, `a tap on code ${code} changes nothing`);
      assert.notEqual(zrun(context, 'zp.picking'), null);
    }

    // even a source that marks the door's code as the one it is sure of: no "Found it", no one-tap use, the row refused
    zrun(context, 'api = async (p, b) => p === "sources/search" ? {candidates: __codes} : {}');
    const code = (c, extra) => ({address: {kind: 5, input: c + 1, id, ref: ''}, name: `Code ${c}`, detail: 'Sent first, once', state: 'just now',
      zone: 0, device_class: 0, changed: true, best: false, unusable: false, ...extra});
    const answer = async list => { context.__codes = list; await zrun(context, 'zpSearch()'); return body(); };
    b = await answer([code(3, {best: true})]);
    assert.ok(!words(b).includes('Found it') && !b.includes('data-k="use-best"') && /disabled/.test(row(b, key(3))));
    assert.ok(words(b).includes('Only its door codes so far. If taking the cover off sends one of them, this sensor can&#39;t report a tamper apart from the door: leave Tamper input as None.'));
    await zclick(context, {zp: 'cand', key: key(3)});
    assert.equal(zrun(context, 'zp.inputs[1].address.kind'), 0, 'never its tamper code in one tap');
    // nothing yet; another zone's code
    b = await answer([]);
    assert.ok(words(b).includes('Listening: nothing heard from this sensor yet.') && !words(b).includes('Listening: nothing heard yet.'));
    b = await answer([code(3), code(7, {zone: 2})]);
    assert.ok(/disabled/.test(row(b, key(7))) && words(row(b, key(7))).includes('In use: Garage door'));
    // the cover's code: picked
    b = await answer([code(3), code(7, {detail: 'Sent second, once'})]);
    await zclick(context, {zp: 'cand', key: key(7)});
    assert.equal(zrun(context, 'zp.inputs[1].address.input'), 8, 'its tamper code');
    assert.equal(zrun(context, 'zp.picking'), null);

    // the low-battery input: its own words; the tamper code just picked is said, and refused
    await zclick(context, {zp: 'pick', role: '2'});
    await zclick(context, {zp: 'kind', kind: '5'});
    assert.ok(words(zrun(context, 'zpPicker()')).includes('Find the low-battery code'));
    b = await answer([code(3), code(7)]);
    assert.ok(words(row(b, key(7))).includes('The zone&#39;s tamper code') && /disabled/.test(row(b, key(7))));
    assert.ok(words(b).includes('Only its door codes so far.') && !words(b).includes('leave Tamper input as None'));

    // a zone whose own sensor is not on 433 MHz: its tamper input's 433 MHz picker is the sensor search, as on the touchscreen
    zrun(context, 'zp.inputs[0].address = {kind: 2, input: 7, id: "0", ref: ""}');
    assert.equal(zrun(context, 'zpCodesQuery()'), '');
  });
});

test('a wired contact in the picker keeps Open and Closed and its "Just opened or closed" (RF24 control, BL-134)', async () => {
  await withPage('installer', {}, async (context, html) => {
    await zrun(context, 'refreshStatus()');
    await html('zone-setup', '5');
    await zclick(context, {zp: 'pick', role: '0'});
    await zclick(context, {zp: 'kind', kind: '2'});
    zrun(context, `zp.seen.set('2/7/0/', 'open')`);
    await zrun(context, 'zpSearch()');
    const body = zrun(context, 'zpCandsBody()');
    assert.ok(words(body).includes('Closed') && !words(body).includes('ago') && !words(body).includes('Just now'), 'contacts say Open or Closed');
    assert.ok(words(body).includes('Just opened or closed') && !words(body).includes('Already transmitting') && !body.includes('zp-status'), 'no 433 MHz words');
  });
});

test('the save bar counts the changes and saves the zone and its sensor with one review and no code (BL-134, WORD-006)', async () => {
  await withPage('installer', {}, async (context, html, a) => {
    await zrun(context, 'refreshStatus()');
    await html('zone-setup', '1');
    assert.equal(zrun(context, 'zpDock()'), '', 'nothing changed, no bar');
    await zclick(context, {zp: 'pick', role: '0'});
    await zclick(context, {zp: 'kind', kind: '2'});
    await zclick(context, {zp: 'cand', key: '2/7/0/'});
    assert.ok(words(zrun(context, 'zpDock()')).includes('1 change') && words(zrun(context, 'zpDock()')).includes('Review and save'));
    zinput(context, 'contact', '1');
    zinput(context, 'zone.name', 'Front gate', 'input');
    const dock = words(zrun(context, 'zpDock()'));
    assert.ok(dock.includes('3 changes') && dock.includes('Review and save') && ['Name', 'Source', 'Contact'].every(x => dock.includes(x)), dock);
    const review = zreview(context, () => zrun(context, 'zpSave()'));
    assert.deepEqual(Array.from(review.changes, c => [c.label, c.from, c.to]),
      [['Name', 'Front door', 'Front gate'], ['Source', 'Wired contact · Input 1', 'Wired contact · Input 7'], ['Contact', 'Normally closed', 'Normally open']], 'before -> after');
    assert.ok(!review.password, 'no password for programming');
    await review.run();
    const alarmPosts = sentTo(context, 'alarm'), bindingPosts = sentTo(context, 'binding/1');
    assert.equal(alarmPosts.length, 1);
    assert.deepEqual(alarmPosts[0].changes, {zones: [{number: 1, name: 'Front gate'}]});
    assert.equal(bindingPosts.length, 1, 'the sensor goes with the zone');
    assert.equal(bindingPosts[0].inputs[0].address.input, 7);
    assert.equal(bindingPosts[0].inputs[0].mapping.invert, true);
    for (const body of [...alarmPosts, ...bindingPosts]) assert.ok(!('code' in body), 'no keypad code in the request');
    assert.equal((await a.get('binding/1')).inputs[0].address.input, 7);
    assert.equal((await a.get('alarm')).zones[0].name, 'Front gate');
    // a change undone is no change; leaving drops the rest
    await html('zone-setup', '1');
    zinput(context, 'flag.1', false);
    assert.ok(words(zrun(context, 'zpDock()')).includes('1 change'));
    zinput(context, 'flag.1', true);
    assert.equal(zrun(context, 'zpDock()'), '');
    zinput(context, 'flag.1', false);
    zrun(context, 'zpLeave()');
    await html('zone-setup', '1');
    assert.equal(zrun(context, 'zpDock()'), '', 'unsaved changes are dropped on leaving');
  });
});

test('the save bar counts a Hold, a Hysteresis or an Acts as changed alone (BL-134)', async () => {
  await withPage('installer', {}, async (context, html) => {
    await zrun(context, 'refreshStatus()');
    const alone = () => Array.from(zrun(context, 'zpChanges()'), c => [c.label, c.from, c.to]);
    const sensor = (kind, mapping) => `zp.inputsBefore[0] = {role: 0, address: {kind: ${kind}, input: 1, id: '7', ref: ''}, mapping: {...ZP_MAP0, ${mapping}}};
      zp.inputs[0] = JSON.parse(JSON.stringify(zp.inputsBefore[0]))`;
    await html('zone-setup', '6');
    zrun(context, sensor(5, 'hold_s: 0'));
    assert.deepEqual(alone(), []);
    zrun(context, 'zp.inputs[0].mapping.hold_s = 600');
    assert.deepEqual(alone(), [['Hold', 'Automatic', '10 min']]);
    assert.ok(words(zrun(context, 'zpDock()')).includes('1 change'), 'the bar shows');
    zrun(context, sensor(4, 'mode: 1, threshold_x100: 2000, hysteresis_x100: 0'));
    zrun(context, 'zp.inputs[0].mapping.hysteresis_x100 = 50');
    assert.deepEqual(alone(), [['Hysteresis', '0', '0.5']]);
    await html('zone-setup', '18');
    zrun(context, 'zp.zone.user = 3');
    assert.deepEqual(alone(), [['Acts as', String(zrun(context, 'zp.before.user')), '3']]);
  });
});

test('Remove the sensor keeps the zone; Remove this zone asks on its own and frees its sensor (BL-134)', async () => {
  await withPage('installer', {}, async (context, html, a) => {
    await zrun(context, 'refreshStatus()');
    await html('zone-setup', '1');
    await zclick(context, {zp: 'clear', role: '0'});
    const review = zreview(context, () => zrun(context, 'zpSave()'));
    assert.deepEqual(Array.from(review.changes, c => [c.label, c.from, c.to]), [['Source', 'Wired contact · Input 1', 'None']], 'one more change in the bar');
    await review.run();
    assert.equal(sentTo(context, 'alarm').length, 0, 'the zone is untouched');
    assert.deepEqual(sentTo(context, 'binding/1'), [{action: 'unbind'}]);
    assert.ok(!(await a.get('zone-sources')).zones.some(x => x.zone === 1) && (await a.get('alarm')).zones[0].type === 1, 'no sensor, the zone stays');
    // Remove this zone: its own question, its sensor freed
    await html('zone-setup', '2');
    const taken = await a.post('sources/search', {kind: 2, query: ''}).then(r => r.json());
    assert.equal(taken.candidates[1].zone, 2, 'input 2 is zone 2\'s');
    const ask = zreview(context, () => zrun(context, 'zpRemove()'));
    assert.ok(ask.danger && ask.title.includes('Garage door') && ask.changes.some(c => c.label === 'Source' && c.to === 'Free for another zone'));
    await ask.run();
    assert.equal((await a.get('alarm')).zones[1].type, 0, 'the zone is gone');
    assert.ok(!(await a.get('zone-sources')).zones.some(x => x.zone === 2), 'its sensor is not left behind');
    assert.equal((await a.post('sources/search', {kind: 2, query: ''}).then(r => r.json())).candidates[1].zone, 0, 'the terminal can go to another zone');
  });
});

test('GET zone-sources says where each zone\'s sensor is, for the installer only; the search answers the wired terminals too (BL-134)', async () => {
  const get = (who, p) => fetch(servers[who].base + '/api/v1/' + p);
  assert.equal((await get('master', 'zone-sources')).status, 403);
  assert.equal((await get('standard', 'zone-sources')).status, 403);
  const mapped = await (await get('installer', 'zone-sources')).json();
  const at = n => mapped.zones.find(x => x.zone === n)?.address;
  assert.deepEqual(at(1), {kind: 2, input: 1, id: '0', ref: ''});
  assert.deepEqual(at(3), {kind: 4, input: 0, id: '0', ref: 'binary_sensor.kitchen_window'});
  assert.equal(at(5), undefined, 'no sensor, not listed');
  assert.ok(mapped.zones.every(x => typeof x.zone === 'number' && typeof x.address.id === 'string'));
  const search = async (kind, query = '') => (await (await fetch(servers.installer.base + '/api/v1/sources/search', {method: 'POST',
    headers: {'Content-Type': 'application/json', 'X-Panel-CSRF': 'auto'}, body: JSON.stringify({kind, query})})).json()).candidates;
  const wired = await search(2);
  assert.deepEqual(wired.map(c => c.address.input), [1, 2, 3, 4, 5, 6, 7, 8]);
  assert.equal(wired[0].name, 'Input 1');
  assert.equal(wired[0].zone, 1);
  // the firmware's keys (web_extra.c candidates()); changed, best and unusable were added for RF24 (RF-011, RF-021)
  assert.deepEqual(Object.keys(wired[0]).sort(), ['address', 'best', 'changed', 'detail', 'device_class', 'name', 'state', 'unusable', 'zone']);
  const rf = await search(5);
  assert.deepEqual(rf.map(c => [c.name, c.state, c.changed, c.best, c.unusable]),
    [['Sensor A2B3C', 'just now', true, true, false], ['Sensor 51D07', '40 s ago', false, false, false]], 'a 433 MHz answer: ages and flags');
  // one sensor's codes for a tamper or low-battery input (RF-021): the closing one advised against
  assert.deepEqual((await search(5, 'codes 1 A2B3C')).map(c => [c.name, c.unusable, c.best]), [['Code 3', false, false], ['Code 5', true, false], ['Code 7', false, false]]);
  assert.equal((await search(1))[2].name, 'Simulated input 3');
  const doors = await search(4, 'door');
  assert.deepEqual(doors.map(c => c.name), ['Back door contact', 'Shed door']);
  assert.equal((await search(4, 'kitchen'))[0].zone, 3);
  assert.equal((await search(4)).length, 6, 'an empty search shows everything');
});

// RF-022: a binding's hold time is offered on the zone page for the sources that read it (433 MHz, Z-Wave), not
// for a wired input; it round-trips through the panel; a low-battery input newly set to a 433 MHz sensor holds
// 12 h, as at the panel (RF-021), a value set by hand stays, and the tamper input keeps automatic.
test('the zone page has a hold for 433 MHz and Z-Wave, keeps it when saved, and defaults a new low-battery input to 12 h (RF-022)', async () => {
  await withPage('installer', {}, async (context, html, a) => {
    await zrun(context, 'refreshStatus()');
    await html('zone-setup', '6');
    const input = (kind, hold) => ({address: {kind, input: 1, id: '7', ref: ''}, mapping: {mode: 0, invert: false, threshold_x100: 0,
      hysteresis_x100: 0, states: '', grace_s: 0, response_ms: 0, hold_s: hold}});
    const shows = kind => {
      context.__sensor = {role: 0, ...input(kind, 0)};
      zrun(context, 'zp.inputs[0] = __sensor');
      return zrun(context, 'zpSensorCard()').includes('data-f="map.hold_s"');
    };
    assert.equal(shows(5), true, '433 MHz shows the hold');
    assert.equal(shows(7), true, 'Z-Wave shows the hold');
    assert.equal(shows(2), false, 'a wired input does not');
    assert.equal(shows(4), false, 'a Home Assistant entity does not');
    // a hold already set shows as it is, and is saved as it is
    context.__held = input(5, 600);
    zrun(context, 'zp.inputs[0] = __held');
    assert.ok(zrun(context, 'zpSensorCard()').includes('<option value="600" selected>'), 'the stored hold is offered');
    const review = zreview(context, () => zrun(context, 'zpSave()'));
    await review.run();
    assert.equal(sentTo(context, 'binding/6')[0].inputs[0].mapping.hold_s, 600, 'saving keeps the hold');
    assert.equal((await a.get('binding/6')).inputs[0].mapping.hold_s, 600, 'the panel keeps it');
    // a new low-battery input on a 433 MHz sensor: 12 h; one set by hand stays; the tamper input keeps automatic
    const pick = (role, address) => { context.__address = address; zrun(context, `zp.picking = {role: ${role}, kind: 5}; zpUse(__address, null)`); };
    const sensor = {kind: 5, input: 0, id: '7', ref: ''};
    pick(1, sensor);
    pick(2, sensor);
    assert.equal(zrun(context, 'zp.inputs[1].mapping.hold_s'), 0);
    assert.equal(zrun(context, 'zp.inputs[2].mapping.hold_s'), 43200);
    zrun(context, 'zp.inputs[2] = {role: 2, address: {...ZP_NONE}, mapping: {...ZP_MAP0, hold_s: 90}}');
    pick(2, sensor);
    assert.equal(zrun(context, 'zp.inputs[2].mapping.hold_s'), 90, 'set by hand');
    zrun(context, 'zp.inputs[2] = {role: 2, address: {kind: 5, input: 0, id: \'7\', ref: \'\'}, mapping: {...ZP_MAP0}}');
    pick(2, {...sensor, id: '8'});
    assert.equal(zrun(context, 'zp.inputs[2].mapping.hold_s'), 0, 'already bound: no default');
    // the mock's round trip, and a wired zone's binding
    const post = await a.post('binding/6', {inputs: [input(5, 3600), input(0, 0), input(0, 0)]});
    assert.equal(post.status, 200);
    assert.equal((await a.get('binding/6')).inputs[0].mapping.hold_s, 3600);
    assert.equal((await a.get('binding/1')).inputs[0].mapping.hold_s, 0);
  });
});

// ---- BL-136, WEB-017: People and codes, a page per person; the eye on every password box ----------------------------

async function withServer(who, extra, run) {
  const server = await start(who, extra);
  try { return await run(server); } finally { server.child.kill(); }
}
// The page functions in the stand-in browser: go(), toast() and the dialog are recorded, not drawn.
async function personContext(server) {
  const context = browser(server.base);
  context.__went = []; context.__toasts = []; context.__dialogs = [];
  const original = context.document.querySelector;
  context.document.querySelector = selector => selector === '#me-password' ? null : original(selector);
  vm.runInContext(`go = (id, arg = '') => { __went.push([id, arg]); }; toast = m => { __toasts.push(m); };
    openDialog = html => { __dialogs.push(html); }; closeDialog = () => {};`, context);
  await vm.runInContext('refreshStatus()', context);
  const run = code => vm.runInContext(code, context);
  const page = async arg => { context.__arg = arg; const out = await run('PAGES.people.render(__arg)'); return typeof out === 'string' ? {html: out} : out; };
  const edit = (f, extra = {}, type = 'input') => { context.__ev = {type, target: {dataset: {f}, ...extra}}; run('pcInput(__ev)'); };
  const click = async act => { context.__act = act; await run('pcClick({target: {closest: () => ({disabled: false, dataset: {pc: __act}})}})'); };
  const dock = () => run('pcDock()').replace(/<[^>]*>/g, ' ').replace(/\s+/g, ' ').trim();
  const error = () => context.document.querySelector('#pc-error').textContent;
  const posts = () => context.__posts.filter(([url]) => url.endsWith('/people')).map(([, body]) => body);
  return {context, run, page, edit, click, dock, error, posts};
}
const anchors = html => [...html.matchAll(/href="#\/people\/(\d+)"/g)].map(m => Number(m[1]));

test('BL-136 WEB-017: the list is a row per person, and a row opens a page only for people you manage', async () => {
  const installer = await personContext(servers.installer);
  const all = (await installer.page('')).html;
  assert.deepEqual(anchors(all), [1, 2, 3, 4, 5], 'the installer opens everyone');
  assert.match(all, /data-go="people" data-arg="new"/, 'Add a person');
  assert.ok(!all.includes('<table'), 'a list of rows, not a table');
  const pat = await personContext(servers.partition);
  const mine = (await pat.page('')).html;
  assert.deepEqual(anchors(mine), [3, 4, 5], 'a partition master opens only the people it manages');
  assert.match(mine, /Installer/, 'the installer is still listed');
  const refused = (await pat.page('1')).html;
  assert.match(refused, /You don.t manage this person/);
  assert.ok(!refused.includes('pc-body'), 'no form for someone you don’t manage');
});

test('BL-136 WEB-017: the bar counts and names the changes, Undo puts them back, Save sends no code and asks for no review', async () => {
  await withServer('installer', {}, async server => {
    const p = await personContext(server);
    const view = await p.page('3');
    assert.match(view.html, /<h1>Sam<\/h1>/);
    assert.equal(p.dock(), '', 'no bar until something changes');
    p.edit('name', {value: 'Samuel'});
    assert.equal(p.dock(), '1 change Name Undo Save');
    p.edit('role', {value: '3'}, 'change');
    assert.equal(p.dock(), '2 changes Name and What this code can do Undo Save');
    await p.click('undo');
    assert.equal(p.dock(), '');
    assert.equal(p.run('pc.now.name'), 'Sam');
    p.edit('name', {value: 'Samuel'});
    await p.click('save');
    const [sent] = p.posts();
    assert.deepEqual([sent.action, sent.number, sent.name, sent.authority, sent.new_code], ['save', 3, 'Samuel', 4, '']);
    assert.ok(!('code' in sent) && !('me_password' in sent), 'no keypad code and no password');
    assert.equal(p.context.__dialogs.length, 0, 'no review dialog');
    assert.equal(JSON.stringify(p.context.__went.at(-1)), '["people","3"]');
    assert.deepEqual(p.context.__toasts, [], 'the page shows a new name: no toast');
    const now = (await (await fetch(server.base + '/api/v1/people')).json()).users.find(u => u.number === 3);
    assert.equal(now.name, 'Samuel');
  });
});

test('BL-136 WEB-017: Remove asks first, and removes only when confirmed', async () => {
  await withServer('installer', {}, async server => {
    const p = await personContext(server);
    await p.page('3');
    await p.click('remove');
    assert.equal(p.context.__dialogs.length, 1);
    assert.match(p.context.__dialogs[0], /Remove Sam\?/);
    assert.deepEqual(p.posts(), [], 'nothing is sent before the answer');
    await p.context.document.querySelector('#review-form').onsubmit({preventDefault() {}, submitter: {}});
    assert.deepEqual(p.posts().map(b => [b.action, b.number]), [['delete', 3]]);
    assert.equal(JSON.stringify(p.context.__went.at(-1)), '["people",""]');
  });
});

test('BL-136 WEB-017: a password set for someone else is theirs to keep unless "Ask them to change it" is on', async () => {
  await withServer('installer', {}, async server => {
    const p = await personContext(server);
    const sam = async () => (await (await fetch(server.base + '/api/v1/people')).json()).users.find(u => u.number === 3);
    await p.page('3/password');
    assert.equal(p.run('pc.setting'), true, '#/people/3/password opens the new-password box');
    assert.equal(p.run('pc.mustChange'), false, 'off by default');
    p.edit('password', {value: 'sams own new password'});
    assert.match(p.dock(), /Browser password/);
    await p.click('save');
    assert.equal(p.posts().at(-1).temporary, false);
    assert.deepEqual(p.context.__toasts, ['Saved.'], 'a password is never shown: say it worked');
    assert.equal((await sam()).web_login_temporary, false, 'they keep it');
    await p.page('3/password');
    p.edit('password', {value: 'another new password'});
    p.edit('must', {checked: true}, 'change');
    await p.click('save');
    assert.equal(p.posts().at(-1).temporary, true);
    assert.equal((await sam()).web_login_temporary, true, 'they choose their own at the next sign-in');
    assert.ok(!('code' in p.posts().at(-1)) && !('me_password' in p.posts().at(-1)));
  });
});

test('BL-136 WEB-017: a code’s length follows the panel’s shortest allowed code', async () => {
  await withServer('installer', {MOCK_CODE_MIN: '6'}, async server => {
    const p = await personContext(server);
    assert.match((await p.page('3')).html, /6 to 10 digits/);
    p.edit('code', {value: '12345'});
    await p.click('save');
    assert.equal(p.error(), 'A code has 6 to 10 digits.');
    assert.deepEqual(p.posts(), [], 'nothing sent');
  });
  await withServer('installer', {}, async server => {
    const p = await personContext(server);
    assert.match((await p.page('3')).html, /4 to 10 digits/);
    p.edit('code', {value: '123'});
    await p.click('save');
    assert.equal(p.error(), 'A code has 4 to 10 digits.');
  });
});

test('BL-136 WEB-017: a code another person has shows in the bar, and the panel moves nothing', async () => {
  await withServer('installer', {}, async server => {
    const p = await personContext(server);
    await p.page('3');
    p.edit('name', {value: 'Samuel'});
    p.edit('code', {value: '1234'}); // Alex's
    await p.click('save');
    assert.equal(p.error(), 'Another user already has that code.');
    assert.equal(p.context.__went.length, 0, 'the page stays, with the changes still waiting');
    assert.match(p.dock(), /2 changes/);
    const post = body => fetch(server.base + '/api/v1/people', {method: 'POST',
      headers: {'Content-Type': 'application/json', 'X-Panel-CSRF': 'auto'}, body: JSON.stringify(body)});
    const refused = await post({number: 3, action: 'save', name: 'Sam', authority: 4, partitions: 1, new_code: '1234'});
    assert.equal(refused.status, 409);
    assert.equal((await refused.json()).error, 'code_in_use');
    assert.equal((await post({number: 3, action: 'save', name: 'Sam', authority: 4, partitions: 1, new_code: '1111'})).status, 200,
      'a person’s own code is not "in use"');
  });
});

test('BL-136 WEB-017: the panel answers for your own number only on Your password, and temporary defaults to true', async () => {
  await withServer('installer', {}, async server => {
    const post = body => fetch(server.base + '/api/v1/people', {method: 'POST',
      headers: {'Content-Type': 'application/json', 'X-Panel-CSRF': 'auto'}, body: JSON.stringify(body)});
    for (const action of ['web-password', 'web-off']) {
      const own = await post({number: 1, action, password: 'a new installer password'});
      assert.equal(own.status, 403, action);
      assert.equal((await own.json()).error, 'not_allowed');
    }
    assert.equal((await post({number: 3, action: 'web-password', password: 'sam no flag at all'})).status, 200);
    const people = async () => (await fetch(server.base + '/api/v1/people')).json();
    assert.equal((await people()).users.find(u => u.number === 3).web_login_temporary, true, 'no flag: temporary, as the panel');
    assert.equal((await people()).code_min, 4);
    assert.equal((await post({number: 3, action: 'web-password', password: 'sam keeps this one', temporary: false})).status, 200);
    assert.equal((await people()).users.find(u => u.number === 3).web_login_temporary, false);
  });
});

test('BL-136 WEB-017: the guest, arm-only and duress roles have no Panel Manager card', async () => {
  await withServer('installer', {}, async server => {
    const p = await personContext(server);
    const guest = (await p.page('4')).html; // a code that disarms only what it armed: no sign-in
    assert.ok(!guest.includes('Panel Manager') && !guest.includes('id="pc-login"'));
    await p.page('3');
    assert.match(p.run('pcLoginCard()'), /id="pc-login"/, 'a standard user has the switch');
    for (const role of ['5', '6', '7']) {
      p.edit('role', {value: role}, 'change');
      const card = p.run('pcLoginCard()');
      assert.ok(!card.includes('id="pc-login"') && !card.includes('pc-password'), `role ${role}: no switch`);
      assert.match(card, /sign-in ends/, 'turning someone into one says their sign-in ends');
      assert.match(p.dock(), /Browser sign-in/);
    }
    p.edit('role', {value: '4'}, 'change');
    assert.match(p.run('pcLoginCard()'), /id="pc-login"/);
    await p.page('new');
    p.edit('role', {value: '5'}, 'change');
    assert.equal(p.run('pcLoginCard()'), '', 'a new arm-only person has none');
    assert.match(p.dock(), /^New person/);
    await p.click('save');
    assert.equal(p.error(), 'Choose a code for them.');
  });
});

test('BL-136 WEB-017: your own page has no switch and no password box, and Change your password opens Your password', async () => {
  const p = await personContext(servers.installer);
  const html = (await p.page('1')).html;
  assert.ok(!html.includes('id="pc-login"') && !html.includes('id="pc-password"'));
  assert.ok(!html.includes('Remove this person'), 'you do not remove yourself');
  assert.match(html, /Change your password/);
  await p.click('password');
  assert.equal(JSON.stringify(p.context.__went.at(-1)), '["password",""]');
});

// A stand-in for what the eye touches: a box, its wrapper and the button.
function eyeNodes() {
  const node = tag => ({tag, dataset: {}, attrs: {}, innerHTML: '', title: '', setAttribute(k, v) { this.attrs[k] = v; }, focus() {},
    append(x) { x.parentElement = this; if (x.tag === 'button') this.button = x; }, querySelector() { return this.button; }});
  return node;
}
test('BL-136 WEB-017: every password box gets an eye; pressed it shows the text, and it hides again when the form is sent', async () => {
  const context = browser(servers.installer.base);
  const node = eyeNodes();
  const input = {...node('input'), type: 'password', inputMode: 'numeric', id: 'pc-code', before() {}};
  context.document.createElement = tag => node(tag);
  context.__root = {querySelectorAll: selector => selector.includes(':not([data-eye])') && input.dataset.eye ? [] : [input]};
  vm.runInContext('eyes(__root)', context);
  const wrap = input.parentElement, button = wrap.button;
  assert.equal(wrap.className, 'pc-secret');
  assert.equal(button.attrs['aria-label'], 'Show the code');
  assert.equal(button.attrs['aria-pressed'], 'false');
  button.onclick();
  assert.equal(input.type, 'text');
  assert.equal(button.attrs['aria-label'], 'Hide the code');
  assert.equal(button.attrs['aria-pressed'], 'true');
  button.onclick();
  assert.equal(input.type, 'password', 'pressed again: hidden');
  button.onclick();
  context.__form = {querySelectorAll: () => [input]};
  vm.runInContext('hideSecrets(__form)', context);
  assert.equal(input.type, 'password', 'sent: hidden again, so the browser still sees a password box');
  assert.equal(button.attrs['aria-pressed'], 'false');
  vm.runInContext('eyes(__root)', context); // a second pass leaves the box alone
  assert.equal(input.parentElement, wrap);
  // Drawn pages get theirs: the page, a dialog, the sign-in; and every form hides them when sent
  const source = read('panel.js');
  assert.match(source, /eyes\(\$\('#main'\)\)/);
  assert.match(source, /eyes\(\$\('#dialog-content'\)\)/);
  assert.match(source, /addEventListener\('submit', event => hideSecrets/);
});

test('BL-136 WEB-017: there are no Repeat boxes; Your password has one new box', async () => {
  const source = read('panel.js') + read('panel-pages.js');
  assert.ok(!/Repeat the (new )?(code|password)|New password again|pw-again|repeat_code|web_repeat/.test(source), 'a Repeat box is left');
  const context = browser(servers.standard.base);
  await vm.runInContext('refreshStatus()', context);
  const html = vm.runInContext('state.password_change_required = false; PAGES.password.render().html', context);
  assert.equal((html.match(/type="password"/g) || []).length, 2, 'the current password and one new one');
  assert.match(html, /id="pw-new"/);
  const installer = await personContext(servers.installer);
  const fresh = (await installer.page('new')).html;
  assert.ok(!/Repeat/.test(fresh));
  assert.match(fresh, /type="password" inputmode="numeric"/, 'the code is a password box, so it gets the eye');
});

// ---- Zigbee (docs/features/20 devices.md §14, ZB-025; the pages BL-145 draws; WEB-026) -------------
const ZB_C = readFileSync(path.join(__dirname, '../src/web_zigbee.c'), 'utf8');
// The words web_zigbee.c answers: a request's results (web_zigbee_result_word) and an operation's (RESULT[]).
const zbCWords = {
  requests: [...ZB_C.slice(ZB_C.indexOf('web_zigbee_result_word(ui_zb_req_result_t r)'), ZB_C.indexOf('#if CONFIG_ALARM_ZSRC_ZIGBEE'))
    .matchAll(/return "([a-z_]+)"/g)].map(m => m[1]),
  results: [...(/RESULT\[\] = \{([^}]*)\}/.exec(ZB_C)[1]).matchAll(/"([a-z_]+)"/g)].map(m => m[1]),
};
async function withZb(who, extra, run) {
  const s = await start(who, {MOCK_FAST: '1', ...extra});
  try { await run({...api(s.base), base: s.base}); } finally { s.child.kill(); }
}
const zbPost = (a, body) => a.post('zigbee', body).then(async r => [r.status, await r.json()]);

// ZB-025: the page and its API are the installer's alone; a device is read by its registry id.
test('Zigbee: the installer reads the radio, the devices and a device’s page; nobody else (ZB-025)', async () => {
  for (const who of ['master', 'partition', 'standard']) {
    assert.equal((await fetch(servers[who].base + '/api/v1/zigbee')).status, 403, `the ${who} reads the Zigbee page`);
    assert.equal((await api(servers[who].base).post('zigbee', {op: 'add'})).status, 403, `the ${who} adds a Zigbee device`);
  }
  await withZb('installer', {}, async a => {
    const z = await a.get('zigbee');
    assert.equal(z.available, true);
    assert.equal(z.may_change, true);
    assert.equal(z.radio.state, 'working');
    assert.match(z.radio.ieee, /^[0-9A-F]{16}$/, 'addresses travel as 16 hex digits');
    assert.deepEqual(z.radio.survey.map(c => c.channel), [11, 15, 20, 25]);
    assert.ok(z.devices.length >= 8 && z.devices.every(d => d.id > 0 && Array.isArray(d.zones)));
    assert.ok(z.codes.every(c => /^[0-9A-F]{16}$/.test(c.ieee) && !('code' in c)), 'a waiting install code is never sent back');
    assert.equal(z.progress.op, 'none');
    const siren = await a.get('zigbee?device=6');
    assert.equal(siren.device.siren.fit, 'available');
    assert.ok(siren.device.siren.supervision_min === 0 || siren.device.siren.supervision_min >= 60);
    for (const bad of ['99', '0', 'abc'])
      assert.equal((await fetch(a.base + '/api/v1/zigbee?device=' + bad)).status, 404, `device ${bad} found`);
  });
  await withZb('installer', {MOCK_ZIGBEE: '0'}, async a => {
    assert.deepEqual(await a.get('zigbee'), {available: false});
    assert.equal((await a.get('status')).capabilities.zigbee, false);
  });
});

// ZB-025: the requests map onto the zigbee service's (devices.md §6) and answer its result words.
test('Zigbee: adding, removing and the codes answer the panel’s result words (ZB-025, ZB-015)', async () => {
  await withZb('installer', {}, async a => {
    assert.deepEqual(await zbPost(a, {op: 'bogus'}), [200, {result: 'invalid'}]);
    assert.deepEqual(await zbPost(a, {op: 'remove', id: 0}), [200, {result: 'not_found'}], 'id 0 names no device');
    assert.deepEqual(await zbPost(a, {op: 'code_add', ieee: '02AB CD00 0000 0003', code: '12'}), [200, {result: 'bad_code'}]);
    assert.deepEqual(await zbPost(a, {op: 'code_add', ieee: 'nope', code: '83FED3407A939723A5C639B26916D505C3B5'}), [200, {result: 'bad_address'}]);
    assert.deepEqual(await zbPost(a, {op: 'code_add', ieee: '02ABCD0000000003', code: '83FED3407A939723A5C639B26916D505C3B6'}), [200, {result: 'checksum'}]);
    assert.deepEqual(await zbPost(a, {op: 'code_add', ieee: '02ABCD0000000003', code: '83FE D340 7A93 9723 A5C6 39B2 6916 D505 C3B5'}), [200, {result: 'ok'}]);
    assert.ok((await a.get('zigbee')).codes.some(c => c.ieee === '02ABCD0000000003'));
    assert.deepEqual(await zbPost(a, {op: 'siren', id: 6, siren: {partitions: 1, burglary: true, fire: true, strobe: true, squawk: false, volume: 1,
      supervision_min: 30}}), [200, {result: 'invalid'}], 'a siren’s supervision is never off, nor shorter than an hour');
    assert.deepEqual(await zbPost(a, {op: 'add'}), [200, {result: 'ok'}]);
    assert.deepEqual(await zbPost(a, {op: 'add'}), [200, {result: 'busy'}]);
    await until(async () => (await a.get('zigbee')).progress.joined.some(j => j.result === 'added'), 'nothing joined');
    assert.deepEqual(await zbPost(a, {op: 'stop'}), [200, {result: 'ok'}]);
    assert.equal((await a.get('zigbee')).progress.result, 'stopped');
    assert.deepEqual(await zbPost(a, {op: 'dismiss'}), [200, {result: 'ok'}]);
    assert.deepEqual(await zbPost(a, {op: 'remove', id: 2}), [200, {result: 'ok'}]);
    const removed = await until(async () => { const g = (await a.get('zigbee')).progress; return g.step === 'done' && g; }, 'never removed');
    assert.deepEqual([removed.op, removed.result, removed.zones], ['remove', 'removed', [4]]);
    await zbPost(a, {op: 'dismiss'});
  });
  // A partition armed: the page is for looking only, and the panel refuses every change (409 locked).
  await withZb('installer', {MOCK_LOCKED: '1'}, async a => {
    const z = await a.get('zigbee');
    assert.deepEqual([z.may_change, z.why], [false, 'armed']);
    assert.equal((await a.post('zigbee', {op: 'add'})).status, 409);
  });
});

// ZB-017, devices.md §14: a network reset asks for the installer's password again, never a keypad code.
test('Zigbee: the network reset re-asks the installer’s password, never a code (ZB-017, ZB-025)', async () => {
  await withZb('installer', {}, async a => {
    const confirm = recent => fetch(a.base + '/api/v1/mock/confirmation', {method: 'POST',
      headers: {'Content-Type': 'application/json', 'X-Panel-CSRF': 'auto'}, body: JSON.stringify({recent})});
    await confirm(false);
    assert.deepEqual(await zbPost(a, {op: 'reset'}), [403, {error: 'password_required'}]);
    assert.deepEqual(await zbPost(a, {op: 'reset', installer_code: '69420'}), [403, {error: 'password_required'}], 'a keypad code is not a password');
    assert.deepEqual(await zbPost(a, {op: 'reset', me_password: 'correct horse battery'}), [403, {error: 'wrong_password'}]);
    assert.deepEqual(await zbPost(a, {op: 'reset', me_password: 'installer password'}), [200, {result: 'ok'}]);
    await until(async () => (await a.get('zigbee')).devices.length === 0, 'the network was not reset');
    assert.ok((await a.get('zigbee')).codes.length > 0, 'install codes waiting are kept');
  });
  // The firmware: web_zigbee.c never fills installer_code, and panel_web.c checks the password before the request.
  assert.ok(!/installer_code/.test(ZB_C.slice(ZB_C.indexOf('static const char *read_request'))), 'web_zigbee.c reads a code for the reset');
  const PW = readFileSync(path.join(__dirname, '../src/panel_web.c'), 'utf8');
  const branch = PW.slice(PW.indexOf('if (!strcmp(path, "zigbee")) { /* the installer\'s, not while armed'));
  assert.ok(branch.indexOf('recent_password') < branch.indexOf('web_zigbee_post'), 'the password is checked after the request went on');
  assert.ok(!read('panel-pages.js').includes('installer_code'), 'the browser sends a keypad code');
});

// ZB-025: every result the panel can answer has its own words on the page, the C side and the page in step.
test('Zigbee: every request result and every operation result has words (ZB-025)', async () => {
  assert.deepEqual(zbCWords.requests, ['ok', 'not_allowed', 'busy', 'no_radio', 'invalid', 'unsupported', 'bad_code', 'checksum',
    'bad_address', 'full', 'not_found', 'in_use', 'wrong_code', 'locked_out', 'invalid'], 'ui_zb_req_result_t, in its order, then the default');
  assert.equal(zbCWords.results.length, 16, 'ui_zb_result_t has 16 values');
  await withPage('installer', {}, async context => {
    await vm.runInContext('refreshStatus()', context);
    for (const code of new Set(zbCWords.requests.filter(c => c !== 'ok'))) {
      context.__code = code;
      const words = vm.runInContext('zgRefusal(__code)', context);
      assert.ok(words !== code && words.length > 10 && !words.includes('{'), `no words for ${code}`);
    }
    for (const result of zbCWords.results.filter(r => r !== 'none')) {
      context.__g = {op: 'remove', result, zones: [4]};
      const [look, title] = Array.from(vm.runInContext('zgResultWords(__g)', context));
      assert.ok(title && title !== 'Finished', `no words for ${result}`);
      assert.ok(['good', 'warn', 'info'].includes(look), `no look for ${result}`);
    }
    // A siren's supervision has no "Off" (it is never off): the default, or an hour and more.
    const choices = Array.from(vm.runInContext('zgSupervision(0)', context));
    assert.ok(choices.every(([v, w]) => (v === 0 || v >= 60) && w !== 'Off'));
    assert.ok(Array.from(vm.runInContext('zgSupervision(90)', context)).some(([v]) => v === 90), 'a stored value outside the list is kept');
  });
});

// BL-145: the pages as drawn, in the radio's states; Zigbee sits right after Z-Wave.
test('Zigbee: the pages render in each radio state, and the device page says what each role does (BL-145, ZB-025)', async () => {
  for (const radio of ['working', 'paused', 'socket_zwave', 'not_connected']) {
    await withPage('installer', {MOCK_ZIGBEE_RADIO: radio}, async (context, html) => {
      await vm.runInContext('refreshStatus()', context);
      for (const [id, arg] of [['zigbee', ''], ['zigbee-add', ''], ['zigbee-code', ''], ['zigbee-radio', ''], ['zigbee-channel', ''],
        ['zigbee-device', '1'], ['zigbee-device', '4'], ['zigbee-device', '5'], ['zigbee-device', '6'], ['zigbee-device', '7'], ['zigbee-device', '8'], ['zigbee-device', '99']]) {
        const out = await html(id, arg);
        const words = out.replace(/<[^>]*>/g, ' ');
        for (const bad of ['undefined', 'NaN', '[object Object]']) assert.ok(!words.includes(bad), `${radio}: ${id}/${arg} shows "${bad}"`);
        assert.ok(out.includes('<h1>'), `${id}/${arg} has a heading`);
      }
      const main = await html('zigbee');
      if (radio === 'paused') assert.match(main, /data-zb="retry"/);
      if (radio !== 'working') assert.match(main, /data-zb="add" disabled/, `${radio}: adding offered without a radio`);
    });
  }
  await withPage('installer', {}, async (context, html) => {
    await vm.runInContext('refreshStatus()', context);
    assert.match(await html('zigbee-device', '6'), /Chirp when arming and disarming/);
    assert.match(await html('zigbee-device', '7'), /as device 17/);
    assert.match(await html('zigbee-device', '8'), /Belongs to/);
    assert.match(await html('zigbee-device', '99'), /isn’t in the network any more/);
    assert.match(await html('zigbee'), /Not heard for 2 h 30 min/);
  });
});

// The review of the Zigbee pages: what the pages offer and say matches what the panel does.
test('Zigbee: no silent panic on a keypad, fob owners by authority, and words that match the panel (ZB-024, ZB-012, ZB-005, ZB-016)', async () => {
  await withPage('installer', {MOCK_ZIGBEE_RADIO: 'paused'}, async (context, html) => {
    await vm.runInContext('refreshStatus()', context);
    // ZB-024: zbz_ace raises only the audible panic from a keypad's Panic button: a silent choice
    // would be a button that raises nothing.
    const keypad = Array.from(vm.runInContext('zgRoleSpecs({keypad: {}}, [])[1]', context));
    const mask = keypad.find(spec => spec.name === 'panic_mask');
    assert.deepEqual(Array.from(mask.options, ([value]) => value), [1, 4, 8]);
    assert.ok(!Array.from(mask.options, ([, words]) => words).some(w => /silent/i.test(w)), 'a keypad offers a silent panic');
    // ZB-024 (alarm-devices.md §3): a fob belongs to a master, partition master, standard, arm-only
    // or guest user - never the installer, a duress code or an empty slot.
    context.__users = [{number: 1, name: 'Installer', authority: 1}, {number: 2, name: 'Alex', authority: 2},
      {number: 3, name: 'Pat', authority: 3}, {number: 4, name: 'Sam', authority: 4}, {number: 5, name: 'Arm only', authority: 5},
      {number: 6, name: 'Guest', authority: 6}, {number: 7, name: 'Duress', authority: 7}, {number: 8, name: '', authority: 0}];
    const fob = Array.from(vm.runInContext('zgRoleSpecs({fob: {}}, __users)[1]', context));
    assert.deepEqual(Array.from(fob.find(spec => spec.name === 'user').options, ([value]) => value), [0, 2, 3, 4, 5, 6]);
    // ZB-005: a radio that doesn't answer leaves its zones as they were (the hub raises sensor_radio
    // after the link's grace); the page never says they show CHECK.
    const main = (await html('zigbee')).replace(/<[^>]*>/g, ' ');
    assert.ok(!/their zones show CHECK/.test(main), 'the paused radio says its zones show CHECK');
    assert.match(main, /Their zones keep their last state; a Sensor radio problem follows if it lasts/);
    // ZB-012: a stopped setup never goes on by itself: only Set it up again restarts it.
    context.__g = {op: 'add', result: 'setup_not_finished', zones: []};
    const [, , ending] = Array.from(vm.runInContext('zgResultWords(__g)', context));
    assert.match(ending, /set it up again/);
    assert.ok(!/next wakes/.test(ending), 'a stopped setup is said to finish by itself');
    // ZB-016: the channel's noise in the touchscreen's words and thresholds (-75 and -85 dBm).
    const noise = dbm => { context.__dbm = dbm; return vm.runInContext('zgNoiseWords(__dbm)', context); };
    assert.deepEqual([-60, -75, -76, -85, -86, -88, -97].map(noise), ['Busy', 'Busy', 'Some noise', 'Some noise', 'Quiet', 'Quiet', 'Quiet']);
    assert.equal(noise(undefined), 'Not checked yet');
    // ZB-027: the model table's wake words read as a sentence of their own in English (a capital
    // and a full stop), and never as an English clause in another language: without their
    // translation the page's own sentence.
    context.__d = {wake_words: 'press its button once'};
    assert.equal(vm.runInContext('zgWakeWords(__d)', context), 'Press its button once.');
    context.__d = {wake_words: 'Hold its button for 5 seconds.'};
    assert.equal(vm.runInContext('zgWakeWords(__d)', context), 'Hold its button for 5 seconds.');
    context.__d = {wake_words: 'press its button once'};
    vm.runInContext('words.entries = {}', context);
    try {
      assert.equal(vm.runInContext('zgWakeWords(__d)', context), 'Press its button to wake it.');
    } finally {
      vm.runInContext('words.entries = null', context);
    }
  });
});

// docs/features/36 screens.md §4 (ZW-006, ZW-029, ZW-005): nothing joined says the device may belong to another
// system and offers Remove; the questions show the time left; a radio restart says so and offers Try again
test('Z-Wave: the add cards name the likely cause, the time left and a radio restart', async () => {
  const context = browser(servers.installer.base);
  const card = g => {
    context.__g = g;
    return vm.runInContext('zwOpCard(__g, []).html', context);
  };
  const none = card({op: 'add', step: 'done', result: 'nothing_joined'});
  assert.match(none, /If it was used with another system, remove it first/);
  assert.match(none, /data-zwop="remove"[^>]*>Remove a device/);
  const allow = card({op: 'add', step: 'ask_allow', requested: 6, node: 9, seconds_left: 125});
  assert.match(allow, /id="zw-left">2:05</);
  assert.doesNotMatch(allow, /a few seconds/);
  assert.match(card({op: 'add', step: 'ask_pin', dsk: 'xxxxx-40311', seconds_left: 59}), /id="zw-left">0:59</);
  const restarted = card({op: 'add', step: 'done', result: 'radio_restarted'});
  assert.match(restarted, /The Z-Wave radio restarted, so it stopped listening\. Try again\./);
  assert.match(restarted, /data-zwop="add"[^>]*>Try again/);
  assert.match(card({op: 'remove', step: 'done', result: 'radio_restarted'}), /data-zwop="remove"[^>]*>Try again/);
});

// docs/features/36 screens.md §3 (ZW-024, BL-152): the route rebuild names the devices to wake while it runs, and
// the devices that failed or were skipped when it ends
test('Z-Wave: the route rebuild card names the devices waiting, failed and skipped', async () => {
  const context = browser(servers.installer.base);
  const card = (g, devices = [{node: 4, model: 'Front Door'}, {node: 6, model: 'Garage tilt'}, {node: 9, model: 'Shed'}]) => {
    context.__g = g;
    context.__devices = devices;
    return vm.runInContext('zwOpCard(__g, __devices).html', context);
  };
  const running = card({op: 'routes', step: 'working', routes_done: 2, routes_total: 5, routes_waiting: 2,
    routes_list: [{node: 4, what: 'waiting'}, {node: 6, what: 'waiting'}]});
  assert.match(running, /2 of 5 done/);
  assert.match(running, /Wake these: Front Door, Garage tilt/);
  const done = card({op: 'routes', step: 'done', result: 'done', routes_done: 5, routes_total: 5, routes_failed: 1, routes_skipped: 1,
    routes_list: [{node: 4, what: 'failed'}, {node: 9, what: 'skipped'}]});
  assert.match(done, /Routes rebuilt for 3 of 5/);
  assert.match(done, /Didn’t answer: Front Door/);
  assert.match(done, /Skipped \(dead, Long Range or stopped\): Shed/);
  assert.match(done, /status-card warn/);
  assert.match(card({op: 'routes', step: 'done', result: 'done', routes_done: 3, routes_total: 3}), /status-card good/);
});

// ---- Household > Automations (docs/features/41 DESIGN.md section 10, BL-168) ------------------------------------
// The page functions in the stand-in browser, as personContext: go(), show(), toast() and the dialog are recorded.
async function auContext(server) {
  const context = browser(server.base);
  context.__went = []; context.__toasts = []; context.__dialogs = []; context.__shows = 0;
  const original = context.document.querySelector;
  context.document.querySelector = selector => selector === '#me-password' ? null : original(selector);
  vm.runInContext(`go = (id, arg = '') => { __went.push([id, arg]); }; toast = m => { __toasts.push(m); };
    openDialog = html => { __dialogs.push(html); }; closeDialog = () => {}; show = async () => { __shows++; };`, context);
  await vm.runInContext('refreshStatus()', context);
  const run = code => vm.runInContext(code, context);
  const page = async arg => { context.__arg = arg; const out = await run('PAGES.automations.render(__arg)'); return typeof out === 'string' ? {html: out} : out; };
  const edit = (f, value, extra = {}) => { context.__ev = {target: {value, dataset: {f, ...extra}}}; run('auInput(__ev)'); };
  const click = async act => { context.__act = act; await run('auClick({target: {closest: () => ({disabled: false, dataset: {au: __act}})}})'); };
  const submit = () => context.document.querySelector('#review-form').onsubmit({preventDefault() {}, submitter: {}});
  const posts = () => context.__posts.filter(([url]) => url.endsWith('/automations')).map(([, body]) => body);
  const words = html => html.replace(/<[^>]*>/g, ' ').replace(/\s+/g, ' ');
  return {context, run, page, edit, click, submit, posts, words};
}
const auGet = async (server, p = 'automations') => (await fetch(server.base + '/api/v1/' + p)).json();
const auPost = (server, body) => fetch(server.base + '/api/v1/automations', {method: 'POST',
  headers: {'Content-Type': 'application/json', 'X-Panel-CSRF': 'auto'}, body: JSON.stringify(body)});

test('AUT-001 WEB-026: the list says each automation in the touchscreen\'s sentences, with its state and tags', async () => {
  const a = await auContext(servers.master);
  const view = await a.page('');
  const text = a.words(view.html);
  assert.deepEqual([...view.html.matchAll(/href="#\/automations\/(\d+)"/g)].map(m => Number(m[1])), [1, 2, 3, 4], 'slots 1..4, in order');
  for (const words of ['When Front door opens', 'Turn on Porch Light for 10 minutes', 'When the panel is armed Away', 'Lock Front Door · Run Good Night',
    'When a fire or CO alarm sounds', 'Turn off Hallway', 'At 11:00 PM on weekdays', 'Close Garage Door', 'Front Door didn’t answer', '4 of 16 automations'])
    assert.ok(text.includes(words), `the list says "${words}"`);
  assert.match(text, /Ran at \d+:42 PM/, 'the state line of the porch light');
  assert.match(view.html, /<span class="tag ">Off<\/span>/, 'the garage door one is off');
  assert.match(view.html, /<span class="tag warn">Didn’t work<\/span>/, 'a problem is amber');
  assert.match(view.html, /data-go="automations" data-arg="new"(?! disabled)/, 'Add is there');
  // Ideas show only when their parts exist: the mock has a door, a light, a lock, a leak sensor and a valve.
  const ideas = a.words((await a.page('new')).html);
  for (const idea of ['Turn on a light when a door opens', 'Lock up when you arm Away', 'Turn on lights when an alarm sounds', 'Close the water when a leak is found', 'Start from scratch'])
    assert.ok(ideas.includes(idea), idea);
});

test('AUT-001: the empty, full, clock and newer lists say so; a newer record offers only Delete (AUT-008)', async () => {
  for (const [mode, expect] of [['empty', /The panel can do things by itself/], ['full', /16 is the most\. Delete one to add another\./],
    ['clock', /The clock isn’t set, so times wait until it is\./], ['readonly', /Automations can’t be changed right now\./]]) {
    await withServer('master', {MOCK_AUTOMATIONS: mode}, async server => {
      const a = await auContext(server);
      const html = (await a.page('')).html;
      assert.match(a.words(html), expect, mode);
      if (mode === 'full' || mode === 'readonly') assert.match(html, /data-arg="new" disabled/, `${mode}: Add is off`);
    });
  }
  await withServer('master', {MOCK_AUTOMATIONS: 'newer'}, async server => {
    const a = await auContext(server);
    assert.match(a.words((await a.page('')).html), /Automation 5 Made by newer software/);
    const page = (await a.page('5')).html;
    assert.match(a.words(page), /Made by newer software: it can only be deleted\./);
    assert.ok(page.includes('data-au="delete"') && !page.includes('data-f="kind"'), 'only Delete');
  });
});

test('AUT-016 D24: Add from scratch, an unlock after a door asks "Save this automation?" with its sentence, then it is stored', async () => {
  await withServer('master', {}, async server => {
    const a = await auContext(server);
    await a.page('new/scratch');
    a.edit('kind', 'zone_opened');
    a.edit('dev', 'ha:lock.front_door', {i: '0'});
    a.edit('act', 'unlock', {i: '0'});
    assert.equal(a.run('au.now.when.zone'), 1, 'the first usable sensor');
    await a.click('save');
    assert.deepEqual(a.posts(), [], 'nothing is sent before the answer');
    assert.equal(a.context.__dialogs.length, 1);
    assert.match(a.context.__dialogs[0], /Save this automation\?/);
    assert.match(a.context.__dialogs[0], /Anyone who opens Front door will unlock Front Door, even when the alarm is on\./);
    await a.submit();
    const [sent] = a.posts();
    assert.deepEqual([sent.action, sent.slot, sent.gen, sent.automation.when.kind, sent.automation.then[0].action, sent.automation.then[0].confirmed],
      ['save', 0, 1, 'zone_opened', 'unlock', true]);
    assert.equal(JSON.stringify(a.context.__went.at(-1)), '["automations",""]');
    const list = await auGet(server);
    assert.equal(list.used, 5);
    assert.deepEqual(list.automations.at(-1).then.map(x => [x.ref, x.action, x.confirmed]), [['lock.front_door', 'unlock', true]]);
    assert.equal(a.context.__toasts.length, 0, 'no toast: the list shows it');
  });
});

test('AUT-002 D8 D9: a scene or a switch turned on asks "Use it" once when picked; the server refuses one unconfirmed', async () => {
  await withServer('master', {}, async server => {
    const a = await auContext(server);
    await a.page('new/scratch');
    a.edit('kind', 'disarmed');
    a.edit('dev', 'ha:switch.christmas', {i: '0'});
    assert.match(a.context.__dialogs.at(-1), /Use Christmas Lights in an automation\?/);
    assert.match(a.context.__dialogs.at(-1), /never for a switch that opens a door, a gate or a garage/);
    assert.equal(a.run('au.now.then[0].ref'), '', 'the pick waits for the answer');
    await a.submit();
    assert.equal(a.run('JSON.stringify([au.now.then[0].ref, au.now.then[0].action, au.now.then[0].confirmed])'), '["switch.christmas","on",true]');
    const refused = await auPost(server, {action: 'save', slot: 0, gen: 1, automation: {when: {kind: 'disarmed'},
      then: [{kind: 'device', source: 'ha', ref: 'scene.good_night', action: 'run'}]}});
    assert.equal(refused.status, 409);
    assert.deepEqual(await refused.json(), {error: 'action_not_allowed', index: 0}, 'never not_allowed: that is the access word');
  });
});

test('AUT-016: a needs_confirm refusal opens the confirm with that action\'s sentence, and sends it again confirmed', async () => {
  await withServer('master', {}, async server => {
    const a = await auContext(server);
    await a.page('new/scratch');
    a.edit('kind', 'zone_opened');
    a.edit('dev', 'ha:cover.garage_door', {i: '0'});
    a.edit('act', 'stop', {i: '0'});
    a.run("au.devices.find(d => d.ref === 'cover.garage_door').kind = 'gate'"); // the page doesn't know a stop is less secure here
    await a.click('save');
    assert.equal(a.posts().length, 1, 'sent unconfirmed');
    assert.match(a.context.__dialogs.at(-1), /Save this automation\?/);
    assert.match(a.context.__dialogs.at(-1), /Anyone who opens Front door will stop Garage Door, even when the alarm is on\./);
    await a.submit();
    assert.equal(a.posts().at(-1).automation.then[0].confirmed, true);
    assert.equal((await auGet(server)).used, 5);
  });
});

test('WEB-026: a stale gen is refused as changed; the page reads the panel again and says so', async () => {
  await withServer('master', {}, async server => {
    const a = await auContext(server);
    await a.page('1');
    assert.equal((await auPost(server, {action: 'switch', slot: 3, gen: 1, on: false})).status, 200, 'someone else switches one off');
    a.edit('off', '15', {i: '0'});
    await a.click('save');
    assert.deepEqual((await (await auPost(server, {action: 'save', slot: 1, gen: 1, automation: {when: {kind: 'fire'}, then: [{kind: 'device', source: 'ha', ref: 'light.porch', action: 'off'}]}})).json()),
      {error: 'changed'});
    assert.equal(a.context.__shows, 1, 'reloaded');
    assert.equal(a.run('au.said'), 'Someone changed automations meanwhile. The page now shows them as they are.');
    assert.equal((await auGet(server)).automations[0].then[0].off_after, 10, 'nothing was saved');
  });
});

test('AUT-015 D25: with the installer option off, Unlock, Open and Stop aren\'t offered, the page says why, and a save is refused', async () => {
  await withServer('installer', {}, async server => {
    const a = await auContext(server);
    // The installer sees the switch in System options > Arming, after "Integrations may arm without a code", and saves it.
    const options = (await a.run('PAGES.options.render()')).html;
    assert.match(options, /Integrations may arm without a code[\s\S]*Allow less secure automations[\s\S]*Automations may disarm, unlock and open/);
    assert.match(options, /name="auto_less_secure"/);
    await a.run('loadAlarm().then(() => applyAlarm({options: {auto_less_secure: false}}))');
    assert.equal((await auGet(server, 'alarm')).options.auto_less_secure, false);
    const devices = (await auGet(server, 'automations/devices')).devices;
    assert.deepEqual(devices.find(d => d.ref === 'lock.front_door').actions.map(x => x.action), ['lock']);
    assert.deepEqual(devices.find(d => d.ref === 'cover.garage_door').actions.map(x => x.action), ['close']);
    assert.ok(!devices.some(d => d.kind === 'script'), 'a script is never offered (AUT-002)');
    await a.page('new/scratch');
    a.edit('kind', 'disarmed');
    a.edit('dev', 'ha:lock.front_door', {i: '0'});
    const body = a.run('auBody()');
    assert.ok(!/value="unlock"/.test(body), 'Unlock isn\'t offered');
    assert.match(a.words(body), /Your installer has turned off automations that unlock, open or disarm\./);
    const refused = await auPost(server, {action: 'save', slot: 0, gen: 1, automation: {when: {kind: 'disarmed'},
      then: [{kind: 'device', source: 'ha', ref: 'lock.front_door', action: 'unlock'}]}});
    assert.deepEqual(await refused.json(), {error: 'less_secure_off', index: 0});
    assert.equal(a.run("auMessage({code: 'less_secure_off'})"), 'Your installer has turned off automations that unlock, open or disarm.');
  });
});

test('AUT-015: the option and the page are only on a build with automations; a master never sees System options', async () => {
  const master = await auContext(servers.master);
  assert.equal(master.run('pageAllowed(PAGES.options)'), false);
  await withServer('installer', {MOCK_AUTOMATIONS: '0'}, async server => {
    const a = await auContext(server);
    assert.equal(a.run('pageAllowed(PAGES.automations)'), false, 'no Automations entry');
    const options = (await a.run('PAGES.options.render()')).html;
    assert.ok(!options.includes('auto_less_secure') && !options.includes('Allow less secure automations'), 'no option row, not even under Other');
    assert.equal((await fetch(server.base + '/api/v1/automations')).status, 404);
  });
});

test('AUT-001 WEB-026: a partition master and a standard user don\'t see the page, and the panel refuses them', async () => {
  for (const who of ['partition', 'standard']) {
    const a = await auContext(servers[who]);
    assert.equal(a.run('pageAllowed(PAGES.automations)'), false, `${who}: no menu entry`);
    assert.match(a.words((await a.page('')).html), /Only the system master or the installer changes automations\./, `${who}: by hand`);
    for (const p of ['automations', 'automations/devices']) assert.equal((await fetch(servers[who].base + '/api/v1/' + p)).status, 403, `${who} reads ${p}`);
    assert.equal((await auPost(servers[who], {action: 'delete', slot: 1, gen: 1})).status, 403, `${who} deletes one`);
  }
});

test('AUT-008: Delete asks first, then deletes by slot; the list follows', async () => {
  await withServer('master', {}, async server => {
    const a = await auContext(server);
    await a.page('4');
    await a.click('delete');
    assert.match(a.context.__dialogs.at(-1), /Delete this automation\?[\s\S]*It stops at once\. This can’t be undone\./);
    assert.deepEqual(a.posts(), [], 'nothing is sent before the answer');
    await a.submit();
    assert.deepEqual(a.posts().map(b => [b.action, b.slot, b.gen]), [['delete', 4, 1]]);
    assert.equal(JSON.stringify(a.context.__went.at(-1)), '["automations",""]');
    assert.deepEqual((await auGet(server)).automations.map(x => x.slot), [1, 2, 3]);
  });
});

test('AUT-009: the switch acts at once, Try it now puts its outcome in Last run without a toast', async () => {
  await withServer('master', {}, async server => {
    const a = await auContext(server);
    await a.page('4');
    assert.match(a.words(a.run('auLastHtml()')), /Off: it won’t run/);
    a.context.__ev = {target: {checked: true, dataset: {f: 'on'}}};
    await a.run('auInput(__ev)');
    assert.deepEqual(a.posts().map(b => [b.action, b.slot, b.on]).at(-1), ['switch', 4, true]);
    assert.equal((await auGet(server)).automations[3].on, true);
    await a.click('try');
    assert.deepEqual(a.posts().at(-1), {action: 'try', slot: 4, gen: 2});
    assert.match(a.words(a.run('auLastHtml()')), /Didn’t work at .*: Garage Door wasn’t reachable/);
    assert.equal(a.context.__toasts.length, 0);
  });
});

test('AUT-010: armed, the cards open read-only and a change is refused as locked, in words', async () => {
  await withServer('master', {MOCK_LOCKED: '1'}, async server => {
    const a = await auContext(server);
    const list = (await a.page('')).html;
    assert.match(list, /data-arg="new" disabled/);
    assert.match(a.words(list), /Automations keep running while the panel is armed\./);
    const page = (await a.page('1')).html;
    assert.match(page, /<fieldset class="au-set" disabled>/);
    assert.ok(!page.includes('data-au="delete"') && !page.includes('data-au="save"'), 'no Save, Undo or Delete');
    assert.deepEqual(await (await auPost(server, {action: 'switch', slot: 1, gen: 1, on: false})).json(), {error: 'locked'});
    assert.equal(a.run("auMessage({code: 'locked'})"), 'Armed: automations can be changed once every partition is disarmed and quiet.');
  });
});

test('WEB-026: every refusal of section 10 has words, and the mock answers them as web_automations.c does', async () => {
  const a = await auContext(servers.master);
  for (const code of ['changed', 'full', 'action_not_allowed', 'same_device', 'less_secure_off', 'incomplete', 'bad_zone', 'missing_device',
    'storage_full', 'newer', 'read_only', 'locked', 'busy', 'failed', 'bad_request']) {
    a.context.__code = code;
    const words = a.run("auMessage({code: __code, message: __code})");
    assert.ok(words !== code && /^[A-Z0-9]/.test(words) && /\.$/.test(words), `${code}: "${words}"`);
  }
  await withServer('master', {}, async server => {
    const save = async (then, when = {kind: 'disarmed'}, extra = {}) => {
      const r = await auPost(server, {action: 'save', slot: 0, gen: 1, automation: {when, then}, ...extra});
      return [r.status, await r.json()];
    };
    const dev = (ref, action, extra = {}) => ({kind: 'device', source: 'ha', ref, action, ...extra});
    assert.deepEqual(await save([dev('light.porch', 'on')], {kind: 'disarmed'}, {gen: undefined}), [400, {error: 'bad_request'}], 'no gen');
    assert.deepEqual(await save([dev('light.porch', 'on'), dev('light.porch', 'off')]), [409, {error: 'same_device', index: 1}]);
    assert.deepEqual(await save([dev('light.porch', 'on')], {kind: 'zone_opened', zone: 9}), [409, {error: 'bad_zone'}], 'a smoke detector');
    assert.deepEqual(await save([dev('light.garden', 'on')]), [409, {error: 'missing_device', index: 0}]);
    assert.deepEqual(await save([dev('light.porch', 'lock')]), [409, {error: 'action_not_allowed', index: 0}]);
    assert.deepEqual(await save([dev('light.porch', 'on')], {kind: 'time', minute: 60, days: []}), [409, {error: 'incomplete'}], 'no days');
    assert.deepEqual(await save([dev('lock.front_door', 'unlock')], {kind: 'alarm'}), [409, {error: 'needs_confirm', index: 0}]);
    // AUT-016, D31 (review finding 3): auto_action_needs_trigger_confirm asks it after an armed trigger too
    assert.deepEqual(await save([dev('lock.front_door', 'unlock')], {kind: 'armed'}), [409, {error: 'needs_confirm', index: 0}], 'an armed trigger asks the confirm');
    assert.deepEqual(await save([dev('lock.front_door', 'unlock', {confirmed: true})], {kind: 'armed'}), [200, {result: 'saved', gen: 2, slot: 5}]);
    assert.deepEqual(await save([dev('x'.repeat(96), 'on')]), [400, {error: 'bad_request'}], 'a ref past 95 bytes is never cut');
  });
  await withServer('master', {MOCK_AUTOMATIONS: 'full'}, async server => {
    const r = await auPost(server, {action: 'save', slot: 0, gen: 1, automation: {when: {kind: 'fire'}, then: [{kind: 'device', source: 'ha', ref: 'light.porch', action: 'on'}]}});
    assert.deepEqual(await r.json(), {error: 'full'});
  });
});

test('UI-023: the state lines are read again every 2 s at most, and written only when their words changed', async () => {
  await withServer('master', {}, async server => {
    const a = await auContext(server);
    await a.page('1');
    assert.ok(a.run('AU_POLL_MS') >= 1000, 'at most once a second');
    let writes = 0, html = a.run('auLastHtml()');
    const box = a.context.document.querySelector('#au-last');
    Object.defineProperty(box, 'innerHTML', {get: () => html, set: v => { writes++; html = v; }, configurable: true});
    await a.run('auRefresh()');
    assert.equal(writes, 0, 'nothing new: nothing written');
    await auPost(server, {action: 'try', slot: 1, gen: 1});
    await a.run('auRefresh()');
    assert.equal(writes, 1, 'tried: one write');
    assert.match(a.words(html), /Tried at/);
    await a.run('auRefresh()');
    assert.equal(writes, 1);
  });
});

// ---- Arming and disarming by automation (docs/features/41 phase C, BL-170 C6; drawn in docs/features/07 mockup screens/automations.js) ----------

test('AUT-011 D30: GET automations sends an arm and a disarm with who set them up; a save reads back the same', async () => {
  await withServer('master', {MOCK_AUTOMATIONS: 'arm'}, async server => {
    const list = await auGet(server);
    assert.deepEqual(list.automations[4].then, [{kind: 'arm', partition: 1, level: 'night', warn_min: 5, bypass: false, made_by: 'Alex'}]);
    assert.deepEqual(list.automations[9].then.map(x => x.kind), ['disarm', 'device'], 'a Disarm, then one device action');
    assert.deepEqual(list.automations.map(x => x.record), Array(10).fill('ok'), 'never "newer" any more');
    assert.deepEqual(list.warnings, []);
    // made_by is never read: the panel makes whoever saves it the maker (AUT-011)
    const r = await auPost(server, {action: 'save', slot: 0, gen: 1, automation: {when: {kind: 'disarmed'},
      then: [{kind: 'arm', partition: 1, level: 'away', warn_min: 15, bypass: true, made_by: 'Sam'}]}});
    assert.deepEqual(await r.json(), {result: 'saved', gen: 2, slot: 11});
    assert.deepEqual((await auGet(server)).automations[10].then, [{kind: 'arm', partition: 1, level: 'away', warn_min: 15, bypass: true, made_by: 'Alex'}]);
  });
});

test('AUT-011 AUT-015 AUT-016 D29 D31: the server refuses what the validator refuses, in the page\'s codes', async () => {
  await withServer('installer', {}, async server => {
    const save = async (then, when = {kind: 'time', minute: 600, days: ['mon']}) => {
      const r = await auPost(server, {action: 'save', slot: 0, gen: 1, automation: {when, then}});
      return [r.status, await r.json()];
    };
    const arm = (extra = {}) => ({kind: 'arm', partition: 1, level: 'night', warn_min: 5, ...extra});
    const disarm = (extra = {}) => ({kind: 'disarm', partition: 1, ...extra});
    const light = {kind: 'device', source: 'ha', ref: 'light.porch', action: 'on'};
    assert.deepEqual(await save([arm(), light]), [409, {error: 'arm_alone'}], 'arming is the only action');
    assert.deepEqual(await save([arm({warn_min: 0})]), [409, {error: 'incomplete'}], 'Warn first 1-15');
    assert.deepEqual(await save([arm({warn_min: 16})]), [409, {error: 'incomplete'}]);
    assert.deepEqual(await save([arm({level: 'party'})]), [400, {error: 'bad_request'}]);
    assert.deepEqual(await save([arm({partition: 2})]), [409, {error: 'incomplete'}], 'a partition not in use');
    assert.deepEqual(await save([arm({partition: 9})]), [400, {error: 'bad_request'}]);
    assert.deepEqual(await save([disarm({bypass: true})]), [400, {error: 'bad_request'}], 'Bypass is an arm\'s');
    assert.deepEqual(await save([disarm(), disarm()]), [409, {error: 'same_device', index: 1}], 'one alarm action');
    assert.deepEqual(await save([disarm()], {kind: 'armed'}), [409, {error: 'undoes_arming', index: 0}], 'D31: any partition');
    assert.deepEqual(await save([disarm()], {kind: 'armed', partitions: [1]}), [409, {error: 'undoes_arming', index: 0}], 'D31: its own');
    assert.deepEqual(await save([disarm()], {kind: 'zone_opened', zone: 1}), [409, {error: 'needs_confirm', index: 0}], 'D24');
    assert.deepEqual(await save([disarm({confirmed: true})], {kind: 'zone_opened', zone: 1}), [200, {result: 'saved', gen: 2, slot: 5}]);
    assert.deepEqual(await save([disarm(), light]), [409, {error: 'changed'}], 'the old gen');
    const a = await auContext(server);
    await a.run('loadAlarm().then(() => applyAlarm({options: {auto_less_secure: false}}))');
    const off = async then => { const r = await auPost(server, {action: 'save', slot: 0, gen: 2, automation: {when: {kind: 'time', minute: 600, days: ['mon']}, then}}); return [r.status, await r.json()]; };
    assert.deepEqual(await off([disarm()]), [409, {error: 'less_secure_off', index: 0}], 'D25');
    assert.deepEqual(await off([arm({bypass: true})]), [409, {error: 'less_secure_off', index: 0}], 'D31: Bypass open zones');
    assert.deepEqual(await off([arm()]), [200, {result: 'saved', gen: 3, slot: 6}], 'an arm without bypassing');
    for (const code of ['needs_confirm', 'undoes_arming', 'arm_alone']) {
      a.context.__code = code;
      assert.ok(/^[A-Z].*\.$/.test(a.run('auMessage({code: __code, message: __code})')), `${code} has words`);
    }
  });
});

test('AUT-011 AUT-012 AUT-014 D29 D31: Then\'s first group "The alarm"; the arm rows, its result line and notes', async () => {
  await withServer('master', {}, async server => {
    const a = await auContext(server);
    await a.page('new/scratch');
    a.edit('kind', 'zone_closed');
    const group = () => a.run('auDeviceSelect(au.now.then[0], 0)');
    assert.match(group(), /<optgroup label="The alarm"><option value="alarm:arm">Arm the panel<\/option><option value="alarm:disarm">Disarm<\/option><\/optgroup><optgroup/,
      'first, before the devices, after any trigger (D23)');
    a.edit('kind', 'alarm');
    assert.ok(group().includes('Arm the panel') && !group().includes('alarm:disarm'), 'no Disarm after an alarm (D22)');
    a.edit('kind', 'armed');
    assert.ok(!group().includes('alarm:disarm'), 'nor where it could only undo the arming (D31)');
    a.edit('kind', 'time');
    a.edit('dev', 'alarm:arm', {i: '0', k: 'dev-0'});
    const body = a.words(a.run('auBody()'));
    for (const words of ['Arm Night', 'Set up by Alex · warns 5 minutes before', 'Arm how?', 'Warn first', 'Bypass open zones',
      'Off: it won’t arm while a door or window is open',
      'Fire, CO and 24-hour zones are never bypassed. It does nothing if the panel is already armed. Anyone can put it off with their code.',
      'Warns at 10:55 PM, arms Night at 11:00 PM.'])
      assert.ok(body.includes(words), `"${words}" in ${body}`);
    assert.ok(!body.includes('Add a second action') && !body.includes('Try it now') && !body.includes('Inside sensors'), 'the only action; no Try it now');
    assert.ok(!body.includes('Which partition?'), 'one partition: no partition row');
    a.edit('arm-level', 'away', {i: '0', k: 'arm-level-0'});
    a.edit('arm-warn', '1', {i: '0', k: 'arm-warn-0'});
    a.context.__ev = {target: {checked: true, value: 'on', dataset: {f: 'arm-bypass', i: '0', k: 'arm-bypass-0'}}};
    a.run('auInput(__ev)');
    const away = a.words(a.run('auBody()'));
    assert.ok(away.includes('Inside sensors will be on.') && away.includes('Arms anyway; each one is watched again once it closes') &&
      away.includes('Set up by Alex · warns 1 minute before') && away.includes('Warns at 10:59 PM, arms Away at 11:00 PM.'), away);
    await a.click('save');
    assert.deepEqual(a.posts().at(-1).automation.then, [{kind: 'arm', partition: 1, level: 'away', warn_min: 1, bypass: true}]);
    assert.deepEqual(a.context.__dialogs, [], 'arming asks no confirm');
    // A door trigger: "Warns for N minutes, then arms ..."
    await a.page('new/scratch');
    a.edit('kind', 'zone_opened');
    a.edit('dev', 'alarm:arm', {i: '0', k: 'dev-0'});
    assert.match(a.words(a.run('auBody()')), /Warns for 5 minutes, then arms Night\./);
  });
  await withServer('master', {}, async server => { // the option off (D31): no Bypass row, no Disarm, the arm's own note
    const a = await auContext(server);
    await a.page('new/arm-night');
    a.run('au.data.less_secure = false; au.now.then[0].bypass = true');
    const body = a.words(a.run('auBody()'));
    assert.ok(!body.includes('Bypass open zones') && !a.run('auDeviceSelect(au.now.then[0], 0)').includes('alarm:disarm'));
    assert.match(body, /It arms only if every door and window is closed and nobody armed it already\. Anyone can put it off with their code\./);
    assert.equal(a.run('auWire(au.now).then[0].bypass'), false, 'never sent while it is off');
  });
});

test('AUT-011 AUT-016 D24 D31: a Disarm is first with at most one device action; its rows; its confirm after a door', async () => {
  await withServer('master', {MOCK_PARTITIONS: '2'}, async server => {
    const a = await auContext(server);
    await a.page('new/scratch');
    a.edit('kind', 'armed');
    a.edit('part', '1');
    assert.ok(a.run('auDeviceSelect(au.now.then[0], 0)').includes('alarm:disarm'), 'after Home armed, the Garage may be disarmed');
    a.edit('dev', 'alarm:disarm', {i: '0', k: 'dev-0'});
    assert.equal(a.run('au.now.then[0].partition'), 2, 'not the partition its trigger arms');
    let body = a.words(a.run('auBody()'));
    assert.ok(body.includes('Disarm Garage') && body.includes('Which partition?') &&
      body.includes('It never disarms while an alarm sounds, and stops if Alex can no longer disarm Garage.') && body.includes('Add a second action'), body);
    a.edit('disarm-part', '1', {i: '0', k: 'disarm-part-0'});
    assert.match(a.words(a.run('auBody()')), /It would undo the arming at once\./);
    await a.click('save');
    assert.equal(a.posts().length, 0, 'refused before it is sent');
    a.edit('kind', 'zone_opened');
    await a.click('add2');
    assert.ok(!a.run('auDeviceSelect(au.now.then[1], 1)').includes('The alarm'), 'the second action is a device');
    a.edit('dev', 'ha:light.porch', {i: '1'});
    await a.click('save');
    assert.match(a.context.__dialogs.at(-1), /Anyone who opens Front door will disarm Home\./);
    await a.submit();
    assert.deepEqual(a.posts().at(-1).automation.then.map(x => [x.kind, x.confirmed]), [['disarm', true], ['device', false]]);
    a.edit('kind', 'alarm');
    assert.deepEqual(a.run('au.now.then.map(x => x.kind)'), ['device'], 'an alarm trigger drops the Disarm (D22)');
  });
});

test('AUT-012 AUT-013 AUT-014 D20 D30: the statuses of arming and disarming automations, on the list and Last run', async () => {
  await withServer('master', {MOCK_AUTOMATIONS: 'arm'}, async server => {
    const a = await auContext(server);
    const list = a.words((await a.page('')).html);
    for (const words of ['At 10:30 PM every day Didn’t arm', 'Arm Night', 'Didn’t arm at 10:30 PM: Kitchen window was open', 'Already armed at 10:30 PM',
      'Put off by Pat until 11:30 PM', 'Didn’t arm at 7:00 AM: no time to warn first', 'Can’t run: Pat can no longer arm this partition',
      'Disarm · Unlock Front Door', 'Ran at 10:00 AM'])
      assert.ok(list.includes(words), `"${words}"`);
    assert.ok(!/Arms as/.test(list), 'D30');
    const page = a.words((await a.page('7')).html);
    assert.ok(page.includes('Put off by Pat until 11:30 PM') && page.includes('Set up by Alex · warns 10 minutes before'), page);
    assert.ok(!page.includes('Try it now'), 'no Try it now for an arm');
    assert.deepEqual(await (await auPost(server, {action: 'try', slot: 5, gen: 1})).json(), {error: 'action_not_allowed', index: 0});
    assert.equal((await auPost(server, {action: 'switch', slot: 5, gen: 1, on: false})).status, 200, 'turned off like any other');
    a.run("au.rec.status = {result: 'put_off', by: 'Pat', choice: 'night', until: null, at: Date.now()}");
    assert.match(a.words(a.run('auLastHtml()')), /Won’t arm tonight · put off by Pat/);
  });
});

test('D19 D20 D27: the Overview\'s warning card above Home\'s unchanged card, then the put-off note; never while armed', async () => {
  await withPage('partition', {MOCK_AUTO_WARN: 'on'}, async (context, html, api) => {
    let page = await html('overview');
    const without = vm.runInContext("(() => { const w = state.automation_warnings; state.automation_warnings = []; const h = overviewBody(); state.automation_warnings = w; return h; })()", context);
    clean(page, 'the warning');
    assert.match(words(page), /Arming Night at \d+:\d\d [AP]M Set by an automation\. Anyone can put it off at the panel, with their code\./, 'Pat sees it too');
    assert.ok(page.includes(without.match(/<section class="card command[\s\S]*?<\/section>/)[0]), 'Home\'s card exactly as without it: Away, Stay, Night and Chime');
    assert.ok(page.indexOf('Arming Night') < page.indexOf('data-arm="away"'), 'above it');
    assert.ok(!/<button[^>]*>[^<]*Put off/.test(page) && !/data-count|<progress/.test(page.split('<section class="card command')[0]), 'no button, nothing counts down');
    assert.equal(vm.runInContext('overviewBody()', context), vm.runInContext('overviewBody()', context), 'the same words while nothing changed');
    await api.post('mock/automation-warning', {state: 'hour'});
    page = await html('overview');
    assert.ok(!page.includes('Arming Night') && /Arms Night at \d+:\d\d [AP]M · put off by Pat/.test(words(page)), 'D20: the card goes, the note stays');
    await api.post('mock/automation-warning', {state: 'night'});
    assert.match(words(await html('overview')), /Won’t arm tonight · put off by Pat/);
    await api.post('mock/automation-warning', {state: 'on'});
    await html('overview');
    const armed = vm.runInContext("Object.assign(state.partitions[0], {state: 'armed', level: 'away'}); overviewBody()", context);
    assert.ok(!armed.includes('Arming Night') && !armed.includes('put off by'), 'never while that partition is armed');
  });
});

test('AUT-016 D31 (review findings 1, 2): an armed trigger\'s confirm in its own sentence, asked again for each refused action', async () => {
  await withServer('master', {}, async server => {
    const a = await auContext(server);
    await a.page('new/scratch');
    a.edit('kind', 'armed');
    a.edit('dev', 'ha:lock.front_door', {i: '0'});
    a.edit('act', 'unlock', {i: '0'});
    await a.click('add2');
    a.edit('dev', 'ha:cover.garage_door', {i: '1'});
    a.edit('act', 'stop', {i: '1'});
    a.run("au.devices.find(d => d.ref === 'cover.garage_door').kind = 'gate'"); // the page doesn't know a stop is less secure here
    await a.click('save');
    assert.match(a.context.__dialogs.at(-1), /Every time the panel is armed, Front Door will unlock\./);
    assert.ok(!/Anyone who opens  will/.test(a.context.__dialogs.at(-1)));
    await a.submit();
    assert.deepEqual(a.posts().at(-1).automation.then.map(x => x.confirmed), [true, false], 'sent; the panel refuses index 1');
    assert.match(a.context.__dialogs.at(-1), /Every time the panel is armed, Garage Door will stop\./, 'asked for that one');
    assert.ok(!a.context.__dialogs.some(d => d.includes('needs_confirm')), 'never the raw code');
    await a.submit();
    assert.deepEqual(a.posts().at(-1).automation.then.map(x => x.confirmed), [true, true]);
    assert.equal((await auGet(server)).used, 5, 'saved');
  });
});

test('UI-023 (review finding 4): Last run is compared with the string last written, never read back from the page', async () => {
  await withServer('master', {}, async server => {
    const a = await auContext(server);
    await a.page('1');
    let writes = 0, html = a.run('auLastHtml()');
    const box = a.context.document.querySelector('#au-last');
    // The browser gives innerHTML back re-serialized (esc()'s &#39; reads back as '): never the string written.
    Object.defineProperty(box, 'innerHTML', {get: () => html.replace(/&#39;/g, "'") + ' ', set: v => { writes++; html = v; }, configurable: true});
    for (let i = 0; i < 3; i++) await a.run('auRefresh()');
    assert.equal(writes, 0, 'nothing new: nothing written');
  });
});

test('WEB-024 (review findings 5, 6): Delete on a broken page binds its own root; a grant can\'t save; one notice while armed', async () => {
  await withServer('master', {MOCK_AUTOMATIONS: 'newer'}, async server => {
    const a = await auContext(server);
    const main = a.context.document.querySelector('#main');
    main.onclick = null;
    const view = await a.page('5');
    view.bind();
    assert.equal(main.onclick, null, '#main keeps no listener after the page is left');
    assert.match(view.html, /<div id="au-root">[\s\S]*data-au="delete"/);
  });
  await withServer('master', {MOCK_GRANT: '1'}, async server => {
    const r = await auPost(server, {action: 'save', slot: 0, gen: 1, automation: {when: {kind: 'disarmed'},
      then: [{kind: 'device', source: 'ha', ref: 'light.porch', action: 'on'}]}});
    assert.equal(r.status, 403);
    assert.deepEqual(await r.json(), {error: 'password_sign_in_required'});
    const a = await auContext(server);
    await a.page('1');
    a.edit('off', '15', {i: '0'});
    await a.click('save');
    assert.equal(a.context.document.querySelector('#au-error').textContent, 'This needs your own password sign-in, not a workstation’s.');
    assert.equal((await auGet(server)).automations[0].then[0].off_after, 10, 'nothing saved');
  });
  await withServer('master', {MOCK_LOCKED: '1'}, async server => {
    const a = await auContext(server);
    const list = (await a.page('')).html, record = (await a.page('1')).html;
    assert.equal((list.match(/class="notice/g) || []).length, 1, 'the list: its own notice only');
    assert.equal((record.match(/class="notice/g) || []).length, 1, 'the record: its own notice only');
    const prepends = a.run("(() => { let n = 0; const m = document.querySelector('#main'), p = m.prepend; m.prepend = () => { n++; }; page = 'automations'; lockedNotice(); m.prepend = p; return n; })()");
    assert.equal(prepends, 0, 'panel.js adds none on Automations');
  });
});

test('AUT-016 D22 D30 D31 (C6w2 review): Disarm after an armed trigger asks; the touchscreen\'s words; maker, partitions and bypass wording', async () => {
  await withServer('master', {MOCK_PARTITIONS: '2'}, async server => {
    const a = await auContext(server);
    // 1, 2: a Disarm of another partition after an armed trigger asks the plain confirm, in the level and partition sentence
    await a.page('new/scratch');
    a.edit('kind', 'armed');
    a.edit('part', '1');
    a.edit('level', 'away');
    a.edit('dev', 'alarm:disarm', {i: '0', k: 'dev-0'});
    assert.equal(a.run('au.now.then[0].partition'), 2);
    await a.click('save');
    assert.deepEqual(a.posts(), [], 'never sent without the confirm');
    assert.match(a.context.__dialogs.at(-1), /Every time Home is armed Away, it will disarm Garage\./);
    await a.submit();
    assert.equal(a.posts().at(-1).automation.then[0].confirmed, true);
    assert.equal(a.run("auConfirmText({kind: 'armed', partitions: [1], levels: ['stay']}, {kind: 'device', action: 'open', device: 'Garage Door'})"),
      'Every time Home is armed Stay, Garage Door will open.');
    assert.equal(a.run("auConfirmText({kind: 'armed', partitions: [], levels: []}, {kind: 'device', action: 'unlock', device: 'Front Door'})"),
      'Every time the panel is armed, Front Door will unlock.');
    assert.equal(a.run("auConfirmText({kind: 'armed', partitions: [], levels: ['night']}, {kind: 'device', action: 'stop', device: 'Gate'})"),
      'Every time the panel is armed Night, Gate will stop.');
    // 3: no "even when the alarm is on" for a Disarm; one partition says "the panel"
    const zone = a.run('au.data.zones.find(z => z.usable_open)');
    assert.equal(a.run("auConfirmText({kind: 'zone_closed', zone: " + zone.number + "}, {kind: 'disarm', partition: 2})"), 'Anyone who closes ' + zone.name + ' will disarm Garage.');
    a.run("au.data.partitions = au.data.partitions.slice(0, 1)");
    assert.equal(a.run("auConfirmText({kind: 'zone_opened', zone: " + zone.number + "}, {kind: 'disarm', partition: 1})"), 'Anyone who opens ' + zone.name + ' will disarm the panel.');
  });
  await withServer('master', {MOCK_PARTITIONS: '2'}, async server => {
    const a = await auContext(server);
    await a.page('new/scratch');
    // 4: an empty maker reads as nothing, except on a new draft
    a.edit('kind', 'time');
    a.edit('dev', 'alarm:arm', {i: '0', k: 'dev-0'});
    assert.match(a.words(a.run('auBody()')), /Set up by Alex · warns 5 minutes before/, 'a new draft: the viewer');
    a.run("au.isNew = false; au.now.then[0].made_by = ''");
    assert.ok(!/Set up by/.test(a.words(a.run('auBody()'))), 'a stored one with no maker says nothing');
    assert.equal(a.run("auActionDetail({kind: 'arm', warn_min: 5, made_by: ''})"), '');
    // 6: the bypass variant
    assert.equal(a.run("auActionDetail({kind: 'arm', warn_min: 2, made_by: 'Pat', bypass: true})"), 'Set up by Pat · warns 2 minutes before · bypasses open zones');
    assert.equal(a.run("auActionDetail({kind: 'arm', warn_min: 1, made_by: 'Pat', bypass: true})"), 'Set up by Pat · warns 1 minute before · bypasses open zones');
    a.run('au.data.less_secure = false');
    assert.equal(a.run("auActionDetail({kind: 'arm', warn_min: 2, made_by: 'Pat', bypass: true})"), 'Set up by Pat · warns 2 minutes before');
  });
  await withServer('master', {MOCK_PARTITIONS: '2'}, async server => { // 5: only what the saver may disarm
    const a = await auContext(server);
    await a.page('new/scratch');
    a.edit('kind', 'zone_opened');
    a.edit('dev', 'alarm:disarm', {i: '0', k: 'dev-0'});
    const offered = () => [...a.run('auBody()').matchAll(/id="au-disarm-part-0"[\s\S]*?<\/select>/g)].flatMap(m => [...m[0].matchAll(/<option value="(\d+)"/g)].map(o => Number(o[1])));
    assert.deepEqual(offered(), [1, 2]);
    a.run('state.partitions = state.partitions.filter(p => p.number === 2)'); // the saver sees (may disarm) only the Garage
    a.run('au.now.then[0].partition = 2');
    assert.deepEqual(offered(), [2], 'the Home is not offered');
    assert.equal(a.run("auNewDisarm({kind: 'zone_opened'}).partition"), 2);
  });
});

// ---- BL-186, BL-187: the update server, the channel, and the installer who never arms -------------------------------
const postTo = (base, p, body) => fetch(base + '/api/v1/' + p, {method: 'POST',
  headers: {'Content-Type': 'application/json', 'X-Panel-CSRF': 'auto'}, body: JSON.stringify(body)});
const updatesOf = async base => (await fetch(base + '/api/v1/updates')).json();
// Asks for a check and waits for it to finish, as the page does.
async function checked(base, domain) {
  assert.equal((await postTo(base, 'updates', {action: 'server_check', domain})).status, 200);
  for (let i = 0; i < 40; i++) {
    const c = (await updatesOf(base)).check;
    if (c?.domain === domain && c.state === 'done') return c;
    await new Promise(r => setTimeout(r, 100));
  }
  throw new Error('the check never finished');
}

// OTA-017: who sees the page and which cards
test('Software updates: the installer has the server and the channel cards, the system master only the status (OTA-017)', async () => {
  await withPage('installer', {}, async (context, html) => {
    await vm.runInContext('refreshStatus()', context);
    const page = await html('updates');
    clean(page, 'updates (installer)');
    for (const w of ['Where updates come from', 'Update server', 'roboalarms.com', 'RoboAlarms release', '3F9A 1C2B 77D0 E415', 'Change the update server…',
      'Update channel', 'Check now'])
      assert.ok(words(page).includes(w), `the installer's page lacks "${w}"`);
    assert.ok(!words(page).includes('Go back to roboalarms.com'), 'roboalarms.com is in use already');
    assert.ok(!page.includes('disabled>') || !/id="fws-change"[^>]*disabled/.test(page), 'Change is held back only for the factory code');
  });
  await withPage('master', {}, async (context, html) => {
    await vm.runInContext('refreshStatus()', context);
    const page = await html('updates');
    clean(page, 'updates (master)');
    assert.ok(words(page).includes('Check now'));
    for (const w of ['Where updates come from', 'Update channel', 'Change the update server'])
      assert.ok(!words(page).includes(w), `the system master sees "${w}"`);
  });
  for (const who of ['partition', 'standard']) {
    const context = browser(servers[who].base);
    await vm.runInContext('refreshStatus()', context);
    assert.equal(vm.runInContext("pageAllowed(PAGES.updates)", context), false, `${who} has Software updates`);
  }
});

// OTA-018, OTA-019, OTA-020: the check, the warning for a key the panel doesn't trust, then the commit
test('update server: a check, then the warning (key, tick, password), then it is kept (OTA-018, OTA-019, OTA-020)', async () => {
  const s = await start('installer');
  try {
    const call = (action, extra = {}) => postTo(s.base, 'updates', {action, ...extra});
    assert.equal((await call('server_check', {domain: 'not a domain'})).status, 400);
    assert.equal((await (await call('server_check', {domain: '??'})).json()).error, 'bad_domain');
    // the typed address is cleaned: a pasted https://example.net/ is the domain example.net
    const c = await checked(s.base, 'example.net');
    assert.equal(c.result, 'ok');
    assert.equal(c.trusted, false, 'a key the panel has not met');
    assert.equal(c.key.fingerprint, '8C41 0E9D 52A7 B3F6');
    assert.equal(c.key.sha256.length, 64);
    // what the installer saw is what is kept
    const stale = await call('server', {domain: 'example.net', key_sha256: 'f'.repeat(64), me_password: 'installer password'});
    assert.equal(stale.status, 409);
    assert.equal((await stale.json()).error, 'stale');
    // a password, since the window has run out (WEB-016); the stale answer above asked for it first and counted it
    await postTo(s.base, 'mock/confirmation', {recent: false});
    const none = await call('server', {domain: 'example.net', key_sha256: c.key.sha256});
    assert.equal((await none.json()).error, 'password_required');
    const wrong = await call('server', {domain: 'example.net', key_sha256: c.key.sha256, me_password: 'nope'});
    assert.equal((await wrong.json()).error, 'wrong_password');
    assert.equal((await call('server', {domain: 'example.net', key_sha256: c.key.sha256, me_password: 'installer password'})).status, 200);
    const after = await updatesOf(s.base);
    assert.equal(after.server.domain, 'example.net');
    assert.equal(after.server.official, false);
    assert.equal(after.check, null, 'the check is used up');
    // going back: the panel trusts RoboAlarms' key, so the answer says so and no warning or password is needed
    const back = await checked(s.base, 'roboalarms.com');
    assert.equal(back.trusted, true);
    assert.equal((await call('server', {domain: 'roboalarms.com', key_sha256: back.key.sha256})).status, 200);
    assert.equal((await updatesOf(s.base)).server.official, true);
    // going back dropped the key accepted for example.net (the panel keeps one, with its server): the warning and the
    // password again
    const again = await checked(s.base, 'example.net');
    assert.equal(again.trusted, false, 'the accepted key went with the server');
    await postTo(s.base, 'mock/confirmation', {recent: false});
    assert.equal((await (await call('server', {domain: 'example.net', key_sha256: again.key.sha256})).json()).error,
      'password_required');
    // the page offers Go back while another server is in use
    assert.equal((await call('server', {domain: 'example.net', key_sha256: again.key.sha256,
      me_password: 'installer password'})).status, 200);
    const context = browser(s.base);
    await vm.runInContext('refreshStatus()', context);
    const out = await vm.runInContext("PAGES.updates.render('')", context);
    assert.ok(words(out.html).includes('Not the RoboAlarms server') && words(out.html).includes('Go back to roboalarms.com'));
  } finally {
    s.child.kill();
  }
});

test('update server: the three refusals, and the factory installer code holds a new key back (OTA-019, OTA-021)', async () => {
  const s = await start('installer', {MOCK_FACTORY_CODE: '1'});
  try {
    for (const [domain, result] of [['example.org', 'not_a_server'], ['example.com', 'no_firmware'], ['nothing-here.example.net', 'unreachable']]) {
      const c = await checked(s.base, domain);
      assert.equal(c.result, result, domain);
      assert.ok(!c.key, `${domain} has no key to show`);
    }
    const c = await checked(s.base, 'example.net');
    const refused = await postTo(s.base, 'updates', {action: 'server', domain: 'example.net', key_sha256: c.key.sha256, me_password: 'installer password'});
    assert.equal(refused.status, 409);
    assert.equal((await refused.json()).error, 'factory_code');
    assert.equal((await updatesOf(s.base)).factory_code, true);
    const context = browser(s.base);
    await vm.runInContext('refreshStatus()', context);
    const out = await vm.runInContext("PAGES.updates.render('')", context);
    assert.ok(words(out.html).includes('Change the installer code first: the factory one is public.'));
    assert.match(out.html, /id="fws-change" disabled/);
    // a key the panel trusts already needs no code change
    const back = await checked(s.base, 'roboalarms.com');
    assert.equal(back.trusted, true);
    assert.equal((await postTo(s.base, 'updates', {action: 'server', domain: 'roboalarms.com', key_sha256: back.key.sha256})).status, 200);
  } finally {
    s.child.kill();
  }
});

// OTA-022: the channel
test('update channel: the installer picks one of four (OTA-022)', async () => {
  const s = await start('installer');
  try {
    assert.equal((await updatesOf(s.base)).channel, 'stable');
    assert.equal((await postTo(s.base, 'updates', {action: 'channel', channel: 'nightly'})).status, 400);
    assert.equal((await postTo(s.base, 'updates', {action: 'channel', channel: 'beta'})).status, 200);
    const d = await updatesOf(s.base);
    assert.equal(d.channel, 'beta');
    assert.equal(d.installed_channel, 'beta');
  } finally {
    s.child.kill();
  }
  await withPage('installer', {}, async (context, html) => {
    await vm.runInContext('refreshStatus()', context);
    const page = await html('updates');
    for (const w of ['Stable', 'Beta', 'Alpha', 'Dev']) assert.ok(page.includes(w), `${w} is not a choice`);
  });
});

// OTA-017: a withdrawn version and an important fix
test('Software updates: a withdrawn version and an important fix are said (OTA-017)', async () => {
  await withPage('master', {MOCK_WITHDRAWN: '1'}, async (context, html, call) => {
    await vm.runInContext('refreshStatus()', context);
    const d = await call.get('updates');
    assert.equal(d.withdrawn, true);
    assert.equal(d.critical, true);
    const page = await html('updates');
    clean(page, 'updates (withdrawn)');
    assert.ok(words(page).includes('Version 0.6.1 was withdrawn'));
    assert.ok(words(page).includes('Version 0.6.2 is available · an important fix'));
  });
});

// USR-001: the installer neither arms nor disarms, and the Overview stops offering it
test('the installer never arms or disarms; the panel refuses it (USR-001)', async () => {
  const post = (who, body) => postTo(servers[who].base, 'partition', body);
  const status = async who => (await fetch(servers[who].base + '/api/v1/status')).json();
  assert.equal((await status('installer')).access.arm, false);
  for (const who of ['master', 'partition', 'standard']) assert.equal((await status(who)).access.arm, true, who);
  const refused = async (r, what) => {
    assert.equal(r.status, 409, what);
    assert.equal((await r.json()).error, 'not_authorized', what);
  };
  await refused(await post('installer', {number: 1, action: 'arm', level: 'away'}), 'arm');
  // disarm and clear, in the states they are for
  for (const [scenario, action] of [['armed', 'disarm'], ['alarm', 'disarm'], ['memory', 'clear']]) {
    const s = await start('installer', {MOCK_SCENARIO: scenario});
    try { await refused(await postTo(s.base, 'partition', {number: 1, action}), `${action} in ${scenario}`); } finally { s.child.kill(); }
  }
  assert.equal((await post('installer', {number: 1, action: 'acknowledge'})).status, 200, 'silencing stays');
});

async function overviewOf(who, env, then) {
  const s = await start(who, env);
  try {
    const context = browser(s.base);
    await vm.runInContext('refreshStatus()', context);
    // the cards are drawn from the status's partitions and the zones; load them as the Overview does
    await vm.runInContext('(async () => { live.zones = (await api("zone-status")).zones; })()', context);
    const cards = vm.runInContext('state.partitions.map(p => commandCard(p))', context);
    return await then(cards.join('\n'), context);
  } finally {
    s.child.kill();
  }
}
test('the installer’s Overview has no arm or disarm, whatever the state (USR-001)', async () => {
  const arming = /data-arm=|data-disarm=|data-clear=/;
  const said = /The installer doesn’t (arm|disarm|clear alarms)/;
  await overviewOf('installer', {}, html => {
    assert.ok(!arming.test(html), 'disarmed: arm buttons offered');
    assert.match(html, /The installer doesn’t arm\./);
    assert.ok(html.includes('data-chime='), 'the chime stays');
  });
  await overviewOf('master', {}, html => {
    assert.ok(/data-arm=/.test(html) && !said.test(html), 'the system master has every button');
  });
  await overviewOf('installer', {MOCK_SCENARIO: 'armed'}, html => {
    assert.ok(!arming.test(html), 'armed: Disarm offered');
    assert.match(html, /The installer doesn’t disarm\./);
  });
  await overviewOf('installer', {MOCK_SCENARIO: 'alarm'}, html => {
    assert.ok(!arming.test(html), 'alarm: Disarm offered');
    assert.match(html, said);
  });
  await overviewOf('master', {MOCK_SCENARIO: 'alarm'}, html => assert.ok(/data-disarm=/.test(html)));
});
test('the installer keeps Silence for a fire, and Add an automation has no arming idea (USR-001)', async () => {
  await overviewOf('installer', {MOCK_SCENARIO: 'fire'}, html => {
    assert.ok(/data-silence=/.test(html), 'Silence is gone');
    assert.ok(!/data-disarm=|data-arm=/.test(html));
    assert.ok(!said(html), 'the fire card keeps its own buttons without the line');
  });
  const ids = async who => {
    const context = browser(servers[who].base);
    await vm.runInContext('refreshStatus()', context);
    return vm.runInContext("auIdeas({zones: []}, []).map(i => i.id)", context);
  };
  assert.ok(!(await ids('installer')).includes('arm-night'));
  assert.ok((await ids('master')).includes('arm-night'));
});
function said(html) { return /The installer doesn’t/.test(html); }

// ---- BL-188: an update's progress, Stop the update, the restart followed (OTA-026..OTA-030) -------------------------
// The Software updates page and the panel-wide watcher in the stand-in browser, against the mock panel's run. The test control
// POST mock/update sets a step by hand ({set, update, elapse, reset}); MOCK_UPDATE_SPEED runs a live one fast.
const sleep = ms => new Promise(r => setTimeout(r, ms));
async function updContext(who, env = {}, then) {
  const server = await start(who, env);
  try {
    const context = browser(server.base);
    const store = new Map();
    context.sessionStorage = {getItem: k => store.has(k) ? store.get(k) : null, setItem: (k, v) => store.set(k, String(v)), removeItem: k => store.delete(k)};
    Object.assign(context, {__dialogs: [], __toasts: [], __password: null});
    const original = context.document.querySelector;
    context.document.querySelector = s => s === '#me-password' ? context.__password : original(s);
    vm.runInContext(`toast = m => { __toasts.push(m); }; openDialog = html => { __dialogs.push(html); }; closeDialog = () => {}; show = async () => {};`, context);
    await vm.runInContext('refreshStatus()', context);
    const u = {server, context, store, el: original,
      run: code => vm.runInContext(code, context),
      set: body => postTo(server.base, 'mock/update', body),
      get: async () => (await fetch(server.base + '/api/v1/updates')).json(),
      status: async () => (await fetch(server.base + '/api/v1/status')).json(),
      post: (body, p = 'updates') => postTo(server.base, p, body),
      page: async () => vm.runInContext("PAGES.updates.render('')", context),
      card: async () => /<section class="card status-card[\s\S]*?<\/section>/.exec((await u.page()).html)[0],
    };
    return await then(u);
  } finally {
    server.child.kill();
  }
}
const OFFERED = {latest: '0.6.2', available: true};

test('Software updates: each stage of an update has its own card, with the steps, the size and Stop (OTA-026, OTA-028)', async () => {
  await updContext('master', {}, async u => {
    const show = async (set, update = OFFERED) => { await u.set({reset: true, set, update}); const card = await u.card(); clean(card, JSON.stringify(set)); return card; };
    const text = card => words(card);
    let card = await show({stage: 'downloading', percent: 42, kind: 'release'});
    for (const w of ['Installed: version 0.6.1', 'Downloading version 0.6.2', '1.1 of 2.7 MB · 42 %', 'Stop the update', 'Check the download', 'Self-test'])
      assert.ok(text(card).includes(w), `downloading lacks "${w}"`);
    assert.match(card, /<progress max="100" value="42"/);
    assert.match(card, /<li class="now" aria-current="step">/);
    assert.ok(card.includes('data-update="stop"'));
    card = await show({stage: 'downloading', percent: -1});
    assert.ok(text(card).includes('Starting the download.') && !/<p id="upd-bytes">[^<]*of/.test(card));
    card = await show({stage: 'verifying', percent: 100});
    assert.ok(text(card).includes('Checking the download') && text(card).includes('Its signature and checksum') && card.includes('data-update="stop"'));
    card = await show({stage: 'waiting', percent: 100});
    assert.ok(text(card).includes('Waiting to install version 0.6.2') && text(card).includes('It installs in a moment.') && card.includes('data-update="stop"'));
    card = await show({stage: 'installing', percent: 100});
    assert.ok(text(card).includes('Installing version 0.6.2') && !card.includes('data-update="stop"'), 'the seconds before the restart: no Stop');
    card = await show({stage: 'stopping', percent: 42});
    assert.ok(text(card).includes('Stopping the update') && text(card).includes('keeps version 0.6.1') && !card.includes('<button'), 'stopping: no buttons');
    card = await show({stage: 'rolling_back'});
    assert.ok(text(card).includes('Going back to version 0.6.1') && !card.includes('data-update="stop"'));
    card = await show({stage: 'wifi_module', kind: 'wifi_module', percent: 42});
    assert.ok(text(card).includes('Updating the Wi-Fi module') && text(card).includes('can’t be stopped') && !card.includes('upd-steps') && !card.includes('data-update="stop"'));
    assert.match(card, /<progress max="100" value="42"/);
    card = await show({stage: 'file', kind: 'file', percent: 10});
    assert.ok(text(card).includes('Installing a file from developer tools') && !card.includes('upd-steps') && !card.includes('data-update="stop"'));
    card = await show({confirming: true, running: '0.6.2'});
    assert.ok(text(card).includes('Self-test of version 0.6.2') && text(card).includes('Usually a minute'), 'the self-test');
    card = await show({}, {latest: '0.6.2', available: true});
    assert.ok(text(card).includes('Version 0.6.2 is available') && card.includes('data-update="install"') && !card.includes('upd-steps'));
    card = await show({check: 'checking'}, {});
    assert.ok(text(card).includes('Checking for updates') && /data-update="check" disabled/.test(card));
    card = await show({check: 'failed', checkReason: 'The update server can’t be reached'}, {});
    assert.ok(text(card).includes('Couldn’t check for updates') && text(card).includes('The update server can’t be reached.'));
    // the bar is the exit time's timebar and never moves by itself. The section is found by its
    // comment, which the shipped (minified) file no longer has: read the source.
    const css = readFileSync(path.join(dir, 'panel.css'), 'utf8'), mine = css.slice(css.indexOf('BL-188'), css.indexOf('@media', css.indexOf('BL-188')));
    assert.ok(mine.length > 200 && !/animation|transition/.test(mine), 'nothing animates');
    // a plain check has no steps and no Stop; the page says each step once, in a hidden status line
    const page = (await u.page()).html;
    assert.match(page, /<p class="upd-hidden" role="status" id="upd-said"><\/p>/);
  });
});

test('Software updates: a held step names what holds it, and the armed notice dims Install (OTA-026, OTA-027)', async () => {
  await updContext('master', {MOCK_SCENARIO: 'armed'}, async u => {
    await u.set({reset: true, set: {stage: 'waiting', percent: 100}, update: OFFERED});
    let card = await u.card();
    for (const w of ['Waiting to install version 0.6.2', 'The system is armed. Disarm everything and the panel restarts to install it.', 'You can stop the update until the panel restarts.'])
      assert.ok(words(card).includes(w), `waiting lacks "${w}"`);
    assert.ok(card.includes('data-update="stop"'), 'Stop works while armed');
    await u.set({reset: true, set: {stage: 'downloading', percent: 42}});
    assert.ok(words(await u.card()).includes('The system is armed. It installs once everything is disarmed; until then you can stop the update.'));
    // idle and armed: Install is dimmed and says when
    await u.set({reset: true, update: OFFERED});
    card = await u.card();
    assert.match(card, /data-update="install" disabled/);
    assert.ok(words(card).includes('You can install it once everything is disarmed.'));
    // the panel itself: cancel passes while armed, install does not
    const refused = await u.post({action: 'install'});
    assert.equal(refused.status, 409);
    assert.equal((await refused.json()).error, 'locked');
    await u.set({set: {stage: 'downloading', percent: 5}});
    assert.equal((await u.post({action: 'cancel'})).status, 200, 'a stop is allowed while armed (OTA-027)');
    // the page's armed notice gives way to a card that names what holds it, else says what still works
    const notice = {textContent: 'The panel is armed or an alarm is sounding', removed: false, remove() { this.removed = true; }, querySelector: () => ({textContent: ''})};
    u.context.__notice = notice;
    u.context.document.querySelectorAll = s => s === '#main > .notice' ? [u.context.__notice] : [];
    u.run("updNotice({stage: 'waiting', reason: 'The system is armed'})");
    assert.ok(notice.removed, 'named: the notice gives way');
    const note = {textContent: 'The panel is armed or an alarm is sounding', removed: false, remove() { this.removed = true; }, p: {textContent: ''}};
    note.querySelector = () => note.p;
    u.context.__notice = note;
    u.run("updNotice({stage: 'downloading', reason: ''})");
    assert.ok(!note.removed && note.p.textContent === 'Other changes wait until everything is disarmed and quiet. You can still stop the update.');
  });
});

test('Stop the update: the question keeps updating by default; stopping, then stopped by Alex (OTA-027)', async () => {
  await updContext('master', {}, async u => {
    await u.set({reset: true, set: {stage: 'downloading', percent: 42, kind: 'release'}, update: OFFERED});
    u.context.__d = await u.get();
    u.context.__after = 0;
    u.run('updStopDialog(__d, async () => { __after++; })');
    const dialog = u.context.__dialogs.at(-1);
    assert.ok(words(dialog).includes('Stop the update? The panel keeps version 0.6.1. You can install 0.6.2 again later; it downloads it again.'));
    assert.ok(dialog.indexOf('Keep updating') < dialog.indexOf('Stop it') && /id="dialog-cancel" autofocus/.test(dialog), 'Keep updating comes first and has the focus');
    await u.el('#upd-stop').onclick();
    assert.equal(u.context.__after, 1);
    let d = await u.get();
    assert.equal(d.stage, 'stopping');
    assert.equal(d.phase, 2);
    assert.equal((await u.post({action: 'cancel'})).status, 200, 'again while stopping');
    await u.set({elapse: 3500});
    d = await u.get();
    assert.equal(d.stage, '');
    assert.equal(d.last.result, 'stopped');
    assert.equal(d.last.by, 'Alex');
    assert.ok(!('rolled_back' in d), 'rolled_back is gone: last says it');
    const card = await u.card();
    assert.ok(words(card).includes('Update stopped') && /Alex stopped it (at|on) /.test(words(card)) && words(card).includes('Nothing was changed.'));
    assert.ok(card.includes('data-update="install"') && !/data-update="install" disabled/.test(card), 'Install again');
    assert.equal((await u.status()).updating, false);
  });
});

test('Stop the update: a stop that finds nothing says so, and a step that cannot be stopped is not stoppable (OTA-027, OTA-030)', async () => {
  await updContext('master', {}, async u => {
    await u.set({reset: true, update: OFFERED});
    u.context.__d = {...(await u.get()), running: '0.6.1', target: '0.6.2'};
    u.context.__after = 0;
    u.run('updStopDialog(__d, async () => { __after++; })');
    await u.el('#upd-stop').onclick();
    assert.deepEqual(u.context.__toasts, ['It isn’t running any more.']);
    assert.equal(u.context.__after, 1, 'the card shows what happened');
    const none = await u.post({action: 'cancel'});
    assert.equal(none.status, 409);
    assert.equal((await none.json()).error, 'no_update');
    for (const stage of ['wifi_module', 'file']) {
      await u.set({reset: true, set: {stage, percent: 3}});
      const refused = await u.post({action: 'cancel'});
      assert.equal(refused.status, 409, stage);
      assert.equal((await refused.json()).error, 'not_stoppable', stage);
    }
    assert.equal(u.run("friendly('not_stoppable')"), 'This step can’t be stopped. It finishes in a moment.');
  });
});

test('Software updates: updated, rolled back and failed come from `last` (OTA-026)', async () => {
  await updContext('master', {}, async u => {
    const at = Math.floor(Date.now() / 1000);
    const show = async last => { await u.set({reset: true, set: {last}, update: OFFERED}); return u.card(); };
    let card = await show({result: 'updated', version: '0.6.2', from: '0.6.1', at});
    assert.ok(words(card).includes('Updated to version 0.6.2') && /It passed its self-test at /.test(words(card)) && !card.includes('data-update="install"'));
    card = await show({result: 'rolled_back', version: '0.6.2', from: '0.6.1', at});
    assert.ok(words(card).includes('Version 0.6.2 failed its self-test') && words(card).includes('The panel went back to version 0.6.1 by itself.') && card.includes('data-update="install"'));
    const page = (await u.page()).html;
    assert.ok(!words(page).includes('was rolled back'), 'the old notice is gone');
    card = await show({result: 'failed', version: '0.6.2', from: '0.6.1', reason: 'The download broke off', at});
    assert.ok(words(card).includes('Update failed') && words(card).includes('The download broke off. Nothing was changed.'));
    assert.equal((await u.get()).failed, 'install');
  });
});

test('Software updates: the server and the channel are held while an update runs (OTA-026, OTA-028)', async () => {
  await updContext('installer', {}, async u => {
    await u.set({reset: true, set: {stage: 'downloading', percent: 42}, update: OFFERED});
    const html = (await u.page()).html;
    assert.ok(words(html).includes('Not while an update runs.'));
    assert.match(html, /id="fws-change" disabled/);
    assert.match(html, /<fieldset class="fws-hold" disabled>/);
    assert.ok(words(html).includes('Stop the update'), 'the installer has the same card');
    for (const [body, error] of [[{action: 'server_check', domain: 'example.net'}, 'update_running'], [{action: 'channel', channel: 'beta'}, 'update_running'],
      [{action: 'server', domain: 'roboalarms.com', key_sha256: '3f9a1c2b77d0e415' + '5c0e'.repeat(12)}, 'update_running'], [{action: 'check'}, 'busy'], [{action: 'install'}, 'busy']]) {
      const r = await u.post(body);
      assert.equal(r.status, 409, body.action);
      assert.equal((await r.json()).error, error, body.action);
    }
    assert.equal((await u.status()).updating, true);
    await u.set({reset: true});
    const idle = (await u.page()).html;
    assert.ok(!/id="fws-change" disabled/.test(idle) && !/<fieldset class="fws-hold"/.test(idle) && !words(idle).includes('Not while an update runs.'));
    for (const [code, want] of [['busy', 'Not while an update runs.'], ['update_running', 'Not while an update runs.'], ['no_update', 'It isn’t running any more.'],
      ['stale', 'The panel’s check of that server is out of date. Check it again.'], ['bad_domain', 'Type a domain, like roboalarms.com.'],
      ['factory_code', 'Change the installer code first: the factory one is public.']])
      assert.equal(u.run(`friendly('${code}')`), want, code);
  });
});

test('the server change answers as the firmware: password, digest, a run, then the check (OTA-019, OTA-028)', async () => {
  await updContext('installer', {}, async u => {
    const c = await checked(u.server.base, 'example.net');
    await u.post({recent: false}, 'mock/confirmation');
    const call = body => u.post({action: 'server', domain: 'example.net', ...body});
    const code = async r => (await r.json()).error;
    assert.equal(await code(await call({key_sha256: 'abc'})), 'password_required', 'the password first, for a key the panel does not trust');
    assert.equal(await code(await call({key_sha256: 'abc', me_password: 'installer password'})), 'bad_request', 'then the digest');
    await u.set({set: {stage: 'verifying', percent: 100}});
    assert.equal(await code(await call({key_sha256: c.key.sha256, me_password: 'installer password'})), 'update_running', 'then a run');
    await u.set({reset: true});
    assert.equal(await code(await call({key_sha256: 'f'.repeat(64), me_password: 'installer password'})), 'stale');
    assert.equal((await call({key_sha256: c.key.sha256, me_password: 'installer password'})).status, 200);
    // a trusted key needs no password, even when the window has run out
    await u.post({recent: false}, 'mock/confirmation');
    const back = await checked(u.server.base, 'roboalarms.com');
    assert.equal(back.trusted, true);
    assert.equal((await u.post({action: 'server', domain: 'roboalarms.com', key_sha256: back.key.sha256})).status, 200);
  });
});

test('update server dialogs, end to end: type, checking, the warning (tick, password), then the review of a trusted key (OTA-018..OTA-020)', async () => {
  await updContext('installer', {}, async u => {
    u.context.setTimeout = fn => setTimeout(fn, 5);
    u.el('#dialog').open = true;
    u.context.__s = (await u.get()).server;
    const submit = (selector, event) => u.el(selector).onsubmit({preventDefault() {}, ...event});
    const dialogs = () => u.context.__dialogs;
    const until = async what => { for (let i = 0; i < 100 && !dialogs().some(what); i++) await sleep(20); assert.ok(dialogs().some(what), 'the dialog never came'); };
    u.run('fwsTypeDialog(__s)');
    assert.ok(words(dialogs().at(-1)).includes('Change the update server'));
    submit('#fws-form', {target: {domain: {value: 'https://Example.net/'}}});
    await until(h => h.includes('id="fws-warn"'));
    const warn = dialogs().find(h => h.includes('id="fws-warn"'));
    assert.ok(dialogs().some(h => words(h).includes('Checking example.net')), 'the checking step');
    for (const w of ['Use example.net for updates?', 'Not the RoboAlarms update server', '8C41 0E9D 52A7 B3F6', 'I trust example.net with this panel’s firmware'])
      assert.ok(words(warn).includes(w), `the warning lacks "${w}"`);
    assert.ok(warn.includes('id="me-password"'), 'the password, since the window has run out');
    u.context.__password = {value: '', focus() {}};
    submit('#fws-warn', {target: {trust: {checked: false}}});
    assert.equal(u.el('#dialog-error').textContent, 'Tick the box to say you trust this server.');
    submit('#fws-warn', {target: {trust: {checked: true}}});
    assert.equal(u.el('#dialog-error').textContent, 'Enter your password.');
    u.context.__password = {value: 'installer password', focus() {}};
    await submit('#fws-warn', {target: {trust: {checked: true}}});
    for (let i = 0; i < 50 && (await u.get()).server.domain !== 'example.net'; i++) await sleep(20);
    assert.equal((await u.get()).server.domain, 'example.net');
    // back to roboalarms.com: its key is trusted now, so the review asks no password even after the window ran out
    u.context.__password = null;
    await u.post({recent: false}, 'mock/confirmation');
    u.context.__s = (await u.get()).server;
    await u.run('fwsRunCheck(__s, "roboalarms.com")');
    const review = dialogs().at(-1);
    assert.ok(words(review).includes('Use roboalarms.com for updates?') && words(review).includes('The panel trusts its signing key already.'));
    assert.ok(!review.includes('me-password'), 'no password for a trusted key');
    const before = dialogs().length;
    await submit('#review-form', {submitter: {disabled: false}});
    assert.equal((await u.get()).server.domain, 'roboalarms.com');
    assert.equal(dialogs().length, before, 'no second dialog: no loop');
  });
});

test('the update server card shows a panel that sends no key, or a key without a name (OTA-017)', async () => {
  for (const [mode, want] of [['none', ['The key this version is signed with']], ['unnamed', ['The key this version is signed with', '3F9A 1C2B 77D0 E415']]]) {
    await updContext('installer', {MOCK_SERVER_KEY: mode}, async u => {
      const html = (await u.page()).html;
      clean(html, `server key ${mode}`);
      for (const w of want) assert.ok(words(html).includes(w), `${mode}: lacks "${w}"`);
    });
  }
  assert.ok(true);
});

test('the page follows an update through its restart: lost, then the sign-in says why (OTA-029)', async () => {
  const kinds = [[{stage: 'installing'}, 'restart'], [{stage: 'verifying'}, 'restart'], [{stage: 'waiting', reason: ''}, 'restart'], [{stage: 'downloading', percent: 100}, 'restart'],
    [{stage: 'waiting', reason: 'The system is armed'}, 'other'], [{stage: 'downloading', percent: 42}, 'other'], [{stage: 'stopping'}, 'other'],
    [{confirming: true}, 'self-test'], [{stage: 'rolling_back'}, 'self-test']];
  await updContext('master', {}, async u => {
    for (const [d, want] of kinds) { u.context.__d = d; assert.equal(u.run('updLostKind(__d)'), want, JSON.stringify(d)); }
    await u.set({reset: true, set: {stage: 'downloading', percent: 42, kind: 'release'}, update: OFFERED});
    const view = await u.page();
    view.bind();
    const tick = () => u.run('updFollow.tick()');
    await tick();
    assert.equal(u.store.get('panel-restart'), undefined, 'one answer answered');
    await u.set({set: {offline: true}});
    await tick();
    assert.equal(u.store.get('panel-restart'), undefined, 'a first miss counts for nothing');
    await sleep(2100);
    await tick();
    const saved = JSON.parse(u.store.get('panel-restart'));
    assert.equal(saved.lost, 'other');
    assert.equal(saved.version, '0.6.2');
    assert.ok(u.el('#connection').innerHTML.includes('Not answering'), 'never "Restarting" for a step that cannot be restarting');
    assert.ok(words(u.el('#upd-card').outerHTML).includes('The panel isn’t answering. The update goes on without this page; this page keeps trying.'));
    assert.ok(!u.el('#upd-card').outerHTML.includes('data-update="stop"'), 'Stop is dropped');
    // it comes back, having restarted: the sign-in says so, as a note, and opens Software updates
    await u.set({set: {offline: false, signedOut: true}});
    for (let i = 0; i < 3; i++) await tick();
    const main = u.el('#main').innerHTML;
    assert.ok(main.includes('The panel restarted during the update to version 0.6.2. Sign in to see where it stands.'), main.slice(0, 300));
    assert.ok(!main.includes('Your session ended'));
    assert.equal(u.context.location.hash, '#/updates');
    assert.ok(u.el('#login-error').classList !== undefined);
  });
  // the restart step: the header says Restarting
  await updContext('master', {}, async u => {
    await u.set({reset: true, set: {stage: 'installing', percent: 100, kind: 'release'}, update: OFFERED});
    (await u.page()).bind();
    await u.set({set: {offline: true}});
    await u.run('updFollow.tick()');
    await sleep(2100);
    await u.run('updFollow.tick()');
    assert.ok(u.el('#connection').innerHTML.includes('Restarting'));
    assert.equal(JSON.parse(u.store.get('panel-restart')).lost, 'restart');
  });
  // no gap before the 401, or an old one: today's "Your session ended"
  for (const saved of [null, {version: '0.6.2', lost: 'restart', at: Date.now() - 11 * 60000}]) {
    await updContext('master', {}, async u => {
      if (saved) u.store.set('panel-restart', JSON.stringify(saved));
      await u.set({set: {signedOut: true}});
      await assert.rejects(u.run("api('updates')"));
      assert.ok(u.el('#main').innerHTML.includes('Your session ended. Sign in again.'));
    });
  }
});

test('Software updates: back from a restart on the old version with no result says "Update didn’t finish" (OTA-029)', async () => {
  await updContext('master', {}, async u => {
    const saved = version => u.store.set('panel-restart', JSON.stringify({version, lost: 'restart', at: Date.now()}));
    saved('0.6.2');
    await u.set({reset: true, update: OFFERED});
    const card = await u.card();
    assert.ok(words(card).includes('Update didn’t finish') && words(card).includes('The panel restarted before installing version 0.6.2. Nothing was changed.'));
    assert.ok(card.includes('data-update="install"'));
    assert.equal(u.store.get('panel-restart'), undefined, 'said once');
    saved('0.6.2');
    await u.set({set: {running: '0.6.2'}});
    assert.ok(!words(await u.card()).includes('didn’t finish'), 'the new version runs');
    saved('0.6.2');
    await u.set({reset: true, set: {last: {result: 'rolled_back', version: '0.6.2', from: '0.6.1', at: 1}}});
    assert.ok(words(await u.card()).includes('failed its self-test'), 'a result says why');
    u.store.set('panel-restart', JSON.stringify({version: '0.6.2', lost: 'restart', at: Date.now() - 11 * 60000}));
    await u.set({reset: true});
    assert.ok(!words(await u.card()).includes('didn’t finish'), 'kept 10 minutes');
  });
});

test('other pages: the status says an update runs, then the header says Restarting and the sign-in says why (OTA-030)', async () => {
  await updContext('standard', {}, async u => {
    assert.equal(u.run('state.updating'), false);
    await u.set({set: {stage: 'downloading', percent: 10}});
    await u.run('refreshStatus()');
    assert.equal(u.run('state.updating'), true);
    u.run("page = 'overview'; watchRestart()");
    await u.run('watching.tick()');
    await u.set({set: {offline: true}});
    await u.run('watching.tick()');
    await sleep(2100);
    await u.run('watching.tick()');
    assert.ok(u.el('#connection').innerHTML.includes('Restarting'));
    assert.equal(JSON.parse(u.store.get('panel-restart')).lost, 'others');
    await u.set({set: {offline: false, signedOut: true}});
    await u.run('watching.tick()');
    assert.ok(u.el('#main').innerHTML.includes('The panel restarted to install an update. Sign in again.'));
    assert.equal(u.context.location.hash, '', 'no Software updates for them');
  });
});

test('a whole run against the mock: download, check, wait, restart, self-test, updated; and a live stop (OTA-026..OTA-028)', async () => {
  await updContext('master', {MOCK_UPDATE_SPEED: '20'}, async u => {
    assert.equal((await u.post({action: 'check'})).status, 200);
    for (let i = 0; i < 50 && !(await u.get()).available; i++) await sleep(20);
    assert.equal((await u.get()).latest, '0.6.2');
    assert.equal((await u.post({action: 'install'})).status, 200);
    const seen = new Set();
    let signedOut = false;
    for (let i = 0; i < 400; i++) {
      let r;
      try { r = await fetch(u.server.base + '/api/v1/updates'); } catch { seen.add('offline'); await sleep(5); continue; }
      if (r.status === 401) {
        signedOut = true;
        const login = await postTo(u.server.base, 'login', {username: 'alex', password: 'correct horse battery'});
        assert.equal(login.status, 200);
        continue;
      }
      const d = await r.json();
      seen.add(d.stage || (d.confirming ? 'self-test' : d.last ? 'done' : ''));
      if (d.last?.result === 'updated') { assert.equal(d.running, '0.6.2'); break; }
      await sleep(5);
    }
    assert.ok(signedOut, 'the restart forgot the sign-in');
    for (const s of ['downloading', 'verifying', 'self-test', 'done']) assert.ok(seen.has(s), `never saw ${s}: ${[...seen]}`);
    assert.equal((await u.status()).firmware, '0.6.2');
  });
  await updContext('master', {MOCK_UPDATE_SPEED: '150'}, async u => {
    await u.set({update: OFFERED});
    assert.equal((await u.post({action: 'install'})).status, 200);
    assert.equal((await u.post({action: 'cancel'})).status, 200);
    for (let i = 0; i < 100 && !(await u.get()).last; i++) await sleep(10);
    const d = await u.get();
    assert.equal(d.last.result, 'stopped');
    assert.equal(d.running, '0.6.1');
  });
});

test('AUD-032: Chime, Voice and Beeps volumes, in that order, each its own setting', async () => {
  await withPage('standard', {}, async (context, html, a) => {
    await vm.runInContext('refreshStatus()', context);
    const page = await html('display');
    const at = name => page.search(new RegExp(`name="${name}"`));
    assert.ok(at('chime_volume') > 0 && at('chime_volume') < at('voice_volume') && at('voice_volume') < at('volume'), 'order');
    const text = words(page);
    for (const phrase of ['Chime volume', 'Voice volume', 'Beeps volume', 'Uses the Voice volume above.'])
      assert.ok(text.includes(phrase), `"${phrase}" missing`);
    assert.ok(!text.includes('Speaker volume') && !text.includes('Uses the volume above.'));
    // they start at the volume, anyone saves them, and a value over 100 is refused
    const first = (await a.get('config/display')).values;
    assert.deepEqual([first.chime_volume, first.voice_volume], [first.volume, first.volume]);
    assert.equal((await a.post('config/display', {values: {chime_volume: 30, voice_volume: 55, volume: 80}})).status, 200);
    const kept = (await a.get('config/display')).values;
    assert.deepEqual([kept.chime_volume, kept.voice_volume, kept.volume], [30, 55, 80]);
    assert.equal((await a.post('config/display', {values: {chime_volume: 101}})).status, 400);
    assert.equal((await a.post('config/display', {values: {voice_volume: 101}})).status, 400);
    assert.equal((await a.get('config/display')).values.chime_volume, 30);
  });
});

test('AUD-032: a firmware without a voice has no Voice volume', async () => {
  await withPage('standard', {MOCK_VOICE: 'absent'}, async (context, html, a) => {
    await vm.runInContext('refreshStatus()', context);
    const page = await html('display');
    assert.match(page, /name="chime_volume"/);
    assert.doesNotMatch(page, /name="voice_volume"/);
    assert.ok(!(await a.get('config/display')).fields.some(f => f.name === 'voice_volume'));
  });
});
