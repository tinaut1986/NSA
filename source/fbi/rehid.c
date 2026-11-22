#include <dirent.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <3ds.h>
#include <jansson.h>
#include <mbedtls/sha256.h>

#include "rehid.h"
#include "section.h"
#include "../core/core.h"

// Rehid's files and module (rehid.h). Nothing here draws: rehidview.c is the screen.

// libctru's session with HID (hid:USER or hid:SPVR), opened at start.
extern Handle hidHandle;

#define REHID_NSA_DIR "sdmc:" APP_DATA_DIR "rehid"
#define REHID_PREVIOUS_PATH REHID_NSA_DIR "/previous.cxi"
#define REHID_PREVIOUS_VERSION_PATH REHID_NSA_DIR "/previous-version.txt"
#define REHID_VERSION_PATH REHID_NSA_DIR "/version.txt"
#define REHID_RELEASE_URL "https://api.github.com/repos/hax0kartik/rehid/releases/latest"
#define REHID_ASSET_NAME "0004013000001D02.cxi"
#define REHID_MODULE_MAX (512 * 1024)
#define REHID_FILE_MAX (64 * 1024)
#define REHID_LUMA_MODULES "enable_external_firm_and_modules"

#define COUNT_OF(a) (sizeof(a) / sizeof(*(a)))

static bool rehid_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

// ---- the module ----

int rehid_luma_modules() {
    char value = 0;
    return utilities_luma_option_find(REHID_LUMA_MODULES, &value) < 0 ? -1 : value == '1';
}

bool rehid_set_luma_modules(bool on) {
    char value = 0;
    long offset = utilities_luma_option_find(REHID_LUMA_MODULES, &value);
    if(offset < 0) {
        return false;
    }

    return value == (on ? '1' : '0') || utilities_luma_option_set(offset, on ? '1' : '0');
}

// Rehid answers its own command 0x19 with 0; the console's HID does not know it (0xD900182F).
static bool rehid_running() {
    if(hidHandle == 0) {
        return false;
    }

    // Rehid replies with its own header (0x190040) and 0; anything else (an error reply, or a service that leaves the
    // buffer as it was, as emulators do) is not Rehid.
    u32* cmdbuf = getThreadCommandBuffer();
    cmdbuf[0] = IPC_MakeHeader(0x19, 0, 0);
    cmdbuf[1] = 0xFFFFFFFF;
    if(R_FAILED(svcSendSyncRequest(hidHandle))) {
        return false;
    }

    return cmdbuf[0] == IPC_MakeHeader(0x19, 1, 0) && cmdbuf[1] == 0;
}

rehid_state rehid_get_state() {
    bool running = rehid_running();
    if(rehid_exists(REHID_MODULE_PATH)) {
        if(running) {
            return REHID_RUNNING;
        }

        return rehid_luma_modules() == 1 ? REHID_NEEDS_RESTART : REHID_NEEDS_LUMA;
    }

    return rehid_exists(REHID_MODULE_OFF_PATH) ? REHID_OFF : REHID_NOT_INSTALLED;
}

bool rehid_set_enabled(bool on) {
    const char* from = on ? REHID_MODULE_OFF_PATH : REHID_MODULE_PATH;
    const char* to = on ? REHID_MODULE_PATH : REHID_MODULE_OFF_PATH;
    if(!rehid_exists(from)) {
        return rehid_exists(to);
    }

    remove(to);
    if(rename(from, to) != 0) {
        return false;
    }

    // The IR patch Rehid writes for itself goes with it (it is only there for Rehid).
    const char* irFrom = on ? REHID_IR_OFF_PATH : REHID_IR_PATH;
    const char* irTo = on ? REHID_IR_PATH : REHID_IR_OFF_PATH;
    if(rehid_exists(irFrom)) {
        remove(irTo);
        rename(irFrom, irTo);
    }

    return true;
}

static bool rehid_write_file(const char* path, const void* data, size_t size) {
    FILE* f = fopen(path, "wb");
    if(f == NULL) {
        return false;
    }

    bool ok = fwrite(data, 1, size, f) == size;
    ok = fclose(f) == 0 && ok;
    if(!ok) {
        remove(path);
    }

    return ok;
}

