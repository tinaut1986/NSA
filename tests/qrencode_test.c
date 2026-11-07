// Encodes texts of every length up to the largest version (and a few of NSA's own addresses), draws each code as
// an image with its quiet zone and reads it back with quirc, the decoder NSA's camera uses.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../source/core/qrencode.h"
#include "../source/libs/quirc/quirc.h"

#define SCALE 4
#define QUIET 4

static int failures = 0;

static void check(const char* text) {
    qr_code qr;
    if(!qr_encode(text, &qr)) {
        printf("FAIL encode: %zu bytes did not fit\n", strlen(text));
        failures++;
        return;
    }

    int side = (qr.size + 2 * QUIET) * SCALE;
    struct quirc* q = quirc_new();
    if(q == NULL || quirc_resize(q, side, side) < 0) {
        printf("FAIL quirc\n");
        exit(1);
    }

    int w, h;
    uint8_t* image = quirc_begin(q, &w, &h);
    memset(image, 0xFF, (size_t) w * h);
    for(int y = 0; y < qr.size; y++) {
        for(int x = 0; x < qr.size; x++) {
            if(qr_dark(&qr, x, y)) {
                for(int dy = 0; dy < SCALE; dy++) {
                    memset(image + ((y + QUIET) * SCALE + dy) * w + (x + QUIET) * SCALE, 0, SCALE);
                }
            }
        }
    }

    quirc_end(q);

    int found = quirc_count(q);
    bool ok = false;
    for(int i = 0; i < found && !ok; i++) {
        struct quirc_code code;
        struct quirc_data data;
        quirc_extract(q, i, &code);
        quirc_decode_error_t err = quirc_decode(&code, &data);
        if(err == QUIRC_SUCCESS && data.payload_len == (int) strlen(text) && memcmp(data.payload, text, strlen(text)) == 0) {
            ok = true;
        } else if(err != QUIRC_SUCCESS) {
            printf("  decode error: %s\n", quirc_strerror(err));
        }
    }

    if(!ok) {
        printf("FAIL decode: %zu bytes (version %d, %d codes found): %.60s\n", strlen(text), (qr.size - 17) / 4, found, text);
        failures++;
    }

    quirc_destroy(q);
}

int main() {
    check("http://192.168.1.133:8080/?pin=123456");
    check("http://10.0.0.2:8080/?pin=000000");
    check("A");

    char text[256];
    for(int len = 1; len <= 213; len++) {
        for(int i = 0; i < len; i++) {
            text[i] = (char) ('!' + (i * 7 + len) % 90);
        }

        text[len] = '\0';
        check(text);
    }

    qr_code qr;
    memset(text, 'x', 214);
    text[214] = '\0';
    if(qr_encode(text, &qr)) {
        printf("FAIL: 214 bytes should not fit version 10\n");
        failures++;
    }

    printf(failures == 0 ? "qrencode: all codes read back\n" : "qrencode: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
