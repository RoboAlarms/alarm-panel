/*
 * test_zb_bringup.c - getting the sensor radio running by itself (components/core/zigbee_host,
 * docs/features/20 ZB-001). The owner's requirement is plug and play: flash the panel, push the
 * ESP32-H2 module into the wireless socket, and be finished. The socket gives the panel one
 * UART and no reset or boot line, so every path below is about what can still be done over that
 * one wire - and about saying something useful when nothing can.
 *
 * The radio here is a fake whose behaviour each test chooses: already in a ROM bootloader
 * accessible on its connected UART, running an older image of ours, running somebody
 * else's firmware entirely, missing, or failing halfway through a write.
 *
 * Requirements: ZB-001 (docs/features/20)
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "zigbee_host/zb_bringup.h"

#define OURS "alarmpanel-h2-rcp/1.4.0"
#define OLD  "alarmpanel-h2-rcp/1.2.0"

/* ---- a radio that can be made to misbehave -------------------------------------------------- */

typedef enum {
    RADIO_NONE = 0,   /* an empty socket: nothing ever answers */
    RADIO_BLANK,      /* factory fresh: no app, so its ROM bootloader is already listening */
    RADIO_OURS,       /* running the image this firmware carries */
    RADIO_OLD,        /* running an older image of ours */
    RADIO_FOREIGN,    /* running somebody else's firmware: no reboot-to-bootloader request */
} radio_kind_t;

static struct {
    radio_kind_t kind;
    bool in_bootloader;
    int written;       /* percent written so far */
    int fail_at;       /* percent at which the write fails, 101 = never */
    unsigned probes;
    unsigned syncs;
    unsigned to_bootloader;
    unsigned restarts;
    unsigned flash_runs; /* whole-image attempts begun */
    unsigned probe_waits; /* make the next N probes answer "not yet" */
} RADIO;

static uint64_t NOW;
static zb_boot_t B;

static zb_probe_t fake_probe(void *ctx, char *version, size_t len)
{
    (void)ctx;
    RADIO.probes++;
    if (RADIO.probe_waits > 0u) {
        RADIO.probe_waits--;
        return ZB_PROBE_WAIT;
    }
    if (RADIO.in_bootloader) {
        return ZB_PROBE_SILENT; /* the ROM speaks the loader protocol, not ours */
    }
    switch (RADIO.kind) {
    case RADIO_OURS:
        snprintf(version, len, "%s", OURS);
        return ZB_PROBE_OK;
    case RADIO_OLD:
        snprintf(version, len, "%s", OLD);
        return ZB_PROBE_OK;
    case RADIO_FOREIGN:
        snprintf(version, len, "%s", "elecrow-demo/0.1");
        return ZB_PROBE_OK;
    case RADIO_BLANK:
    case RADIO_NONE:
    default:
        return ZB_PROBE_SILENT;
    }
}

static bool fake_to_bootloader(void *ctx)
{
    (void)ctx;
    RADIO.to_bootloader++;
    if (RADIO.kind == RADIO_OURS || RADIO.kind == RADIO_OLD) {
        RADIO.in_bootloader = true;
        return true;
    }
    return false; /* somebody else's firmware has never heard of the request */
}

static bool fake_sync(void *ctx)
{
    (void)ctx;
    RADIO.syncs++;
    if (RADIO.kind == RADIO_BLANK) {
        RADIO.in_bootloader = true; /* the fake explicitly models a listening bootloader */
    }
    return RADIO.in_bootloader;
}

static int fake_flash_step(void *ctx)
{
    (void)ctx;
    if (!RADIO.in_bootloader) {
        return -1;
    }
    if (RADIO.written == 0) {
        RADIO.flash_runs++;
    }
    RADIO.written += 10;
    if (RADIO.written >= RADIO.fail_at) {
        return -1;
    }
    if (RADIO.written >= 100) {
        RADIO.written = 100;
        RADIO.kind = RADIO_OURS;
    }
    return RADIO.written;
}

static void fake_flash_reset(void *ctx)
{
    (void)ctx;
    RADIO.written = 0;
}

static bool fake_restart(void *ctx)
{
    (void)ctx;
    RADIO.restarts++;
    RADIO.in_bootloader = false;
    return true;
}

static const zb_boot_port_t PORT = {
    .ctx = NULL,
    .probe = fake_probe,
    .to_bootloader = fake_to_bootloader,
    .sync = fake_sync,
    .flash_step = fake_flash_step,
    .flash_reset = fake_flash_reset,
    .restart = fake_restart,
};

