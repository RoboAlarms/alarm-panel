// Builds the Panel Manager mockup (docs/features/31 mockup/README.md): the real Panel Manager
// (components/services/panel_web/web) against its pretend panel (mock-panel.cjs, unchanged), both running
// inside the page, with the proposals in proposals/ to switch on beside it.
//
//   node docs/features/31-Browser-Panel-Manager/mockup/build.cjs [--out <folder>] [--languages <folder>]
//
// writes <out>/index.html (the viewer: the striped bar, the frames, a proposal's notes) and <out>/app.html
// (the Panel Manager itself, every file inlined), by default build/mockups/manager. Both open from a
// folder, a local web server or any static web host; nothing in them talks to a panel.
// --languages build/i18n/web (after `python tools/i18n.py web`) puts those catalogs in, as MOCK_I18N does.
'use strict';
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const crypto = require('node:crypto');

const root = path.resolve(__dirname, '../../../..');
const web = path.join(root, 'components/services/panel_web/web');
const read = file => fs.readFileSync(file, 'utf8');

// Inlined text must not end its own <script> or <style> early; "<\/" means the same inside JS strings.
const inScript = js => {
  if (js.includes('<!--')) throw new Error('build.cjs: "<!--" in a script would confuse the page; write it as "<" + "!--"');
  return js.replace(/<\/script/gi, '<\\/script');
};
const inStyle = css => css.replace(/<\/style/gi, '<\\/style');
const must = (text, re, what) => { const m = re.exec(text); if (!m) throw new Error(`build.cjs: ${what} not found; the Panel Manager's files changed shape`); return m; };

// ---- the proposals: each file calls proposal({...}) once; the viewer lists them from here ------------------
const PROPOSAL_FIELDS = ['bl', 'title', 'status', 'why'];
const STATUSES = ['draft', 'approved', 'rejected'];
function readProposals() {
  const dir = path.join(__dirname, 'proposals');
  const list = [];
  for (const name of fs.readdirSync(dir).filter(f => f.endsWith('.js') && !f.startsWith('_')).sort()) {
    const source = read(path.join(dir, name)), id = name.replace(/\.js$/, '');
    const found = [];
    try {
      vm.runInNewContext(source, {proposal: spec => found.push(spec)}, {filename: name});
    } catch (error) {
      throw new Error(`proposals/${name}: only proposal({...}) and function declarations may run at load (${error.message})`);
    }
    if (found.length !== 1) throw new Error(`proposals/${name}: calls proposal() ${found.length} times, not once`);
    const spec = found[0];
    for (const f of PROPOSAL_FIELDS) if (!spec[f]) throw new Error(`proposals/${name}: no ${f}`);
    if (!STATUSES.includes(spec.status)) throw new Error(`proposals/${name}: status is ${spec.status}, not one of ${STATUSES.join(', ')}`);
    if (!/^BL-\d{3}$/.test(spec.bl)) throw new Error(`proposals/${name}: bl ${spec.bl} is not a backlog number`);
    list.push({id, source, meta: {id, bl: spec.bl, title: spec.title, status: spec.status, why: spec.why, decided: spec.decided || '',
      look: spec.look || [], replaces: spec.replaces || [], role: spec.role || '', notes: spec.notes || [],
      today: spec.today || {}, builds: spec.builds || []}});
  }
  for (const p of list) for (const b of p.meta.builds)
    if (!list.some(x => x.id === b)) throw new Error(`proposals/${p.id}.js: builds on ${b}, which isn't there`);
  return list;
}

