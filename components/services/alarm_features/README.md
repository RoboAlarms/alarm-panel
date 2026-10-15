# alarm_features

**Tier:** services (Kconfig only) · **Features:**
[33 Modular architecture](../../../docs/features/33-Modular-Architecture-and-Boards/README.md)
(step 6, the feature model) · **Tests:** every firmware build; the board manifest check
(step 9) reads the same options

The "Alarm features" menu every firmware shares: the build profile and the switches for each
optional part of the panel. A board picks what it can carry in its `sdkconfig.defaults`, which
states every option here explicitly and is therefore the board's manifest. Nothing is
compiled here.

## Options
| Kconfig | Default | Component(s) it switches |
|---|---|---|
| `ALARM_BUILD_DEV` / `ALARM_BUILD_RELEASE` | n | the build profile (signed project name, release guard) |
| `ALARM_FEATURE_UI` | y | the touch UI (`alarm_ui`, LVGL); off, `panel_app` runs headless |
| `ALARM_FEATURE_MQTT` | y | `net_mqtt`, the MQTT page |
| `ALARM_FEATURE_NOTIFY` | y with PSRAM | `net_notify`, the ntfy and Pushover pages (with the QR code) |
| `ALARM_FEATURE_HA` | y with PSRAM | `ha_link`, the Home Assistant page, `zsrc_ha` |
| `ALARM_PANEL_WEB` | y with PSRAM | `panel_web` |
| `ALARM_DEV_REMOTE` | y with PSRAM | `dev_remote` |
| `ALARM_FEATURE_STORAGE` | y | `panel_storage`: the microSD card and Settings > About > Storage |
| `ALARM_FEATURE_PHOTO_SAVER` | y with PSRAM | the screen saver's JPEG picture |
| `ALARM_FEATURE_AUTOMATIONS` | y with PSRAM (needs `ALARM_HOME_CONTROL`) | `automation`, `automation_service`: the built-in automations (docs/features/41) |

Other options live with the component that uses them and follow the same rules: the 433 MHz
and Zigbee sources in `rf_service` and `zigbee_service`, Devices (`ALARM_HOME_CONTROL`) in
`device_service`, and the simulator, wired and Home Assistant sources, the service console and
the task settings in `platform_esp`.

## Rules
- Off means not linked. ESP-IDF resolves requirements before it reads the configuration, so a
  requirement can't depend on an option: an optional component is always safe to require, and
  its sources follow the option in one of two ways, which its README states:
  - **absent**: the whole source is inside `#if CONFIG_<option>`, and callers guard their calls
    the same way (`net_mqtt`, `net_notify`, `ha_link`);
  - **stubs**: off, the component still provides its functions, answering "not in this
    firmware", so callers need no guard (`rf_service`, `zigbee_service`, `device_service`,
    `panel_storage`).
  A board's firmware simply doesn't require a component it never carries.
- The UI hides what an option removed: its pages are left out of the build and the backend
  leaves their entries NULL.
- Defaults follow PSRAM because the heavy features need it; a board's manifest states every
  option anyway, so defaults only matter for a board that forgot one (the manifest check
  catches that, step 9).
- Option names are stored in every panel's build configuration: rename none, only add.