void setUp(void)
{
    memset(&RADIO, 0, sizeof(RADIO));
    RADIO.fail_at = 101;
    NOW = 1000u;
    zb_boot_init(&B, &PORT, OURS);
}

void tearDown(void) {}

/* Run the machine the way the panel's task does: wake at the time it asked for, up to a limit
 * so a test can never hang. Returns the number of ticks it took. */
static unsigned run(bool may_update, unsigned max_ticks)
{
    unsigned n = 0;
    while (n < max_ticks) {
        const uint64_t next = zb_boot_next(&B);
        if (next == UINT64_MAX) {
            break;
        }
        if (next > NOW) {
            NOW = next;
        }
        zb_boot_tick(&B, NOW, may_update);
        n++;
        if (zb_boot_state(&B) == ZB_BOOT_RUNNING || zb_boot_state(&B) == ZB_BOOT_NEEDS_HELP ||
            zb_boot_state(&B) == ZB_BOOT_NO_RADIO || zb_boot_state(&B) == ZB_BOOT_WAIT_IDLE) {
            break;
        }
    }
    return n;
}

/* ZB-001: the everyday case. The radio already runs the image this firmware carries, so nothing
 * is written and the stack starts at once. */
static void test_a_radio_that_is_already_right_is_left_alone(void)
{
    RADIO.kind = RADIO_OURS;
    zb_boot_start(&B, NOW);
    run(true, 20);
    TEST_ASSERT_EQUAL_INT(ZB_BOOT_RUNNING, zb_boot_state(&B));
    TEST_ASSERT_TRUE(zb_boot_ready(&B));
    TEST_ASSERT_EQUAL_STRING(OURS, B.found);
    TEST_ASSERT_FALSE(B.updated);
    TEST_ASSERT_EQUAL_UINT(0u, RADIO.syncs);
    TEST_ASSERT_EQUAL_UINT(0u, RADIO.flash_runs);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, zb_boot_next(&B)); /* nothing more to wait for */
    TEST_ASSERT_EQUAL_STRING("Sensor radio ready", zb_boot_words(&B));
}

/* ZB-001, a fake module is already in download mode on the connected UART. Its
 * bootloader is listening. The policy probes, writes its image
 * and restarts it. This test does not prove those prerequisites on an Elecrow module. */
static void test_a_module_already_in_the_bootloader_can_be_flashed(void)
{
    RADIO.kind = RADIO_BLANK;
    zb_boot_start(&B, NOW);
    run(true, 60);
    TEST_ASSERT_EQUAL_INT(ZB_BOOT_RUNNING, zb_boot_state(&B));
    TEST_ASSERT_TRUE(B.updated);
    TEST_ASSERT_EQUAL_STRING(OURS, B.found);
    TEST_ASSERT_EQUAL_UINT(1u, RADIO.flash_runs);
    TEST_ASSERT_EQUAL_UINT(1u, RADIO.restarts);
    TEST_ASSERT_EQUAL_UINT(0u, RADIO.to_bootloader); /* nothing was running to be asked */
}

/* ZB-001: a radio running an older image of ours is asked - over the radio protocol itself - to
 * reboot into its bootloader, and is then updated. This is what makes a panel update carry the
 * radio's firmware with it. */
static void test_an_older_radio_is_updated_over_the_same_wire(void)
{
    RADIO.kind = RADIO_OLD;
    zb_boot_start(&B, NOW);
    run(true, 60);
    TEST_ASSERT_EQUAL_INT(ZB_BOOT_RUNNING, zb_boot_state(&B));
    TEST_ASSERT_TRUE(B.updated);
    TEST_ASSERT_EQUAL_UINT(1u, RADIO.to_bootloader);
    TEST_ASSERT_EQUAL_UINT(1u, RADIO.flash_runs);
    TEST_ASSERT_EQUAL_STRING(OURS, B.found);
}

/* ZB-001: the one case this wire cannot fix. Somebody else's firmware - the module's own demo,
 * say - never answers the request, and the socket has no reset or boot line. The panel says so
 * in the only terms that help: the two buttons on the module itself. */
static void test_foreign_firmware_asks_for_the_modules_own_buttons(void)
{
    RADIO.kind = RADIO_FOREIGN;
    zb_boot_start(&B, NOW);
    run(true, 60);
    TEST_ASSERT_EQUAL_INT(ZB_BOOT_NEEDS_HELP, zb_boot_state(&B));
    TEST_ASSERT_FALSE(zb_boot_ready(&B));
    TEST_ASSERT_EQUAL_STRING("Sensor radio setup required; see the installation guide",
                             zb_boot_words(&B));
    TEST_ASSERT_EQUAL_UINT(0u, RADIO.flash_runs); /* nothing was written to it */

    /* And when somebody does press them, the panel picks it up on its next look. */
    RADIO.kind = RADIO_BLANK;
    RADIO.in_bootloader = true;
    NOW = zb_boot_next(&B);
    zb_boot_tick(&B, NOW, true);
    run(true, 60);
    TEST_ASSERT_EQUAL_INT(ZB_BOOT_RUNNING, zb_boot_state(&B));
    TEST_ASSERT_TRUE(B.updated);
}

