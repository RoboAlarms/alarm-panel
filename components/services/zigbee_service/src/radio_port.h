/*
 * radio_port.h - everything the sensor radio service needs a real ESP32-H2 for (docs/features/20).
 *
 * This is deliberately the only file in the panel that knows about the Zigbee stack or about
 * writing the radio's flash, so the bring-up job is one file wide. Everything above it - the
 * device table, IAS enrollment, the cluster mapping, the quirks, supervision, jamming, and the
 * decision of when and whether to write the radio - is portable and already host-tested
 * (components/core/zone_hub/src/zsrc_zigbee.c, components/core/zigbee_host/src/bringup.c).
 *
 * What is built (CONFIG_ALARM_ZSRC_ZIGBEE, on in every P4 image): the probe (a Spinel version question on
 * the socket's UART), and Espressif's Zigbee stack (esp-zigbee-lib) as coordinator and trust
 * center on top of the H2 running firmware/radios/h2_rcp. What is not: writing the H2's flash
 * from the panel. The socket brings out only the module's UART1 (the ROM loader listens on
 * UART0) and no BOOT or RESET line, so the first image goes on over the module's own USB-C, and
 * a later one needs an RCP that can reboot itself into its loader (docs/features/20 step 6).
 */
#ifndef RADIO_PORT_H
#define RADIO_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "zigbee_host/zb_bringup.h"
#include "zone_hub/zsrc_zigbee.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opens the wireless socket's UART. */
esp_err_t radio_port_open(uint8_t tx_gpio, uint8_t rx_gpio);

/* Called (on the stack's task) whenever something waits for radio_port_pump. */
void radio_port_set_wake(void (*wake)(void));

/* The version string of the ESP32-H2 image this firmware carries; "" when it carries none, in
 * which case whatever radio is plugged in is used as it is (ZB-001). */
const char *radio_port_image_version(void);

/* Probing, rebooting into the bootloader, knocking, writing and restarting - the five things
 * the bring-up state machine does to a radio. */
void radio_port_boot_port(zb_boot_port_t *out);

/* Start the Zigbee coordinator on top of a radio that is running our image. False at once while
 * paused (radio_port_paused). */
bool radio_port_stack_start(void);

/* What the radio's Spinel client said before it last restarted the panel. */
typedef enum {
    RADIO_PORT_STOP_NONE = 0,     /* nothing: it timed out, or crashed while starting */
    RADIO_PORT_STOP_INCOMPATIBLE, /* its RCP firmware doesn't suit the stack */
    RADIO_PORT_STOP_NO_RESET,     /* it didn't answer after a reset */
} radio_port_stop_t;

const char *radio_port_stop_words(radio_port_stop_t why);

/* True when the radio restarted the panel on its last starts (twice in a row): the stack is not
 * started again until radio_port_resume(). fails and why may be NULL. */
bool radio_port_paused(unsigned *fails, radio_port_stop_t *why);
void radio_port_resume(void);
/* The radio is still answering. */
bool radio_port_alive(void);
/* Hand whatever the stack has heard - joins, leaves, attribute reports, IAS notifications - to
 * the portable source. Called from the radio's task with the zone hub's lock taken as needed. */
void radio_port_pump(zsrc_zb_t *src, uint64_t now);
/* `zigbee trace on|off` (devices.md §12): the pump prints each event it hands the source itself
 * (the reports are printed by the service's sink). */
void radio_port_set_trace(bool on);
/* Events the stack's callbacks could not queue since start: `dropped` of any kind but the alarm
 * path, `alarm_lost` of the alarm path (a zone status, an enroll request, an ACE command; README
 * "Rules"). Safe from any task. */
void radio_port_queue_counts(uint32_t *dropped, uint32_t *alarm_lost);

/* The formed network, for the Settings page and the console. */
void radio_port_network(uint8_t *channel, uint16_t *pan_id);

/* What zsrc_zb_radio_t asks of the stack. */
bool radio_port_permit_join(uint16_t seconds);
bool radio_port_ias_cie_write(uint64_t ieee, uint8_t endpoint);
bool radio_port_ias_enroll_response(uint64_t ieee, uint8_t endpoint, uint8_t zone_id);
bool radio_port_read_attribute(uint64_t ieee, uint8_t endpoint, uint16_t cluster, uint16_t attr);
bool radio_port_remove_device(uint64_t ieee);
bool radio_port_energy_scan(void);

