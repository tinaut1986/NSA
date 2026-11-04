#include "foldermanifest.h"

#include <stdlib.h>
#include <string.h>

bool folder_path_ok(const char* rel) {
    if(rel[0] == '\0' || rel[0] == '/' || strchr(rel, '\\') != NULL || strchr(rel, ':') != NULL || strlen(rel) > FOLDER_PATH_MAX) {
        return false;
    }

    for(const char* part = rel; part != NULL; ) {
        const char* slash = strchr(part, '/');
        size_t len = slash != NULL ? (size_t) (slash - part) : strlen(part);
        if(len == 0 || (len == 1 && part[0] == '.') || (len == 2 && part[0] == '.' && part[1] == '.')) {
            return false;
        }

        part = slash != NULL ? slash + 1 : NULL;
    }

    for(const char* c = rel; *c != '\0'; c++) {
        if((unsigned char) *c < 0x20 || strchr("*?\"<>|", *c) != NULL) {
            return false;
        }
    }

    return true;
}

bool folder_layout_add(folder_layout* layout, const char* path, bool isDir, uint64_t size) {
    if(layout->count >= FOLDER_MANIFEST_MAX_ENTRIES || !folder_path_ok(path)) {
        return false;
    }

    if(layout->count % 256 == 0) {
        folder_entry* grown = (folder_entry*) realloc(layout->entries, ((size_t) layout->count + 256) * sizeof(folder_entry));
        if(grown == NULL) {
            return false;
        }

        layout->entries = grown;
    }

    char* copy = strdup(path);
    if(copy == NULL) {
        return false;
    }

    folder_entry* entry = &layout->entries[layout->count++];
    entry->path = copy;
    entry->size = isDir ? 0 : size;
    entry->isDir = isDir;
    entry->start = 0;
    return true;
}

bool folder_layout_finish(folder_layout* layout) {
    free(layout->files);
    layout->files = NULL;
    layout->fileCount = 0;

    uint64_t head = 8;
    for(uint32_t i = 0; i < layout->count; i++) {
        head += 1 + 2 + strlen(layout->entries[i].path) + 8;
        if(!layout->entries[i].isDir) {
            layout->fileCount++;
        }
    }

    if(head > FOLDER_MANIFEST_MAX_SIZE) {
        return false;
    }

    layout->files = (uint32_t*) calloc(layout->fileCount > 0 ? layout->fileCount : 1, sizeof(uint32_t));
    if(layout->files == NULL) {
        return false;
    }

    uint64_t at = head;
    uint32_t file = 0;
    for(uint32_t i = 0; i < layout->count; i++) {
        folder_entry* entry = &layout->entries[i];
        if(entry->isDir) {
            continue;
        }

        if(entry->size > UINT64_MAX - at) {
            return false;
        }

        entry->start = at;
        at += entry->size;
        layout->files[file++] = i;
    }

    layout->head = head;
    layout->total = at;
    return true;
}

static void put16(uint8_t* out, uint16_t value) {
    out[0] = (uint8_t) (value >> 8);
    out[1] = (uint8_t) value;
}

static void put32(uint8_t* out, uint32_t value) {
    out[0] = (uint8_t) (value >> 24);
    out[1] = (uint8_t) (value >> 16);
    out[2] = (uint8_t) (value >> 8);
    out[3] = (uint8_t) value;
}

static void put64(uint8_t* out, uint64_t value) {
    put32(out, (uint32_t) (value >> 32));
    put32(out + 4, (uint32_t) value);
}

static uint16_t get16(const uint8_t* in) {
    return (uint16_t) ((in[0] << 8) | in[1]);
}

static uint32_t get32(const uint8_t* in) {
    return ((uint32_t) in[0] << 24) | ((uint32_t) in[1] << 16) | ((uint32_t) in[2] << 8) | in[3];
}

static uint64_t get64(const uint8_t* in) {
    return ((uint64_t) get32(in) << 32) | get32(in + 4);
}

uint8_t* folder_manifest_build(const folder_layout* layout, uint32_t* size) {
    uint8_t* out = (uint8_t*) malloc((size_t) layout->head);
    if(out == NULL) {
        return NULL;
    }

    memcpy(out, "NSAF", 4);
    put32(out + 4, layout->count);

    size_t at = 8;
    for(uint32_t i = 0; i < layout->count; i++) {
        const folder_entry* entry = &layout->entries[i];
        size_t len = strlen(entry->path);

        out[at++] = entry->isDir ? 0 : 1;
        put16(out + at, (uint16_t) len);
        at += 2;
        memcpy(out + at, entry->path, len);
        at += len;
        put64(out + at, entry->size);
        at += 8;
    }

    *size = (uint32_t) at;
    return out;
}

bool folder_manifest_parse(const uint8_t* data, size_t size, folder_layout* out) {
    memset(out, 0, sizeof(*out));

    if(size < 8 || size > FOLDER_MANIFEST_MAX_SIZE || memcmp(data, "NSAF", 4) != 0) {
        return false;
    }

    uint32_t count = get32(data + 4);
    if(count > FOLDER_MANIFEST_MAX_ENTRIES) {
        return false;
    }

    size_t at = 8;
    for(uint32_t i = 0; i < count; i++) {
        if(size - at < 3) {
            folder_layout_free(out);
            return false;
        }

        uint8_t type = data[at];
        size_t len = get16(data + at + 1);
        at += 3;

        if(type > 1 || len == 0 || len > FOLDER_PATH_MAX || size - at < len + 8) {
            folder_layout_free(out);
            return false;
        }

        char path[FOLDER_PATH_MAX + 1];
        memcpy(path, data + at, len);
        path[len] = '\0';
        at += len;

        uint64_t fileSize = get64(data + at);
        at += 8;

        if(strlen(path) != len || (type == 0 && fileSize != 0) || !folder_layout_add(out, path, type == 0, fileSize)) {
            folder_layout_free(out);
            return false;
        }
    }

    if(at != size || !folder_layout_finish(out) || out->head != size) {
        folder_layout_free(out);
        return false;
    }

    return true;
}

const folder_entry* folder_layout_find(const folder_layout* layout, uint64_t offset) {
    if(offset < layout->head || offset >= layout->total) {
        return NULL;
    }

    uint32_t low = 0;
    uint32_t high = layout->fileCount;
    while(low < high) {
        uint32_t mid = low + (high - low) / 2;
        const folder_entry* entry = &layout->entries[layout->files[mid]];
        if(offset < entry->start) {
            high = mid;
        } else if(offset >= entry->start + entry->size) {
            low = mid + 1;
        } else {
            return entry;
        }
    }

    return NULL;
}

void folder_layout_free(folder_layout* layout) {
    for(uint32_t i = 0; i < layout->count; i++) {
        free(layout->entries[i].path);
    }

    free(layout->entries);
    free(layout->files);
    memset(layout, 0, sizeof(*layout));
}
