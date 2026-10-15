/*
 * alarm_ports.h - callbacks the platform provides to the engine.
 *
 * Callbacks run synchronously inside engine calls and must NOT call back into
 * the engine (queue work instead). Every callback except hash_code is optional.
 */
#ifndef ALARM_PORTS_H
#define ALARM_PORTS_H

#include "alarm/alarm_config.h"
#include "alarm/alarm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *ctx;

    /* Every event, immediately (feeds the local event log). */
    void (*on_event)(void *ctx, const alarm_event_t *ev);

    /* An event is due for remote reporting (MQTT / Home Assistant / server).
     * Burglary alarms arrive after the abort window; aborted ones never do.
     * The event is the same one (same seq) previously passed to on_event. */
    void (*on_report)(void *ctx, const alarm_event_t *ev);

    /* Status of a partition changed; partition 0 = system-level change. */
    void (*on_status)(void *ctx, uint8_t partition);

    /* One-shot keypad sound for a partition (0 = all keypads).
     * zone is set for ALARM_SOUND_CHIME and ALARM_SOUND_WALK_TEST. */
    void (*on_sound)(void *ctx, uint8_t partition, alarm_sound_t sound, uint16_t zone);

    /* Output pattern changed. partition 0 = system output (smoke reset). */
    void (*on_output)(void *ctx, alarm_output_t output, uint8_t partition,
                      alarm_pattern_t pattern);

    /* Persistable runtime state changed (arming, bypass, memory, chime). */
    void (*on_persist)(void *ctx, const alarm_runtime_state_t *state);

    /* Configuration changed inside the engine (user codes); persist
     * alarm_engine_config(). */
    void (*on_config_changed)(void *ctx);

    /* REQUIRED. Deterministic hash of a code (PBKDF2 / SHA-256 on device). */
    alarm_hash_fn hash_code;

    /* Wall clock in ms since the Unix epoch, 0 when unknown. */
    int64_t (*get_epoch_ms)(void *ctx);

    /* Optional for infallible host hashes; REQUIRED when hash_code can fail. Monotonic
     * failure counter, sampled immediately around each hash on the owning engine task.
     * A change rejects the operation before comparison, lockout accounting or storage. */
    uint32_t (*hash_faults)(void *ctx);
} alarm_ports_t;

#ifdef __cplusplus
}
#endif

#endif /* ALARM_PORTS_H */
