#include <arpa/inet.h>
#include <errno.h>
#include <malloc.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <3ds.h>

#include "datetime.h"
#include "resources.h"
#include "section.h"
#include "settings.h"
#include "../core/core.h"
#include "../core/datefmt.h"

// The console's clock is local time with no zone (libctru's time() read with gmtime). Setting it from the Internet
// needs the zone: NTP answers in UTC. Rosalina (Luma3DS) sets it the same way: ptm:sysm's SetUserTime, then ptm:sets'
// SetSystemTime when NSA may open it.

typedef enum {
    RULE_NONE,
    RULE_EU,     // Last Sunday of March to the last of October, 01:00 UTC.
    RULE_US,     // Second Sunday of March to the first of November, 02:00 local.
    RULE_AU,     // South-east Australia: first Sunday of October to the first of April.
    RULE_NZ,     // Last Sunday of September to the first of April.
} datetime_rule;

static const datetime_zone datetime_zones[] = {
        {-600, RULE_NONE, "honolulu", "Hawaii"},
        {-540, RULE_US, "anchorage", "Alaska"},
        {-480, RULE_US, "los_angeles", "Los Angeles, Vancouver"},
        {-420, RULE_US, "denver", "Denver, Edmonton"},
        {-420, RULE_NONE, "phoenix", "Phoenix"},
        {-360, RULE_US, "chicago", "Chicago, Winnipeg"},
        {-360, RULE_NONE, "mexico", "Mexico City, Guatemala"},
        {-300, RULE_US, "new_york", "New York, Toronto, Miami"},
        {-300, RULE_NONE, "bogota", "Bogota, Lima, Panama"},
        {-240, RULE_US, "halifax", "Halifax"},
        {-240, RULE_NONE, "caracas", "Caracas, La Paz, Puerto Rico"},
        {-180, RULE_NONE, "sao_paulo", "Sao Paulo, Buenos Aires, Montevideo"},
        {-60, RULE_EU, "azores", "Azores"},
        {0, RULE_NONE, "utc", "UTC, Reykjavik"},
        {0, RULE_EU, "london", "London, Lisbon, Dublin, Canary Islands"},
        {60, RULE_EU, "madrid", "Madrid, Paris, Berlin, Rome"},
        {60, RULE_NONE, "lagos", "Lagos, Algiers"},
        {120, RULE_EU, "athens", "Athens, Helsinki, Bucharest, Kyiv"},
        {120, RULE_NONE, "johannesburg", "Johannesburg, Cairo"},
        {180, RULE_NONE, "moscow", "Moscow, Istanbul, Riyadh"},
        {240, RULE_NONE, "dubai", "Dubai"},
        {300, RULE_NONE, "karachi", "Karachi, Tashkent"},
        {330, RULE_NONE, "kolkata", "India"},
        {420, RULE_NONE, "bangkok", "Bangkok, Jakarta, Hanoi"},
        {480, RULE_NONE, "beijing", "Beijing, Hong Kong, Taipei, Singapore, Perth"},
        {540, RULE_NONE, "tokyo", "Tokyo, Seoul"},
        {570, RULE_AU, "adelaide", "Adelaide"},
        {600, RULE_NONE, "brisbane", "Brisbane"},
        {600, RULE_AU, "sydney", "Sydney, Melbourne"},
        {720, RULE_NZ, "auckland", "Auckland"},
};

#define ZONE_COUNT (sizeof(datetime_zones) / sizeof(*datetime_zones))

// The zone a country starts on (its capital's, or its most populous one's).
static const struct {
    const char* countries;
    const char* zone;
} datetime_country_zones[] = {
        {"ES FR DE IT NL BE LU AT CH LI SE NO DK PL CZ SK HU HR SI MT RS BA ME MK AL AD MC SM VA", "madrid"},
        {"GB IE PT IS", "london"},
        {"FI GR RO BG EE LV LT CY UA MD", "athens"},
        {"RU TR BY SA", "moscow"},
        {"ZA EG", "johannesburg"},
        {"AE", "dubai"},
        {"IN", "kolkata"},
        {"US", "new_york"},
        {"CA", "new_york"},
        {"MX GT HN SV NI CR", "mexico"},
        {"CO PE PA EC", "bogota"},
        {"VE BO PR DO", "caracas"},
        {"BR AR UY PY CL", "sao_paulo"},
        {"JP KR", "tokyo"},
        {"CN TW HK MO SG MY PH", "beijing"},
        {"TH ID VN", "bangkok"},
        {"AU", "sydney"},
        {"NZ", "auckland"},
};