// A whole file (up to `max` bytes) into a buffer of its own, with a NUL after it.
static u8* rehid_read_file(const char* path, size_t max, size_t* size) {
    FILE* f = fopen(path, "rb");
    if(f == NULL) {
        return NULL;
    }

    u8* data = (u8*) malloc(max + 1);
    if(data != NULL) {
        *size = fread(data, 1, max + 1, f);
        if(*size > max || ferror(f)) {
            free(data);
            data = NULL;
        } else {
            data[*size] = '\0';
        }
    }

    fclose(f);
    return data;
}

// Into place through a file of its own: the old one stays until the new one is whole.
static bool rehid_replace(const char* path, const void* data, size_t size) {
    char temp[300];
    snprintf(temp, sizeof(temp), "%s.tmp", path);
    if(!rehid_write_file(temp, data, size)) {
        return false;
    }

    remove(path);
    if(rename(temp, path) != 0) {
        remove(temp);
        return false;
    }

    return true;
}

static void rehid_read_text(const char* path, char* out, size_t size) {
    out[0] = '\0';
    size_t length = 0;
    char* text = (char*) rehid_read_file(path, 64, &length);
    if(text != NULL) {
        text[strcspn(text, "\r\n")] = '\0';
        string_copy(out, text, size);
        free(text);
    }
}

void rehid_installed_version(char* out, size_t size) {
    rehid_read_text(REHID_VERSION_PATH, out, size);
}

static bool rehid_hex_digest(const char* text, u8* out) {
    if(strncmp(text, "sha256:", 7) == 0) {
        text += 7;
    }

    if(strlen(text) != 64) {
        return false;
    }

    for(u32 i = 0; i < 32; i++) {
        unsigned int byte = 0;
        if(sscanf(text + i * 2, "%2x", &byte) != 1) {
            return false;
        }

        out[i] = (u8) byte;
    }

    return true;
}

