#include <ctype.h>
#include <dirent.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include <3ds.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>

#include "wifislot.h"
#include "../core/core.h"

_Static_assert(sizeof(wifi_slot_raw) == 0xC00, "a Wi-Fi slot is 0xC00 bytes");

const char* wifi_encryption_name(u8 encryption) {
    switch(encryption) {
        case WIFI_ENCRYPTION_OPEN: return _("Open");
        case WIFI_ENCRYPTION_WEP_64: return "WEP 64";
        case WIFI_ENCRYPTION_WEP_128: return "WEP 128";
        case WIFI_ENCRYPTION_WEP_152: return "WEP 152";
        case WIFI_ENCRYPTION_WPA_PSK_TKIP: return "WPA TKIP";
        case WIFI_ENCRYPTION_WPA_PSK_AES: return "WPA AES";
        case WIFI_ENCRYPTION_WPA2_PSK_TKIP: return "WPA2 TKIP";
        case WIFI_ENCRYPTION_WPA2_PSK_AES: return "WPA2 AES";
        default: return _("Unknown");
    }
}

wifi_security wifi_security_of(u8 encryption) {
    switch(encryption) {
        case WIFI_ENCRYPTION_OPEN: return WIFI_SECURITY_OPEN;
        case WIFI_ENCRYPTION_WEP_64:
        case WIFI_ENCRYPTION_WEP_128:
        case WIFI_ENCRYPTION_WEP_152: return WIFI_SECURITY_WEP;
        case WIFI_ENCRYPTION_WPA_PSK_TKIP:
        case WIFI_ENCRYPTION_WPA_PSK_AES: return WIFI_SECURITY_WPA;
        default: return WIFI_SECURITY_WPA2;
    }
}

// CRC-16/ARC over 0x410 bytes from the byte after the CRC.
static void wifi_fix_crc(wifi_slot_raw* raw) {
    const u8* data = (const u8*) raw + 4;
    u16 crc = 0;
    for(u32 i = 0; i < 0x410; i++) {
        crc ^= data[i];
        for(u32 bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
        }
    }

    raw->crc = crc;
}

static void wifi_copy_text(char* out, size_t size, const char* in, size_t max) {
    size_t len = strnlen(in, max);
    if(len >= size) {
        len = size - 1;
    }

    memcpy(out, in, len);
    out[len] = '\0';
}

static void wifi_decode(wifi_slot* slot) {
    const wifi_slot_raw* raw = &slot->raw;
    slot->exists = raw->version != 0;
    slot->wps = raw->editableSecurity == 0;
    slot->ssid[0] = '\0';
    slot->password[0] = '\0';
    slot->encryption = WIFI_ENCRYPTION_OPEN;
    if(!slot->exists) {
        return;
    }

    const wifi_network_raw* network = slot->wps ? &raw->multissid[0] : &raw->network;
    size_t ssidLength = raw->network.ssidLength;
    if(ssidLength > sizeof(network->ssid)) {
        ssidLength = sizeof(network->ssid);
    }

    memcpy(slot->ssid, network->ssid, ssidLength);
    slot->ssid[ssidLength] = '\0';
    slot->encryption = network->encryption;
    wifi_copy_text(slot->password, sizeof(slot->password), network->passwordText, sizeof(network->passwordText));

    // A WEP key typed as text is only in the key.
    if(slot->password[0] == '\0' && wifi_security_of(slot->encryption) == WIFI_SECURITY_WEP) {
        wifi_copy_text(slot->password, sizeof(slot->password), (const char*) network->passwordKey, sizeof(network->passwordKey));
        for(char* c = slot->password; *c != '\0'; c++) {
            if(!isprint((unsigned char) *c)) {
                slot->password[0] = '\0';
                break;
            }
        }
    }
}

bool wifi_slot_read(u32 index, wifi_slot* out) {
    memset(out, 0, sizeof(*out));
    if(index >= WIFI_SLOT_COUNT || R_FAILED(CFG_GetConfigInfoBlk8(sizeof(out->raw), WIFI_SLOT_BLOCK + index, (u8*) &out->raw))) {
        return false;
    }

    wifi_decode(out);
    return true;
}

Result wifi_slot_write(u32 index, const wifi_slot* in) {
    if(index >= WIFI_SLOT_COUNT) {
        return R_APP_INVALID_ARGUMENT;
    }

    wifi_slot_raw raw = in->raw;
    wifi_fix_crc(&raw);

    Result res = CFG_SetConfigInfoBlk8(sizeof(raw), WIFI_SLOT_BLOCK + index, (u8*) &raw);
    if(R_SUCCEEDED(res)) {
        res = CFG_UpdateConfigSavegame();
    }

    return res;
}

