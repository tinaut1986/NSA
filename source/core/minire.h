#pragma once

#include <stdbool.h>
#include <stddef.h>

// A small regular expression search for the paths in Universal-DB's scripts (newlib has no regcomp): ^ and $, . any
// character, [abc] [a-z] [^/] sets, the * + ? repeats (greedy) and \ to take the next character as it is. No groups or |.
// The leftmost match: true with where it starts and ends in `text`.
bool minire_search(const char* pattern, const char* text, size_t* start, size_t* end);
