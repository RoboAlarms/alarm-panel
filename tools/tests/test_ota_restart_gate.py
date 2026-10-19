"""Compile the actual restart gate against controlled safety/coordination inputs."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

class RestartGateTest(unittest.TestCase):
    def test_safety_precedes_coordination(self):
        cc = shutil.which(os.environ.get('CC', 'gcc'))
        if not cc:
            self.skipTest('C compiler required (tools/host_env.ps1)')
        source = (ROOT / 'components/services/panel_ota/src/panel_ota.c').read_text()
        start = source.index('static bool restart_gate(')
        end = source.index('/* ---- the restart itself', start)
        gate = source[start:end]
        harness = r'''
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>
#define I18N_NOOP_C(context, text) text
static bool s_stop, s_cancelled, idle;
static const char *held;
static bool panel_ota_system_idle(const char **why) {
    if (!idle) *why = "armed";
    return idle;
}
static const char *restart_held(void) { return held; }
'''+gate+r'''
int main(void) {
    for (unsigned bits = 0; bits < 16; ++bits) {
        s_stop = bits & 1; s_cancelled = bits & 2;
        idle = bits & 4; held = bits & 8 ? "keypads getting update" : NULL;
        const char *why = NULL;
        bool coordinating = true;
        bool allowed = restart_gate(&why, &coordinating);
        bool safe = !s_stop && !s_cancelled && idle;
        assert(allowed == (safe && !held));
        assert(coordinating == (safe && held != NULL));
        assert(restart_allowed(&why) == allowed);
        if (!allowed) assert(why != NULL);
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as temp:
            src = Path(temp) / 'gate.c'
            exe = Path(temp) / 'gate.exe'
            src.write_text(harness)
            subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

if __name__ == '__main__':
    unittest.main()