Result wifi_slot_clear(u32 index) {
    wifi_slot empty;
    memset(&empty, 0, sizeof(empty));
    return wifi_slot_write(index, &empty);
}

static int wifi_hex(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

Result wifi_slot_from_credentials(wifi_slot* out, const wifi_slot* base, const char* ssid, const char* password, wifi_security security) {
    size_t ssidLength = strlen(ssid);
    size_t passwordLength = strlen(password);
    if(ssidLength == 0 || ssidLength > sizeof(out->raw.network.ssid) || passwordLength >= sizeof(out->raw.network.passwordText)) {
        return R_APP_INVALID_ARGUMENT;
    }

    wifi_slot slot;
    memset(&slot, 0, sizeof(slot));
    if(base != NULL && base->exists) {
        slot.raw = base->raw;
    } else {
        slot.raw.version = 1;
        slot.raw.dhcp = 1;
        slot.raw.autoDns = 1;
        slot.raw.mtu = 1400;
    }

    // A plain network, its fields the ones the console reads.
    slot.raw.networkEnable = 1;
    slot.raw.editableSecurity = 1;
    slot.raw.multissidEnable = 0;
    slot.raw.multissidType = 0;
    slot.raw.multissidAmount = 0;
    memset(slot.raw.multissid, 0, sizeof(slot.raw.multissid));
    slot.raw.connectionTestSuccess = 0;

    wifi_network_raw* network = &slot.raw.network;
    memset(network, 0, sizeof(*network));
    memcpy(network->ssid, ssid, ssidLength);
    network->ssidLength = (u8) ssidLength;

    if(security == WIFI_SECURITY_OPEN) {
        network->encryption = WIFI_ENCRYPTION_OPEN;
    } else if(security == WIFI_SECURITY_WEP) {
        // 5, 13 or 16 characters, or twice as many hex digits.
        u8 key[16];
        size_t keyLength = 0;
        bool hex = passwordLength == 10 || passwordLength == 26 || passwordLength == 32;
        for(size_t i = 0; hex && i < passwordLength; i++) {
            hex = wifi_hex(password[i]) >= 0;
        }

        if(hex) {
            keyLength = passwordLength / 2;
            for(size_t i = 0; i < keyLength; i++) {
                key[i] = (u8) ((wifi_hex(password[2 * i]) << 4) | wifi_hex(password[2 * i + 1]));
            }
        } else if(passwordLength == 5 || passwordLength == 13 || passwordLength == 16) {
            keyLength = passwordLength;
            memcpy(key, password, keyLength);
        } else {
            return R_APP_INVALID_ARGUMENT;
        }

        network->encryption = keyLength == 5 ? WIFI_ENCRYPTION_WEP_64 : keyLength == 13 ? WIFI_ENCRYPTION_WEP_128 : WIFI_ENCRYPTION_WEP_152;
        memcpy(network->passwordKey, key, keyLength);
        memcpy(network->passwordText, password, passwordLength);
    } else {
        // WPA: 8 to 63 characters; the console connects with the PSK made from them.
        if(passwordLength < 8 || passwordLength > 63) {
            return R_APP_INVALID_ARGUMENT;
        }

        network->encryption = security == WIFI_SECURITY_WPA ? WIFI_ENCRYPTION_WPA_PSK_AES : WIFI_ENCRYPTION_WPA2_PSK_AES;
        memcpy(network->passwordText, password, passwordLength);

        mbedtls_md_context_t md;
        mbedtls_md_init(&md);
        int ret = mbedtls_md_setup(&md, mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), 1);
        if(ret == 0) {
            ret = mbedtls_pkcs5_pbkdf2_hmac(&md, (const unsigned char*) password, passwordLength, (const unsigned char*) ssid, ssidLength, 4096, sizeof(network->passwordKey), network->passwordKey);
        }

        mbedtls_md_free(&md);
        if(ret != 0) {
            return R_APP_BAD_DATA;
        }
    }

    wifi_fix_crc(&slot.raw);
    wifi_decode(&slot);
    *out = slot;
    return 0;
}

// ---- backups ----

// The file is the slot's 0xC00 bytes, then "\nWMB1\t<unix time>\t<alias>\n"
// (WiFiManager+'s trailer; older files have "\nWMA1\t<alias>\n" or nothing).
static void wifi_parse_trailer(const char* tail, wifi_backup* backup) {
    const char* tag = strstr(tail, "WMB1\t");
    if(tag != NULL) {
        char* end = NULL;
        backup->saved = strtoll(tag + 5, &end, 10);
        if(end != NULL && *end == '\t') {
            const char* alias = end + 1;
            size_t len = strcspn(alias, "\n");
            wifi_copy_text(backup->alias, sizeof(backup->alias), alias, len);
        }

        return;
    }

    tag = strstr(tail, "WMA1\t");
    if(tag != NULL) {
        const char* alias = tag + 5;
        wifi_copy_text(backup->alias, sizeof(backup->alias), alias, strcspn(alias, "\n"));
    }
}

