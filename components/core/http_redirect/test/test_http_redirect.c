/*
 * test_http_redirect.c - where a plain-HTTP request to the panel is sent
 * (components/core/http_redirect, docs/features/31 WEB-023).
 *
 * Requirements: WEB-023
 */
#include <string.h>

#include "http_redirect/http_redirect.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static char out[600];

static const char *where(const char *host, const char *uri)
{
    TEST_ASSERT_TRUE(http_redirect_location(host, "192.0.2.10", uri, 443, out, sizeof(out)));
    return out;
}

/* WEB-023: the address the person typed, the page they asked for, over HTTPS. */
static void test_the_same_page_over_https(void)
{
    TEST_ASSERT_EQUAL_STRING("https://192.0.2.10/", where("192.0.2.10", "/"));
    TEST_ASSERT_EQUAL_STRING("https://panel.local/panel.html?x=1", where("panel.local", "/panel.html?x=1"));
    TEST_ASSERT_EQUAL_STRING("https://alarm-5c81.home.arpa/", where("alarm-5c81.home.arpa:80", "/"));
    TEST_ASSERT_EQUAL_STRING("https://198.51.100.7/", where("  198.51.100.7:8080 ", "/"));
    TEST_ASSERT_EQUAL_STRING("https://[fe80::1]/", where("[fe80::1]:80", "/"));
    TEST_ASSERT_EQUAL_STRING("https://[::ffff:10.0.0.2]/a", where("[::ffff:10.0.0.2]", "/a"));
    TEST_ASSERT_TRUE(http_redirect_location("panel", NULL, "/", 8443, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("https://panel:8443/", out);
}

/* WEB-023: a Host that isn't plainly a host is never repeated - the panel's own address is
 * used, so the Location can't be split into another header or point somewhere else. */
static void test_a_strange_host_is_not_repeated(void)
{
    static const char *const BAD[] = {
        "",
        "   ",
        "evil.example\r\nSet-Cookie: x=1",
        "evil.example/path",
        "user@evil.example",
        "evil.example\\x",
        "\"quoted\"",
        "-leading.dash",
        "trailing-",
        ".dot",
        "has space",
        "host:",
        "host:0",
        "host:65536",
        "host:12ab",
        "host:80:80",
        "[fe80::1",
        "[fe80::g]",
        "[]",
        "[::1]x",
        "caf\xc3\xa9.example",
    };
    for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
        TEST_ASSERT_EQUAL_STRING_MESSAGE("https://192.0.2.10/", where(BAD[i], "/"), BAD[i]);
    }
    TEST_ASSERT_EQUAL_STRING("https://192.0.2.10/", where(NULL, "/"));
    char longest[300];
    memset(longest, 'a', sizeof(longest) - 1u);
    longest[sizeof(longest) - 1u] = '\0';
    TEST_ASSERT_EQUAL_STRING("https://192.0.2.10/", where(longest, "/")); /* over 253 */
    longest[253] = '\0';
    TEST_ASSERT_TRUE(http_redirect_location(longest, NULL, "/", 443, out, sizeof(out)));
    /* Neither usable: no redirect at all. */
    TEST_ASSERT_FALSE(http_redirect_location("bad host", "", "/", 443, out, sizeof(out)));
    TEST_ASSERT_FALSE(http_redirect_location(NULL, NULL, "/", 443, out, sizeof(out)));
}

/* WEB-023: a request target that isn't a plain path starts over at the start page. */
static void test_a_strange_path_becomes_the_start_page(void)
{
    static const char *const BAD[] = {
        "", "x", "//evil.example/", "/a b", "/a\r\nX: 1", "/a\\b", "/caf\xc3\xa9", "/\x7f",
        "http://evil.example/",
    };
    for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
        TEST_ASSERT_EQUAL_STRING_MESSAGE("https://panel/", where("panel", BAD[i]), BAD[i]);
    }
    TEST_ASSERT_EQUAL_STRING("https://panel/", where("panel", NULL));
    TEST_ASSERT_EQUAL_STRING("https://panel/api/v1/status?a=%20b#", where("panel", "/api/v1/status?a=%20b#"));
}

/* WEB-023: too long for the buffer - the start page; too small even for that - no redirect. */
static void test_bounds(void)
{
    char path[700];
    path[0] = '/';
    memset(path + 1, 'p', sizeof(path) - 2u);
    path[sizeof(path) - 1u] = '\0';
    TEST_ASSERT_EQUAL_STRING("https://panel/", where("panel", path));
    char tiny[12];
    TEST_ASSERT_FALSE(http_redirect_location("panel.local", NULL, "/", 443, tiny, sizeof(tiny)));
    char host[8];
    TEST_ASSERT_FALSE(http_redirect_host_ok("panel.local", host, sizeof(host)));
    TEST_ASSERT_TRUE(http_redirect_host_ok("panel:80", host, sizeof(host)));
    TEST_ASSERT_EQUAL_STRING("panel", host);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_same_page_over_https);
    RUN_TEST(test_a_strange_host_is_not_repeated);
    RUN_TEST(test_a_strange_path_becomes_the_start_page);
    RUN_TEST(test_bounds);
    return UNITY_END();
}
