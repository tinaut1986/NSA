#include <dirent.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <3ds.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>

#include "wifislots.h"

// ---- the connections as saved ----

// CRC-16/ARC (reflected 0x8005, starting at 0), as the system checks the block.
static u16 wifislots_crc16(const u8* data, size_t size) {
    u16 crc = 0;
    for(size_t i = 0; i < size; i++) {
        crc ^= data[i];
        for(u32 bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (u16) ((crc >> 1) ^ 0xA001) : (u16) (crc >> 1);
        }
    }

    return crc;
}

static bool wifislots_crc_ok(const wifislots_slot* slot) {
    return slot->crc == wifislots_crc16((const u8*) slot + 4, 0x410);
}

Result wifislots_read(u32 index, wifislots_slot* out) {
    if(index >= WIFISLOTS_COUNT) {
        return -1;
    }

    return CFG_GetConfigInfoBlk8(sizeof(*out), WIFISLOTS_BLOCK + index, out);
}

bool wifislots_empty(const wifislots_slot* slot) {
    return slot->version == 0;
}

const wifislots_network* wifislots_network_of(const wifislots_slot* slot) {
    return slot->editableSecurity ? &slot->network : &slot->multiSsid[0];
}

void wifislots_name(const wifislots_slot* slot, char* out, size_t size) {
    const wifislots_network* net = wifislots_network_of(slot);
    size_t len = net->ssidLength;
    if(len == 0 || len > sizeof(net->ssid)) {
        len = slot->network.ssidLength <= sizeof(net->ssid) ? slot->network.ssidLength : sizeof(net->ssid);
    }

    if(len >= size) {
        len = size - 1;
    }

    memcpy(out, net->ssid, len);
    out[len] = '\0';
}

void wifislots_password(const wifislots_slot* slot, char* out, size_t size) {
    const wifislots_network* net = wifislots_network_of(slot);
    size_t len = strnlen(net->password, sizeof(net->password));
    const char* from = net->password;
    if(len == 0 && net->security >= WIFISLOTS_WEP_64 && net->security <= WIFISLOTS_WEP_152) {
        from = (const char*) net->key;
        len = strnlen(from, sizeof(net->key));
    }

    if(len >= size) {
        len = size - 1;
    }

    memcpy(out, from, len);
    out[len] = '\0';
}

const char* wifislots_security_name(u8 security) {
    switch(security) {
        case WIFISLOTS_OPEN: return "Open";
        case WIFISLOTS_WEP_64: return "WEP 64";
        case WIFISLOTS_WEP_128: return "WEP 128";
        case WIFISLOTS_WEP_152: return "WEP 152";
        case WIFISLOTS_WPA_TKIP: return "WPA-PSK (TKIP)";
        case WIFISLOTS_WPA_AES: return "WPA-PSK (AES)";
        case WIFISLOTS_WPA2_TKIP: return "WPA2-PSK (TKIP)";
        case WIFISLOTS_WPA2_AES: return "WPA2-PSK (AES)";
        default: return "?";
    }
}

// ---- writing through ac:i, as System Settings does ----
//
// LoadNetworkSetting(n) (or InitializeNetworkSetting for an empty connection), a setter per field, UpdateNetworkSetting(n)
// and FlushNetworkSetting, which saves ac's copy to the config. Command ids from 3dbrew's AC Services page; the static
// buffer each setter takes was found on a New 3DS XL (11.17) by sending empty buffers (ac checks the buffer's number, not
// its size, and an empty one cannot overflow): the name 2, the key 3, the BSSID 4, the passphrase 8, the address 9, the
// gateway 10, the netmask 11, both DNS 12, the proxy host 13, its user 14, its password 15. A buffer bigger than what ac
// set aside for that number brings the whole console down (seen with 0x40 bytes to number 2): each field goes with its
// own size, never more.

static Result aci_call(u32 header, u32 param) {
    u32* cmdbuf = getThreadCommandBuffer();
    cmdbuf[0] = header;
    cmdbuf[1] = param;
    Result res = svcSendSyncRequest(*acGetSessionHandle());
    return R_FAILED(res) ? res : (Result) cmdbuf[1];
}

