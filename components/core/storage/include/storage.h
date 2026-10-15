#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    STORAGE_STARTING, STORAGE_NO_CARD, STORAGE_READY, STORAGE_UNSUPPORTED,
    STORAGE_ERROR, STORAGE_EJECTED, STORAGE_WORKING
} storage_state_t;
typedef enum { STORAGE_CHECK, STORAGE_EJECT, STORAGE_FORMAT } storage_action_t;
typedef struct {
    storage_state_t state;
    uint32_t generation;
    uint64_t capacity_bytes, total_bytes, free_bytes;
    bool usage_valid;
    char name[16], filesystem[12], message[96];
} storage_sd_t;
#define STORAGE_PARTITIONS 16u
typedef struct { char label[17]; uint32_t bytes; } storage_partition_t;
typedef struct {
    bool chip_valid, image_valid;
    uint32_t chip_bytes, image_bytes, slot_bytes, update_bytes, allocated_bytes;
    char running_slot[17];
    size_t count;
    storage_partition_t partitions[STORAGE_PARTITIONS];
} storage_flash_t;

/* SD-003/004/006: confirmation is for one observed medium, while it is idle. */
bool storage_allowed(const storage_sd_t *sd, storage_action_t action, uint32_t generation,
                     bool authorized, bool idle, bool leased);
const char *storage_state_name(storage_state_t state);
void storage_size(char *out, size_t len, uint64_t bytes);
uint64_t storage_free(uint64_t capacity, uint64_t used);
/* STOR-001: signed RSA images occupy an aligned payload plus a signature sector. */
uint32_t storage_image_bytes(uint32_t payload, uint32_t slot, bool signed_v2);
