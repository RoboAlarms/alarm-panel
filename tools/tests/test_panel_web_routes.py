"""docs/bugs/B017: the Panel Manager's HTTPS server has room for every handler it registers.

A route that doesn't fit is refused by ESP-IDF at start-up; before B017 that aborted the panel on
every boot. No PC test starts the real server, so this reads the sources: the routes registered
when the server starts, plus the remote keypad's WebSocket, against WEB_URI_HANDLERS.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WEB = ROOT / "components/services/panel_web/src"


def handler_budget(text):
    return int(re.search(r"#define WEB_URI_HANDLERS (\d+)", text).group(1))


def bodies(text, signature):
    """The bodies of every definition that starts with this signature (a stub may come first)."""
    return [part.split("\n}\n", 1)[0] for part in text.split(signature)[1:]]


def handlers_at_start(panel_web, web_screen):
    """The handlers registered between the server's start and its redirect: route() calls, and
    the handlers web_screen_register() adds."""
    start = panel_web.index("httpd_ssl_start(&server, &c)")
    end = panel_web.index("redirect_start();", start)
    block = panel_web[start:end]
    routes = len(re.findall(r"^\s*route\(", block, re.M))
    screen = 0
    if "web_screen_register(server)" in block:
        screen = max(b.count("httpd_register_uri_handler(")
                     for b in bodies(web_screen, "void web_screen_register("))
    return routes, screen


class PanelWebRoutes(unittest.TestCase):
    def setUp(self):
        self.panel_web = (WEB / "panel_web.c").read_text(encoding="utf-8")
        self.web_screen = (WEB / "web_screen.c").read_text(encoding="utf-8")

    def test_every_handler_has_a_slot(self):
        routes, screen = handlers_at_start(self.panel_web, self.web_screen)
        self.assertGreater(routes, 0)
        self.assertEqual(screen, 1)
        budget = handler_budget(self.panel_web)
        self.assertLessEqual(routes + screen, budget,
                             "%d routes and %d WebSocket handler(s) need more than the %d slots of "
                             "WEB_URI_HANDLERS" % (routes, screen, budget))

    def test_the_server_uses_the_budget(self):
        self.assertIn("c.httpd.max_uri_handlers = WEB_URI_HANDLERS;", self.panel_web)

    def test_a_route_that_does_not_fit_never_aborts(self):
        # the panel keeps running without that page (B017)
        for body in bodies(self.panel_web, "static void route("):
            self.assertNotIn("ESP_ERROR_CHECK", body)
        for body in bodies(self.web_screen, "void web_screen_register("):
            self.assertNotIn("ESP_ERROR_CHECK", body)

    def test_the_table_that_crashed_would_fail(self):
        # B017 itself: these handlers did not fit the 10 slots the server had
        routes, screen = handlers_at_start(self.panel_web, self.web_screen)
        self.assertGreater(routes + screen, 10)


if __name__ == "__main__":
    unittest.main()
