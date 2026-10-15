/* voice_walk_smoke - voice_walk.c on its own, windowless and fast: the same scripted check the
 * preview's walk ends with (docs/features/08 voice-first-build.md D15). argv[1]: the folder whose
 * sibling "voice" gets the WAV files (default voice_walk_out/shots). */
#include <stdio.h>
#include <sys/stat.h>

#include "voice_walk.h"

int main(int argc, char **argv)
{
    const char *shots = argc > 1 ? argv[1] : "voice_walk_out/shots";
    char parent[260];
    snprintf(parent, sizeof(parent), "%s", shots);
#if defined(_WIN32)
    mkdir("voice_walk_out");
    mkdir(parent);
#else
    mkdir("voice_walk_out", 0755);
    mkdir(parent, 0755);
#endif
    return voice_walk(shots) == 0 ? 0 : 1;
}
