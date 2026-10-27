#include <string.h>

#include <3ds.h>

#include "i18n.h"

extern const i18n_entry i18n_es[];
extern const u32 i18n_es_count;

static language_id selected = LANGUAGE_SYSTEM;
static const i18n_entry* table = NULL;
static u32 table_count = 0;

// Lookups by string address, so a literal is searched for once.
#define CACHE_SIZE 1024
static const char* cache_key[CACHE_SIZE];
static const char* cache_value[CACHE_SIZE];

static language_id i18n_resolve(language_id language) {
    if(language != LANGUAGE_SYSTEM) {
        return language;
    }

    u8 system = CFG_LANGUAGE_EN;
    if(R_SUCCEEDED(CFGU_GetSystemLanguage(&system)) && system == CFG_LANGUAGE_ES) {
        return LANGUAGE_SPANISH;
    }

    return LANGUAGE_ENGLISH;
}

void i18n_set_language(language_id language) {
    selected = language < LANGUAGE_COUNT ? language : LANGUAGE_SYSTEM;

    switch(i18n_resolve(selected)) {
        case LANGUAGE_SPANISH:
            table = i18n_es;
            table_count = i18n_es_count;
            break;
        default:
            table = NULL;
            table_count = 0;
            break;
    }

    memset(cache_key, 0, sizeof(cache_key));
}

language_id i18n_get_language() {
    return selected;
}

const char* i18n_language_name(language_id language) {
    switch(language) {
        case LANGUAGE_ENGLISH:
            return "English";
        case LANGUAGE_SPANISH:
            return "Español";
        default:
            return _("System");
    }
}

const char* i18n_translate(const char* english) {
    if(english == NULL || table == NULL) {
        return english;
    }

    u32 slot = ((u32) english >> 2) % CACHE_SIZE;
    for(u32 probe = 0; probe < CACHE_SIZE; probe++) {
        u32 i = (slot + probe) % CACHE_SIZE;
        if(cache_key[i] == english) {
            return cache_value[i];
        }

        if(cache_key[i] == NULL) {
            const char* result = english;
            for(u32 e = 0; e < table_count; e++) {
                if(strcmp(table[e].english, english) == 0) {
                    result = table[e].translated;
                    break;
                }
            }

            cache_key[i] = english;
            cache_value[i] = result;
            return result;
        }
    }

    // A full cache: look it up every time.
    for(u32 e = 0; e < table_count; e++) {
        if(strcmp(table[e].english, english) == 0) {
            return table[e].translated;
        }
    }

    return english;
}