// ---- app.html: panel.html with every file inlined, the pretend panel first ---------------------------------
function buildApp(proposals, catalogs) {
  const html = read(path.join(web, 'panel.html'));
  const css = read(path.join(web, 'panel.css'));
  const light = must(css, /:root \{([\s\S]*?)\n\}/, 'panel.css light tokens')[1];
  const dark = must(css, /@media \(prefers-color-scheme: dark\) \{\s*:root \{([\s\S]*?)\n {2}\}/, 'panel.css dark tokens')[1];
  // The viewer's theme switch wins over the browser's preference either way (as the Overview mockup's).
  const themed = `${css}\n:root[data-theme="light"] {${light}\n}\n:root[data-theme="dark"] {${dark}\n}\n`;
  const core = read(path.join(web, 'panel.js'));
  const startAt = core.lastIndexOf('\nstartup();');
  if (startAt < 0) throw new Error('build.cjs: panel.js no longer ends with startup();');
  const mock = read(path.join(web, 'mock-panel.cjs'));
  // What a proposal or the viewer may reach inside the pretend panel (its own names, unchanged).
  const INTERNALS = ['alarm', 'config', 'users', 'bindings', 'savedInputs', 'live', 'parts', 'zwave', 'events', 'walks', 'marks',
    'addEvent', 'tripZone', 'beginEntry', 'walkMark', 'armedIn', 'zoneCfg', 'zoneLive', 'isLocked', 'disarm',
    'CODES', 'LOGINS', 'webLogins', 'temporary', 'manages', 'loginPossible', 'zigbee', 'zbOperation'];
  for (const name of INTERNALS)
    must(mock, new RegExp(`(?:const|let|function) ${name}\\b`), `mock-panel.cjs's ${name}`);
  const files = catalogs ? `window.MOCKUP_FILES = ${JSON.stringify(catalogs)};\n` : '';
  const scripts = [
    files + read(path.join(__dirname, 'shim.js')),
    `MOCKUP.start(function (require, process, Buffer, module, exports, __dirname, __filename) {\n${mock}\n;return {${INTERNALS.join(', ')}};\n});`,
    read(path.join(web, 'panel-words.js')),
    read(path.join(web, 'panel-keypad.js')),
    read(path.join(web, 'panel-pages.js')),
    core.slice(0, startAt) + '\n',
    ...proposals.map(p => `// ---- proposals/${p.id}.js\n(function (proposal) {\n${p.source}\n})(spec => MOCKUP.proposals.push({...spec, id: ${JSON.stringify(p.id)}}));`),
    read(path.join(__dirname, 'boot.js')),
  ];
  const head = `<style>\n${inStyle(themed)}\n</style>`;
  const body = must(html, /<body>([\s\S]*)<\/body>/, 'panel.html body')[1];
  return `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="color-scheme" content="light dark">
<title>Panel Manager · mockup</title>
<link rel="icon" href="data:,">
${head}
</head>
<body>${body}
${scripts.map(s => `<script>\n${inScript(s)}\n</script>`).join('\n')}
</body>
</html>
`;
}

// ---- index.html: the viewer -------------------------------------------------------------------------------
function buildViewer(proposals, stamp) {
  const css = read(path.join(web, 'panel.css'));
  const light = must(css, /:root \{([\s\S]*?)\n\}/, 'panel.css light tokens')[1];
  const dark = must(css, /@media \(prefers-color-scheme: dark\) \{\s*:root \{([\s\S]*?)\n {2}\}/, 'panel.css dark tokens')[1];
  const tokens = `:root {${light}\n}\n@media (prefers-color-scheme: dark) { :root:not([data-theme="light"]) {${dark}\n} }\n` +
    `:root[data-theme="light"] {${light}\n}\n:root[data-theme="dark"] {${dark}\n}\n`;
  const brand = must(read(path.join(web, 'panel.html')), /src="(data:image\/png;base64,[^"]+)"/, 'the brand mark')[1];
  return read(path.join(__dirname, 'viewer.html'))
    .replace('/*@TOKENS@*/', () => inStyle(tokens))
    .replace('/*@BRAND@*/', () => brand)
    .replace('/*@PROPOSALS@*/', () => inScript(JSON.stringify(proposals.map(p => p.meta), null, 1)))
    .replace('/*@BUILD@*/', () => JSON.stringify(stamp));
}

function readCatalogs(folder) {
  if (!folder) return null;
  const dir = path.resolve(root, folder), files = {};
  for (const name of fs.readdirSync(dir).filter(f => f.endsWith('.json'))) files['/i18n/' + name] = read(path.join(dir, name));
  return files;
}

function build({out = 'build/mockups/manager', languages} = {}) {
  out = path.resolve(root, out);
  const proposals = readProposals();
  fs.mkdirSync(out, {recursive: true});
  const app = buildApp(proposals, readCatalogs(languages));
  // app.html's address carries a stamp of its contents, so a browser never shows a copy from before a rebuild.
  const viewer = buildViewer(proposals, crypto.createHash('sha1').update(app).digest('hex').slice(0, 10));
  fs.writeFileSync(path.join(out, 'app.html'), app);
  fs.writeFileSync(path.join(out, 'index.html'), viewer);
  console.log(`wrote ${path.relative(root, out)}/index.html (${viewer.length} bytes) and app.html (${app.length} bytes); ` +
    `${proposals.length} proposal${proposals.length === 1 ? '' : 's'}${proposals.length ? ': ' + proposals.map(p => p.id).join(', ') : ''}`);
  return {out, proposals: proposals.map(p => p.meta)};
}

if (require.main === module) {
  const args = process.argv.slice(2);
  const option = name => { const i = args.indexOf(name); return i >= 0 ? args[i + 1] : undefined; };
  try { build({out: option('--out'), languages: option('--languages')}); } catch (error) { console.error(error.message); process.exit(1); }
}
module.exports = {build, readProposals};
