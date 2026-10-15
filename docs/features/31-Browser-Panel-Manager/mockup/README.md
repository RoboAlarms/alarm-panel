# The Panel Manager's mockup

The whole Panel Manager as it is today, runnable anywhere, and the place where every change to it is
drawn and agreed **before** anyone builds it (WEB-026). It is not a drawing of the Panel Manager: it
*is* the Panel Manager - `panel.html`, `panel.css`, `panel.js`, `panel-pages.js`, `panel-words.js` and
`panel-keypad.js` from `components/services/panel_web/web`, unchanged - with its pretend panel
(`mock-panel.cjs`, also unchanged) running inside the page instead of on a PC. So today's mockup is
complete by construction and can't drift from the code, and nobody draws a mockup per feature any more
(the Overview's in docs/features/38 and the walk test's in docs/features/40 were drawn by hand, page by
page; this replaces that way of working).

A change is a **proposal**: a small file that changes pages the way `panel-pages.js` does, switched on
beside today's in the viewer. Approved, it is built by moving that code into `panel-pages.js`.

## Why this way (research, 2026-10-02)
The Panel Manager is plain HTML and JavaScript with no build step, and it already had a pretend panel
for the PC (`mock-panel.cjs`) that answers `/api/v1` with the firmware's shapes, schemas and access
table. The ways to mock it up, from what front-end teams do:

| Way | Kept? | Why |
|---|---|---|
| Draw each page again in HTML, as the Overview and walk test mockups did | No | About 3,300 lines of pages to copy, and they drift the day after; a mockup per feature is what this replaces |
| A component catalogue (Storybook and the like) | No | Needs npm and a bundler the project doesn't have; the pages are whole flows built from strings, not components, and a reviewer needs the flow (the zone, then its sensor, then the review), not parts |
| Screenshots of the mock, in a gallery | No | Still pictures: no review of a flow, and they need a PC to make |
| `node mock-panel.cjs` on a PC | As the developer's tool | Needs Node and a PC, can't be opened on a phone, and each state is its own start with its own `MOCK_*` settings |
| **The real files with the pretend panel in the page**, "API mocking in the browser" | **Yes** | The approach Mock Service Worker made common: requests are answered inside the browser, so a static copy works with no back end. Here `fetch()` and the remote keypad's WebSocket are answered by `mock-panel.cjs` itself, so the mock written for the tests is the one the mockup uses. It opens from a folder, a local server or a static web host on a phone |