// ---- the format ----

static u8 datetime_region() {
    static u8 region = 0xFF;
    if(region == 0xFF && R_FAILED(CFGU_SecureInfoGetRegion(&region))) {
        region = CFG_REGION_EUR;
    }

    return region;
}

// As the HOME Menu writes them by region: America month first and 12 hours (the HOME Menu of a European console
// writes 9/11 for 9 November); Japan, China, Korea and Taiwan month first, 24 hours.
static bool datetime_region_month_first() {
    u8 region = datetime_region();
    return region == CFG_REGION_USA || region == CFG_REGION_JPN || region == CFG_REGION_CHN || region == CFG_REGION_KOR || region == CFG_REGION_TWN;
}

static bool datetime_region_clock24() {
    return datetime_region() != CFG_REGION_USA;
}

static bool datetime_clock24() {
    u32 clock = settings_get()->datetime.clock;
    return clock == 0 ? datetime_region_clock24() : clock == 1;
}

static bool datetime_month_first() {
    u32 order = settings_get()->datetime.order;
    return order == 0 ? datetime_region_month_first() : order == 2;
}

void datetime_apply() {
    datefmt_set(datetime_clock24(), datetime_month_first());
}

// ---- zones and summer time ----

static void datetime_country(char* out) {
    out[0] = '\0';
    u8 block[4] = {0};
    u16 code = 0;
    if(R_SUCCEEDED(CFGU_GetConfigInfoBlk2(sizeof(block), 0x000B0000, block)) && R_SUCCEEDED(CFGU_GetCountryCodeString(block[3], &code)) && code != 0) {
        out[0] = (char) (code & 0xFF);
        out[1] = (char) (code >> 8);
        out[2] = '\0';
    }
}

static const datetime_zone* datetime_zone_find(const char* id) {
    for(u32 i = 0; i < ZONE_COUNT; i++) {
        if(strcmp(datetime_zones[i].id, id) == 0) {
            return &datetime_zones[i];
        }
    }

    return NULL;
}

// The country's zone, or the region's when the country is not in the table.
static const datetime_zone* datetime_zone_auto() {
    static const datetime_zone* found = NULL;
    if(found != NULL) {
        return found;
    }

    char country[3];
    datetime_country(country);
    if(country[0] != '\0') {
        for(u32 i = 0; i < sizeof(datetime_country_zones) / sizeof(*datetime_country_zones) && found == NULL; i++) {
            const char* list = datetime_country_zones[i].countries;
            for(const char* c = list; *c != '\0' && found == NULL; c += (c[2] == ' ' ? 3 : 2)) {
                if(c[0] == country[0] && c[1] == country[1]) {
                    found = datetime_zone_find(datetime_country_zones[i].zone);
                }

                if(c[2] == '\0') {
                    break;
                }
            }
        }
    }

    if(found == NULL) {
        switch(datetime_region()) {
            case CFG_REGION_USA: found = datetime_zone_find("new_york"); break;
            case CFG_REGION_JPN:
            case CFG_REGION_KOR: found = datetime_zone_find("tokyo"); break;
            case CFG_REGION_CHN:
            case CFG_REGION_TWN: found = datetime_zone_find("beijing"); break;
            case CFG_REGION_AUS: found = datetime_zone_find("sydney"); break;
            default: found = datetime_zone_find("madrid"); break;
        }
    }

    return found;
}

const datetime_zone* datetime_zone_current() {
    const datetime_zone* zone = datetime_zone_find(settings_get()->datetime.zone);
    return zone != NULL ? zone : datetime_zone_auto();
}

