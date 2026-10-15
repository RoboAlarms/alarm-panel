// BL-151: a Zigbee request the panel has taken but not yet answered says "still running", not "busy"
// (docs/features/20 devices.md §6; zbsvc_submit). The API gains the result word "pending"; this file
// answers it for Start listening (the first press starts listening 3 s later, the next never starts)
// and for Check it now, and draws what the pages say.
proposal({
  bl: 'BL-151',
  title: 'A Zigbee request the panel is slow to answer says it is still running',
  status: 'draft',
  decided: '',
  role: 'installer',
  why: [
    'When the panel takes a Zigbee request but answers it late (a slow save, a device being removed), the Panel Manager says "Another Zigbee operation is running." though the request runs: Start listening seems to fail and then listens, and pressing again asks twice.',
    'The panel answers "pending" instead. Start listening shows "Starting" and follows it until the panel listens; after 20 s without, it says the panel didn\'t answer. Other requests say "Still running: the panel is taking longer than usual." "Another Zigbee operation is running." stays for a request the panel never took.',
  ],
  look: [
    ['zigbee-add', 'Press Start listening: "Starting" for 3 s, then listening'],
    ['zigbee-add', 'Then Stop and Add another: after 20 s, "Not started"'],
    ['zigbee-device/1', 'Check it now: the toast says it is still running'],
  ],
  replaces: ['zigbee-add', 'zigbee-device'],
  notes: [
    'API: the result word "pending" (UI_ZB_REQ_PENDING, appended): the panel took the request and it runs, but it didn\'t answer within 4 s. "busy" keeps its meaning: it never ran.',
    'Start listening, Add another, Add with an install code and Replace answer "pending" with the Starting card (no buttons; the page asks the panel once a second and becomes the listening card when it listens). After 20 s without listening: "Not started" with Try again.',
    'Every other request (rename, remove, forget, check, blink, set up again, channel, restart, reset, codes, siren, keypad, key fob) toasts "Still running: the panel is taking longer than usual." in place of its own words; the page shows the outcome when it next reads it.',
    'Built, zgSend returns the result and its callers choose the words; here the toast is put over theirs.',
    'Mock: the pretend panel\'s zigbee and zbOperation become reachable from proposals (build.cjs INTERNALS).',
  ],
  css: ``,
  routes(route) {
    let presses = 0;
    route('POST', /^zigbee$/, (match, body, mock) => {
      if (body.op === 'add') {
        presses++;
        if (presses % 2 === 1) setTimeout(() => mock.zbOperation({op: 'add'}), 3000);
        return [200, {result: 'pending'}];
      }
      if (body.op === 'ping') { mock.zbOperation(body); return [200, {result: 'pending'}]; }
      return null;
    });
  },
  apply() {
    const today = PAGES['zigbee-add'].render;
    PAGES['zigbee-add'].render = () => zbPendingAddPage(today);
    zgSend = zbPendingSend;
  },
});

// When a "pending" Start listening was answered (ms), 0 none.
let zbStarting = 0;
const ZB_START_GIVE_UP_MS = 20000;

async function zbPendingSend(body, accept = ['ok']) {
  const r = await api('zigbee', body);
  if (r.result === 'pending') {
    if (['add', 'add_code', 'replace'].includes(body.op)) zbStarting = Date.now();
    else setTimeout(() => toast(t('Still running: the panel is taking longer than usual.')), 0);
    return r;
  }
  if (!accept.includes(r.result)) throw new Error(zgRefusal(r.result));
  return r;
}

async function zbPendingAddPage(today) {
  if (!zbStarting) return today();
  const z = await api('zigbee');
  if (!z.available || zgListening(z.progress)) { zbStarting = 0; return today(); }
  const head = pageHead(t('Add a Zigbee device'), t('Any Zigbee 3.0 device, for 3 minutes.'), '', zgBack());
  if (Date.now() - zbStarting >= ZB_START_GIVE_UP_MS) {
    return {html: head + `<section class="card status-card warn"><span class="state">${ht('Not started')}</span>
      <p>${ht('The panel didn’t answer. Try again.')}</p>
      <div class="card-foot"><button type="button" class="primary" id="zb-start" ${zgOff(z.may_change && zgUsable(z.radio))}>${ht('Try again')}</button></div></section>`,
      bind() {
        $('#zb-start').onclick = async () => {
          zbStarting = 0;
          try { await zgSend({op: 'add'}); await show(); } catch (error) { toast(error.message, 'error'); }
        };
      }};
  }
  return {html: head + `<section class="card status-card info"><span class="state">${ht('Starting')}</span>
    <p>${ht('The panel is taking longer than usual to start listening. This page follows it.')}</p></section>`,
    bind() {
      poll(async () => {
        const now = (await api('zigbee')).progress;
        if (zgListening(now) || Date.now() - zbStarting >= ZB_START_GIVE_UP_MS) await show();
      }, 1000);
    }};
}
