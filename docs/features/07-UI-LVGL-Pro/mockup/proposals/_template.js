// A proposal: a change to the touchscreen, drawn before it is built. Copy this file to
// proposals/BL-NNN-short-name.js (files starting with "_" are not built), fill it in, build, and
// show it to the person you work for. Only build it on the panel once its status is 'approved'.
//
// One file per backlog row. Each screen it adds or changes is an M.screen() with a proposal:
//   bl        the backlog row (docs/BACKLOG.md)
//   status    'draft' (open for review), 'approved' or 'rejected' (the person you work for said so;
//             write what they decided in `decided`), 'built' (on the panel; now move the
//             screen into screens/ as today's and delete the proposal)
//   replaces  the id of today's screen it changes; leave it out for a new screen
//   why       what problem it solves, for whom, in a sentence or two
// Wrap what changed in M.changed(...) so the reviewer sees it at a glance. Use only the kit's
// components, tones and tokens; a new component goes into kit/ first, named as its XML will be.

M.screen({
  id: 'bl-000-home-example', area: 'Home', title: 'Home, with an example change', shot: null,
  proposal: {bl: 'BL-000', status: 'draft', replaces: 'home-ready', why: 'What this solves, for whom.'},
  notes: 'What the reviewer should look at.',
  render: () => M.main({content: M.home(
    {tone: 'green', glyph: 'shield-check', headline: 'Disarmed', detail: 'Ready to arm', note: 'All zones closed'},
    M.changed(M.panel_arm()))}),
});
