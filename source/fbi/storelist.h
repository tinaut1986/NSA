#pragma once

// The stores the Store opens: Universal-DB, built in, and the ones the user adds (any .unistore, the format
// Universal-Updater reads), kept in sdmc:/nsa/stores.json. Opening the Store shows them first.

#define STORE_SOURCE_URL_MAX 512

typedef struct {
    char key[12];       // "" for Universal-DB (its files keep the names they always had); else 8 hex digits of the URL.
    char url[STORE_SOURCE_URL_MAX];       // The .unistore.
    char infoUrl[STORE_SOURCE_URL_MAX];   // A small file with its revision (storeInfo.infoURL), or "": the whole store tells.
    char title[64];
    char author[64];
    char description[256];
} store_source;

// The files a store keeps on the SD card.
typedef enum {
    STORE_FILE_CACHE,       // The list, boiled down (store.c).
    STORE_FILE_PACK,        // The icons, shrunk.
    STORE_FILE_MARK,        // Which revision the icons are of.
    STORE_FILE_INSTALLED,   // What the Store installed or downloaded.
    STORE_FILE_COUNT
} store_file;

void store_source_file(const store_source* source, store_file which, char* out, size_t size);
// Makes the folder a store's files go in (Universal-DB's are in sdmc:/nsa/ and sdmc:/nsa/store-icons/).
void store_source_make_dir(const store_source* source);

// store.c: the Store on `source`'s apps (copied: the caller's may go).
void store_open_source(const store_source* source);
