#pragma once

// How dates and times are written: 24 or 12 hours, and the day or the month first (9/11 or 11/9). The console has no
// setting for it (HOME writes it by region); NSA's Date and time screen picks it or leaves it to the region
// (fbi/datetime.c sets it here). All times are the console's clock, which is local time with no zone: libctru's
// time() is read with gmtime(), never converted.

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

void datefmt_set(bool clock24, bool monthFirst);
bool datefmt_clock24();
bool datefmt_month_first();

// "14:05" or "2:05 PM".
void datefmt_time(char* out, size_t size, const struct tm* tm);
// "09/11" or "11/09".
void datefmt_day_month(char* out, size_t size, const struct tm* tm);
// "09/11/2026" or "11/09/2026".
void datefmt_date(char* out, size_t size, const struct tm* tm);
// "09/11 14:05" / "11/09 2:05 PM".
void datefmt_day_month_time(char* out, size_t size, const struct tm* tm);
// "09/11/2026 14:05" / "11/09/2026 2:05 PM".
void datefmt_date_time(char* out, size_t size, const struct tm* tm);