Result rehid_install_latest(char* version, size_t versionSize, const char** why) {
    *why = NULL;
    version[0] = '\0';

    json_t* release = NULL;
    Result res = http_download_json(REHID_RELEASE_URL, &release, 256 * 1024);
    if(R_FAILED(res)) {
        *why = "Could not reach Rehid's releases on GitHub (hax0kartik/rehid). Check the internet connection.";
        return res;
    }

    const char* url = NULL;
    const char* digest = NULL;
    json_int_t assetSize = 0;
    json_t* tag = json_object_get(release, "tag_name");
    if(json_is_string(tag)) {
        string_copy(version, json_string_value(tag), versionSize);
    }

    json_t* assets = json_object_get(release, "assets");
    for(size_t i = 0; json_is_array(assets) && i < json_array_size(assets); i++) {
        json_t* asset = json_array_get(assets, i);
        json_t* name = json_object_get(asset, "name");
        if(json_is_string(name) && strcmp(json_string_value(name), REHID_ASSET_NAME) == 0) {
            json_t* u = json_object_get(asset, "browser_download_url");
            json_t* d = json_object_get(asset, "digest");
            json_t* s = json_object_get(asset, "size");
            url = json_is_string(u) ? json_string_value(u) : NULL;
            digest = json_is_string(d) ? json_string_value(d) : NULL;
            assetSize = json_is_integer(s) ? json_integer_value(s) : 0;
            break;
        }
    }

    u8 expected[32];
    if(url == NULL || strncmp(url, "https://github.com/hax0kartik/rehid/", 36) != 0) {
        *why = "Rehid's latest release has no module (0004013000001D02.cxi).";
        json_decref(release);
        return R_APP_BAD_DATA;
    }

    if(digest == NULL || !rehid_hex_digest(digest, expected)) {
        *why = "GitHub gives no checksum for Rehid's module, so NSA cannot check the download.";
        json_decref(release);
        return R_APP_BAD_DATA;
    }

    if(assetSize <= 0 || assetSize > REHID_MODULE_MAX) {
        *why = "Rehid's module is not the size NSA expects.";
        json_decref(release);
        return R_APP_BAD_DATA;
    }

    char moduleUrl[512];
    string_copy(moduleUrl, url, sizeof(moduleUrl));
    json_decref(release);

    u8* module = (u8*) malloc(REHID_MODULE_MAX);
    if(module == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    u32 size = 0;
    if(R_FAILED(res = http_download_buffer(moduleUrl, &size, module, REHID_MODULE_MAX))) {
        *why = "Could not download Rehid's module from GitHub. Check the internet connection.";
        free(module);
        return res;
    }

    u8 hash[32];
    mbedtls_sha256_ret(module, size, hash, 0);
    if(size != (u32) assetSize || memcmp(hash, expected, sizeof(hash)) != 0) {
        *why = "The downloaded module is not the one GitHub describes (its checksum differs). Nothing was changed.";
        free(module);
        return R_APP_BAD_DATA;
    }

    // The module there now is kept, to go back to; a module turned off is turned on by installing.
    mkdir(REHID_NSA_DIR, 0777);
    const char* old = rehid_exists(REHID_MODULE_PATH) ? REHID_MODULE_PATH : rehid_exists(REHID_MODULE_OFF_PATH) ? REHID_MODULE_OFF_PATH : NULL;
    if(old != NULL) {
        size_t oldSize = 0;
        u8* previous = rehid_read_file(old, REHID_MODULE_MAX, &oldSize);
        if(previous != NULL) {
            if(oldSize != size || memcmp(previous, module, size) != 0) {
                char oldVersion[32];
                rehid_installed_version(oldVersion, sizeof(oldVersion));
                if(rehid_replace(REHID_PREVIOUS_PATH, previous, oldSize)) {
                    rehid_replace(REHID_PREVIOUS_VERSION_PATH, oldVersion, strlen(oldVersion));
                }
            }

            free(previous);
        }
    }

    mkdir("sdmc:/luma", 0777);
    mkdir("sdmc:/luma/sysmodules", 0777);
    bool ok = rehid_replace(REHID_MODULE_PATH, module, size);
    free(module);
    if(!ok) {
        *why = "Could not write /luma/sysmodules/0004013000001D02.cxi on the SD card.";
        return R_APP_IO_ERROR;
    }

    if(old != NULL && strcmp(old, REHID_MODULE_OFF_PATH) == 0) {
        remove(REHID_MODULE_OFF_PATH);
        if(rehid_exists(REHID_IR_OFF_PATH) && !rehid_exists(REHID_IR_PATH)) {
            rename(REHID_IR_OFF_PATH, REHID_IR_PATH);
        }
    }

    rehid_replace(REHID_VERSION_PATH, version, strlen(version));
    return 0;
}

bool rehid_has_previous() {
    return rehid_exists(REHID_PREVIOUS_PATH);
}

bool rehid_restore_previous() {
    size_t size = 0;
    u8* previous = rehid_read_file(REHID_PREVIOUS_PATH, REHID_MODULE_MAX, &size);
    if(previous == NULL || size == 0) {
        free(previous);
        return false;
    }

    const char* target = rehid_exists(REHID_MODULE_OFF_PATH) && !rehid_exists(REHID_MODULE_PATH) ? REHID_MODULE_OFF_PATH : REHID_MODULE_PATH;
    bool ok = rehid_replace(target, previous, size);
    free(previous);
    if(ok) {
        char version[32];
        rehid_read_text(REHID_PREVIOUS_VERSION_PATH, version, sizeof(version));
        rehid_replace(REHID_VERSION_PATH, version, strlen(version));
        remove(REHID_PREVIOUS_PATH);
        remove(REHID_PREVIOUS_VERSION_PATH);
    }

    return ok;
}

// ---- key names ----

static const struct {
    const char* name;     // Rehid's.
    const char* label;    // People's (English, translated when shown).
    u32 key;
} rehid_key_names[] = {
        {"A", "A", KEY_A},
        {"B", "B", KEY_B},
        {"X", "X", KEY_X},
        {"Y", "Y", KEY_Y},
        {"L", "L", KEY_L},
        {"R", "R", KEY_R},
        {"ZL", "ZL", KEY_ZL},
        {"ZR", "ZR", KEY_ZR},
        {"START", "START", KEY_START},
        {"SELECT", "SELECT", KEY_SELECT},
        {"UP", "D-Pad Up", KEY_DUP},
        {"DOWN", "D-Pad Down", KEY_DDOWN},
        {"LEFT", "D-Pad Left", KEY_DLEFT},
        {"RIGHT", "D-Pad Right", KEY_DRIGHT},
        {"CUP", "Circle Pad Up", KEY_CPAD_UP},
        {"CDOWN", "Circle Pad Down", KEY_CPAD_DOWN},
        {"CLEFT", "Circle Pad Left", KEY_CPAD_LEFT},
        {"CRIGHT", "Circle Pad Right", KEY_CPAD_RIGHT},
        {"CSUP", "C-Stick Up", KEY_CSTICK_UP},
        {"CSDOWN", "C-Stick Down", KEY_CSTICK_DOWN},
        {"CSLEFT", "C-Stick Left", KEY_CSTICK_LEFT},
        {"CSRIGHT", "C-Stick Right", KEY_CSTICK_RIGHT},
};

static void rehid_keys_join(u32 keys, bool people, char* out, size_t size) {
    out[0] = '\0';
    size_t used = 0;
    for(u32 i = 0; i < COUNT_OF(rehid_key_names); i++) {
        if(!(keys & rehid_key_names[i].key)) {
            continue;
        }

        const char* name = people ? i18n_translate_text(rehid_key_names[i].label) : rehid_key_names[i].name;
        int written = snprintf(out + used, size - used, "%s%s", used > 0 ? (people ? " + " : "+") : "", name);
        if(written < 0 || (size_t) written >= size - used) {
            break;
        }

        used += (size_t) written;
    }
}

void rehid_keys_name(u32 keys, char* out, size_t size) {
    rehid_keys_join(keys, false, out, size);
}

void rehid_keys_label(u32 keys, char* out, size_t size) {
    rehid_keys_join(keys, true, out, size);
}

u32 rehid_keys_parse(const char* text) {
    u32 keys = 0;
    while(text != NULL && *text != '\0') {
        size_t length = strcspn(text, "+");
        for(u32 i = 0; i < COUNT_OF(rehid_key_names); i++) {
            if(strlen(rehid_key_names[i].name) == length && strncasecmp(rehid_key_names[i].name, text, length) == 0) {
                keys |= rehid_key_names[i].key;
            }
        }

        text += length;
        if(*text == '+') {
            text++;
        }
    }

    return keys;
}

// ---- remappings ----

void rehid_config_init(rehid_config* config) {
    memset(config, 0, sizeof(*config));
}

void rehid_config_free(rehid_config* config) {
    if(config->extra != NULL) {
        json_decref((json_t*) config->extra);
    }

    rehid_config_init(config);
}

bool rehid_config_empty(const rehid_config* config) {
    return config->keyCount == 0 && config->turboCount == 0 && config->touchCount == 0 && config->boxCount == 0 && config->cpadCount == 0
           && !config->cpadToDpad && !config->dpadToCpad && !config->overrideCpadPro && config->home == 0;
}

void rehid_config_path(u64 titleId, char* out, size_t size) {
    if(titleId == 0) {
        string_copy(out, REHID_GLOBAL_PATH, size);
    } else {
        snprintf(out, size, REHID_DIR "/%016llX/rehid.json", titleId);
    }
}

bool rehid_config_exists(u64 titleId) {
    char path[64];
    rehid_config_path(titleId, path, sizeof(path));
    return rehid_exists(path);
}

// An object of exactly the fields Rehid reads two of: "get" and "press".
static bool rehid_pair(json_t* entry, json_t** get, json_t** press) {
    if(!json_is_object(entry) || json_object_size(entry) != 2) {
        return false;
    }

    *get = json_object_get(entry, "get");
    *press = json_object_get(entry, "press");
    return *get != NULL && *press != NULL;
}

// [n, n] or [n, n, n, n]: whole numbers within Rehid's types.
static bool rehid_numbers(json_t* array, size_t count, int* out, int low, int high) {
    if(!json_is_array(array) || json_array_size(array) != count) {
        return false;
    }

    for(size_t i = 0; i < count; i++) {
        json_t* n = json_array_get(array, i);
        if(!json_is_integer(n) || json_integer_value(n) < low || json_integer_value(n) > high) {
            return false;
        }

        out[i] = (int) json_integer_value(n);
    }

    return true;
}

static u32 rehid_keys_of(json_t* value) {
    return json_is_string(value) ? rehid_keys_parse(json_string_value(value)) & REHID_KEYS_ALL : 0;
}

Result rehid_config_parse(const char* text, rehid_config* config, const char** why) {
    rehid_config_init(config);
    *why = NULL;

    json_error_t error;
    json_t* root = json_loads(text, 0, &error);
    if(!json_is_object(root)) {
        if(root != NULL) {
            json_decref(root);
        }

        *why = "This file is not JSON Rehid can read. Rehid stops the game with an error screen when it reads a game's file like this: delete it or fix it on a computer.";
        return R_APP_PARSE_FAILED;
    }

    json_t* extra = json_object();
    const char* name;
    json_t* value;
    json_object_foreach(root, name, value) {
        if(strcasecmp(name, "keys") == 0 || strcasecmp(name, "turbo") == 0 || strcasecmp(name, "touch") == 0 || strcasecmp(name, "touchtokeys") == 0
           || strcasecmp(name, "cpad") == 0) {
            if(!json_is_array(value)) {
                config->dropped++;
                continue;
            }

            for(size_t i = 0; i < json_array_size(value); i++) {
                json_t* entry = json_array_get(value, i);
                json_t* get = NULL;
                json_t* press = NULL;
                int n[4];
                bool ok = false;
                if(strcasecmp(name, "keys") == 0 && config->keyCount < REHID_ENTRIES_MAX && rehid_pair(entry, &get, &press)) {
                    rehid_key* k = &config->keys[config->keyCount];
                    k->press = rehid_keys_of(press);
                    k->get = rehid_keys_of(get);
                    ok = k->press != 0 && k->get != 0;
                    config->keyCount += ok;
                } else if(strcasecmp(name, "turbo") == 0 && config->turboCount < REHID_ENTRIES_MAX && json_is_object(entry)) {
                    rehid_turbo* t = &config->turbo[config->turboCount];
                    json_t* frames = json_object_get(entry, "framedelay");
                    t->press = rehid_keys_of(json_object_get(entry, "press"));
                    t->get = rehid_keys_of(json_object_get(entry, "get"));
                    t->frames = json_is_integer(frames) && json_integer_value(frames) > 0 && json_integer_value(frames) < 256 ? (u8) json_integer_value(frames) : 0;
                    ok = t->press != 0 && t->get != 0 && t->frames != 0;
                    config->turboCount += ok;
                } else if((strcasecmp(name, "touch") == 0 || strcasecmp(name, "cpad") == 0) && rehid_pair(entry, &get, &press)) {
                    bool touch = strcasecmp(name, "touch") == 0;
                    u32* count = touch ? &config->touchCount : &config->cpadCount;
                    rehid_point* p = touch ? &config->touch[*count] : &config->cpad[*count];
                    if(*count < REHID_ENTRIES_MAX && rehid_numbers(get, 2, n, touch ? 1 : -32768, touch ? 319 : 32767)) {
                        p->press = rehid_keys_of(press);
                        p->x = (s16) n[0];
                        p->y = (s16) n[1];
                        ok = p->press != 0 && (!touch || n[1] <= 239);
                        *count += ok;
                    }
                } else if(strcasecmp(name, "touchtokeys") == 0 && config->boxCount < REHID_ENTRIES_MAX && rehid_pair(entry, &get, &press)
                          && rehid_numbers(press, 4, n, 0, 320)) {
                    rehid_box* b = &config->boxes[config->boxCount];
                    b->get = rehid_keys_of(get);
                    b->x = (u16) n[0];
                    b->y = (u16) n[1];
                    b->h = (u16) n[2];
                    b->w = (u16) n[3];
                    ok = b->get != 0 && b->w > 0 && b->h > 0;
                    config->boxCount += ok;
                }

                if(!ok) {
                    config->dropped++;
                }
            }
        } else if(strcasecmp(name, "cpadtodpad") == 0 || strcasecmp(name, "dpadtocpad") == 0 || strcasecmp(name, "overridecpadpro") == 0) {
            if(!json_is_boolean(value)) {
                config->dropped++;
                continue;
            }

            bool on = json_is_true(value);
            if(strcasecmp(name, "cpadtodpad") == 0) {
                config->cpadToDpad = on;
            } else if(strcasecmp(name, "dpadtocpad") == 0) {
                config->dpadToCpad = on;
            } else {
                config->overrideCpadPro = on;
            }
        } else if(strcasecmp(name, "homebutton") == 0) {
            config->home = rehid_keys_of(value);
            if(config->home == 0) {
                config->dropped++;
            }
        } else {
            json_object_set(extra, name, value);
        }
    }

    json_decref(root);
    if(json_object_size(extra) == 0) {
        json_decref(extra);
    } else {
        config->extra = extra;
    }

    return 0;
}

Result rehid_config_load(u64 titleId, rehid_config* config, const char** why) {
    rehid_config_init(config);
    *why = NULL;

    char path[64];
    rehid_config_path(titleId, path, sizeof(path));
    if(!rehid_exists(path)) {
        return 0;
    }

    size_t size = 0;
    char* text = (char*) rehid_read_file(path, REHID_FILE_MAX, &size);
    if(text == NULL) {
        *why = "Could not read this remapping file (or it is too big for one).";
        return R_APP_IO_ERROR;
    }

    Result res = rehid_config_parse(text, config, why);
    free(text);
    return res;
}

static json_t* rehid_keys_json(u32 keys) {
    char name[160];
    rehid_keys_name(keys, name, sizeof(name));
    return json_string(name);
}

static json_t* rehid_numbers_json(int a, int b) {
    json_t* array = json_array();
    json_array_append_new(array, json_integer(a));
    json_array_append_new(array, json_integer(b));
    return array;
}

char* rehid_config_dump(const rehid_config* config) {
    json_t* root = json_object();
    if(config->extra != NULL) {
        json_object_update(root, (json_t*) config->extra);
    }

    // Each list keeps Rehid's limit whatever the struct holds.
    if(config->keyCount > 0) {
        json_t* list = json_array();
        for(u32 i = 0; i < config->keyCount && i < REHID_ENTRIES_MAX; i++) {
            json_t* entry = json_object();
            json_object_set_new(entry, "get", rehid_keys_json(config->keys[i].get));
            json_object_set_new(entry, "press", rehid_keys_json(config->keys[i].press));
            json_array_append_new(list, entry);
        }

        json_object_set_new(root, "keys", list);
    }

    if(config->turboCount > 0) {
        json_t* list = json_array();
        for(u32 i = 0; i < config->turboCount && i < REHID_ENTRIES_MAX; i++) {
            json_t* entry = json_object();
            json_object_set_new(entry, "get", rehid_keys_json(config->turbo[i].get));
            json_object_set_new(entry, "press", rehid_keys_json(config->turbo[i].press));
            json_object_set_new(entry, "framedelay", json_integer(config->turbo[i].frames));
            json_array_append_new(list, entry);
        }

        json_object_set_new(root, "turbo", list);
    }

    for(u32 which = 0; which < 2; which++) {
        u32 count = which == 0 ? config->touchCount : config->cpadCount;
        const rehid_point* points = which == 0 ? config->touch : config->cpad;
        if(count == 0) {
            continue;
        }

        json_t* list = json_array();
        for(u32 i = 0; i < count && i < REHID_ENTRIES_MAX; i++) {
            json_t* entry = json_object();
            json_object_set_new(entry, "get", rehid_numbers_json(points[i].x, points[i].y));
            json_object_set_new(entry, "press", rehid_keys_json(points[i].press));
            json_array_append_new(list, entry);
        }

        json_object_set_new(root, which == 0 ? "touch" : "cpad", list);
    }

    if(config->boxCount > 0) {
        json_t* list = json_array();
        for(u32 i = 0; i < config->boxCount && i < REHID_ENTRIES_MAX; i++) {
            const rehid_box* b = &config->boxes[i];
            json_t* box = json_array();
            json_array_append_new(box, json_integer(b->x));
            json_array_append_new(box, json_integer(b->y));
            json_array_append_new(box, json_integer(b->h));
            json_array_append_new(box, json_integer(b->w));
            json_t* entry = json_object();
            json_object_set_new(entry, "get", rehid_keys_json(b->get));
            json_object_set_new(entry, "press", box);
            json_array_append_new(list, entry);
        }

        json_object_set_new(root, "touchtokeys", list);
    }

    if(config->cpadToDpad) {
        json_object_set_new(root, "cpadtodpad", json_true());
    }

    if(config->dpadToCpad) {
        json_object_set_new(root, "dpadtocpad", json_true());
    }

    if(config->overrideCpadPro) {
        json_object_set_new(root, "overridecpadpro", json_true());
    }

    if(config->home != 0) {
        json_object_set_new(root, "homebutton", rehid_keys_json(config->home));
    }

    char* text = json_dumps(root, JSON_INDENT(4) | JSON_PRESERVE_ORDER);
    json_decref(root);
    return text;
}

static void rehid_make_dirs(u64 titleId) {
    mkdir(REHID_DIR, 0777);
    if(titleId != 0) {
        char dir[48];
        snprintf(dir, sizeof(dir), REHID_DIR "/%016llX", titleId);
        mkdir(dir, 0777);
    }
}

Result rehid_config_save(u64 titleId, const rehid_config* config) {
    char* text = rehid_config_dump(config);
    if(text == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    // What goes to the SD card is read back as Rehid would before it replaces anything.
    rehid_config check;
    const char* why = NULL;
    Result res = rehid_config_parse(text, &check, &why);
    bool same = R_SUCCEEDED(res) && check.dropped == 0 && check.keyCount == config->keyCount && check.turboCount == config->turboCount
                && check.touchCount == config->touchCount && check.boxCount == config->boxCount && check.cpadCount == config->cpadCount;
    rehid_config_free(&check);
    if(!same) {
        free(text);
        return R_APP_BAD_DATA;
    }

    char path[64];
    rehid_config_path(titleId, path, sizeof(path));
    rehid_make_dirs(titleId);
    bool ok = rehid_replace(path, text, strlen(text));
    free(text);
    return ok ? 0 : R_APP_IO_ERROR;
}

bool rehid_config_delete(u64 titleId) {
    char path[64];
    rehid_config_path(titleId, path, sizeof(path));
    if(remove(path) != 0 && rehid_exists(path)) {
        return false;
    }

    if(titleId != 0) {
        char dir[48];
        snprintf(dir, sizeof(dir), REHID_DIR "/%016llX", titleId);
        rmdir(dir);   // Only when nothing else is in it.
    }

    return true;
}

void rehid_protect_nsa() {
    if(rehid_config_exists(REHID_NSA_TITLE_ID)) {
        return;
    }

    char path[64];
    rehid_config_path(REHID_NSA_TITLE_ID, path, sizeof(path));
    rehid_make_dirs(REHID_NSA_TITLE_ID);
    rehid_replace(path, "{}", 2);
}

u32 rehid_list_titles(u64* out, u32 max) {
    DIR* dir = opendir(REHID_DIR);
    if(dir == NULL) {
        return 0;
    }

    u32 count = 0;
    struct dirent* entry;
    while(count < max && (entry = readdir(dir)) != NULL) {
        const char* name = entry->d_name;
        if(strlen(name) != 16 || strspn(name, "0123456789abcdefABCDEF") != 16) {
            continue;
        }

        u64 titleId = strtoull(name, NULL, 16);
        if(titleId != 0 && rehid_config_exists(titleId)) {
            out[count++] = titleId;
        }
    }

    closedir(dir);
    return count;
}