static Result aci_set_static(u16 command, u32 id, const void* data, size_t size) {
    u32* cmdbuf = getThreadCommandBuffer();
    cmdbuf[0] = IPC_MakeHeader(command, 0, 2);
    cmdbuf[1] = IPC_Desc_StaticBuffer(size, id);
    cmdbuf[2] = (u32) data;
    Result res = svcSendSyncRequest(*acGetSessionHandle());
    return R_FAILED(res) ? res : (Result) cmdbuf[1];
}

#define ACI_SET(command, value) aci_call(IPC_MakeHeader((command), 1, 0), (u32) (value))

// One network of a connection: the main one (setters 0x410-0x418) or WPS/AOSS setting n (0x420 + n * 10).
static Result aci_set_network(u16 base, const wifislots_network* net) {
    Result res = aci_set_static(base, 2, net->ssid, sizeof(net->ssid));
    if(R_SUCCEEDED(res)) res = ACI_SET(base + 2, net->ssidLength);
    if(R_SUCCEEDED(res)) res = ACI_SET(base + 4, net->security);
    if(R_SUCCEEDED(res)) res = aci_set_static(base + 6, 8, net->password, sizeof(net->password));
    if(R_SUCCEEDED(res)) res = aci_set_static(base + 8, 3, net->key, sizeof(net->key));
    return res;
}

Result wifislots_write(u32 index, const wifislots_slot* in, const char** step) {
    static const char* none = "";
    const char* dummy;
    if(step == NULL) {
        step = &dummy;
    }

    if(index >= WIFISLOTS_COUNT) {
        *step = "slot";
        return -1;
    }

    const wifislots_settings* s = &in->settings;
    Result res;

#define STEP(name, call) do { *step = (name); if(R_FAILED(res = (call))) return res; } while(0)
    // An empty connection cannot be loaded: a new setting is started instead.
    if(R_FAILED(aci_call(IPC_MakeHeader(0x401, 1, 0), index))) {
        STEP("initialize", aci_call(IPC_MakeHeader(0x406, 1, 0), 1));
    }

    STEP("version", ACI_SET(0x408, in->version));
    STEP("enable", ACI_SET(0x40C, in->enabled));
    STEP("editable", ACI_SET(0x40E, in->editableSecurity));
    STEP("network", aci_set_network(0x410, &in->network));
    STEP("multi enable", ACI_SET(0x41A, in->multiSsidEnabled));
    STEP("multi type", ACI_SET(0x41C, in->multiSsidType));
    STEP("multi count", ACI_SET(0x41E, in->multiSsidCount));
    for(u32 i = 0; i < 4; i++) {
        STEP("multi network", aci_set_network((u16) (0x420 + i * 10), &in->multiSsid[i]));
    }

    STEP("dhcp", ACI_SET(0x448, s->dhcp));
    STEP("auto dns", ACI_SET(0x44A, s->autoDns));
    STEP("ip", aci_set_static(0x44C, 9, s->ip, sizeof(s->ip)));
    STEP("gateway", aci_set_static(0x44E, 10, s->gateway, sizeof(s->gateway)));
    STEP("netmask", aci_set_static(0x450, 11, s->netmask, sizeof(s->netmask)));
    STEP("dns 1", aci_set_static(0x452, 12, s->dns1, sizeof(s->dns1)));
    STEP("dns 2", aci_set_static(0x454, 12, s->dns2, sizeof(s->dns2)));
    STEP("tested", ACI_SET(0x456, s->tested));
    STEP("bssid", aci_set_static(0x45A, 4, s->bssid, sizeof(s->bssid)));
    STEP("proxy", ACI_SET(0x460, s->proxyEnabled));
    STEP("proxy auth", ACI_SET(0x462, s->proxyAuth));
    STEP("proxy port", ACI_SET(0x464, s->proxyPort));
    STEP("proxy host", aci_set_static(0x466, 13, s->proxyHost, sizeof(s->proxyHost)));
    STEP("proxy user", aci_set_static(0x468, 14, s->proxyUser, sizeof(s->proxyUser)));
    STEP("proxy password", aci_set_static(0x46A, 15, s->proxyPassword, sizeof(s->proxyPassword)));
    STEP("upnp", ACI_SET(0x46C, s->upnp));
    STEP("mtu", ACI_SET(0x46E, s->mtu));
    STEP("update", aci_call(IPC_MakeHeader(0x402, 1, 0), index));
    STEP("flush", aci_call(IPC_MakeHeader(0x404, 0, 0), 0));
#undef STEP

    *step = none;
    return 0;
}