/* ZB-001: an empty socket is an empty socket, and is not reported as a broken radio. */
static void test_an_empty_socket_says_so(void)
{
    RADIO.kind = RADIO_NONE;
    zb_boot_start(&B, NOW);
    run(true, 60);
    TEST_ASSERT_EQUAL_INT(ZB_BOOT_NO_RADIO, zb_boot_state(&B));
    TEST_ASSERT_EQUAL_STRING("Sensor radio not responding", zb_boot_words(&B));
    TEST_ASSERT_EQUAL_UINT(0u, RADIO.to_bootloader);
    TEST_ASSERT_EQUAL_UINT(ZB_BOOT_SYNC_TRIES, RADIO.syncs); /* it did knock, just in case */

    /* Plug one in and it is found without anyone restarting the panel. */
    RADIO.kind = RADIO_OURS;
    NOW = zb_boot_next(&B);
    zb_boot_tick(&B, NOW, true);
    run(true, 20);
    TEST_ASSERT_EQUAL_INT(ZB_BOOT_RUNNING, zb_boot_state(&B));
}

/* ZB-001: a radio that takes its time to answer is waited for, not written over. */
static void test_a_slow_radio_is_given_time_to_answer(void)
{
    RADIO.kind = RADIO_OURS;
    RADIO.probe_waits = ZB_BOOT_PROBE_TRIES - 1u;
    zb_boot_start(&B, NOW);
    run(true, 20);
    TEST_ASSERT_EQUAL_INT(ZB_BOOT_RUNNING, zb_boot_state(&B));
    TEST_ASSERT_EQUAL_UINT(0u, RADIO.flash_runs);
    TEST_ASSERT_EQUAL_UINT(ZB_BOOT_PROBE_TRIES, RADIO.probes);
}

/* ZB-001: writing the radio takes every wireless sensor off the air for minutes, so it never
 * happens while a partition is armed. The panel says why, and does it once disarmed. */
static void test_the_radio_is_never_written_while_armed(void)
{
    RADIO.kind = RADIO_OLD;
    zb_boot_start(&B, NOW);
    run(false, 60);
    TEST_ASSERT_EQUAL_INT(ZB_BOOT_WAIT_IDLE, zb_boot_state(&B));
    TEST_ASSERT_EQUAL_STRING("Sensor radio update waits until the system is disarmed",
                             zb_boot_words(&B));
    TEST_ASSERT_EQUAL_UINT(0u, RADIO.flash_runs);
    TEST_ASSERT_EQUAL_UINT(0u, RADIO.to_bootloader);

    /* It keeps waiting rather than spinning, and asks to be looked at again. */
    const uint64_t next = zb_boot_next(&B);
    TEST_ASSERT_TRUE(next > NOW);
    NOW = next;
    zb_boot_tick(&B, NOW, false);
    TEST_ASSERT_EQUAL_INT(ZB_BOOT_WAIT_IDLE, zb_boot_state(&B));
    TEST_ASSERT_EQUAL_UINT(0u, RADIO.flash_runs);

    /* Disarmed: it gets on with it. */
    NOW = zb_boot_next(&B);
    zb_boot_tick(&B, NOW, true);
    run(true, 60);
    TEST_ASSERT_EQUAL_INT(ZB_BOOT_RUNNING, zb_boot_state(&B));
    TEST_ASSERT_TRUE(B.updated);
}

/* ZB-001: a write that fails halfway is started again from the beginning, and gives up rather
 * than looping for ever. */
static void test_a_failed_write_is_retried_then_abandoned(void)
{
    RADIO.kind = RADIO_OLD;
    RADIO.fail_at = 50;
    zb_boot_start(&B, NOW);
    run(true, 200);
    TEST_ASSERT_EQUAL_INT(ZB_BOOT_NEEDS_HELP, zb_boot_state(&B));
    TEST_ASSERT_EQUAL_UINT(ZB_BOOT_FLASH_TRIES, RADIO.flash_runs);
    TEST_ASSERT_FALSE(B.updated);

    /* With whatever was wrong put right, the next look finishes the job. */
    RADIO.fail_at = 101;
    RADIO.written = 0;
    NOW = zb_boot_next(&B);
    zb_boot_tick(&B, NOW, true);
    run(true, 200);
    TEST_ASSERT_EQUAL_INT(ZB_BOOT_RUNNING, zb_boot_state(&B));
    TEST_ASSERT_TRUE(B.updated);
}