static bool wifi_backup_read(const char* path, wifi_backup* out) {
    memset(out, 0, sizeof(*out));
    string_copy(out->path, path, sizeof(out->path));

    FILE* fd = fopen(path, "rb");
    if(fd == NULL) {
        return false;
    }

    bool ok = fread(&out->slot.raw, 1, sizeof(out->slot.raw), fd) == sizeof(out->slot.raw);
    if(ok) {
        char tail[512];
        size_t read = fread(tail, 1, sizeof(tail) - 1, fd);
        tail[read] = '\0';
        wifi_parse_trailer(tail, out);
    }

    fclose(fd);
    if(!ok) {
        return false;
    }

    wifi_decode(&out->slot);

    const char* base = strrchr(path, '/');
    base = base != NULL ? base + 1 : path;
    if(out->alias[0] != '\0') {
        string_copy(out->name, out->alias, sizeof(out->name));
    } else if(out->slot.ssid[0] != '\0') {
        string_copy(out->name, out->slot.ssid, sizeof(out->name));
    } else {
        string_copy(out->name, base, sizeof(out->name));
        char* dot = strrchr(out->name, '.');
        if(dot != NULL) {
            *dot = '\0';
        }
    }

    return true;
}

static void wifi_backup_scan(const char* dir, bool legacy, wifi_backup** list, u32* count, u32* capacity) {
    DIR* handle = opendir(dir);
    if(handle == NULL) {
        return;
    }

    struct dirent* entry;
    while((entry = readdir(handle)) != NULL) {
        size_t len = strlen(entry->d_name);
        if(entry->d_type == DT_DIR || len < 5 || strcasecmp(entry->d_name + len - 4, ".bin") != 0) {
            continue;
        }

        if(*count == *capacity) {
            u32 grown = *capacity == 0 ? 8 : *capacity * 2;
            wifi_backup* bigger = (wifi_backup*) realloc(*list, grown * sizeof(wifi_backup));
            if(bigger == NULL) {
                break;
            }

            *list = bigger;
            *capacity = grown;
        }

        char path[256];
        snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
        if(wifi_backup_read(path, &(*list)[*count])) {
            (*list)[*count].legacy = legacy;
            (*count)++;
        }
    }

    closedir(handle);
}

static int wifi_backup_compare(const void* a, const void* b) {
    const wifi_backup* first = (const wifi_backup*) a;
    const wifi_backup* second = (const wifi_backup*) b;
    if(first->saved != second->saved) {
        return first->saved > second->saved ? -1 : 1;
    }

    return strcasecmp(first->name, second->name);
}

u32 wifi_backup_list(wifi_backup** out) {
    wifi_backup* list = NULL;
    u32 count = 0;
    u32 capacity = 0;
    wifi_backup_scan(WIFI_BACKUP_DIR, false, &list, &count, &capacity);
    wifi_backup_scan(WIFI_LEGACY_DIR, true, &list, &count, &capacity);
    if(count > 1) {
        qsort(list, count, sizeof(*list), wifi_backup_compare);
    }

    *out = list;
    return count;
}

static Result wifi_backup_write(const char* path, const wifi_slot* slot, const char* alias, s64 saved) {
    wifi_slot_raw raw = slot->raw;
    wifi_fix_crc(&raw);

    FILE* fd = fopen(path, "wb");
    if(fd == NULL) {
        return R_APP_IO_ERROR;
    }

    char trailer[128];
    int trailerLength = snprintf(trailer, sizeof(trailer), "\nWMB1\t%lld\t%s\n", saved, alias != NULL ? alias : "");
    bool ok = fwrite(&raw, 1, sizeof(raw), fd) == sizeof(raw) && fwrite(trailer, 1, (size_t) trailerLength, fd) == (size_t) trailerLength;
    ok = fclose(fd) == 0 && ok;
    return ok ? 0 : R_APP_IO_ERROR;
}

