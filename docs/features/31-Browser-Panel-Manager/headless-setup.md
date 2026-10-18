# Screen-free setup and recovery

Status: approved; implementation in progress (BL-048). The portable setup/recovery controller
and BOOT gesture have host tests. The setup AP, provisioning, durable account adapter and HTTP
integration are not implemented yet. This does not change the already implemented keypad
enrollment flow. [Approved proposal](mockup/proposals/BL-048-headless-setup.js).

## Approved flow

Automatic setup access is for headless masters only. On panels with screens, Panel Manager
stays off until explicitly enabled. Neither first boot nor BOOT recovery enables it implicitly.
Once enabled, screen-equipped panels can reuse the shared management services. Completing
headless setup must retain authenticated Panel Manager access over the configured network.

1. Connect locally using a protected setup Wi-Fi network, or Ethernet on supported hardware.
   Verify the panel identity against its setup label and enter its unique initial setup code.
2. Create separate household/system-master and installer browser accounts. Keep their existing
   privileges: installer access cannot arm/disarm. Save a newly generated private recovery key.
3. Test the home Wi-Fi settings. Reconnect the browser on that network, authenticate and confirm
   the trial within two minutes. Only then close temporary setup connectivity. A failed trial
   restores the prior working network, or setup-only connectivity on a new panel.
4. Open normal Panel Manager and use Partitions and devices to add keypads and configure the
   system. Display-equipped and headless masters share this flow; screen-only controls are
   absent on a headless panel. Language/audio/display remain local to each keypad.

The new panel exposes only a narrowly scoped setup service, not authenticated alarm control.
First setup must not require an already configured browser account. The mock viewer's installer
identity is only its rendering fixture. Production setup needs a separate bootstrap session.
Once accounts exist, an interrupted setup resumes using those credentials rather than creating
duplicates or accepting the original setup code again. Account creation and bootstrap-secret
consumption must be atomic and restart-safe. Recovery-key acknowledgement is a separate durable
step: interruption must not leave an unrecoverable, unseen key or re-expose an old key.

## Recovery

On the working LAN, existing authenticated management remains the normal recovery route.
Without LAN access, a protected physical recovery control opens a bounded local setup listener.
Joining that network alone grants no alarm or configuration access. An existing browser account
can repair networking, subject to its normal role. Local recovery must still work when ordinary
web access is disabled; it is a separate, limited listener, not a silent re-enable of remote access.

A lost browser password requires physical recovery activation plus the private recovery key,
or assistance from another authorized administrator. Replacing a selected account's browser
password revokes its sessions, rotates the recovery key and records an audit event. It does not
change alarm codes, users' roles, enrolled keypads, sensors or configuration. Network/access
changes wait while any partition is armed or alarming. Recovery activation never disarms or
silences the system. Factory reset remains a separate destructive procedure.

The panel owns attempt IDs, expiry, stage, network trial and final outcome. Browser loss means
unknown outcome until status is reread. Retry/resume must be idempotent, and a second controller
must not take over an active claim. Setup credentials are unique per device, rate-limited and
never accepted on a configured panel. Recovery secrets are high-entropy, hashed at rest and
excluded from logs/ordinary backups. The public label is not a recovery credential.

## Hardware and implementation prerequisites

These remain prerequisites, not working controls on the existing development panels:

- Provision a unique protected setup-network credential, one-use claim secret and verifiable
  device certificate identity. Establish how the label/QR is produced and how it remains valid
  across permitted firmware updates and reset/reprovisioning.
- The approved development control is BOOT after normal startup: hold five seconds, then
  release to open recovery for five minutes. Short presses do nothing. A button already held
  at startup must first be released; repeated activation cannot extend an active window.
  The input policy debounces both edges and cancels a gesture after a sampling gap over 100 ms.
  Board GPIO wiring and the S3 firmware target still need implementation and physical acceptance.
  For an enclosure, use a protected dedicated recovery button on a verified non-strapping pin.
  [Espressif's DevKitC-1 guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.1.html)
  documents that holding BOOT during reset enters firmware-download mode; it is not this recovery
  action. The five-second/five-minute timings are product policy, not vendor requirements.
- Verify P4/C6 AP+station operation, channel changes and ESP-NOW coexistence. Decide the
  connection handover behavior if simultaneous setup AP and station mode cannot be supported.
- Resolve browser certificate trust before collecting secrets. No automatic certificate-warning
  bypass. A future mobile app can use a QR identity pin; browser onboarding needs an independently
  verifiable identity path. The mockup's certificate paragraph is not a completed trust design.
- Add restricted bootstrap/recovery services, durable account/setup transactions and network
  trial reconciliation. Reuse existing authenticated Panel Manager operations after completion.
- Preserve display-panel defaults permanently: Feature 09 uses touchscreen provisioning and
  Feature 31 ordinary web access remains off until explicitly enabled.

## Controller integration contract

`web_setup_flow.c` is a portable policy controller, not a running setup service. Its host tests
exercise competing claims, expiry, failed commits, resumption from committed state, physical
input, armed refusal and the screen-panel listener restriction. Storage callbacks in those tests
are fakes; they do not demonstrate power-fail-safe NVS writes or working network provisioning.

The future adapter must provide verified device identity, rate limiting and server-authenticated
actors, serialize access, and recheck alarm state under the alarm service's programming lock.
It must atomically commit account/key changes with the setup record, consume the claim secret,
revoke sessions, and audit password recovery. Never serialize the native record struct as a
stored format. Persist a versioned representation with the transaction. Network confirmation
must commit the network trial and its setup state as one recoverable operation. On startup,
reconcile or roll back a pending network trial before exposing the service. The controller
intentionally forgets claims and physical recovery windows at restart. Poll expiry independently
of browser requests; a rollback signal must be applied before accepting another trial.

## Acceptance before release

- [x] Owner approved setup mockups and BOOT hold/release policy; screen panels retain explicit enable (BL-048).
- [x] Portable controller and BOOT gesture tests (BL-048); no live service or GPIO hookup yet.
- [ ] Implement bootstrap trust and durable setup/recovery services shared by browser and future app (BL-048).
- [ ] Verify no-screen first boot, duplicate claims, expired sessions, each interruption point,
  trial rollback, wrong/lost credentials, armed refusal and restored access on hardware (BL-048).
