#include "storage.h"

#include "i18n/i18n.h"
#include <stdio.h>

bool storage_allowed(const storage_sd_t *sd, storage_action_t action, uint32_t generation,
                     bool authorized, bool idle, bool leased)
{
    if (sd == NULL || !authorized || !idle || leased || sd->generation != generation ||
        sd->state == STORAGE_WORKING || sd->state == STORAGE_STARTING) return false;
    switch (action) {
    case STORAGE_CHECK: return true;
    case STORAGE_EJECT: return sd->state == STORAGE_READY || sd->state == STORAGE_UNSUPPORTED || sd->state == STORAGE_ERROR;
    case STORAGE_FORMAT: return sd->capacity_bytes > 0u &&
        (sd->state == STORAGE_READY || sd->state == STORAGE_UNSUPPORTED);
    default: return false;
    }
}

/* English for the console, the Panel Manager and MQTT; the touchscreen shows the same words in
 * its language (TR_C("storage state", name), docs/features/37). */
const char *storage_state_name(storage_state_t state)
{
    /* TR: About > Storage, the state of the SD card, a row's value. At most 31 bytes */
    switch (state) {
    case STORAGE_STARTING: return I18N_NOOP_C("storage state", "Starting");
    case STORAGE_NO_CARD: return I18N_NOOP_C("storage state", "No readable card");
    case STORAGE_READY: return I18N_NOOP_C("storage state", "Ready");
    case STORAGE_UNSUPPORTED: return I18N_NOOP_C("storage state", "Unsupported filesystem");
    case STORAGE_EJECTED: return I18N_NOOP_C("storage state", "Safe to remove");
    case STORAGE_WORKING: return I18N_NOOP_C("storage state", "Working");
    default: return I18N_NOOP_C("storage state", "Card error");
    }
}

void storage_size(char *out, size_t len, uint64_t bytes)
{
    static const char *const units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    uint64_t divisor = 1u;
    size_t unit = 0;
    while (unit < 4u && bytes / divisor >= 1024u) { divisor *= 1024u; unit++; }
    const uint64_t whole = bytes / divisor;
    const uint64_t tenth = (bytes % divisor) * 10u / divisor;
    if (unit == 0u) snprintf(out, len, "%llu B", (unsigned long long)whole);
    else snprintf(out, len, "%llu.%llu %s", (unsigned long long)whole,
                  (unsigned long long)tenth, units[unit]);
}

uint64_t storage_free(uint64_t capacity, uint64_t used)
{ return used <= capacity ? capacity - used : 0u; }

uint32_t storage_image_bytes(uint32_t payload, uint32_t slot, bool signed_v2)
{
    if (payload == 0u) return 0u;
    uint64_t size = payload;
    if (signed_v2) size = ((size + 4095u) / 4096u) * 4096u + 4096u;
    return size > 0u && size <= slot ? (uint32_t)size : 0u;
}