// Days since 1970-01-01 of a date (proleptic Gregorian).
static s64 datetime_days(s64 y, s64 m, s64 d) {
    y -= m <= 2;
    s64 era = (y >= 0 ? y : y - 399) / 400;
    s64 yoe = y - era * 400;
    s64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    s64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

// The day of the n-th Sunday of a month (n = -1: the last one).
static s64 datetime_sunday(s64 year, s64 month, int n) {
    if(n > 0) {
        s64 first = datetime_days(year, month, 1);
        s64 weekday = ((first % 7) + 11) % 7;   // 0 = Sunday (1970-01-01 was a Thursday).
        return first + (7 - weekday) % 7 + 7 * (n - 1);
    }

    s64 next = month == 12 ? datetime_days(year + 1, 1, 1) : datetime_days(year, month + 1, 1);
    s64 last = next - 1;
    s64 weekday = ((last % 7) + 11) % 7;
    return last - weekday;
}

// Whether summer time is on at a UTC moment, by the zone's rule.
static bool datetime_rule_summer(const datetime_zone* zone, s64 utc) {
    s64 local = utc + zone->offset * 60;   // Winter time.
    time_t when = (time_t) local;
    struct tm* tm = gmtime(&when);
    if(tm == NULL) {
        return false;
    }

    s64 year = tm->tm_year + 1900;
    switch(zone->rule) {
        case RULE_EU: {
            s64 start = datetime_sunday(year, 3, -1) * 86400 + 3600;
            s64 end = datetime_sunday(year, 10, -1) * 86400 + 3600;
            return utc >= start && utc < end;
        }
        case RULE_US: {
            s64 start = datetime_sunday(year, 3, 2) * 86400 + 2 * 3600;
            s64 end = datetime_sunday(year, 11, 1) * 86400 + 1 * 3600;
            return local >= start && local < end;
        }
        case RULE_AU: {
            s64 start = datetime_sunday(year, 10, 1) * 86400 + 2 * 3600;
            s64 end = datetime_sunday(year, 4, 1) * 86400 + 2 * 3600;
            return local >= start || local < end;
        }
        case RULE_NZ: {
            s64 start = datetime_sunday(year, 9, -1) * 86400 + 2 * 3600;
            s64 end = datetime_sunday(year, 4, 1) * 86400 + 2 * 3600;
            return local >= start || local < end;
        }
        default:
            return false;
    }
}

static bool datetime_summer(const datetime_zone* zone, s64 utc) {
    u32 summer = settings_get()->datetime.summer;
    return summer == 0 ? datetime_rule_summer(zone, utc) : summer == 2;
}

// ---- the clock ----

#define DATETIME_UNIX_2000 946684800LL

static Result datetime_set_clock(s64 msLocal) {
    s64 msY2k = msLocal - DATETIME_UNIX_2000 * 1000;
    Result res = ptmSysmInit();
    if(R_FAILED(res)) {
        return res;
    }

    res = PTMSYSM_SetUserTime(msY2k);
    ptmSysmExit();

    // ptm:sets is not in every build's service list: the user time above is what time() reads.
    if(R_SUCCEEDED(res) && R_SUCCEEDED(ptmSetsInit())) {
        PTMSETS_SetSystemTime(msY2k);
        ptmSetsExit();
    }

    return res;
}

#define NTP_SERVER "pool.ntp.org"
#define NTP_UNIX_DELTA 2208988800ULL
#define NTP_TRIES 3
#define NTP_WAIT_MS 2000

// UTC now from an NTP server, in milliseconds since 1970.
static bool datetime_ntp(s64* msUtc, const char** problem) {
    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo* addresses = NULL;
    if(getaddrinfo(NTP_SERVER, "123", &hints, &addresses) != 0 || addresses == NULL) {
        *problem = "Could not find the time server (" NTP_SERVER "): is the console connected to the Internet?";
        return false;
    }

    bool got = false;
    for(u32 attempt = 0; attempt < NTP_TRIES && !got; attempt++) {
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if(sock < 0) {
            break;
        }

        u8 packet[48] = {0};
        packet[0] = 0x1B;   // Version 3, client.
        u64 sent = osGetTime();
        if(sendto(sock, packet, sizeof(packet), 0, addresses->ai_addr, addresses->ai_addrlen) == sizeof(packet)) {
            struct pollfd fd = {.fd = sock, .events = POLLIN};
            if(poll(&fd, 1, NTP_WAIT_MS) > 0 && (fd.revents & POLLIN)) {
                u8 answer[48];
                if(recv(sock, answer, sizeof(answer), 0) == sizeof(answer) && (answer[0] & 0x07) == 4 && answer[1] != 0) {
                    u64 roundTrip = osGetTime() - sent;
                    u32 seconds = ((u32) answer[40] << 24) | ((u32) answer[41] << 16) | ((u32) answer[42] << 8) | answer[43];
                    u32 fraction = ((u32) answer[44] << 24) | ((u32) answer[45] << 16) | ((u32) answer[46] << 8) | answer[47];
                    *msUtc = ((s64) seconds - (s64) NTP_UNIX_DELTA) * 1000 + (s64) (((u64) fraction * 1000) >> 32) + (s64) (roundTrip / 2);
                    got = true;
                }
            }
        }

        close(sock);
    }

    freeaddrinfo(addresses);
    if(!got) {
        *problem = "The time server did not answer.";
    }

    return got;
}

static time_t datetime_last_sync = 0;   // The console's time (after setting it) of the last setting from the Internet.
static s64 datetime_last_change = 0; // What it moved the clock, in seconds.

bool datetime_sync_now(const char** problem, s64* changedBy) {
    u32 wifi = 0;
    if(R_FAILED(ACU_GetWifiStatus(&wifi)) || wifi == 0) {
        *problem = "The console is not connected to the Internet.";
        return false;
    }

    s64 msUtc = 0;
    if(!datetime_ntp(&msUtc, problem)) {
        return false;
    }

    const datetime_zone* zone = datetime_zone_current();
    s64 msLocal = msUtc + (s64) (zone->offset + (datetime_summer(zone, msUtc / 1000) ? 60 : 0)) * 60 * 1000;
    s64 before = (s64) time(NULL);
    Result res = datetime_set_clock(msLocal);
    if(R_FAILED(res)) {
        *problem = "The console did not take the new time.";
        return false;
    }

    datetime_last_sync = time(NULL);
    datetime_last_change = msLocal / 1000 - before;
    if(changedBy != NULL) {
        *changedBy = datetime_last_change;
    }

    return true;
}

// At start: once the Wi-Fi is up (it may take a while, or never come), one try.
static void datetime_start_thread(void* arg) {
    for(u32 waited = 0; waited < 120; waited++) {
        u32 wifi = 0;
        if(R_SUCCEEDED(ACU_GetWifiStatus(&wifi)) && wifi != 0) {
            const char* problem = NULL;
            datetime_sync_now(&problem, NULL);
            return;
        }

        svcSleepThread(1000000000LL);
    }
}

void datetime_start() {
    if(settings_get()->datetime.syncOnStart) {
        threadCreate(datetime_start_thread, NULL, 0x4000, 0x30, -2, true);
    }
}

// ---- the screen ----

typedef enum {
    FIELD_NOW,
    FIELD_SYNC,
    FIELD_SYNC_START,
    FIELD_ZONE,
    FIELD_SUMMER,
    FIELD_CLOCK,
    FIELD_ORDER,
    FIELD_COUNT
} dt_field;

typedef struct {
    list_item* items[FIELD_COUNT];
    dt_field current;
} dt_data;

static const char* dt_label(dt_field field) {
    switch(field) {
        case FIELD_NOW: return _("Date and time");
        case FIELD_SYNC: return _("Set the time from the Internet");
        case FIELD_SYNC_START: return _("From the Internet when NSA opens");
        case FIELD_ZONE: return _("Time zone");
        case FIELD_SUMMER: return _("Summer time");
        case FIELD_CLOCK: return _("Clock");
        case FIELD_ORDER: return _("Date order");
        default: return "";
    }
}

static void dt_offset_text(char* out, size_t size, s32 minutes) {
    s32 a = minutes < 0 ? -minutes : minutes;
    snprintf(out, size, "UTC%c%02ld:%02ld", minutes < 0 ? '-' : '+', a / 60, a % 60);
}

static void dt_zone_text(char* out, size_t size, const datetime_zone* zone) {
    char offset[16];
    dt_offset_text(offset, sizeof(offset), zone->offset);
    snprintf(out, size, "%s  %s", offset, zone->places);
}

static void dt_value(dt_field field, char* out, size_t size) {
    datetime_settings* s = &settings_get()->datetime;
    out[0] = '\0';
    switch(field) {
        case FIELD_NOW: {
            time_t now = time(NULL);
            struct tm* tm = gmtime(&now);
            if(tm != NULL) {
                datefmt_date_time(out, size, tm);
            }

            break;
        }
        case FIELD_SYNC_START: string_copy(out, s->syncOnStart ? _("Yes") : _("No"), size); break;
        case FIELD_ZONE: {
            char zone[96];
            dt_zone_text(zone, sizeof(zone), datetime_zone_current());
            if(s->zone[0] == '\0') {
                snprintf(out, size, "%s (%s)", zone, _("by country"));
            } else {
                string_copy(out, zone, size);
            }

            break;
        }
        case FIELD_SUMMER: {
            const datetime_zone* zone = datetime_zone_current();
            bool now = datetime_summer(zone, (s64) time(NULL) - zone->offset * 60);
            if(s->summer == 0) {
                snprintf(out, size, "%s (%s)", zone->rule == RULE_NONE ? _("This zone has none") : _("By the zone's dates"),
                         now ? _("on now") : _("off now"));
            } else {
                string_copy(out, s->summer == 2 ? _("Always on") : _("Never"), size);
            }

            break;
        }
        case FIELD_CLOCK:
            if(s->clock == 0) {
                snprintf(out, size, "%s (%s)", _("By region"), datetime_region_clock24() ? _("24 hours") : _("12 hours"));
            } else {
                string_copy(out, s->clock == 1 ? _("24 hours") : _("12 hours"), size);
            }

            break;
        case FIELD_ORDER:
            if(s->order == 0) {
                snprintf(out, size, "%s (%s)", _("By region"), datetime_region_month_first() ? _("month/day") : _("day/month"));
            } else {
                string_copy(out, s->order == 2 ? _("month/day") : _("day/month"), size);
            }

            break;
        default:
            break;
    }
}

static void dt_draw_row(void* data, list_item* item, float x, float y, float width, float height, bool selected) {
    dt_field field = (dt_field) (u32) item->data;
    u32 ink = 0;
    u32 muted = 0;
    widget_item_frame(x, y, width, height, selected, &ink, &muted);
    if(field == FIELD_SYNC) {
        const theme_current* t = theme();
        u32 color = selected ? ink : t->accent;
        screen_draw_icon(ICON_WORLD, x + 8, y + (height - 16) / 2, 16, color);
        widget_text_fit(item->name, x + 32, y + (height - widget_text_height(WIDGET_TEXT_SCALE)) / 2, WIDGET_TEXT_SCALE, color, width - 40);
        return;
    }

    char value[128];
    dt_value(field, value, sizeof(value));
    widget_text_fit(item->name, x + 8, y + 3, WIDGET_SMALL_SCALE, muted, width - 16);
    widget_text_fit(value, x + 8, y + height - 3 - widget_text_height(WIDGET_TEXT_SCALE), WIDGET_TEXT_SCALE, ink, width - 16);
}

static void dt_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    const theme_current* t = theme();
    float x = x1 + 10;
    float w = x2 - x1 - 20;

    time_t now = time(NULL);
    struct tm* tm = gmtime(&now);
    char clock[24] = "";
    char date[24] = "";
    if(tm != NULL) {
        datefmt_time(clock, sizeof(clock), tm);
        datefmt_date(date, sizeof(date), tm);
    }

    widget_card(x, y1 + 10, w, 74);
    screen_fill_round_rect(x + 8, y1 + 18, 36, 36, t->def->radius, t->accentSoft);
    screen_draw_icon(ICON_CLOCK, x + 10, y1 + 20, 32, t->accent);
    float big = WIDGET_TITLE_SCALE * 1.6f;
    screen_draw_text(clock, x + 56, y1 + 16, big, t->palette->text);
    float clockWidth = widget_text_width(clock, big);
    widget_text_fit(date, x + 66 + clockWidth, y1 + 16 + widget_text_height(big) - widget_text_height(WIDGET_TEXT_SCALE) - 2, WIDGET_TEXT_SCALE,
                    t->palette->textMuted, w - 76 - clockWidth);

    char zone[112];
    const datetime_zone* current = datetime_zone_current();
    dt_zone_text(zone, sizeof(zone), current);
    if(datetime_summer(current, (s64) now - current->offset * 60)) {
        size_t used = strlen(zone);
        snprintf(zone + used, sizeof(zone) - used, "  +1 h (%s)", _("summer time"));
    }

    widget_text_fit(zone, x + 56, y1 + 22 + widget_text_height(big), WIDGET_SMALL_SCALE, t->palette->textMuted, w - 64);

    char line[160];
    if(datetime_last_sync != 0) {
        time_t when = datetime_last_sync;
        struct tm* at = gmtime(&when);
        char stamp[32] = "";
        if(at != NULL) {
            datefmt_day_month_time(stamp, sizeof(stamp), at);
        }

        snprintf(line, sizeof(line), _("Set from the Internet at %s (moved %+lld s)."), stamp, (long long) datetime_last_change);
    } else {
        string_copy(line, _("The console's clock has no time zone: NSA adds the zone and summer time to the Internet's time (UTC)."), sizeof(line));
    }

    screen_draw_text_wrap(line, x + 2, y1 + 94, WIDGET_SMALL_SCALE + 0.04f, t->palette->textMuted, x + w - 2);
}

