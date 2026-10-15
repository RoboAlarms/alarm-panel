/*
 * test_zbz_contract.c - the Zigbee contracts compile and agree (docs/features/20 testing.md §1;
 * BL-143 A1). Built with the portable components' own warnings (STRICT): every header of
 * zigbee_zones, the radio contract (zone_hub/zsrc_zigbee.h) and ui_backend's zb_* block. The
 * checks are the facts one header promises another - capacities, stored names, enum values two
 * headers share - so a later change to one side fails here before it fails on a panel.
 */
#include <stddef.h>
#include <string.h>

#include "ui_backend/ui_backend.h"
#include "unity.h"
#include "zigbee_zones/zbz_ace.h"
#include "zigbee_zones/zbz_codes.h"
#include "zigbee_zones/zbz_models.h"
#include "zigbee_zones/zbz_panel.h"
#include "zigbee_zones/zbz_registry.h"
#include "zigbee_zones/zbz_wd.h"
#include "zigbee_zones/zbz_zcl.h"
#include "zone_hub/zsrc_zigbee.h"

/* ZB-010: the pages' capacities are the registry's. */
_Static_assert(UI_ZB_DEVICES_MAX == ZBZ_DEV_MAX, "the device list holds the whole registry");
_Static_assert(UI_ZB_CODES_MAX == ZBZ_CODES_MAX, "the codes page holds every waiting code");
_Static_assert(UI_ZB_CHANNELS == ZSRC_ZB_CHANNELS, "the channel page shows the survey's channels");
_Static_assert(UI_ZB_NAME_LEN == ZBZ_NAME_LEN, "a name fits its record");
_Static_assert(UI_ZB_MODEL_LEN >= ZBZ_TEXT_LEN, "a reported model fits the page");
/* ZB-010: a device id fits a page's int32_t argument, positive. */
_Static_assert(sizeof(((ui_zb_device_t *)0)->id) == 2u, "ids are 16 bits");
/* ZB-010: every slot has a dirty bit. */
_Static_assert(ZBZ_DEV_MAX <= 64u, "zbz_registry_t.dirty is 64 bits");
/* The review of wave 6 (devices.md section 4; ZB-010, ZB-013): the zone source keeps up with the
 * registry - an endpoint slot for every device and two on average, a bound input for every zone,
 * and every device forgotten at once (a network reset) remembered, so no zone keeps the hour's
 * grace for a sensor that is gone. */
_Static_assert(ZSRC_ZB_DEV_MAX >= 2u * ZBZ_DEV_MAX, "the source's endpoint slots cover the registry");
_Static_assert(ZSRC_ZB_WATCH_MAX >= ALARM_MAX_ZONES, "every zone can bind a Zigbee input");
_Static_assert(ZSRC_ZB_FORGOTTEN_MAX >= ZBZ_DEV_MAX, "a network reset forgets every device");
_Static_assert((ZSRC_ZB_DEV_MAX - 1u) <= 0x7FFFu, "a source tag carries its slot in 15 bits");
/* ... and zbz_panel_load reads each blob through one buffer of the codes' size. */
_Static_assert(ZBA_BLOB_MAX <= ZBZ_CODES_BLOB_MAX, "the ace blob fits the load buffer");
_Static_assert(ZBW_BLOB_MAX <= ZBZ_CODES_BLOB_MAX, "the wd blob fits the load buffer");
_Static_assert(ZBZ_CFG_BLOB <= ZBZ_CODES_BLOB_MAX, "the cfg blob fits the load buffer");
/* ZB-010: one record of the longest kind fits what the codec promises. */
_Static_assert(1u + (2u + 2u) + (2u + 8u) + (2u + 2u) + (2u + ZBZ_NAME_LEN - 1u) +
                   2u * (2u + ZBZ_TEXT_LEN - 1u) + (2u + ZBZ_SW_LEN - 1u) + 2u * 3u + 4u +
                   5u * 3u + 6u + 3u * 3u + 6u + ZBZ_EP_MAX * (2u + 11u) <=
               ZBZ_RECORD_MAX,
               "the longest record fits ZBZ_RECORD_MAX");
