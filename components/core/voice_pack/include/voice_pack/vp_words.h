/*
 * vp_words.h - the portable logic of the spoken-name editor (docs/features/08 voice-first-build.md
 * D03 "The touchscreen editor"): "Use zone name" turns a zone's display name into a recipe of
 * vocabulary entries, and the word lists page the selectable vocabulary alphabetically for the
 * category pickers and the submitted "Find a word" search. Both only read a pack that vp_open()
 * accepted; no heap, no globals, no state between calls.
 *
 * Plain C11, portable (components/core rules).
 */
#ifndef VOICE_PACK_VP_WORDS_H
#define VOICE_PACK_VP_WORDS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "voice_pack/voice_pack.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Use zone name ---- */
#define VP_MATCH_SPANS_MAX 4u    /* unmatched spans kept for the screen's message */
#define VP_NAME_SCAN_MAX 255u    /* longer names are refused (VP_MATCH_TOO_MANY): zone names are 47 bytes */
#define VP_WORDS_PAGE 64u        /* choices per page of a word list */

typedef enum {
    VP_MATCH_OK = 0,
    VP_MATCH_NO_WORDS,   /* nothing but spaces and punctuation: ids/count are empty */
    VP_MATCH_UNMATCHED,  /* some text has no vocabulary entry: see span[] */
    VP_MATCH_TOO_MANY,   /* the entries needed exceed VP_RECIPE_MAX (or the name is over 255 bytes) */
    VP_MATCH_TOO_LONG,   /* more than 10 s of speech (vp_check_recipe) */
} vp_match_result_t;

typedef struct {
    uint16_t start;      /* byte offset in the name */
    uint16_t length;     /* bytes */
} vp_span_t;

typedef struct {
    vp_match_result_t result;
    uint16_t ids[VP_RECIPE_MAX];       /* the recipe: valid when result is VP_MATCH_OK; on TOO_MANY and
                                          TOO_LONG the entries found (at most 8), not to be saved */
    uint8_t count;                     /* entries in ids[] */
    uint16_t entries;                  /* entries the whole name needs (counts past 8), if it all matched */
    vp_span_t span[VP_MATCH_SPANS_MAX]; /* the first unmatched spans, in order */
    uint8_t span_count;                /* spans kept in span[] (at most 4) */
    uint16_t unmatched_total;          /* unmatched spans in all */
} vp_name_match_t;

/* Matches `name` (UTF-8, NUL-terminated) to vocabulary entries of the four selectable categories:
 * case-insensitive ASCII; whitespace trimmed and collapsed, punctuation and hyphens separate words;
 * at each position the longest complete display-text phrase ("Living room" is one entry; the
 * lowest ID wins a tie); digit runs 0-999 are read with the number.N entries (42 -> Forty Two,
 * 128 -> One Hundred Twenty Eight, no "and"); a run of at most three digits with a leading zero is
 * read digit by digit (007 -> Zero Zero Seven); longer runs, 1000+ and digits glued to letters
 * ("2nd", "B2") are unmatched, as is any text with no entry (non-ASCII included). Returns true
 * and result VP_MATCH_OK only for a complete recipe that vp_check_recipe() accepts; on anything
 * else the caller keeps its current recipe. NULL pack or name: VP_MATCH_NO_WORDS, false. */
bool vp_match_name(const vp_pack_t *pack, const char *name, vp_name_match_t *out);

/* ---- Word lists ---- */
typedef struct {
    uint16_t ids[VP_WORDS_PAGE];  /* clip IDs of this page, alphabetical */
    uint8_t count;                /* entries in ids[] */
    uint16_t total;               /* matching words in all */
    uint16_t pages;               /* pages of VP_WORDS_PAGE (0 when nothing matches) */
    uint16_t page;                /* the page asked for */
} vp_word_page_t;

/* One page (0-based) of the selectable words of `category` (1-4; 0 = all four) in
 * case-insensitive alphabetical order of display text, ties by ID. `prefix` (NULL or empty or only
 * spaces = every word) matches the start of the display text or the start of any word in it,
 * case-insensitive ASCII; spaces at its ends are ignored. Status and walk clips never appear. A page past
 * the last is empty with total and pages still set. False for NULL arguments. */
bool vp_list_words(const vp_pack_t *pack, uint8_t category, const char *prefix, uint16_t page,
                   vp_word_page_t *out);

#ifdef __cplusplus
}
#endif

#endif /* VOICE_PACK_VP_WORDS_H */