// ---- copies on the SD card, as WifiManager keeps them ----
//
// The connection's 0xC00 bytes, then "\nWMB1\t<unix time>\t<alias>\n"; older files have "\nWMA1\t<alias>\n" (no time),
// the oldest nothing. The file is named after the alias or the network.

#define WIFISLOTS_FILE_BYTES WIFISLOTS_SIZE

static const char* wifislots_find(const char* data, size_t size, const char* tag) {
    size_t len = strlen(tag);
    for(size_t i = 0; i + len <= size; i++) {
        if(memcmp(data + i, tag, len) == 0) {
            return data + i;
        }
    }

    return NULL;
}

static bool wifislots_read_backup(const char* path, wifislots_backup* out) {
    FILE* f = fopen(path, "rb");
    if(f == NULL) {
        return false;
    }

    static char buffer[WIFISLOTS_SIZE + 512];
    size_t size = fread(buffer, 1, sizeof(buffer), f);
    fclose(f);
    if(size < 0x414) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    snprintf(out->path, sizeof(out->path), "%s", path);

    // The trailer starts after the bytes the CRC covers.
    const char* tail = wifislots_find(buffer + 0x414, size - 0x414, "\nWMB1\t");
    const char* old = tail == NULL ? wifislots_find(buffer + 0x414, size - 0x414, "\nWMA1\t") : NULL;
    size_t bytes = tail != NULL ? (size_t) (tail - buffer) : old != NULL ? (size_t) (old - buffer) : size;
    memcpy(&out->slot, buffer, bytes < WIFISLOTS_SIZE ? bytes : WIFISLOTS_SIZE);

    const char* alias = NULL;
    if(tail != NULL) {
        char* end = NULL;
        out->saved = (time_t) strtoll(tail + 6, &end, 10);
        alias = end != NULL && *end == '\t' ? end + 1 : NULL;
    } else if(old != NULL) {
        alias = old + 6;
    }

    if(alias != NULL) {
        size_t n = 0;
        while(alias + n < buffer + size && alias[n] != '\n' && n + 1 < sizeof(out->alias)) {
            n++;
        }

        memcpy(out->alias, alias, n);
        out->alias[n] = '\0';
    }

    return !wifislots_empty(&out->slot);
}

static int wifislots_backup_compare(const void* a, const void* b) {
    const wifislots_backup* x = (const wifislots_backup*) a;
    const wifislots_backup* y = (const wifislots_backup*) b;
    if(x->saved != y->saved) {
        return x->saved > y->saved ? -1 : 1;
    }

    return strcasecmp(x->path, y->path);
}

u32 wifislots_list_backups(wifislots_backup** out) {
    *out = NULL;
    DIR* dir = opendir("sdmc:" WIFISLOTS_BACKUP_DIR);
    if(dir == NULL) {
        return 0;
    }

    u32 count = 0;
    u32 capacity = 0;
    struct dirent* entry;
    while((entry = readdir(dir)) != NULL) {
        size_t len = strlen(entry->d_name);
        if(len < 5 || strcasecmp(entry->d_name + len - 4, ".bin") != 0) {
            continue;
        }

        if(count == capacity) {
            u32 more = capacity == 0 ? 8 : capacity * 2;
            wifislots_backup* grown = (wifislots_backup*) realloc(*out, more * sizeof(wifislots_backup));
            if(grown == NULL) {
                break;
            }

            *out = grown;
            capacity = more;
        }

        char path[256];
        snprintf(path, sizeof(path), "sdmc:" WIFISLOTS_BACKUP_DIR "/%s", entry->d_name);
        if(wifislots_read_backup(path, &(*out)[count])) {
            count++;
        }
    }

    closedir(dir);
    if(count > 1) {
        qsort(*out, count, sizeof(wifislots_backup), wifislots_backup_compare);
    }

    return count;
}

// As WifiManager names them: the alias or the network, with what a file name cannot hold made "_".
static void wifislots_file_base(const char* from, char* out, size_t size) {
    size_t n = 0;
    for(const char* c = from; *c != '\0' && n + 1 < size && n < 100; c++) {
        out[n++] = (strchr("/\\:*?\"<>|", *c) != NULL || (unsigned char) *c < 0x20) ? '_' : *c;
    }

    while(n > 0 && (out[n - 1] == ' ' || out[n - 1] == '.')) {
        n--;
    }

    out[n] = '\0';
    size_t start = 0;
    while(out[start] == ' ') {
        start++;
    }

    if(start > 0) {
        memmove(out, out + start, n - start + 1);
    }

    if(out[0] == '\0') {
        snprintf(out, size, "wifi");
    }
}