static void dt_changed() {
    settings_save();
    datetime_apply();
}

// ---- changing a field ----

// Typed in the order the screen writes dates ("09/11/2026 14:05"); hours 0-23, or 1-12 with AM / PM.
static bool dt_parse(const char* text, struct tm* out) {
    int n[5] = {0};
    int count = 0;
    bool pm = false;
    bool am = false;
    const char* c = text;
    while(*c != '\0' && count < 5) {
        if(*c >= '0' && *c <= '9') {
            n[count++] = (int) strtol(c, (char**) &c, 10);
        } else {
            c++;
        }
    }

    for(const char* l = text; *l != '\0'; l++) {
        if((l[0] == 'p' || l[0] == 'P') && (l[1] == 'm' || l[1] == 'M')) {
            pm = true;
        } else if((l[0] == 'a' || l[0] == 'A') && (l[1] == 'm' || l[1] == 'M')) {
            am = true;
        }
    }

    if(count < 5) {
        return false;
    }

    int day = datefmt_month_first() ? n[1] : n[0];
    int month = datefmt_month_first() ? n[0] : n[1];
    int year = n[2] < 100 ? 2000 + n[2] : n[2];
    int hour = n[3];
    if(pm || am) {
        if(hour < 1 || hour > 12) {
            return false;
        }

        hour = hour % 12 + (pm ? 12 : 0);
    }

    if(year < 2000 || year > 2099 || month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || n[4] > 59) {
        return false;
    }

    s64 days = datetime_days(year, month, day);
    time_t check = (time_t) (days * 86400);
    struct tm* back = gmtime(&check);
    if(back == NULL || back->tm_mday != day) {
        return false;   // 31/02 and the like.
    }

    *out = *back;
    out->tm_hour = hour;
    out->tm_min = n[4];
    return true;
}

