#include <string.h>

#include <3ds.h>

#include "i18n.h"
#include "screen.h"

extern const i18n_entry i18n_es[];
extern const u32 i18n_es_count;
extern const i18n_entry i18n_ca[];
extern const u32 i18n_ca_count;
extern const i18n_entry i18n_fr[];
extern const u32 i18n_fr_count;
extern const i18n_entry i18n_de[];
extern const u32 i18n_de_count;
extern const i18n_entry i18n_it[];
extern const u32 i18n_it_count;
extern const i18n_entry i18n_pt[];
extern const u32 i18n_pt_count;
extern const i18n_entry i18n_ru[];
extern const u32 i18n_ru_count;
extern const i18n_entry i18n_ja[];
extern const u32 i18n_ja_count;
extern const i18n_entry i18n_zh[];
extern const u32 i18n_zh_count;
extern const i18n_entry i18n_tw[];
extern const u32 i18n_tw_count;

typedef struct {
    const char* code;               // In settings.json and the web API.
    const char* name;               // In its own language.
    const i18n_entry* entries;      // NULL for English: the keys are the text.
    const u32* count;
    int cfgLanguage;                // The console setting that picks it, or -1.
} language_def;

static const language_def languages[LANGUAGE_COUNT] = {
        [LANGUAGE_SYSTEM] = {"system", NULL, NULL, NULL, -1},
        [LANGUAGE_ENGLISH] = {"en", "English", NULL, NULL, CFG_LANGUAGE_EN},
        [LANGUAGE_SPANISH] = {"es", "Español", i18n_es, &i18n_es_count, CFG_LANGUAGE_ES},
        [LANGUAGE_CATALAN] = {"ca", "Català", i18n_ca, &i18n_ca_count, -1},   // The 3DS has no Catalan: only chosen by hand.
        [LANGUAGE_FRENCH] = {"fr", "Français", i18n_fr, &i18n_fr_count, CFG_LANGUAGE_FR},
        [LANGUAGE_GERMAN] = {"de", "Deutsch", i18n_de, &i18n_de_count, CFG_LANGUAGE_DE},
        [LANGUAGE_ITALIAN] = {"it", "Italiano", i18n_it, &i18n_it_count, CFG_LANGUAGE_IT},
        [LANGUAGE_PORTUGUESE] = {"pt", "Português", i18n_pt, &i18n_pt_count, CFG_LANGUAGE_PT},
        [LANGUAGE_RUSSIAN] = {"ru", "Русский", i18n_ru, &i18n_ru_count, CFG_LANGUAGE_RU},
        [LANGUAGE_JAPANESE] = {"ja", "日本語", i18n_ja, &i18n_ja_count, CFG_LANGUAGE_JP},
        [LANGUAGE_CHINESE_SIMPLIFIED] = {"zh", "简体中文", i18n_zh, &i18n_zh_count, CFG_LANGUAGE_ZH},
        [LANGUAGE_CHINESE_TRADITIONAL] = {"tw", "繁體中文", i18n_tw, &i18n_tw_count, CFG_LANGUAGE_TW},
};

static language_id selected = LANGUAGE_SYSTEM;
static const i18n_entry* table = NULL;
static u32 table_count = 0;

// Lookups by string address, so a literal is searched for once.
#define CACHE_SIZE 1024
static const char* cache_key[CACHE_SIZE];
static const char* cache_value[CACHE_SIZE];

language_id i18n_system_language() {
    u8 system = CFG_LANGUAGE_EN;
    if(R_SUCCEEDED(CFGU_GetSystemLanguage(&system))) {
        for(u32 i = LANGUAGE_ENGLISH; i < LANGUAGE_COUNT; i++) {
            if(languages[i].cfgLanguage == (int) system) {
                return (language_id) i;
            }
        }
    }

    return LANGUAGE_ENGLISH;
}

static language_id i18n_resolve(language_id language) {
    return language != LANGUAGE_SYSTEM ? language : i18n_system_language();
}

void i18n_set_language(language_id language) {
    selected = language < LANGUAGE_COUNT ? language : LANGUAGE_SYSTEM;

    const language_def* def = &languages[i18n_resolve(selected)];
    table = def->entries;
    table_count = def->count != NULL ? *def->count : 0;
    screen_set_han_font(def == &languages[LANGUAGE_CHINESE_SIMPLIFIED]);

    memset(cache_key, 0, sizeof(cache_key));
}

language_id i18n_get_language() {
    return selected;
}

language_id i18n_effective_language() {
    return i18n_resolve(selected);
}

const char* i18n_language_name(language_id language) {
    if(language > LANGUAGE_SYSTEM && language < LANGUAGE_COUNT) {
        return languages[language].name;
    }

    return _("System");
}

const char* i18n_language_code(language_id language) {
    return languages[language < LANGUAGE_COUNT ? language : LANGUAGE_SYSTEM].code;
}

language_id i18n_language_from_code(const char* code) {
    if(code != NULL) {
        for(u32 i = 0; i < LANGUAGE_COUNT; i++) {
            if(strcmp(languages[i].code, code) == 0) {
                return (language_id) i;
            }
        }
    }

    return LANGUAGE_SYSTEM;
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

const char* i18n_translate_text(const char* english) {
    if(english == NULL || table == NULL) {
        return english;
    }

    for(u32 e = 0; e < table_count; e++) {
        if(strcmp(table[e].english, english) == 0) {
            return table[e].translated;
        }
    }

    return english;
}

const char* i18n_translate_as(language_id language, const char* english) {
    if(english == NULL) {
        return NULL;
    }

    const language_def* def = &languages[i18n_resolve(language < LANGUAGE_COUNT ? language : LANGUAGE_SYSTEM)];
    for(u32 e = 0; def->entries != NULL && e < *def->count; e++) {
        if(strcmp(def->entries[e].english, english) == 0) {
            return def->entries[e].translated;
        }
    }

    return english;
}
