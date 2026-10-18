// Draft only: no production endpoint, credential or hardware operation is invoked.
proposal({
  bl: 'BL-048', title: 'Set up a panel without a screen', status: 'draft', decided: '',
  role: 'installer',
  why: ['A new headless panel cannot use the touchscreen to join Wi-Fi or create its first browser login.',
    'A local setup flow creates separate household and installer access. Recovery keeps the existing system.'],
  look: [
    ['headless-start', 'Start here: connect to the panel'],
    ['headless-unlock', 'Verify this panel'],
    ['headless-accounts', 'Create household and installer access'],
    ['headless-network', 'Choose the home network'],
    ['headless-check', 'Connection test and lost connection'],
    ['headless-failed', 'Wrong Wi-Fi password: try again'],
    ['headless-ready', 'Finish and add a keypad'],
    ['headless-recovery', 'Recover an existing panel'],
    ['headless-password', 'Lost browser password'],
    ['headless-locked', 'Recovery while armed'],
  ],
  replaces: [], today: Object.fromEntries(['start','unlock','accounts','network','check','failed','ready','recovery','password','locked'].map(x => ['headless-'+x, 'network'])),
  notes: [
    'Draft for normal and headless panels. All actions here are simulations; do not enter real passwords.',
    'Setup Wi-Fi, unique label secrets, a verifiable device identity and a protected local recovery control are proposed hardware/provisioning requirements, not existing controls on the bench panels.',
    'An unclaimed panel exposes only setup. The preview hides normal navigation during setup. A production setup session has no alarm authority.',
    'Browser and future app share panel-owned setup/recovery state. No mobile app or cloud account is required.',
    'New backend required: claim, account creation, resume, network trial/confirmation, completion and recovery. No existing API is changed by this mockup.',
  ],
  css: `body:has(.hs) #sidebar,body:has(.hs) #menu{display:none}body:has(.hs) .workspace{margin-left:0}.hs{max-width:720px;margin:auto}.hs .card{padding:24px;margin-bottom:16px}.hs h2{margin-top:0}.hs p{line-height:1.55}.hs label{display:block;margin:16px 0 6px}.hs input:not([type=checkbox]),.hs select{width:100%;padding:12px;border:1px solid var(--line);border-radius:8px;background:var(--surface);color:inherit}.hs-actions{display:flex;justify-content:space-between;gap:12px;flex-wrap:wrap;margin-top:24px}.hs-actions button{padding:10px 18px}.hs small{display:block;margin-top:8px}.hs details{margin:16px 0}.hs .hs-progress{font-size:.85rem;opacity:.7;margin-bottom:16px}.hs [role=alert]{color:var(--bad);margin-top:12px}`,
  apply() {
    for (const id of ['start','unlock','accounts','network','check','failed','ready','recovery','password','locked']) {
      PAGES['headless-'+id] = {title: tx('Panel setup · '+id), icon:'wifi', nav:false, render: () => hsPage(id)};
    }
  },
});