static void dt_typed(ui_view* view, void* data, SwkbdButton button, const char* response) {
    if(button != SWKBD_BUTTON_CONFIRM) {
        return;
    }

    struct tm tm;
    if(!dt_parse(response, &tm)) {
        error_display(NULL, NULL, datefmt_month_first() ? "Not a date and time: type it as 11/09/2026 14:05 (month/day/year hours:minutes)."
                                                       : "Not a date and time: type it as 09/11/2026 14:05 (day/month/year hours:minutes).");
        return;
    }

    s64 seconds = datetime_days(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday) * 86400 + tm.tm_hour * 3600 + tm.tm_min * 60;
    Result res = datetime_set_clock(seconds * 1000);
    if(R_FAILED(res)) {
        error_display_res(NULL, NULL, res, "The console did not take the new time.");
    }
}

#define DT_CHOICE(name, field, value)                    \
    static void name(void* data) {                       \
        settings_get()->datetime.field = value;          \
        dt_changed();                                    \
    }

DT_CHOICE(dt_clock_auto, clock, 0)
DT_CHOICE(dt_clock_24, clock, 1)
DT_CHOICE(dt_clock_12, clock, 2)
DT_CHOICE(dt_order_auto, order, 0)
DT_CHOICE(dt_order_day, order, 1)
DT_CHOICE(dt_order_month, order, 2)
DT_CHOICE(dt_summer_auto, summer, 0)
DT_CHOICE(dt_summer_never, summer, 1)
DT_CHOICE(dt_summer_always, summer, 2)

