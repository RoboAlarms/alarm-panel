#pragma once
/* Shared inside server_link: the phones, the push service and the link's own key. */
#include <stddef.h>
#include <stdint.h>

#include "alarm/alarm_config.h"
#include "esp_err.h"

#define SERVER_LINK_NVS_NS "srvlink"

esp_err_t phones_init(void);
void phones_review(const alarm_config_t *cfg);
/* F21: the phones of these users (bit user - 1), deleted since: they go whoever has the slot now. */
void phones_forget_users(uint64_t users);
bool phones_any(void);

/* Starts the task that pushes to paired phones (app_push.c) the first time one asks for push:
 * until then it holds no internal RAM. Called from the link's task about once a second. */
void app_push_review(void);

/* Signs with the panel's key (the relay knows it as the sender). */
bool server_link_sign(const void *msg, size_t len, uint8_t sig[64]);
void server_link_public_key(uint8_t pub[32]);