// The file: the connection, then the trailer with the date and the alias (no tab or line break in it: they end fields).
static Result wifislots_write_file(const char* file, const wifislots_slot* slot, time_t saved, const char* alias) {
    char clean[100];
    size_t n = 0;
    for(const char* c = alias != NULL ? alias : ""; *c != '\0' && n + 1 < sizeof(clean); c++) {
        clean[n++] = (*c == '\t' || *c == '\n' || *c == '\r') ? ' ' : *c;
    }

    clean[n] = '\0';

    FILE* f = fopen(file, "wb");
    if(f == NULL) {
        return -1;
    }

    wifislots_slot copy = *slot;
    copy.crc = wifislots_crc16((const u8*) &copy + 4, 0x410);
    char trailer[160];
    int trailerLen = snprintf(trailer, sizeof(trailer), "\nWMB1\t%lld\t%s\n", (long long) saved, clean);
    bool ok = fwrite(&copy, 1, WIFISLOTS_FILE_BYTES, f) == WIFISLOTS_FILE_BYTES && fwrite(trailer, 1, (size_t) trailerLen, f) == (size_t) trailerLen;
    ok = fclose(f) == 0 && ok;
    return ok ? 0 : -1;
}

Result wifislots_set_alias(wifislots_backup* backup, const char* alias) {
    Result res = wifislots_write_file(backup->path, &backup->slot, backup->saved != 0 ? backup->saved : time(NULL), alias);
    if(R_SUCCEEDED(res)) {
        snprintf(backup->alias, sizeof(backup->alias), "%s", alias != NULL ? alias : "");
    }

    return res;
}

Result wifislots_save_backup(const wifislots_slot* slot, const char* alias, char* path, size_t pathSize) {
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:" WIFISLOTS_BACKUP_DIR, 0777);

    char name[33];
    wifislots_name(slot, name, sizeof(name));
    char base[128];
    wifislots_file_base(alias != NULL && alias[0] != '\0' ? alias : name, base, sizeof(base));

    char file[256];
    snprintf(file, sizeof(file), "sdmc:" WIFISLOTS_BACKUP_DIR "/%s.bin", base);
    struct stat st;
    for(u32 i = 2; stat(file, &st) == 0 && i < 1000; i++) {
        snprintf(file, sizeof(file), "sdmc:" WIFISLOTS_BACKUP_DIR "/%s_%lu.bin", base, i);
    }

    if(R_FAILED(wifislots_write_file(file, slot, time(NULL), alias))) {
        remove(file);
        return -1;
    }

    if(path != NULL) {
        snprintf(path, pathSize, "%s", file + 5);   // Without "sdmc:".
    }

    return 0;
}

Result wifislots_delete_backup(const wifislots_backup* backup) {
    return remove(backup->path) == 0 ? 0 : -1;
}

// ---- DEBUG ----

// SITE WIFIREAD: what the config holds and what ac says of each connection (names only, never passwords).
void wifislots_debug_read(char* out, size_t size) {
    size_t used = 0;
    for(u32 i = 0; i < WIFISLOTS_COUNT && used < size; i++) {
        wifislots_slot slot;
        Result res = wifislots_read(i, &slot);
        char name[33] = "";
        if(R_SUCCEEDED(res) && !wifislots_empty(&slot)) {
            wifislots_name(&slot, name, sizeof(name));
        }

        char acName[33] = "";
        Result loadRes = ACI_LoadNetworkSetting(i);
        if(R_SUCCEEDED(loadRes)) {
            ACI_GetNetworkWirelessEssidSecuritySsid(acName);
        }

        acName[32] = '\0';
        used += (size_t) snprintf(out + used, size - used, "slot %lu: read %08lX v%u sec %u crc %s ssid '%s' | ac load %08lX '%s'; ", i, res,
                                  R_SUCCEEDED(res) ? slot.version : 0, R_SUCCEEDED(res) ? wifislots_network_of(&slot)->security : 0,
                                  R_SUCCEEDED(res) && wifislots_crc_ok(&slot) ? "ok" : "BAD", name, loadRes, acName);
    }
}

