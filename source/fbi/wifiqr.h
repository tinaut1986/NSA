#pragma once

#include <stdbool.h>
#include <stddef.h>

// Wi-Fi networks as QR codes, in the format phones read and show ("WIFI:T:WPA;S:<name>;P:<password>;H:true;;"):
// fields in any order, `\` before `\ ; , : "` in the values. Plain C (no 3DS calls): tests/wifiqr_test.c runs it on the
// host.

typedef enum {
    WIFIQR_OPEN,
    WIFIQR_WEP,
    WIFIQR_WPA,         // WPA or WPA2 with a password ("WPA", "WPA2", "WPA/WPA2", or no T with a password).
    WIFIQR_UNSUPPORTED  // WPA3 (SAE) or enterprise (EAP): the console cannot join them.
} wifiqr_kind;

typedef struct {
    char ssid[33];
    char password[65];
    wifiqr_kind kind;
    bool hidden;
} wifiqr_network;

typedef enum {
    WIFIQR_OK,
    WIFIQR_NOT_WIFI,    // Not a "WIFI:" code.
    WIFIQR_NO_NAME,     // No network name, or one longer than 32 bytes.
    WIFIQR_TOO_LONG,    // A password longer than 64 bytes.
} wifiqr_result;

wifiqr_result wifiqr_parse(const char* text, wifiqr_network* out);
// The code's text for a network; false when it does not fit in `size`.
bool wifiqr_build(const char* ssid, wifiqr_kind kind, const char* password, char* out, size_t size);
