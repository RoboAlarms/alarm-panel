// A proposal for the Panel Manager (docs/features/31 mockup/README.md). Copy this file to
// proposals/BL-NNN-short-name.js, then `node docs/features/31-Browser-Panel-Manager/mockup/build.cjs`.
//
// It runs inside the Panel Manager after panel-pages.js and panel.js, so it has everything they have:
// PAGES, api(), t()/tc()/tn()/ht(), esc(), icon(), pageHead(), notice(), tag(), kv(), fieldRows(),
// liveFields(), collect(), review(), editDialog(), toast(), go(), the zone and source words (zoneTypes(),
// deviceClasses(), sourceName() ...). Write it the way panel-pages.js is written, so building it is moving
// it there. Only proposal({...}) and function declarations run when the file loads.
proposal({
  bl: 'BL-NNN',
  title: 'A few words naming the change',
  status: 'draft', // 'approved' or 'rejected' once the person you work for has said so
  decided: '', // '2026-10-02: what they said'
  role: 'installer', // who the viewer signs in as when this proposal is picked
  why: ['What is hard today, in a sentence or two.', 'What this changes, in a sentence or two.'],
  // Where to look: [route, label, viewer settings]; the first opens when the proposal is picked.
  look: [['overview', 'The page that changes']],
  replaces: ['overview'], // today's pages it changes or replaces
  // today: {'new-page': 'old-page/arg'}, // a page only this proposal has: what the Today frame shows beside it
  // builds: ['BL-NNN-other-proposal'], // proposals this one needs: the Proposed frame turns them on too
  notes: ['Each change in a line, for the review.'],
  css: `
    /* new rules, in panel.css's tokens (var(--surface), var(--line) ...); a new token is part of the proposal */
  `,
  // Answers the pretend panel doesn't give yet, if the proposal needs an API change (say so in notes):
  // routes(route, mock) { route('GET', /^zone\/(\d+)$/, (match, body, mock, query) => [200, {...}]); },
  apply(mock) {
    // Change the pages: PAGES.overview.render = myOverview; PAGES.mine = {title: tx('Mine'), icon: 'grid', group: '', render: myPage};
  },
});