// SITE WIFIKEYCHECK: for each connection, whether the key worked out from its name and password is the one it holds
// (it says yes or no, never the key or the password).
void wifislots_debug_keycheck(char* out, size_t size) {
    size_t used = 0;
    for(u32 i = 0; i < WIFISLOTS_COUNT && used < size; i++) {
        wifislots_slot slot;
        if(R_FAILED(wifislots_read(i, &slot)) || wifislots_empty(&slot)) {
            used += (size_t) snprintf(out + used, size - used, "slot %lu: empty; ", i);
            continue;
        }

        char name[33];
        char password[65];
        wifislots_name(&slot, name, sizeof(name));
        wifislots_password(&slot, password, sizeof(password));
        const wifislots_network* held = wifislots_network_of(&slot);
        wifislots_slot made = slot;
        const char* problem = wifislots_set_network(&made, name, held->security, password);
        bool zero = true;
        for(u32 b = 0; b < sizeof(held->key); b++) {
            zero = zero && held->key[b] == 0;
        }

        used += (size_t) snprintf(out + used, size - used, "slot %lu: sec %u, %s; ", i, held->security,
                                  problem != NULL ? problem : memcmp(made.network.key, held->key, sizeof(held->key)) == 0 ? "key matches" : zero ? "held key is zero" : "KEY DIFFERS");
    }
}

// The bytes 0x4..0x414 (what the CRC covers) that differ between two connections, as ranges.
static u32 wifislots_diff(const wifislots_slot* a, const wifislots_slot* b, char* out, size_t size) {
    const u8* x = (const u8*) a;
    const u8* y = (const u8*) b;
    size_t used = 0;
    u32 count = 0;
    out[0] = '\0';
    for(u32 i = 4; i < 0x414 && used < size; i++) {
        if(x[i] != y[i]) {
            u32 j = i;
            while(j + 1 < 0x414 && x[j + 1] != y[j + 1]) {
                j++;
            }

            used += (size_t) snprintf(out + used, size - used, "%s%03lX-%03lX", count > 0 ? "," : "", i, j);
            count++;
            i = j;
        }
    }

    if(count == 0) {
        snprintf(out, size, "none");
    }

    return count;
}

// SITE WIFIROUNDTRIP <slot> [change]: writes connection `slot` (refused if it is the one in use) through ac:i as it is, or
// with its passphrase changed (change = 1), reads the config back and says which bytes differ from what was meant. With
// a change, it is then written back as it was the same way.
bool wifislots_debug_roundtrip(u32 index, u32 change, char* out, size_t size) {
    char connected[33] = "";
    u32 status = 0;
    if(R_SUCCEEDED(ACU_GetWifiStatus(&status)) && status != 0) {
        ACU_GetSSID(connected);
    }

    wifislots_slot saved;
    Result res = wifislots_read(index, &saved);
    char name[33] = "";
    if(R_SUCCEEDED(res) && !wifislots_empty(&saved)) {
        wifislots_name(&saved, name, sizeof(name));
    }

    if(R_FAILED(res) || wifislots_empty(&saved) || strcmp(name, connected) == 0) {
        snprintf(out, size, "slot %lu: read %08lX, empty or in use: not touched", index, res);
        return false;
    }

    wifislots_slot meant = saved;
    if(change) {
        wifislots_network* net = (wifislots_network*) wifislots_network_of(&meant);
        size_t len = strnlen(net->password, sizeof(net->password));
        if(len + 1 < sizeof(net->password)) {
            net->password[len] = 'X';
        }
    }

    meant.crc = wifislots_crc16((const u8*) &meant + 4, 0x410);
    const char* step = "";
    res = wifislots_write(index, &meant, &step);

    wifislots_slot now;
    Result readRes = wifislots_read(index, &now);
    char diff[160];
    u32 differ = wifislots_diff(&meant, &now, diff, sizeof(diff));
    size_t used = (size_t) snprintf(out, size, "slot %lu: write %08lX%s%s, read %08lX, crc %s, %lu ranges differ: %s", index, res,
                                    step[0] != '\0' ? " at " : "", step, readRes, wifislots_crc_ok(&now) ? "ok" : "BAD", differ, diff);

    if(change) {
        res = wifislots_write(index, &saved, &step);
        wifislots_read(index, &now);
        differ = wifislots_diff(&saved, &now, diff, sizeof(diff));
        snprintf(out + used, size - used, " | back: write %08lX%s%s, %lu ranges differ: %s", res, step[0] != '\0' ? " at " : "", step, differ, diff);
    }

    return true;
}