/* ZB-021: the trust record's values are the same on both sides of the contract. */
_Static_assert((int)UI_ZB_JOIN_UNKNOWN == (int)ZSRC_ZB_JOIN_UNKNOWN, "join: unknown");
_Static_assert((int)UI_ZB_JOIN_INSTALL_CODE == (int)ZSRC_ZB_JOIN_INSTALL_CODE, "join: code");
_Static_assert((int)UI_ZB_JOIN_STANDARD_KEY == (int)ZSRC_ZB_JOIN_STANDARD_KEY, "join: key");
/* ZB-024: an Arm with the longest code the panel takes fits a payload (mode, length, code, zone). */
_Static_assert(1u + 1u + ALARM_CODE_MAX_LEN + 1u <= ZSRC_ZB_ZCL_PAYLOAD_MAX, "an Arm fits");
/* ZB-015: the longest install code with its CRC. */
_Static_assert(ZBZ_CODE_MAX == 16u + 2u, "16 bytes and the CRC");
/* ZB-023, ZB-024: the policies' sends are told apart from the panel's by their tag's top byte. */
_Static_assert((ZBZ_TAG & ZBZ_TAG_MASK) != (ZBW_TAG & ZBW_TAG_MASK), "siren tags differ");
_Static_assert((ZBZ_TAG & ZBZ_TAG_MASK) != (ZBA_TAG & ZBA_TAG_MASK), "keypad tags differ");
_Static_assert((ZBW_TAG & ZBW_TAG_MASK) != (ZBA_TAG & ZBA_TAG_MASK), "siren and keypad differ");

/* ZB-024 (alarm-devices.md §4): a Zigbee keypad's device number never shares a lockout counter or
 * a log source with a Z-Wave keypad's. zw_keyctl.h gives Z-Wave's ZWKC_SLOTS (8) keypads
 * ZWKC_DEVICE(slot) = ALARM_MAX_DEVICES - 8 + 1 + slot, the top eight numbers (9..16). */
#define ZWKC_FIRST_DEVICE (ALARM_MAX_DEVICES - 8 + 1) /* zw_keyctl.h ZWKC_DEVICE(0) */
_Static_assert(ZBA_ZWAVE_DEVICES == 8u, "the Z-Wave keypads keep zw_keyctl.h's ZWKC_SLOTS (8)");
_Static_assert(ZBA_DEVICE_FIRST >= 1u, "device 0 is none");
_Static_assert(ZBA_DEVICE_LAST < ZWKC_FIRST_DEVICE, "below the Z-Wave keypads' numbers");
_Static_assert(ZBA_DEVICE_LAST + ZBA_ZWAVE_DEVICES == ALARM_MAX_DEVICES, "the rest are Zigbee's");
/* ZB-024: the limit fits what is left, and every number exists in the engine. */
_Static_assert(ZBA_KEYPADS_MAX <= ZBA_DEVICE_LAST - ZBA_DEVICE_FIRST + 1u,
               "one number per Zigbee keypad");
_Static_assert(ZBA_KEYPADS_MAX + ZBA_ZWAVE_DEVICES <= ALARM_MAX_DEVICES,
               "Zigbee and Z-Wave keypads together fit the engine's device numbers");
_Static_assert(ALARM_MAX_DEVICES <= 32, "devices_taken and devices_held are 32-bit masks");
/* ZB-024: a key fob's source is alarm_types.h's keyfob button, never a keypad id. */
_Static_assert(ZBA_FOB_SOURCE_KIND == ALARM_SRC_ZONE, "a fob is a keyfob button source");
_Static_assert(ZBA_FOB_SOURCE_ID == 0u, "and feeds no zone");
/* ZB-024: the stored keypad and fob entries are the sizes the NVS budget counts. */
_Static_assert(ZBA_KEYPAD_ENTRY == 1u + 1u + 2u + 8u + 1u + 1u + 1u + 1u + 1u, "keypad entry");
_Static_assert(ZBA_FOB_ENTRY == 1u + 1u + 2u + 8u + 1u + 1u + 1u + 1u, "fob entry");
/* ZB-023: a siren entry as zbz_wd.h's store comment lists it. */
_Static_assert(ZBW_ENTRY == 1u + 2u + 8u + 1u + 1u + 1u + 1u + 1u + 2u, "siren entry");
/* ZB-023: the installer's supervision time fits its stored u16 minutes and the page's field. */
_Static_assert(sizeof(((ui_zb_siren_t *)0)->supervision_min) ==
                   sizeof(((zbw_assign_t *)0)->supervision_min),
               "the page's supervision is the stored one");
_Static_assert(ZBW_SUPERVISION_MIN_MIN * 60u <= ZBW_SILENT_ROUTER_S, "the defaults are allowed");

/* ZB-017: the network reset carries the installer code, as long as any code the engine takes. */
_Static_assert(sizeof(((ui_zb_request_t *)0)->installer_code) == ALARM_CODE_MAX_LEN + 1u,
               "installer_code holds the longest code");
/* ZB-017, ZB-025: the new results are appended: the pages' and the web API's earlier values
 * keep their numbers. */