static void dt_zone_set(int index) {
    datetime_settings* s = &settings_get()->datetime;
    if(index < 0) {
        s->zone[0] = '\0';
    } else {
        string_copy(s->zone, datetime_zones[index].id, sizeof(s->zone));
    }

    dt_changed();
}

#define DT_ZONE(n) static void dt_zone_##n(void* data) { dt_zone_set(n); }

DT_ZONE(0) DT_ZONE(1) DT_ZONE(2) DT_ZONE(3) DT_ZONE(4) DT_ZONE(5) DT_ZONE(6) DT_ZONE(7) DT_ZONE(8) DT_ZONE(9)
DT_ZONE(10) DT_ZONE(11) DT_ZONE(12) DT_ZONE(13) DT_ZONE(14) DT_ZONE(15) DT_ZONE(16) DT_ZONE(17) DT_ZONE(18) DT_ZONE(19)
DT_ZONE(20) DT_ZONE(21) DT_ZONE(22) DT_ZONE(23) DT_ZONE(24) DT_ZONE(25) DT_ZONE(26) DT_ZONE(27) DT_ZONE(28) DT_ZONE(29)

static void (*const dt_zone_runs[])(void*) = {
        dt_zone_0, dt_zone_1, dt_zone_2, dt_zone_3, dt_zone_4, dt_zone_5, dt_zone_6, dt_zone_7, dt_zone_8, dt_zone_9,
        dt_zone_10, dt_zone_11, dt_zone_12, dt_zone_13, dt_zone_14, dt_zone_15, dt_zone_16, dt_zone_17, dt_zone_18, dt_zone_19,
        dt_zone_20, dt_zone_21, dt_zone_22, dt_zone_23, dt_zone_24, dt_zone_25, dt_zone_26, dt_zone_27, dt_zone_28, dt_zone_29,
};