// ---- editing ----

void wifislots_new(wifislots_slot* slot) {
    memset(slot, 0, sizeof(*slot));
    slot->version = 1;
    slot->enabled = 1;
    slot->editableSecurity = 1;
    slot->settings.dhcp = 1;
    slot->settings.autoDns = 1;
    slot->settings.mtu = 1400;
}

// The key the system connects with: for WPA the PSK (PBKDF2-HMAC-SHA1 of the password, salted with the network's name,
// 4096 rounds, as the console stores it: checked against connections set up in System Settings), for WEP the password's
// own bytes.
static bool wifislots_make_key(wifislots_network* net, u8 ssidLength) {
    memset(net->key, 0, sizeof(net->key));
    size_t len = strnlen(net->password, sizeof(net->password));
    if(net->security == WIFISLOTS_OPEN) {
        return true;
    }

    if(net->security <= WIFISLOTS_WEP_152) {
        memcpy(net->key, net->password, len < sizeof(net->key) ? len : sizeof(net->key));
        return true;
    }

    mbedtls_md_context_t md;
    mbedtls_md_init(&md);
    bool ok = mbedtls_md_setup(&md, mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), 1) == 0
              && mbedtls_pkcs5_pbkdf2_hmac(&md, (const unsigned char*) net->password, len, (const unsigned char*) net->ssid, ssidLength, 4096,
                                           sizeof(net->key), net->key) == 0;
    mbedtls_md_free(&md);
    return ok;
}

const char* wifislots_set_network(wifislots_slot* slot, const char* ssid, u8 security, const char* password) {
    size_t ssidLength = strlen(ssid);
    size_t passwordLength = strlen(password);
    if(ssidLength == 0 || ssidLength > sizeof(slot->network.ssid)) {
        return "The network's name takes 1 to 32 characters.";
    }

    if(security >= WIFISLOTS_WPA_TKIP && (passwordLength < 8 || passwordLength > 63)) {
        return "A WPA password takes 8 to 63 characters.";
    }

    if(security >= WIFISLOTS_WEP_64 && security <= WIFISLOTS_WEP_152 && passwordLength != 5 && passwordLength != 13 && passwordLength != 16) {
        return "A WEP password takes 5, 13 or 16 characters.";
    }

    // Typed by hand: a plain network (one set up with WPS or AOSS becomes one), as System Settings' manual setup.
    slot->enabled = 1;
    slot->editableSecurity = 1;
    slot->multiSsidEnabled = 0;
    slot->multiSsidType = 0;
    slot->multiSsidCount = 0;
    memset(slot->multiSsid, 0, sizeof(slot->multiSsid));

    wifislots_network before = slot->network;
    wifislots_network* net = &slot->network;
    memset(net, 0, sizeof(*net));
    memcpy(net->ssid, ssid, ssidLength);
    net->ssidLength = (u8) ssidLength;
    net->security = security;
    if(security != WIFISLOTS_OPEN) {
        memcpy(net->password, password, passwordLength);
    }

    if(!wifislots_make_key(net, net->ssidLength)) {
        return "Failed to work out the network's key.";
    }

    // The connection test passed for another network, not for this one.
    if(memcmp(&before, net, sizeof(before)) != 0) {
        slot->settings.tested = 0;
    }

    return NULL;
}

bool wifislots_parse_ip(const char* text, u8* out) {
    unsigned a, b, c, d;
    char rest;
    if(sscanf(text, "%u.%u.%u.%u%c", &a, &b, &c, &d, &rest) != 4 || a > 255 || b > 255 || c > 255 || d > 255) {
        return false;
    }

    out[0] = (u8) a;
    out[1] = (u8) b;
    out[2] = (u8) c;
    out[3] = (u8) d;
    return true;
}

Result wifislots_update_backup(wifislots_backup* backup, const wifislots_slot* slot) {
    Result res = wifislots_write_file(backup->path, slot, backup->saved != 0 ? backup->saved : time(NULL), backup->alias);
    if(R_SUCCEEDED(res)) {
        backup->slot = *slot;
    }

    return res;
}
