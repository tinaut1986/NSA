#include <string.h>

#include "minire.h"

// One element of the pattern at `p`: how long it is, and whether `c` matches it.
static size_t minire_atom_length(const char* p) {
    if(*p == '\\' && p[1] != '\0') {
        return 2;
    }

    if(*p == '[') {
        const char* q = p + 1;
        if(*q == '^') {
            q++;
        }

        if(*q == ']') {
            q++;   // A ] first is one of the set.
        }

        while(*q != '\0' && *q != ']') {
            q += *q == '\\' && q[1] != '\0' ? 2 : 1;
        }

        return *q == ']' ? (size_t) (q - p + 1) : strlen(p);
    }

    return 1;
}

static bool minire_atom_matches(const char* p, char c) {
    if(*p == '\\' && p[1] != '\0') {
        return p[1] == c;
    }

    if(*p == '.') {
        return true;
    }

    if(*p == '[') {
        const char* q = p + 1;
        bool negate = *q == '^';
        if(negate) {
            q++;
        }

        bool found = false;
        bool first = true;
        while(*q != '\0' && (*q != ']' || first)) {
            first = false;
            char lo = *q;
            if(lo == '\\' && q[1] != '\0') {
                lo = *++q;
            }

            if(q[1] == '-' && q[2] != ']' && q[2] != '\0') {
                char hi = q[2];
                if(hi == '\\' && q[3] != '\0') {
                    hi = q[3];
                    q++;
                }

                if(c >= lo && c <= hi) {
                    found = true;
                }

                q += 3;
            } else {
                if(c == lo) {
                    found = true;
                }

                q++;
            }
        }

        return found != negate;
    }

    return *p == c;
}

// Matches `p` at `t` exactly from there; on success *end is where the match stops.
static bool minire_here(const char* p, const char* t, const char** end) {
    while(true) {
        if(*p == '\0') {
            *end = t;
            return true;
        }

        if(*p == '$' && p[1] == '\0') {
            if(*t == '\0') {
                *end = t;
                return true;
            }

            return false;
        }

        size_t len = minire_atom_length(p);
        char op = p[len];
        if(op == '*' || op == '+' || op == '?') {
            size_t max = op == '?' ? 1 : (size_t) -1;
            size_t min = op == '+' ? 1 : 0;
            size_t n = 0;
            while(n < max && t[n] != '\0' && minire_atom_matches(p, t[n])) {
                n++;
            }

            // Greedy: the longest first, giving back one at a time.
            while(true) {
                if(n >= min && minire_here(p + len + 1, t + n, end)) {
                    return true;
                }

                if(n == 0) {
                    return false;
                }

                n--;
            }
        }

        if(*t == '\0' || !minire_atom_matches(p, *t)) {
            return false;
        }

        p += len;
        t++;
    }
}

bool minire_search(const char* pattern, const char* text, size_t* start, size_t* end) {
    const char* stop = NULL;
    if(*pattern == '^') {
        if(!minire_here(pattern + 1, text, &stop)) {
            return false;
        }

        *start = 0;
        *end = (size_t) (stop - text);
        return true;
    }

    for(const char* t = text;; t++) {
        if(minire_here(pattern, t, &stop)) {
            *start = (size_t) (t - text);
            *end = (size_t) (stop - text);
            return true;
        }

        if(*t == '\0') {
            return false;
        }
    }
}