_Static_assert(sizeof(dt_zone_runs) / sizeof(*dt_zone_runs) == ZONE_COUNT, "one chooser per zone");

static void dt_zone_by_country(void* data) {
    dt_zone_set(-1);
}

static void dt_nothing(void* data) {
}

static void dt_sync_toggle(void* data) {
    settings_get()->datetime.syncOnStart = !settings_get()->datetime.syncOnStart;
    settings_save();
}

static void dt_sync() {
    const char* problem = NULL;
    s64 changed = 0;
    if(datetime_sync_now(&problem, &changed)) {
        char message[200];
        if(changed == 0) {
            string_copy(message, _("The clock was already right."), sizeof(message));
        } else {
            snprintf(message, sizeof(message), _("The clock moved %+lld seconds."), (long long) changed);
        }

        prompt_display_notify(_("Time set"), message, COLOR_TEXT, NULL, NULL, NULL);
    } else {
        error_display(NULL, NULL, problem);
    }
}

static void dt_open_field(dt_field field) {
    switch(field) {
        case FIELD_NOW: {
            time_t now = time(NULL);
            struct tm* tm = gmtime(&now);
            char value[32] = "";
            if(tm != NULL) {
                // Always 24 hours here (AM / PM may be typed too).
                snprintf(value, sizeof(value), "%02d/%02d/%04d %02d:%02d", datefmt_month_first() ? tm->tm_mon + 1 : tm->tm_mday,
                         datefmt_month_first() ? tm->tm_mday : tm->tm_mon + 1, tm->tm_year + 1900, tm->tm_hour, tm->tm_min);
            }

            osk_display(datefmt_month_first() ? _("Month/day/year hours:minutes") : _("Day/month/year hours:minutes"), value, 24, OSK_NOT_BLANK, NULL, dt_typed);
            break;
        }
        case FIELD_SYNC:
            dt_sync();
            break;
        case FIELD_SYNC_START:
            dt_sync_toggle(NULL);
            break;
        case FIELD_ZONE: {
            static sheet_action actions[ZONE_COUNT + 1];
            static char labels[ZONE_COUNT + 1][96];
            char country[3];
            datetime_country(country);
            snprintf(labels[0], sizeof(labels[0]), "%s (%s)", _("By country"), country[0] != '\0' ? country : "-");
            actions[0] = (sheet_action) {ICON_WORLD, labels[0], false, dt_zone_by_country, false, NULL};
            for(u32 i = 0; i < ZONE_COUNT; i++) {
                dt_zone_text(labels[i + 1], sizeof(labels[i + 1]), &datetime_zones[i]);
                actions[i + 1] = (sheet_action) {ICON_CLOCK, labels[i + 1], false, dt_zone_runs[i], false, NULL};
            }

            sheet_display_choice(_("Time zone"), actions, ZONE_COUNT + 1, NULL, NULL, dt_nothing);
            const datetime_zone* chosen = datetime_zone_find(settings_get()->datetime.zone);
            sheet_set_selected(chosen != NULL ? (u32) (chosen - datetime_zones) + 1 : 0);
            break;
        }
        case FIELD_SUMMER: {
            static sheet_action actions[3];
            // Sun and moon: it changes with the dates; the moon: winter time all year; the sun: summer time all year.
            actions[0] = (sheet_action) {ICON_SUN_MOON, _("By the zone's dates"), false, dt_summer_auto, false, NULL};
            actions[1] = (sheet_action) {ICON_MOON, _("Never"), false, dt_summer_never, false, NULL};
            actions[2] = (sheet_action) {ICON_SUN, _("Always on"), false, dt_summer_always, false, NULL};
            sheet_display_choice(_("Summer time"), actions, 3, NULL, NULL, dt_nothing);
            sheet_set_selected(settings_get()->datetime.summer);
            break;
        }
        case FIELD_CLOCK: {
            static sheet_action actions[3];
            static char automatic[64];
            snprintf(automatic, sizeof(automatic), "%s (%s)", _("By region"), datetime_region_clock24() ? _("24 hours") : _("12 hours"));
            actions[0] = (sheet_action) {ICON_WORLD, automatic, false, dt_clock_auto, false, NULL};
            actions[1] = (sheet_action) {ICON_CLOCK, _("24 hours"), false, dt_clock_24, false, NULL};
            actions[2] = (sheet_action) {ICON_CLOCK, _("12 hours"), false, dt_clock_12, false, NULL};
            sheet_display_choice(_("Clock"), actions, 3, NULL, NULL, dt_nothing);
            sheet_set_selected(settings_get()->datetime.clock);
            break;
        }
        case FIELD_ORDER: {
            static sheet_action actions[3];
            static char automatic[64];
            snprintf(automatic, sizeof(automatic), "%s (%s)", _("By region"), datetime_region_month_first() ? _("month/day") : _("day/month"));
            actions[0] = (sheet_action) {ICON_WORLD, automatic, false, dt_order_auto, false, NULL};
            actions[1] = (sheet_action) {ICON_CLOCK, _("day/month"), false, dt_order_day, false, NULL};
            actions[2] = (sheet_action) {ICON_CLOCK, _("month/day"), false, dt_order_month, false, NULL};
            sheet_display_choice(_("Date order"), actions, 3, NULL, NULL, dt_nothing);
            sheet_set_selected(settings_get()->datetime.order);
            break;
        }
        default:
            break;
    }
}