/* ZB-001: the progress the Settings page shows while the radio is being written. */
static void test_progress_is_reported_while_writing(void)
{
    RADIO.kind = RADIO_BLANK;
    zb_boot_start(&B, NOW);
    uint8_t highest = 0u;
    bool saw_flashing = false;
    for (unsigned i = 0; i < 200u; i++) {
        const uint64_t next = zb_boot_next(&B);
        if (next == UINT64_MAX) {
            break;
        }
        if (next > NOW) {
            NOW = next;
        }
        zb_boot_tick(&B, NOW, true);
        if (zb_boot_state(&B) == ZB_BOOT_FLASH) {
            saw_flashing = true;
            TEST_ASSERT_TRUE(B.percent >= highest); /* it never goes backwards */
            highest = B.percent;
            TEST_ASSERT_EQUAL_STRING("Updating the sensor radio", zb_boot_words(&B));
        }
        if (zb_boot_state(&B) == ZB_BOOT_RUNNING) {
            break;
        }
    }
    TEST_ASSERT_TRUE(saw_flashing);
    TEST_ASSERT_EQUAL_INT(ZB_BOOT_RUNNING, zb_boot_state(&B));
}

/* ZB-001: a radio that stops answering while it was running is brought up again from scratch,
 * which is how a module pulled out and pushed back in comes back by itself. */
static void test_a_radio_that_goes_away_is_brought_up_again(void)
{
    RADIO.kind = RADIO_OURS;
    zb_boot_start(&B, NOW);
    run(true, 20);
    TEST_ASSERT_EQUAL_INT(ZB_BOOT_RUNNING, zb_boot_state(&B));

    NOW += 100000u;
    RADIO.kind = RADIO_NONE;
    zb_boot_lost(&B, NOW);
    TEST_ASSERT_EQUAL_INT(ZB_BOOT_PROBE, zb_boot_state(&B));
    run(true, 60);
    TEST_ASSERT_EQUAL_INT(ZB_BOOT_NO_RADIO, zb_boot_state(&B));

    RADIO.kind = RADIO_OURS;
    NOW = zb_boot_next(&B);
    zb_boot_tick(&B, NOW, true);
    run(true, 20);
    TEST_ASSERT_EQUAL_INT(ZB_BOOT_RUNNING, zb_boot_state(&B));
    TEST_ASSERT_TRUE(zb_boot_ready(&B));
}

/* ZB-001: a build that carries no radio image takes whatever is plugged in, rather than
 * refusing to use a perfectly good radio it cannot replace. */
static void test_a_panel_with_no_image_to_give_takes_what_is_there(void)
{
    zb_boot_init(&B, &PORT, "");
    RADIO.kind = RADIO_FOREIGN;
    zb_boot_start(&B, NOW);
    run(true, 20);
    TEST_ASSERT_EQUAL_INT(ZB_BOOT_RUNNING, zb_boot_state(&B));
    TEST_ASSERT_EQUAL_UINT(0u, RADIO.flash_runs);
}

/* Nothing here may reach through a port that was never given. */
static void test_a_bring_up_with_no_port_gives_up_quietly(void)
{
    zb_boot_init(&B, NULL, OURS);
    zb_boot_start(&B, NOW);
    run(true, 60);
    TEST_ASSERT_EQUAL_INT(ZB_BOOT_NEEDS_HELP, zb_boot_state(&B));
    TEST_ASSERT_FALSE(zb_boot_ready(&B));
}

/* An idle machine costs nothing: it is not started until the panel asks. */
static void test_nothing_happens_until_it_is_started(void)
{
    RADIO.kind = RADIO_BLANK;
    TEST_ASSERT_EQUAL_INT(ZB_BOOT_IDLE, zb_boot_state(&B));
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, zb_boot_next(&B));
    zb_boot_tick(&B, NOW + 1000000u, true);
    TEST_ASSERT_EQUAL_INT(ZB_BOOT_IDLE, zb_boot_state(&B));
    TEST_ASSERT_EQUAL_UINT(0u, RADIO.probes);
    TEST_ASSERT_EQUAL_STRING("Sensor radio off", zb_boot_words(&B));
}

