#pragma once

#include <time.h>

// Picking a date and a time on the touch screen: a month's calendar, then wheels for the hours, minutes and seconds
// (and AM / PM on a 12-hour clock), as the console's own settings and phones do. `done` gets the choice; B on the
// first page leaves without calling it.
void datepicker_display(const char* title, const struct tm* start, void* data, void (*done)(void* data, const struct tm* chosen));
