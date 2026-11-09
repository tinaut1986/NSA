#pragma once

// Date and time (System -> Console): how NSA writes them (12/24 h, day or month first: by the console's region, or
// picked), the console's clock set by hand or from the Internet (NTP), the time zone and summer time.

typedef struct {
    s32 offset;          // Minutes from UTC in winter.
    u32 rule;            // datetime_rule.
    const char* id;      // Kept in settings.json.
    const char* places;  // Shown after the offset.
} datetime_zone;

// Pushes the format to datefmt (settings_apply calls it).
void datetime_apply();
// At start: sets the clock from the Internet once the Wi-Fi is up, when Date and time asks for it.
void datetime_start();
// The zone in use (the setting, or the country's).
const datetime_zone* datetime_zone_current();
// Sets the console's clock from the Internet: false and a reason (English, for _()) when it could not.
bool datetime_sync_now(const char** problem, s64* changedBy);

void datetime_open();