_Static_assert((int)UI_ZB_REQ_IN_USE == 11, "the first contract's results keep their values");
_Static_assert((int)UI_ZB_REQ_WRONG_CODE == 12, "wrong code appended");
_Static_assert((int)UI_ZB_REQ_LOCKED_OUT == 13, "locked out appended");

/* ZB-010 (devices.md §4): the NVS budget's arithmetic, the table in devices.md. */
_Static_assert(ZBZ_NVS_ENTRIES(ZBZ_RECORD_MAX) == 10u, "a record: index, header, 8 entries");
_Static_assert(ZBZ_NVS_ENTRIES(ZBZ_META_LEN) == 3u && ZBZ_NVS_ENTRIES(ZBZ_CFG_BLOB) == 3u,
               "meta and cfg");
_Static_assert(ZBZ_NVS_ENTRIES(ZBZ_CODES_BLOB_MAX) == 19u, "codes");
_Static_assert(ZBZ_NVS_ENTRIES(ZBW_BLOB_MAX) == 5u, "wd");
_Static_assert(ZBZ_NVS_ENTRIES(ZBA_BLOB_MAX) == 15u, "ace");
_Static_assert(ZBZ_NVS_WORST == 686u, "zbpanel at its largest: devices.md §4's table");
_Static_assert(ZBZ_NVS_ENTRIES(4001u) == 1u + 2u + 126u, "a blob past one chunk takes two headers");
/* ZB-010: the reserve covers the alarm configuration's rewrite at its largest and a record. */
_Static_assert(ZBZ_NVS_RESERVE >= ALARM_CONFIG_BLOB_MAX / ZBZ_NVS_ENTRY_BYTES + 10u,
               "the configuration can always be written again");
_Static_assert(ZBZ_NVS_ADD_MIN > ZBZ_NVS_RESERVE, "adding stops before the reserve");
/* ZB-010: Zigbee at its largest with the reserve left free fits the partition by itself. */
_Static_assert(ZBZ_NVS_WORST + ZBZ_NVS_ADD_MIN < ZBZ_NVS_USABLE, "zbpanel and the reserve fit");

void setUp(void) {}
void tearDown(void) {}

/* ZB-010: the namespace and every key fit NVS's 15 characters. */
static void test_stored_names_fit_nvs(void)
{
    TEST_ASSERT_LESS_OR_EQUAL_size_t(15u, strlen(ZBZ_NVS_NAMESPACE));
    const char *keys[] = {ZBZ_KEY_META, ZBZ_KEY_CODES, ZBZ_CFG_KEY, ZBW_KEY, ZBA_KEY, "d63"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        TEST_ASSERT_LESS_THAN_size_t(ZBZ_KEY_LEN, strlen(keys[i]) + 1u);
    }
}

/* ZB-027, ZB-025 (BL-144, BL-145): a backend that fills nothing hides every Zigbee row and page,
 * and a radio set up before BL-143 leaves every new hook NULL ("this radio can't": UNSUPPORTED). */
static void test_zeroed_contracts_are_off(void)
{
    ui_backend_t be;
    memset(&be, 0, sizeof(be));
    TEST_ASSERT_NULL(be.zb_status);
    TEST_ASSERT_NULL(be.zb_request);

    const zsrc_zb_radio_t radio = {.ctx = NULL};
    TEST_ASSERT_NULL(radio.leave);
    TEST_ASSERT_NULL(radio.zcl_command);
    TEST_ASSERT_NULL(radio.channel_survey);
}

/* ZB-017, ZB-024, ZB-027: what the second contract added reads as "nothing" when zeroed - a
 * request filled before it carries no installer code, a status no full storage, a port filled
 * before it takes no device numbers from the others, and a siren port never checks in. */
static void test_additions_zeroed_are_neutral(void)
{
    ui_zb_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = UI_ZB_REQ_RESET;
    TEST_ASSERT_EQUAL_size_t(0u, strlen(req.installer_code));

    ui_zb_status_t st;
    memset(&st, 0, sizeof(st));
    TEST_ASSERT_FALSE(st.storage_full);

    const zbz_panel_port_t port = {.ctx = NULL};
    TEST_ASSERT_NULL(port.devices_taken);

    const zbw_port_t wd = {.ctx = NULL};
    TEST_ASSERT_NULL(wd.checkin);

    /* ZB-024: the keypads' range, as the pages and the log will show it on this build. */
    TEST_ASSERT_EQUAL_UINT8(1u, ZBA_DEVICE_FIRST);
    TEST_ASSERT_EQUAL_UINT8(ALARM_MAX_DEVICES - 8, ZBA_DEVICE_LAST);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_stored_names_fit_nvs);
    RUN_TEST(test_zeroed_contracts_are_off);
    RUN_TEST(test_additions_zeroed_are_neutral);
    return UNITY_END();
}
