// Builds the Overview mockup (docs/features/38) into one file that opens on its own:
// build/mockups/overview/overview-mockup.html, with the Panel Manager's real stylesheet, brand mark
// and icons inlined from components/services/panel_web/web.
//
//   node docs/features/38-Wording-and-Design/mockup/build.cjs
//
// The mockup is a design sketch with a pretend panel, not the Panel Manager: its data are made up
// (fictional names, RFC 5737 addresses) and it never talks to a panel. The striped bar at its top
// switches the alarm state, the power, the connection and the theme.
const fs = require('fs');
const path = require('path');
const root = path.resolve(__dirname, '../../../..');
const web = path.join(root, 'components/services/panel_web/web');
const css = fs.readFileSync(path.join(web, 'panel.css'), 'utf8');
const light = css.match(/:root \{([\s\S]*?)\n\}/)[1];
const dark = css.match(/@media \(prefers-color-scheme: dark\) \{\s*:root \{([\s\S]*?)\n  \}/)[1];
// The mockup's theme switch wins over the browser's preference either way.
const themed = `${css}\n:root[data-theme="light"] {${light}\n}\n:root[data-theme="dark"] {${dark}\n}\n`;
const brand = fs.readFileSync(path.join(web, 'panel.html'), 'utf8').match(/src="(data:image\/png;base64,[^"]+)"/)[1];
const icons = fs.readFileSync(path.join(web, 'panel.js'), 'utf8').match(/const ICONS = (\{[\s\S]*?\n\});/)[1];
const out = fs.readFileSync(path.join(__dirname, 'overview.template.html'), 'utf8')
  .replace('/*@PANEL_CSS@*/', () => themed)
  .replace('/*@BRAND@*/', () => brand)
  .replace('/*@ICONS@*/', () => icons);
const dest = path.join(root, 'build/mockups/overview/overview-mockup.html');
fs.mkdirSync(path.dirname(dest), {recursive: true});
fs.writeFileSync(dest, out);
console.log('wrote', dest, out.length, 'bytes');