function hsButton(label, route, primary=false) {
  return `<button type="button" class="${primary ? 'primary' : ''}" data-hs="${route}">${label}</button>`;
}
function hsInput(label, id, type='text', placeholder='') {
  return `<label for="hs-${id}">${label}</label><input id="hs-${id}" type="${type}" placeholder="${placeholder}" autocomplete="off" required>`;
}
function hsPage(id) {
  let title='', text='', fields='', buttons='', step='';
  if (id==='start') {
    title='Set up your alarm panel';
    text='Use this flow with a panel that has a screen or one without a screen.';
    fields='<ol><li>Power on the new panel.</li><li>Join the setup Wi-Fi shown on its setup label.</li><li>Open the Panel Manager address on that label.</li></ol><p>Already connected by Ethernet? Open its Panel Manager address on the local network.</p><small>Ethernet is available only on hardware that supports it.</small>';
    buttons=hsButton('Recover an existing panel','recovery')+hsButton('Start setup','unlock',true);
  } else if (id==='unlock') {
    step='1 of 4 · Verify panel'; title='Check the panel';
    text='Match the panel identity to its setup label before continuing.';
    fields='<p><strong>Harbor House panel · Demo 0042</strong></p><details><summary>Verify the connection</summary><p>Verify the HTTPS certificate fingerprint against the setup label. Do not continue if it differs.</p><small>The final label and trust flow need hardware provisioning support.</small></details>'+hsInput('Setup code','claim','password','From the setup label');
    buttons=hsButton('Cancel','start')+hsButton('Continue','accounts',true);
  } else if (id==='accounts') {
    step='2 of 4 · Access'; title='Create your access';
    text='Household access can arm and disarm. Installer access programs the system.';
    fields=hsInput('Household username','owner','text','Alex')+hsInput('Household password','owner-pass','password')+hsInput('Installer username','installer','text','Installer')+hsInput('Installer password','installer-pass','password')+
      '<details><summary>Keep a recovery key</summary><p>The panel will generate a private recovery key after saving these accounts. Keep it somewhere safe, separate from the panel.</p></details><label><input id="hs-key" type="checkbox"> Simulate saving the recovery key</label>';
    buttons=hsButton('Back','unlock')+hsButton('Save access','network',true);
  } else if (id==='network' || id==='failed') {
    step='3 of 4 · Network'; title=id==='failed'?'Could not join Wi-Fi':'Connect to your network';
    text=id==='failed'?'Check the network and password, then try again. Your accounts are saved.':'Choose the Wi-Fi this panel will use. Nearby keypads can receive these settings after enrollment.';
    fields='<label for="hs-ssid">Wi-Fi network</label><select id="hs-ssid"><option>Harbor House</option><option>Harbor Guest</option></select>'+hsInput('Wi-Fi password','wifi','password')+'<small>The setup connection stays available until you confirm the new connection.</small>';
    buttons=hsButton('Leave setup','recovery')+hsButton('Test connection','check',true);
  } else if (id==='check') {
    step='3 of 4 · Network'; title='Confirm the connection';
    text='After the panel joins, reconnect this device to your home network and open Panel Manager there.';
    fields='<p><strong>Panel address</strong><br>Shown here after the panel joins.</p><p>Sign in with the access you just created. Confirm there within two minutes to keep the network change.</p><details open><summary>Mockup only · choose a result</summary><p>A browser disconnect does not tell us whether the panel joined. Reopening Panel Manager reads the saved setup status.</p></details>';
    buttons=hsButton('Connection failed','failed')+hsButton('Connection lost','recovery')+hsButton('Simulate confirmation','ready',true);
  } else if (id==='ready') {
    step='4 of 4 · Ready'; title='Panel setup complete';
    text='Panel Manager is available on your home network. The temporary setup network is closed.';
    fields='<p>Next, configure partitions, devices and people. You can add a keypad now or later.</p><p>A panel without a screen uses these same pages. The Remote keypad page is shown only when a panel has a display.</p>';
    buttons=hsButton('Open Panel Manager','/overview')+hsButton('Add a keypad','/partitions',true);
  } else if (id==='recovery') {
    title='Reconnect to your panel'; text='Your users, zones and paired keypads stay saved.';
    fields='<p>If your home network still works, open Panel Manager there and sign in.</p><p>Otherwise, activate local recovery at the panel and join its protected setup network.</p><small>The recovery control and activation method depend on the final headless hardware. This is not a button instruction for the bench panels.</small>'+hsInput('Username','recover-user')+hsInput('Browser password','recover-pass','password')+'<p>After signing in, resume unfinished setup or change the network.</p>';
    buttons=hsButton('Cancel','start')+hsButton('Forgot password','password')+hsButton('Sign in','network',true);
  } else if (id==='password') {
    title='Recover browser access'; text='Use your saved recovery key while local recovery is active at the panel.';
    fields=hsInput('Recovery key','recovery-key','password')+hsInput('Username','reset-user')+hsInput('New browser password','reset-pass','password')+'<p>This replaces browser access for the selected account and signs out its sessions. Alarm codes and system configuration stay unchanged.</p><details><summary>No recovery key?</summary><p>Ask another authorized administrator to restore your access. A factory reset is a separate, destructive last resort.</p></details>';
    buttons=hsButton('Cancel','recovery')+hsButton('Replace password','recovery',true);
  } else {
    title='Recovery is waiting'; text='The system is armed. Disarm it using an authorized account or paired keypad before changing access or network settings.';
    fields='<p>Opening recovery does not disarm the system or turn off its alarms.</p>';
    buttons=hsButton('Back','recovery');
  }
  return {html:`<div class="hs"><p class="hs-progress">Draft · simulated setup</p><section class="card">${step?`<p class="hs-progress">${step}</p>`:''}<h1>${title}</h1><p>${text}</p><form id="hs-form">${fields}<p id="hs-error" role="alert"></p><div class="hs-actions">${buttons}</div></form></section></div>`,bind() {
    document.querySelectorAll('[data-hs]').forEach(b => b.onclick=()=>{
      const dest=b.dataset.hs;
      const needs={unlock:'accounts',accounts:'network',network:'check',failed:'check',recovery:'network',password:'recovery'}[id]===dest;
      if(needs && !document.getElementById('hs-form').reportValidity()) return;
      if(id==='accounts' && needs && !document.getElementById('hs-key').checked) {
        document.getElementById('hs-error').textContent='Save the recovery key before continuing.'; return;
      }
      go(dest.startsWith('/')?dest.slice(1):'headless-'+dest);
    });
    document.getElementById('hs-form').onsubmit=e=>e.preventDefault();
  }};
}
