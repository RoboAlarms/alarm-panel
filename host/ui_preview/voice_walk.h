/* voice_walk - the preview's scripted check of what the panel says (voice_walk.c). */
#ifndef VOICE_WALK_H
#define VOICE_WALK_H

/* Runs it with the walk's screenshot folder (the WAVs go to <shots_dir>/../voice). Returns the
 * number of checks that failed, 0 when the build has no voice. Resets the simulated panel. */
int voice_walk(const char *shots_dir);

#endif