/* ZB-001: permission can be withdrawn between any two task ticks. */
static void test_arming_stops_every_destructive_phase(void)
{
    const zb_boot_state_t phases[] = {ZB_BOOT_TO_BOOTLOADER, ZB_BOOT_SYNC,
                                      ZB_BOOT_FLASH, ZB_BOOT_RESTART};
    for (unsigned i = 0; i < sizeof(phases) / sizeof(phases[0]); i++) {
        setUp();
        RADIO.kind = RADIO_OLD;
        zb_boot_start(&B, NOW);
        for (unsigned j = 0; j < 60 && zb_boot_state(&B) != phases[i]; j++) {
            NOW = zb_boot_next(&B);
            zb_boot_tick(&B, NOW, true);
        }
        TEST_ASSERT_EQUAL_INT(phases[i], zb_boot_state(&B));
        const unsigned syncs = RADIO.syncs, boots = RADIO.to_bootloader;
        const unsigned restarts = RADIO.restarts;
        const int written = RADIO.written;
        NOW = zb_boot_next(&B);
        zb_boot_tick(&B, NOW, false);
        TEST_ASSERT_EQUAL_UINT(syncs, RADIO.syncs);
        TEST_ASSERT_EQUAL_UINT(boots, RADIO.to_bootloader);
        TEST_ASSERT_EQUAL_UINT(restarts, RADIO.restarts);
        TEST_ASSERT_EQUAL_INT(written, RADIO.written);
        TEST_ASSERT_TRUE(zb_boot_next(&B) > NOW);
        run(true, 60);
        TEST_ASSERT_TRUE(zb_boot_ready(&B));
    }
}

/* ZB-001: never enter the loader or call a writer without an image. */
static void test_no_image_cannot_flash_a_silent_module(void)
{
    zb_boot_init(&B, &PORT, "");
    RADIO.kind = RADIO_BLANK;
    zb_boot_start(&B, NOW);
    run(true, 60);
    TEST_ASSERT_FALSE(zb_boot_ready(&B));
    TEST_ASSERT_EQUAL_UINT(0, RADIO.syncs);
    TEST_ASSERT_EQUAL_UINT(0, RADIO.flash_runs);
}

static bool failed_restart(void *ctx) { (void)ctx; return false; }

/* ZB-001: a completed transfer is not proof that the installed image runs. */
static void test_failed_restart_is_not_reported_as_an_update(void)
{
    RADIO.kind = RADIO_BLANK;
    B.port.restart = failed_restart;
    zb_boot_start(&B, NOW);
    run(true, 60);
    TEST_ASSERT_EQUAL_INT(ZB_BOOT_NEEDS_HELP, zb_boot_state(&B));
    TEST_ASSERT_FALSE(B.updated);
}

static bool stale_restart(void *ctx)
{
    fake_restart(ctx);
    RADIO.kind = RADIO_OLD;
    return true;
}

/* ZB-001: a writer that claims success without changing the image has a bounded retry. */
static void test_wrong_version_after_flash_does_not_loop_forever(void)
{
    RADIO.kind = RADIO_OLD;
    B.port.restart = stale_restart;
    zb_boot_start(&B, NOW);
    run(true, 200);
    TEST_ASSERT_EQUAL_INT(ZB_BOOT_NEEDS_HELP, zb_boot_state(&B));
    TEST_ASSERT_EQUAL_UINT(ZB_BOOT_FLASH_TRIES, RADIO.flash_runs);
    TEST_ASSERT_FALSE(B.updated);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_arming_stops_every_destructive_phase);
    RUN_TEST(test_no_image_cannot_flash_a_silent_module);
    RUN_TEST(test_failed_restart_is_not_reported_as_an_update);
    RUN_TEST(test_wrong_version_after_flash_does_not_loop_forever);
    RUN_TEST(test_a_radio_that_is_already_right_is_left_alone);
    RUN_TEST(test_a_module_already_in_the_bootloader_can_be_flashed);
    RUN_TEST(test_an_older_radio_is_updated_over_the_same_wire);
    RUN_TEST(test_foreign_firmware_asks_for_the_modules_own_buttons);
    RUN_TEST(test_an_empty_socket_says_so);
    RUN_TEST(test_a_slow_radio_is_given_time_to_answer);
    RUN_TEST(test_the_radio_is_never_written_while_armed);
    RUN_TEST(test_a_failed_write_is_retried_then_abandoned);
    RUN_TEST(test_progress_is_reported_while_writing);
    RUN_TEST(test_a_radio_that_goes_away_is_brought_up_again);
    RUN_TEST(test_a_panel_with_no_image_to_give_takes_what_is_there);
    RUN_TEST(test_a_bring_up_with_no_port_gives_up_quietly);
    RUN_TEST(test_nothing_happens_until_it_is_started);
    return UNITY_END();
}
