#include <ctype.h>
#include <string.h>

#include "wifiqr.h"

static bool wifiqr_prefix(const char* text, const char* prefix) {
    for(; *prefix != '\0'; text++, prefix++) {
        if(toupper((unsigned char) *text) != toupper((unsigned char) *prefix)) {
            return false;
        }
    }

    return true;
}

// Copies a value up to its ';' (unescaped), into `out` (cut at `size` - 1, and `*cut` set). Returns where it ended.
static const char* wifiqr_value(const char* p, char* out, size_t size, bool* cut) {
    size_t len = 0;
    *cut = false;
    while(*p != '\0' && *p != ';') {
        char c = *p++;
        if(c == '\\' && *p != '\0') {
            c = *p++;
        }

        if(len + 1 < size) {
            out[len++] = c;
        } else {
            *cut = true;
        }
    }

    out[len] = '\0';
    return *p == ';' ? p + 1 : p;
}

wifiqr_result wifiqr_parse(const char* text, wifiqr_network* out) {
    memset(out, 0, sizeof(*out));
    while(*text == ' ' || *text == '\n' || *text == '\r' || *text == '\t') {
        text++;
    }

    if(!wifiqr_prefix(text, "WIFI:")) {
        return WIFIQR_NOT_WIFI;
    }

    const char* p = text + 5;
    char type[16] = "";
    bool hasType = false;
    bool enterprise = false;
    bool ssidCut = false;
    bool passwordCut = false;
    while(*p != '\0' && *p != ';') {
        // A key is letters up to ':'.
        char key[8];
        size_t keyLen = 0;
        while(*p != '\0' && *p != ':' && *p != ';') {
            if(keyLen + 1 < sizeof(key)) {
                key[keyLen++] = (char) toupper((unsigned char) *p);
            }

            p++;
        }

        key[keyLen] = '\0';
        if(*p != ':') {
            // "KEY;" without a value: skip it.
            if(*p == ';') {
                p++;
            }

            continue;
        }

        p++;
        char scratch[128];
        bool cut = false;
        if(strcmp(key, "S") == 0) {
            p = wifiqr_value(p, out->ssid, sizeof(out->ssid), &ssidCut);
        } else if(strcmp(key, "P") == 0) {
            p = wifiqr_value(p, out->password, sizeof(out->password), &passwordCut);
        } else if(strcmp(key, "T") == 0) {
            p = wifiqr_value(p, type, sizeof(type), &cut);
            hasType = true;
        } else if(strcmp(key, "H") == 0) {
            p = wifiqr_value(p, scratch, sizeof(scratch), &cut);
            out->hidden = wifiqr_prefix(scratch, "true") || strcmp(scratch, "1") == 0;
        } else {
            // E (EAP method), A, I (identity), PH2 and anything new: only EAP ones matter, as unsupported.
            if(strcmp(key, "E") == 0 || strcmp(key, "I") == 0 || strcmp(key, "PH2") == 0) {
                enterprise = true;
            }

            p = wifiqr_value(p, scratch, sizeof(scratch), &cut);
        }
    }

    if(out->ssid[0] == '\0' || ssidCut) {
        return WIFIQR_NO_NAME;
    }

    if(passwordCut) {
        return WIFIQR_TOO_LONG;
    }

    for(char* c = type; *c != '\0'; c++) {
        *c = (char) toupper((unsigned char) *c);
    }

    if(enterprise || strstr(type, "EAP") != NULL || strstr(type, "SAE") != NULL || strstr(type, "WPA3") != NULL) {
        out->kind = WIFIQR_UNSUPPORTED;
    } else if(strcmp(type, "WEP") == 0) {
        out->kind = WIFIQR_WEP;
    } else if(strncmp(type, "WPA", 3) == 0) {
        out->kind = WIFIQR_WPA;
    } else if(strcmp(type, "NOPASS") == 0 || ((!hasType || type[0] == '\0') && out->password[0] == '\0')) {
        out->kind = WIFIQR_OPEN;
    } else {
        // No type (or one unknown) with a password: WPA, as phones take it.
        out->kind = WIFIQR_WPA;
    }

    if(out->kind == WIFIQR_OPEN) {
        out->password[0] = '\0';
    }

    return WIFIQR_OK;
}

static bool wifiqr_append(char* out, size_t size, size_t* len, const char* text, bool escape) {
    for(; *text != '\0'; text++) {
        bool special = escape && strchr("\\;,:\"", *text) != NULL;
        if(*len + (special ? 2 : 1) >= size) {
            return false;
        }

        if(special) {
            out[(*len)++] = '\\';
        }

        out[(*len)++] = *text;
    }

    out[*len] = '\0';
    return true;
}

bool wifiqr_build(const char* ssid, wifiqr_kind kind, const char* password, char* out, size_t size) {
    if(size == 0) {
        return false;
    }

    size_t len = 0;
    out[0] = '\0';
    const char* type = kind == WIFIQR_WEP ? "WEP" : kind == WIFIQR_WPA ? "WPA" : "nopass";
    bool ok = wifiqr_append(out, size, &len, "WIFI:T:", false) && wifiqr_append(out, size, &len, type, false)
              && wifiqr_append(out, size, &len, ";S:", false) && wifiqr_append(out, size, &len, ssid, true);
    if(ok && kind != WIFIQR_OPEN) {
        ok = wifiqr_append(out, size, &len, ";P:", false) && wifiqr_append(out, size, &len, password, true);
    }

    return ok && wifiqr_append(out, size, &len, ";;", false);
}
