# Screen-free setup and recovery

Status: draft for owner review (BL-048). This extends first setup; it does not change the
already implemented Panel Manager keypad enrollment flow. No firmware or access policy has
been changed. [Interactive proposal](mockup/proposals/BL-048-headless-setup.js).

## Proposed flow

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

These are proposed requirements, not instructions for the existing development boards:

- Provision a unique protected setup-network credential, one-use claim secret and verifiable
  device certificate identity. Establish how the label/QR is produced and how it remains valid
  across permitted firmware updates and reset/reprovisioning.
- Choose a protected physical recovery control on the final headless hardware. Do not assign
  a button, pin or hold duration until its electrical behavior is verified.
- Verify P4/C6 AP+station operation, channel changes and ESP-NOW coexistence. Decide the
  connection handover behavior if simultaneous setup AP and station mode cannot be supported.
- Resolve browser certificate trust before collecting secrets. No automatic certificate-warning
  bypass. A future mobile app can use a QR identity pin; browser onboarding needs an independently
  verifiable identity path. The mockup's certificate paragraph is not a completed trust design.
- Add restricted bootstrap/recovery services, durable account/setup transactions and network
  trial reconciliation. Reuse existing authenticated Panel Manager operations after completion.
- Preserve existing display-panel defaults until this draft is approved; Feature 09 currently
  assumes touchscreen provisioning and Feature 31 defaults ordinary web access off.

## Acceptance before release

- [ ] Owner approval of screen-free setup, recovery and required hardware controls (BL-048).
- [ ] Implement bootstrap trust and durable setup/recovery services shared by browser and future app (BL-048).
- [ ] Verify no-screen first boot, duplicate claims, expired sessions, each interruption point,
  trial rollback, wrong/lost credentials, armed refusal and restored access on hardware (BL-048).