Result wifi_backup_save(const wifi_slot* slot, const char* alias, char* pathOut, size_t pathSize) {
    mkdir("sdmc:/nsa", 0777);
    mkdir(WIFI_BACKUP_DIR, 0777);

    // A file name from the alias or the SSID, without what FAT refuses.
    char base[64];
    string_copy(base, alias != NULL && alias[0] != '\0' ? alias : slot->ssid[0] != '\0' ? slot->ssid : "wifi", sizeof(base));
    for(char* c = base; *c != '\0'; c++) {
        if(strchr("/\\:*?\"<>|", *c) != NULL || (unsigned char) *c < 0x20) {
            *c = '_';
        }
    }

    char path[256];
    snprintf(path, sizeof(path), "%s/%s.bin", WIFI_BACKUP_DIR, base);
    struct stat st;
    for(u32 i = 2; stat(path, &st) == 0 && i < 1000; i++) {
        snprintf(path, sizeof(path), "%s/%s_%lu.bin", WIFI_BACKUP_DIR, base, i);
    }

    Result res = wifi_backup_write(path, slot, alias, (s64) time(NULL));
    if(R_SUCCEEDED(res) && pathOut != NULL) {
        string_copy(pathOut, path, pathSize);
    }

    return res;
}

Result wifi_backup_rewrite(const wifi_backup* backup) {
    return wifi_backup_write(backup->path, &backup->slot, backup->alias, backup->saved != 0 ? backup->saved : (s64) time(NULL));
}

// ---- QR codes ----

// The value of one field of a WIFI: text, up to its unescaped ';'.
static const char* wifi_qr_field(const char* in, char* out, size_t size) {
    size_t len = 0;
    while(*in != '\0' && *in != ';') {
        char c = *in++;
        if(c == '\\' && *in != '\0') {
            c = *in++;
        }

        if(len + 1 < size) {
            out[len++] = c;
        }
    }

    out[len] = '\0';
    return *in == ';' ? in + 1 : in;
}

bool wifi_qr_parse(const char* payload, char* ssid, size_t ssidSize, char* password, size_t passwordSize, wifi_security* security) {
    ssid[0] = '\0';
    password[0] = '\0';
    *security = WIFI_SECURITY_OPEN;

    if(strncasecmp(payload, "WIFI:", 5) == 0) {
        char type[16] = "";
        const char* p = payload + 5;
        while(*p != '\0' && *p != ';') {
            const char* colon = strchr(p, ':');
            if(colon == NULL) {
                break;
            }

            char key = (char) toupper((unsigned char) *p);
            bool single = colon == p + 1;
            char value[128];
            p = wifi_qr_field(colon + 1, value, sizeof(value));
            if(!single) {
                continue;
            }

            if(key == 'S') {
                string_copy(ssid, value, ssidSize);
            } else if(key == 'P') {
                string_copy(password, value, passwordSize);
            } else if(key == 'T') {
                string_copy(type, value, sizeof(type));
            }
        }

        if(type[0] == '\0' || strcasecmp(type, "nopass") == 0) {
            *security = WIFI_SECURITY_OPEN;
        } else if(strcasecmp(type, "WEP") == 0) {
            *security = WIFI_SECURITY_WEP;
        } else if(strcasecmp(type, "WPA") == 0) {
            *security = WIFI_SECURITY_WPA;
        } else {
            *security = WIFI_SECURITY_WPA2;
        }

        return ssid[0] != '\0';
    }

    // Plain text: the SSID, and the password on the next line.
    size_t line = strcspn(payload, "\r\n");
    wifi_copy_text(ssid, ssidSize, payload, line);
    if(payload[line] != '\0') {
        const char* rest = payload + line;
        while(*rest == '\r' || *rest == '\n') {
            rest++;
        }

        wifi_copy_text(password, passwordSize, rest, strcspn(rest, "\r\n"));
        *security = password[0] != '\0' ? WIFI_SECURITY_WPA2 : WIFI_SECURITY_OPEN;
    }

    return ssid[0] != '\0';
}

static size_t wifi_qr_escape(char* out, size_t size, const char* in) {
    size_t len = 0;
    for(; *in != '\0'; in++) {
        if(strchr("\\;,:\"", *in) != NULL && len + 1 < size) {
            out[len++] = '\\';
        }

        if(len + 1 < size) {
            out[len++] = *in;
        }
    }

    out[len] = '\0';
    return len;
}

void wifi_qr_encode(const wifi_slot* slot, char* out, size_t size) {
    static const char* const types[] = {"nopass", "WEP", "WPA", "WPA2"};
    wifi_security security = wifi_security_of(slot->encryption);

    char ssid[0x50];
    char password[0x90];
    wifi_qr_escape(ssid, sizeof(ssid), slot->ssid);
    wifi_qr_escape(password, sizeof(password), slot->password);
    if(security == WIFI_SECURITY_OPEN) {
        snprintf(out, size, "WIFI:S:%s;T:nopass;;", ssid);
    } else {
        snprintf(out, size, "WIFI:S:%s;T:%s;P:%s;;", ssid, types[security], password);
    }
}