static void dt_close(ui_view* view, dt_data* d) {
    ui_pop();
    list_destroy(view);
    for(u32 f = 0; f < FIELD_COUNT; f++) {
        free(d->items[f]);
    }

    free(d);
}

static void dt_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    dt_data* d = (dt_data*) data;
    view->name = _("Date and time");
    view->info = _(" change    back");

    if(linked_list_size(items) == 0) {
        for(u32 f = 0; f < FIELD_COUNT; f++) {
            if(d->items[f] != NULL) {
                linked_list_add(items, d->items[f]);
            }
        }

        return;
    }

    if(hidKeysDown() & KEY_B) {
        dt_close(view, d);
        return;
    }

    if(selected != NULL && (selectedTouched || (hidKeysDown() & KEY_A))) {
        d->current = (dt_field) (u32) selected->data;
        dt_open_field(d->current);
    }
}

void datetime_open() {
    dt_data* d = (dt_data*) calloc(1, sizeof(dt_data));
    if(d == NULL) {
        error_display(NULL, NULL, "Failed to allocate date and time data.");
        return;
    }

    for(u32 f = 0; f < FIELD_COUNT; f++) {
        list_item* item = (list_item*) calloc(1, sizeof(list_item));
        if(item == NULL) {
            continue;
        }

        string_copy(item->name, dt_label((dt_field) f), sizeof(item->name));
        item->data = (void*) f;
        d->items[f] = item;
    }

    ui_view* view = list_display(_("Date and time"), "", d, dt_update, dt_draw_top);
    if(view == NULL) {
        for(u32 f = 0; f < FIELD_COUNT; f++) {
            free(d->items[f]);
        }

        free(d);
        return;
    }

    list_style style = {.rowHeight = 38, .drawRow = dt_draw_row};
    list_set_style(view, &style);
    list_show_title_bar(view);
}
