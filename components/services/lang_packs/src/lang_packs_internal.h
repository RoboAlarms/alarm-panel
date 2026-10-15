/*
 * lang_packs_internal.h - what the service's console (lang_console.c) needs from lang_packs.c.
 * Not a public header.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "lang_packs.h"

/* Runs fn(arg) on the service task (an internal-RAM stack) and waits for it: for work that reads
 * flash with the cache off or writes it, asked from a task whose stack is not known. false when
 * there is no service task. One job at a time. */
bool lang_packs_run_on_task(void (*fn)(void *), void *arg);

/* Service-task context: loads the installed pack into PSRAM now (also one that failed to load for
 * want of room earlier). LANG_OK, LANG_ERR_NOT_FOUND, or LANG_ERR_FLASH / _SIGNATURE / _FORMAT /
 * _DRAFT / _NO_SPACE by what stopped it. */
lang_result_t lang_packs_load_now(const char *locale);

/* The service task's stack headroom so far (bytes; the smallest free amount it has ever had), 0
 * without a task: the `lang` console command prints it so the bench can measure the 10 KB stack. */
uint32_t lang_packs_task_stack_free(void);

typedef struct {
    char locale[16];
    esp_err_t err; /* ESP_OK: the signature in flash verifies */
} lang_verify_t;

/* Service-task context: checks the signature of every installed pack from flash. Returns how many
 * were written to out (at most max). */
size_t lang_packs_verify_all(lang_verify_t *out, size_t max);
