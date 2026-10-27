#pragma once

// Translation. Every user-facing string goes through _("English text"): the
// English text is the key, and a language without an entry for it shows the
// English. Spanish lives in i18n_es.c.

typedef enum {
    LANGUAGE_SYSTEM,   // The console's language, when we have it; else English.
    LANGUAGE_ENGLISH,
    LANGUAGE_SPANISH,
    LANGUAGE_COUNT
} language_id;

void i18n_set_language(language_id language);
language_id i18n_get_language();
// The name of a language in that language ("Español"), or "System" translated.
const char* i18n_language_name(language_id language);

// Only for string literals: lookups are cached by address.
const char* i18n_translate(const char* english);

// For text that may not be a literal (a dialog's message, a hint): looked up
// by content every time, never cached. Not found: the same text.
const char* i18n_translate_text(const char* english);

typedef struct {
    const char* english;
    const char* translated;
} i18n_entry;

#define _(text) i18n_translate(text)
