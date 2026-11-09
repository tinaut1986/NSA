#include <stdio.h>

#include "datefmt.h"

static bool datefmt_24 = true;
static bool datefmt_month = false;

void datefmt_set(bool clock24, bool monthFirst) {
    datefmt_24 = clock24;
    datefmt_month = monthFirst;
}

bool datefmt_clock24() {
    return datefmt_24;
}

bool datefmt_month_first() {
    return datefmt_month;
}

void datefmt_time(char* out, size_t size, const struct tm* tm) {
    if(datefmt_24) {
        snprintf(out, size, "%02d:%02d", tm->tm_hour, tm->tm_min);
    } else {
        int hour = tm->tm_hour % 12;
        snprintf(out, size, "%d:%02d %s", hour == 0 ? 12 : hour, tm->tm_min, tm->tm_hour < 12 ? "AM" : "PM");
    }
}

void datefmt_day_month(char* out, size_t size, const struct tm* tm) {
    snprintf(out, size, "%02d/%02d", datefmt_month ? tm->tm_mon + 1 : tm->tm_mday, datefmt_month ? tm->tm_mday : tm->tm_mon + 1);
}

void datefmt_date(char* out, size_t size, const struct tm* tm) {
    snprintf(out, size, "%02d/%02d/%04d", datefmt_month ? tm->tm_mon + 1 : tm->tm_mday, datefmt_month ? tm->tm_mday : tm->tm_mon + 1,
             tm->tm_year + 1900);
}

void datefmt_day_month_time(char* out, size_t size, const struct tm* tm) {
    char day[8];
    char time[12];
    datefmt_day_month(day, sizeof(day), tm);
    datefmt_time(time, sizeof(time), tm);
    snprintf(out, size, "%s %s", day, time);
}

void datefmt_date_time(char* out, size_t size, const struct tm* tm) {
    char day[16];
    char time[12];
    datefmt_date(day, sizeof(day), tm);
    datefmt_time(time, sizeof(time), tm);
    snprintf(out, size, "%s %s", day, time);
}
