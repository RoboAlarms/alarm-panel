# Security policy

RoboAlarms is an alarm panel, so a security problem in it can leave a home unprotected. Thank
you for taking the time to report one.

## Reporting a vulnerability

**Please don't open a public issue, discussion or pull request for a security problem.**
Report it privately through GitHub instead: the repository's **Security** tab, then **Report a
vulnerability**. Only the maintainers see the report, and the fix can be discussed and prepared
there before anything is public.

A useful report says:

- what an attacker can do, and from where (at the keypad, on the home network, over the air,
  with the panel in hand);
- the firmware version (Settings > About, or `status` on the service console), the board and
  its P4 silicon revision, and the build profile (legacy, `dev` or `release`);
- the steps to reproduce it, and a proof of concept if you have one;
- whether you plan to publish, and when.

This is a volunteer project without a fixed response time. We will acknowledge the report,
keep you told what we find, credit you in the release notes if you want to be named, and
agree a publication date with you once a fix is out.

## Supported versions

Only the latest release and the `main` branch get security fixes. The project is still in
development (0.x): update a panel to the newest release before reporting.

## In scope

Anything in this repository: the alarm engine and its codes and lockouts, the firmware and its
services (MQTT, Home Assistant link, notifications, firmware updates), the Panel Manager the
panel serves over HTTPS, the remote keypad, the RF and Zigbee sensor sources, and the PC tools
(`tools/panelctl.py`, the Flasher). For example: disarming or silencing without a valid code,
getting around the keypad or browser sign-in lockout, running firmware that isn't signed with
the panel's key, reading codes or secrets from the network, or crashing the panel from the
network or the air.

## Known limits (not vulnerabilities)

These are documented properties of the design or of hobby hardware. A report that goes beyond
them is welcome.

- **Not a certified alarm system.** This is a DIY project, not a listed or certified system,
  and it is not monitored by anyone. Don't rely on it alone for life safety.
- **Flash encryption, NVS encryption and eFuse Secure Boot are off by default.** Someone with
  the panel in hand and a USB cable can read its flash. Images are still signed and a panel
  refuses updates that aren't signed with its key (docs/features/15, docs/features/13).
- **Development and legacy builds have a USB service console.** Release builds leave it out.
- **Fixed-code 315/345/433 MHz sensors can be replayed** by anyone nearby with a transmitter;
  the panel says so on the zone and doesn't accept them for fire, CO or panic without the
  installer's say-so. Rolling-code transmitters are checked for replays (docs/features/25).
- **The factory codes are public** (installer 69420, master 123456): the panel asks for new ones
  at first setup and shows a reminder until both are changed.

The threat model and the controls behind these are in
[docs/features/15](docs/features/15-Security-and-Hardening/README.md).
