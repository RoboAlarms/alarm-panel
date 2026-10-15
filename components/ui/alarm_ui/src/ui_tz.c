/*
 * ui_tz.c - time zones offered in setup and in Date & time: a name people recognise, a few
 * places, and the POSIX TZ rule the platform applies (daylight saving included). The rules
 * follow the IANA tz database (2026).
 */
#include <string.h>

#include "ui_internal.h"

const ui_tz_t UI_TIMEZONES[] = {
    {TR_NOOP_C("time zone", "Hawaii"), TR_NOOP_C("time zone places", "Honolulu · UTC-10"), "HST10"},
    {TR_NOOP_C("time zone", "Alaska"), TR_NOOP_C("time zone places", "Anchorage · UTC-9"), "AKST9AKDT,M3.2.0,M11.1.0"},
    {TR_NOOP_C("time zone", "Pacific Time"), TR_NOOP_C("time zone places", "Los Angeles, Vancouver, Seattle · UTC-8"), "PST8PDT,M3.2.0,M11.1.0"},
    {TR_NOOP_C("time zone", "Arizona"), TR_NOOP_C("time zone places", "Phoenix (no daylight saving) · UTC-7"), "MST7"},
    {TR_NOOP_C("time zone", "Mountain Time"), TR_NOOP_C("time zone places", "Denver, Calgary, Salt Lake City · UTC-7"), "MST7MDT,M3.2.0,M11.1.0"},
    {TR_NOOP_C("time zone", "Central Time"), TR_NOOP_C("time zone places", "Chicago, Dallas, Winnipeg · UTC-6"), "CST6CDT,M3.2.0,M11.1.0"},
    {TR_NOOP_C("time zone", "Mexico City"), TR_NOOP_C("time zone places", "Mexico City, Guadalajara · UTC-6"), "CST6"},
    {TR_NOOP_C("time zone", "Eastern Time"), TR_NOOP_C("time zone places", "New York, Toronto, Miami · UTC-5"), "EST5EDT,M3.2.0,M11.1.0"},
    {TR_NOOP_C("time zone", "Colombia and Peru"), TR_NOOP_C("time zone places", "Bogota, Lima · UTC-5"), "<-05>5"},
    {TR_NOOP_C("time zone", "Atlantic Time"), TR_NOOP_C("time zone places", "Halifax, Bermuda · UTC-4"), "AST4ADT,M3.2.0,M11.1.0"},
    {TR_NOOP_C("time zone", "Chile"), TR_NOOP_C("time zone places", "Santiago · UTC-4"), "<-04>4<-03>,M9.1.6/24,M4.1.6/24"},
    {TR_NOOP_C("time zone", "Newfoundland"), TR_NOOP_C("time zone places", "St. John's · UTC-3:30"), "NST3:30NDT,M3.2.0,M11.1.0"},
    {TR_NOOP_C("time zone", "Brazil and Argentina"), TR_NOOP_C("time zone places", "Sao Paulo, Buenos Aires · UTC-3"), "<-03>3"},
    {TR_NOOP_C("time zone", "UTC"), TR_NOOP_C("time zone places", "Coordinated Universal Time"), "UTC0"},
    {TR_NOOP_C("time zone", "Iceland"), TR_NOOP_C("time zone places", "Reykjavik · UTC+0"), "GMT0"},
    {TR_NOOP_C("time zone", "United Kingdom"), TR_NOOP_C("time zone places", "London, Edinburgh · UTC+0"), "GMT0BST,M3.5.0/1,M10.5.0"},
    {TR_NOOP_C("time zone", "Ireland"), TR_NOOP_C("time zone places", "Dublin · UTC+0"), "IST-1GMT0,M10.5.0,M3.5.0/1"},
    {TR_NOOP_C("time zone", "Portugal"), TR_NOOP_C("time zone places", "Lisbon · UTC+0"), "WET0WEST,M3.5.0/1,M10.5.0"},
    {TR_NOOP_C("time zone", "West Africa"), TR_NOOP_C("time zone places", "Lagos, Kinshasa · UTC+1"), "WAT-1"},
    {TR_NOOP_C("time zone", "Central Europe"), TR_NOOP_C("time zone places", "Berlin, Paris, Madrid, Rome, Amsterdam · UTC+1"), "CET-1CEST,M3.5.0,M10.5.0/3"},
    {TR_NOOP_C("time zone", "Eastern Europe"), TR_NOOP_C("time zone places", "Athens, Helsinki, Kyiv, Bucharest · UTC+2"), "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {TR_NOOP_C("time zone", "Egypt"), TR_NOOP_C("time zone places", "Cairo · UTC+2"), "EET-2EEST,M4.5.5/0,M10.5.4/24"},
    {TR_NOOP_C("time zone", "South Africa"), TR_NOOP_C("time zone places", "Johannesburg, Harare · UTC+2"), "SAST-2"},
    {TR_NOOP_C("time zone", "Turkey"), TR_NOOP_C("time zone places", "Istanbul · UTC+3"), "<+03>-3"},
    {TR_NOOP_C("time zone", "Moscow"), TR_NOOP_C("time zone places", "Moscow, Saint Petersburg · UTC+3"), "MSK-3"},
    {TR_NOOP_C("time zone", "East Africa"), TR_NOOP_C("time zone places", "Nairobi, Addis Ababa · UTC+3"), "EAT-3"},
    {TR_NOOP_C("time zone", "Iran"), TR_NOOP_C("time zone places", "Tehran · UTC+3:30"), "<+0330>-3:30"},
    {TR_NOOP_C("time zone", "Gulf"), TR_NOOP_C("time zone places", "Dubai, Abu Dhabi, Muscat · UTC+4"), "<+04>-4"},
    {TR_NOOP_C("time zone", "Pakistan"), TR_NOOP_C("time zone places", "Karachi, Lahore · UTC+5"), "PKT-5"},
    {TR_NOOP_C("time zone", "India"), TR_NOOP_C("time zone places", "Mumbai, Delhi, Kolkata · UTC+5:30"), "IST-5:30"},
    {TR_NOOP_C("time zone", "Nepal"), TR_NOOP_C("time zone places", "Kathmandu · UTC+5:45"), "<+0545>-5:45"},
    {TR_NOOP_C("time zone", "Bangladesh"), TR_NOOP_C("time zone places", "Dhaka · UTC+6"), "<+06>-6"},
    {TR_NOOP_C("time zone", "Indochina"), TR_NOOP_C("time zone places", "Bangkok, Hanoi, Jakarta · UTC+7"), "<+07>-7"},
    {TR_NOOP_C("time zone", "China"), TR_NOOP_C("time zone places", "Beijing, Shanghai · UTC+8"), "CST-8"},
    {TR_NOOP_C("time zone", "Hong Kong"), TR_NOOP_C("time zone places", "Hong Kong · UTC+8"), "HKT-8"},
    {TR_NOOP_C("time zone", "Singapore"), TR_NOOP_C("time zone places", "Singapore, Kuala Lumpur · UTC+8"), "<+08>-8"},
    {TR_NOOP_C("time zone", "Philippines"), TR_NOOP_C("time zone places", "Manila · UTC+8"), "PST-8"},
    {TR_NOOP_C("time zone", "Western Australia"), TR_NOOP_C("time zone places", "Perth · UTC+8"), "AWST-8"},
    {TR_NOOP_C("time zone", "Japan"), TR_NOOP_C("time zone places", "Tokyo, Osaka · UTC+9"), "JST-9"},
    {TR_NOOP_C("time zone", "Korea"), TR_NOOP_C("time zone places", "Seoul · UTC+9"), "KST-9"},
    {TR_NOOP_C("time zone", "South Australia"), TR_NOOP_C("time zone places", "Adelaide · UTC+9:30"), "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
    {TR_NOOP_C("time zone", "Northern Territory"), TR_NOOP_C("time zone places", "Darwin · UTC+9:30"), "ACST-9:30"},
    {TR_NOOP_C("time zone", "Queensland"), TR_NOOP_C("time zone places", "Brisbane (no daylight saving) · UTC+10"), "AEST-10"},
    {TR_NOOP_C("time zone", "Eastern Australia"), TR_NOOP_C("time zone places", "Sydney, Melbourne, Canberra · UTC+10"), "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {TR_NOOP_C("time zone", "New Zealand"), TR_NOOP_C("time zone places", "Auckland, Wellington · UTC+12"), "NZST-12NZDT,M9.5.0,M4.1.0/3"},
};

const size_t UI_TIMEZONE_COUNT = sizeof(UI_TIMEZONES) / sizeof(UI_TIMEZONES[0]);

int ui_tz_find(const char *posix)
{
    for (size_t i = 0; i < UI_TIMEZONE_COUNT; i++) {
        if (strcmp(UI_TIMEZONES[i].posix, posix) == 0) {
            return (int)i;
        }
    }
    return -1;
}

void ui_tz_pick(ui_pick_fn fn)
{
    static ui_choice_t choices[sizeof(UI_TIMEZONES) / sizeof(UI_TIMEZONES[0])];
    for (size_t i = 0; i < UI_TIMEZONE_COUNT; i++) {
        choices[i].glyph = NULL;
        choices[i].tone = ui_tone_blue();
        choices[i].title = TR_C("time zone", UI_TIMEZONES[i].name);
        choices[i].detail = TR_C("time zone places", UI_TIMEZONES[i].places);
    }
    char tz[64] = "UTC0";
    if (S.be.get_timezone != NULL) {
        S.be.get_timezone(S.be.ctx, tz, sizeof(tz));
    }
    ui_pick_sheet(TR("Time zone"), TR("Daylight saving time switches by itself"), choices,
                  UI_TIMEZONE_COUNT, ui_tz_find(tz), fn);
}
