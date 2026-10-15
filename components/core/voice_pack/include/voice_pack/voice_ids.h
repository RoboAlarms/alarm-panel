/*
 * voice_ids.h - the fixed clip IDs of the voice's status and walk-test words (docs/features/08
 * voice-first-build.md D07). The C side of the ID registry (recording-script.csv): an ID is never
 * renumbered, reused or given another meaning. One constant per line, `#define VP_ID_<NAME> 0x....u`:
 * a tool test reads this file;
 * the namespace bounds below the registry rows carry no VP_ID_ prefix.
 *
 * Namespaces: 0 none; 0x0001-0x7FFF selectable vocabulary tokens (categories 1-4);
 * 0x8001-0x800A status (category 5); 0x8101-0x8106 walk test (category 6); 0xC001-0xFFFF a word's
 * ending (category 7, vocabulary schema 2): VP_ENDING_BASE + the word's ID, for words up to
 * VP_ENDING_WORD_LAST.
 */
#ifndef VOICE_PACK_VOICE_IDS_H
#define VOICE_PACK_VOICE_IDS_H

/* Bounds of the namespaces (not registry rows: no VP_ID_ prefix) */
#define VP_TOKEN_NONE 0x0000u
#define VP_TOKEN_FIRST 0x0001u
#define VP_TOKEN_LAST 0x7FFFu
#define VP_ENDING_BASE 0xC000u
#define VP_ENDING_FIRST 0xC001u
#define VP_ENDING_LAST 0xFFFFu
#define VP_ENDING_WORD_LAST 0x3FFFu

/* status (status.<name> in recording-script.csv) */
#define VP_ID_ARMING 0x8001u
#define VP_ID_ARMED 0x8002u
#define VP_ID_AWAY 0x8003u
#define VP_ID_HOME 0x8004u
#define VP_ID_NIGHT 0x8005u
#define VP_ID_EXIT_NOW 0x8006u
#define VP_ID_NO_ENTRY_DELAY 0x8007u
#define VP_ID_DISARMED 0x8008u
#define VP_ID_ENTRY 0x8009u
#define VP_ID_NOT_READY 0x800Au

/* walk test (walk.<name> in recording-script.csv) */
#define VP_ID_WALK_TEST 0x8101u
#define VP_ID_WALK_STARTED 0x8102u
#define VP_ID_WALK_ENDED 0x8103u
#define VP_ID_WALK_ENDS_SOON 0x8104u
#define VP_ID_WALK_ALL_TESTED 0x8105u
#define VP_ID_WALK_SOME_UNTESTED 0x8106u

#define VP_STATUS_FIRST VP_ID_ARMING
#define VP_STATUS_LAST VP_ID_NOT_READY
#define VP_WALK_FIRST VP_ID_WALK_TEST
#define VP_WALK_LAST VP_ID_WALK_SOME_UNTESTED
#define VP_STATUS_COUNT 10u
#define VP_WALK_COUNT 6u

#endif /* VOICE_PACK_VOICE_IDS_H */
