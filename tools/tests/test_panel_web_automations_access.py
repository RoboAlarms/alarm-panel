"""docs/features/41 DESIGN.md section 8 (AUT-010, AUT-015): who may use the Panel Manager's
Automations page.

GET automations, GET automations/devices and POST automations are the system master's and the
installer's (ACC_SYSTEM); a POST is refused for an enrolled workstation's grant session and, being
outside locked_exempt, answers 409 locked while armed. No PC test starts the real server, so this
reads panel_web.c's access table the way the server decides: the first `if` in get_access /
post_access whose condition names the path gives its level, and anything unnamed is the
installer's.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WEB = ROOT / "components/services/panel_web/src"


def function_body(text, signature):
    """The function's body, its comments blanked."""
    start = text.index(signature)
    open_brace = text.index("{", start)
    depth = 0
    for i in range(open_brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                body = text[open_brace + 1:i]
                return re.sub(r"/\*.*?\*/", " ", body, flags=re.S)  # comments may hold a ';'
    raise AssertionError("no end to " + signature)


def level_for(body, path):
    """The access level the table gives `path`: the first statement up to a `return ACC_*;`
    whose text names is(path, "<path>"), else the closing default."""
    needle = 'is(path, "%s")' % path
    for stmt in re.findall(r"[^;]*?return\s+[^;]*;", body, re.S):
        if needle in stmt:
            levels = re.findall(r"ACC_[A-Z]+", stmt.split("return", 1)[1])
            return levels
    return re.findall(r"return\s+(ACC_[A-Z]+);", body)[-1:]


class AutomationsAccess(unittest.TestCase):
    def setUp(self):
        self.src = (WEB / "panel_web.c").read_text(encoding="utf-8")
        self.get_access = function_body(self.src, "static access_t get_access(")
        self.post_access = function_body(self.src, "static access_t post_access(")

    def test_get_paths_are_the_system_masters(self):
        # AUT-010 / DESIGN section 8: a partition master's GET gets 403
        for path in ("automations", "automations/devices"):
            self.assertEqual(level_for(self.get_access, path), ["ACC_SYSTEM"], path)

    def test_post_is_the_system_masters(self):
        self.assertEqual(level_for(self.post_access, "automations"), ["ACC_SYSTEM"])

    def test_devices_has_no_post(self):
        # only GET reads the pickers: a POST to it falls to the installer's default and no handler
        self.assertEqual(level_for(self.post_access, "automations/devices"), ["ACC_INSTALLER"])

    def test_acc_system_means_master_or_installer(self):
        allowed = function_body(self.src, "static bool allowed(")
        self.assertRegex(allowed, r"case ACC_SYSTEM: return a == ALARM_AUTH_INSTALLER \|\| "
                                  r"a == ALARM_AUTH_MASTER;")

    def test_edits_are_locked_while_armed(self):
        # AUT-010: not in locked_exempt, so the generic gate answers 409 locked
        exempt = function_body(self.src, "static bool locked_exempt(")
        self.assertNotIn("automations", exempt)

    def test_a_grant_session_cannot_post(self):
        # AUT-011 / AUT-015: a save makes the signed-in person the maker; never a grant
        action = function_body(self.src, "static esp_err_t action(")
        block = action[action.index('if (!strcmp(path, "automations"))'):]
        block = block[:block.index("web_automations_post(")]
        self.assertIn("if (s->grant)", block)
        self.assertIn('"password_sign_in_required"', block)
        # and the lock gate runs before it
        self.assertLess(action.index("panel_locked()"),
                        action.index('if (!strcmp(path, "automations"))'))

    def test_capability_follows_the_feature(self):
        # AUT-008: capabilities.automations only with CONFIG_ALARM_FEATURE_AUTOMATIONS
        self.assertIn('cJSON_AddBoolToObject(caps, "automations", web_automations_available());',
                      self.src)
        self.assertNotIn('"automation_arm"', self.src)  # phase C's
        web = (WEB / "web_automations.c").read_text(encoding="utf-8")
        stub = web[web.index("#else /* !CONFIG_ALARM_FEATURE_AUTOMATIONS"):]
        self.assertRegex(stub, r"bool web_automations_available\(void\)\s*\{\s*return false;")

    def test_refusals_never_say_not_allowed(self):
        # the page reads "not_allowed" as an access refusal: the service's is action_not_allowed
        web = (WEB / "web_automations.c").read_text(encoding="utf-8")
        self.assertNotRegex(web, r'answer_error\([^)]*"not_allowed"')

    def test_status_warnings_are_the_viewers_partitions(self):
        # D19, D27: the Overview's arming card and put-off note are for everyone who sees the
        # partition (status, ACC_ANYONE), never for a partition they don't see
        body = function_body(self.src, "static cJSON *status(")
        self.assertIn('web_automations_add_warnings(o, "automation_warnings", cfg, snap->partitions & mine);',
                      body)
        web = (WEB / "web_automations.c").read_text(encoding="utf-8")
        stub = web[web.index("#else /* !CONFIG_ALARM_FEATURE_AUTOMATIONS"):]
        self.assertNotIn("cJSON_Add", function_body(stub, "bool web_automations_add_warnings("))

    def test_phase_c_refusals_have_their_own_codes(self):
        # AUT-011, AUT-016, D31: the validator's arm alone and undoes arming reach the page as such
        web = (WEB / "web_automations.c").read_text(encoding="utf-8")
        check = function_body(web, "static cJSON *check_error(")
        self.assertRegex(check, r'AUTO_ERR_UNDOES_ARMING:\s*return answer_error\(http, 409, "undoes_arming"')
        self.assertRegex(check, r'AUTO_ERR_ARM_ALONE:\s*return answer_error\(http, 409, "arm_alone"')


if __name__ == "__main__":
    unittest.main()
