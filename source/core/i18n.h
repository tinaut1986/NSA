#pragma once

// Translation. Every user-facing string goes through _("English text"): the
// English text is the key, and a language without an entry for it shows the
// English. Each language lives in its own i18n_<code>.c; the table of them is
// in i18n.c, and tools/check_i18n.py checks that every table matches Spanish's
// keys and keeps the printf conversions.

// Order is the order of the language list. Ids are stored in settings.json as
// strings (i18n_language_code), so adding one in the middle is safe.
typedef enum {
    LANGUAGE_SYSTEM,   // The console's language, when we have it; else English.
    LANGUAGE_ENGLISH,
    LANGUAGE_SPANISH,
    LANGUAGE_CATALAN,
    LANGUAGE_FRENCH,
    LANGUAGE_GERMAN,
    LANGUAGE_ITALIAN,
    LANGUAGE_PORTUGUESE,
    LANGUAGE_RUSSIAN,
    LANGUAGE_JAPANESE,
    LANGUAGE_CHINESE_SIMPLIFIED,
    LANGUAGE_CHINESE_TRADITIONAL,
    LANGUAGE_COUNT
} language_id;

void i18n_set_language(language_id language);
language_id i18n_get_language();
// The name of a language in that language ("Español"), or "System" translated.
const char* i18n_language_name(language_id language);
// The short code kept in settings.json and used by the web API ("system", "en", "es"...).
const char* i18n_language_code(language_id language);
// The language with that code, or LANGUAGE_SYSTEM when there is none.
language_id i18n_language_from_code(const char* code);
// The language the console is set to, when NSA has it; else English.
language_id i18n_system_language();

// Only for string literals: lookups are cached by address.
const char* i18n_translate(const char* english);

// The text in a given language, whatever is selected (the language list's preview);
// LANGUAGE_SYSTEM is the console's. Not found: the English.
const char* i18n_translate_as(language_id language, const char* english);

// For text that may not be a literal (a dialog's message, a hint): looked up
// by content every time, never cached. Not found: the same text.
const char* i18n_translate_text(const char* english);

typedef struct {
    const char* english;
    const char* translated;
} i18n_entry;

#define _(text) i18n_translate(text)
