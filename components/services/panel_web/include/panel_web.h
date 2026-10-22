#pragma once
#include "esp_err.h"
#include "ui_backend/ui_backend.h"
#include "dev_remote.h"

/* The platform owns the service. The UI only exposes these callbacks after local
 * installer authentication. All network permissions are checked again by the service. */
esp_err_t panel_web_start(const dev_remote_adapter_t *adapter);
void panel_web_get(bool *web, bool *developer);
esp_err_t panel_web_set(bool web, bool developer);
void panel_web_ui_backend(ui_backend_t *backend);
/* Only the already authenticated mTLS development adapter may issue this. One
 * use, 60 seconds, runtime developer mode required; no PIN is exposed or changed. */
bool panel_web_issue_grant(char one_time_token[65]);
/* A headless panel's BOOT button (true while held), from its firmware after panel_web_start():
 * held five seconds and let go, it opens setup recovery for five minutes (docs/features/31
 * headless-setup.md). A panel with a screen ignores it. */
void panel_web_setup_button(bool (*down)(void));