A Service Worker (what MSW itself uses) needs a real origin and a registered script; a file opened
from a folder, or a page shown in a sandboxed frame, has neither, so `shim.js` replaces `fetch`
instead.
Sources: [MSW](https://mswjs.io/docs/), [WireMock on prototyping vs mocking](https://www.wiremock.io/glossary/what-is-api-prototyping-prototyping-vs-mocking),
[Storybook](https://storybook.js.org/docs).

## The rule
Anything that changes what a person sees or does in the Panel Manager - a new page, section, field
or button, a moved one, a changed flow, new or changed words - starts as a proposal here and is built
only once the person you work for has approved it. Fixing the Panel Manager back to what it should
do (a bug) needs no proposal. The touchscreen has the same rule and its own mockup
(docs/features/07-UI-LVGL-Pro/mockup, UI-044).

## Working on a change
1. **See today's page:** build (below), open the viewer, pick the page, who is signed in and the
   panel's state. Everything works: arming, a sensor opening (Sensors), a review with a code.
2. **Draw the proposal.** Copy `proposals/_template.js` to `proposals/BL-NNN-short-name.js` (the
   backlog row it belongs to; a row may have several). Write the changed pages as `panel-pages.js`
   would, using its helpers (`pageHead`, `fieldRows`, `review` ...) and `panel.css`'s tokens; new CSS
   goes in the proposal's `css`. Say where to look (`look`), what it replaces and why; a page only
   the proposal has names the page Today shows beside it (`today: {'garage': 'zones'}`); one that
   builds on another proposal names it (`builds: ['BL-NNN-no-codes']`) and is shown with it on. Show every
   state the change has (empty, full, an error, while armed), with `look` entries that set the role
   and the scenario.
3. **Build and test:** `node docs/features/31-Browser-Panel-Manager/mockup/build.cjs`, then
   `node --test components/services/panel_web/test/manager_mockup.test.cjs` (every proposal's pages
   render for its role with no "undefined" or "NaN").
4. **Show it to the person you work for**, with the viewer's link (it keeps the proposal, the page
   and the switches in its address) and a sentence on what to look at:
   - in a browser on the PC: serve `build/mockups/manager` (`python -m http.server -d
     build/mockups/manager 8545`) or open its `index.html` from the folder;
   - or copied to any static web host (`index.html`, with `app.html` beside it), which they can
     open on a phone.
5. **Record the decision** in the proposal: `status: 'approved'` or `'rejected'`, and `decided:
   '2026-10-02: what they said'`. Change it until it is approved; their words in the chat are the
   approval, the file is the record. The backlog row links the file.
6. **Build it** in `panel-pages.js` (and `panel.css`, the mock if the API changes, the firmware),
   with its tests, then delete the proposal file. Today's mockup is the Panel Manager again.

## The viewer
The striped bar says it is a mockup. **Page** follows the frames and opens any page (and the inside
pages the menu doesn't list); **Who** signs in as the installer, a system or partition master, a
standard user, a guest, someone with a temporary password, or nobody; **Panel** starts the pretend
panel in a state (ready, not ready, motion, exit time, armed, entry time, an alarm, alarm occurred,
fire); **Proposal** picks one, shown **Today**, **Proposed** or **Both** side by side (the pages move
together unless set apart), with its looks in one numbered **Look at** list (back and next beside it)
and its why, details and decision folded under **About this proposal**; **Sensors** opens and closes a zone's sensor in both frames; **More**:
two partitions, Z-Wave (none, jammed), the clock module, a walk test running, delays 20 times
faster, the theme, a computer, tablet or phone frame, and starting the panel again. The address
keeps it all (`index.html#p=BL-NNN-garage&page=/garage&frame=phone`); **Open alone** opens
one frame full size. Each frame has its own pretend panel: what is done in one doesn't happen in
the other.

## Rules for the files
- **Today's pages are never copied here.** A proposal changes `PAGES` entries or adds pages; it
  doesn't edit the Panel Manager's files, which stay what the panel serves.
- **The panel's design:** `panel.css`'s tokens and parts (`card`, `rows`, `notice`, `tag`, `choice`
  ...), the icons in `panel.js`'s `ICONS` (a new one is Lucide's, added by the proposal and named in
  its notes), the wording rules of docs/features/38, nothing that flashes or moves (AGENTS.md
  "Display safety").
- **The API as it is**, where possible: a proposal that only rearranges pages is cheap to build. One
  that needs more from the panel answers it with `routes` and says so in `notes` (the firmware and
  `mock-panel.cjs` change when it is built).
- **Fictional data only**, as everywhere in the repository: the mock's Harbor House, Alex, Pat, Sam.

## Files
| Path | What |
|---|---|
| `build.cjs` | builds `build/mockups/manager/index.html` (the viewer) and `app.html` (the Panel Manager, its pretend panel and the proposals, all inlined); `--languages build/i18n/web` adds the real catalogs |
| `shim.js` | runs `mock-panel.cjs` in the page: just enough Node (`http`, `fs`, `crypto`, `Buffer`), and `fetch()` and the remote keypad's WebSocket answered by it |
| `boot.js` | switches on the proposals asked for, starts the Panel Manager, tells the viewer where it is |
| `viewer.html` | the viewer around the frames |
| `proposals/*.js` | changes under discussion; `_template.js` starts one |
| `components/services/panel_web/test/manager_mockup.test.cjs` | builds it and renders every page, every role and every proposal against the pretend panel in the page |

## Limits
- The pretend panel is `mock-panel.cjs`: what it doesn't do, the mockup doesn't (no firmware
  upload; a backup's "encryption" is a stand-in). Keep the mock in step with the API, as before.
- English, unless built with `--languages`.
- The browser's Panel Manager only; the touchscreen's mockup is docs/features/07-UI-LVGL-Pro/mockup.