/* Install codes (ZB-002): the code printed on a device (with its CRC, 8/10/14/18 bytes), given
 * to the trust center before that device joins, so it joins on a key derived from it rather
 * than the well-known one. _ok checks the length and CRC only. */
bool radio_port_install_code_ok(const uint8_t *ic, size_t len);
bool radio_port_install_code(uint64_t ieee, const uint8_t *ic, size_t len);

/* ---- BL-143 (docs/features/20 devices.md §2-§3) ---- */

/* Where the radio side's reports go (zbz_panel_on_report's shape): called by radio_port_pump with
 * the zone lock held, never from the stack's task. Set it with the zone lock held; NULL drops
 * them (and lets the stack answer keypads' IAS ACE commands itself, as before). */
typedef void (*radio_port_report_fn)(void *ctx, uint64_t now, const zsrc_zb_report_t *r);
void radio_port_set_report_sink(radio_port_report_fn fn, void *ctx);

/* The hooks appended to zsrc_zb_radio_t, one for one (the zone lock held; each takes the stack's
 * lock briefly). Tagged ones refuse tag 0 and end with ZSRC_ZB_REP_DONE through the sink. */
bool radio_port_node_desc_req(uint64_t ieee, uint16_t nwk, uint32_t tag);
bool radio_port_power_desc_req(uint64_t ieee, uint16_t nwk, uint32_t tag);
bool radio_port_active_ep_req(uint64_t ieee, uint16_t nwk, uint32_t tag);
bool radio_port_simple_desc_req(uint64_t ieee, uint16_t nwk, uint8_t endpoint, uint32_t tag);
bool radio_port_read_attributes(uint64_t ieee, uint8_t endpoint, uint16_t cluster,
                                const uint16_t *attrs, uint8_t count, uint32_t tag);
bool radio_port_bind(uint64_t ieee, uint8_t endpoint, uint16_t cluster, uint32_t tag);
bool radio_port_configure_reporting(uint64_t ieee, uint8_t endpoint, const zsrc_zb_reporting_t *r,
                                    uint32_t tag);
bool radio_port_leave(uint64_t ieee, uint16_t nwk, uint32_t tag);
bool radio_port_forget(uint64_t ieee);
bool radio_port_identify(uint64_t ieee, uint8_t endpoint, uint16_t seconds, uint32_t tag);
bool radio_port_ias_test_mode(uint64_t ieee, uint8_t endpoint, uint8_t seconds, uint32_t tag);
bool radio_port_zcl_command(uint64_t ieee, uint8_t endpoint, const zsrc_zb_zcl_t *cmd,
                            uint32_t tag);
/* ZB-M6: a ZCL Default Response (a global command, which zsrc_zb_zcl_t can't carry) to `command`
 * of sequence `tsn` from `ieee`'s `endpoint`, on `cluster`, with ZCL `status`: the keypad
 * policy's refusals (zbz_ace.h zba_port_t.default_rsp). to_client: the panel answers as the
 * cluster's server. Ends at once (sent) with its REP_DONE. */
bool radio_port_zcl_default_rsp(uint64_t ieee, uint8_t endpoint, uint16_t cluster, bool to_client,
                                uint8_t tsn, uint8_t command, uint8_t status, uint32_t tag);
/* install_code with len 0 takes the code back. */
bool radio_port_install_code_set(uint64_t ieee, const uint8_t *code, uint8_t len);
bool radio_port_codes_only(bool on);
bool radio_port_set_channel(uint8_t channel, uint32_t tag);
bool radio_port_reset_network(uint32_t tag);
bool radio_port_channel_survey(uint32_t tag);

/* Answer `ieee`'s Poll Control check-ins with fast polling for the next `seconds` (0: stop
 * asking). Without one, a check-in gets fast polling only while a request to it waits. */
bool radio_port_wake_window(uint64_t ieee, uint16_t seconds);

/* Stop the stack (its main loop ends, every waiting request ends NO_RADIO, the UART is let go):
 * the service then brings the radio up again through the probe, the stored network kept. */
bool radio_port_restart(void);

#ifdef __cplusplus
}
#endif

#endif /* RADIO_PORT_H */
