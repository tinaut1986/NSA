// Wi-Fi QR codes: reading the text phones and routers print, and writing it back.

#include <stdio.h>
#include <string.h>

#include "../source/fbi/wifiqr.h"

static int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if(!(cond)) {                                                   \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                 \
        }                                                               \
    } while(0)

int main(void) {
    wifiqr_network n;

    CHECK(wifiqr_parse("WIFI:T:WPA;S:Casa;P:secreta123;;", &n) == WIFIQR_OK);
    CHECK(strcmp(n.ssid, "Casa") == 0 && strcmp(n.password, "secreta123") == 0 && n.kind == WIFIQR_WPA && !n.hidden);

    // Any order, lower case keys and values, hidden.
    CHECK(wifiqr_parse("wifi:s:My Net;h:true;p:abc;t:wpa2;;", &n) == WIFIQR_OK);
    CHECK(strcmp(n.ssid, "My Net") == 0 && strcmp(n.password, "abc") == 0 && n.kind == WIFIQR_WPA && n.hidden);

    // Escapes.
    CHECK(wifiqr_parse("WIFI:S:a\\;b\\:c\\\\d\\,e\\\"f;T:WPA;P:p\\;w;;", &n) == WIFIQR_OK);
    CHECK(strcmp(n.ssid, "a;b:c\\d,e\"f") == 0 && strcmp(n.password, "p;w") == 0);

    // Open networks.
    CHECK(wifiqr_parse("WIFI:T:nopass;S:Free;P:;;", &n) == WIFIQR_OK && n.kind == WIFIQR_OPEN && n.password[0] == '\0');
    CHECK(wifiqr_parse("WIFI:S:Free;;", &n) == WIFIQR_OK && n.kind == WIFIQR_OPEN);
    CHECK(wifiqr_parse("WIFI:S:NoType;P:12345678;;", &n) == WIFIQR_OK && n.kind == WIFIQR_WPA);

    CHECK(wifiqr_parse("WIFI:T:WEP;S:Old;P:abcde;;", &n) == WIFIQR_OK && n.kind == WIFIQR_WEP);
    CHECK(wifiqr_parse("WIFI:T:SAE;S:New;P:12345678;;", &n) == WIFIQR_OK && n.kind == WIFIQR_UNSUPPORTED);
    CHECK(wifiqr_parse("WIFI:T:WPA2-EAP;S:Work;E:PEAP;I:me;P:x;;", &n) == WIFIQR_OK && n.kind == WIFIQR_UNSUPPORTED);
    CHECK(wifiqr_parse("WIFI:T:WPA;S:Work;E:PEAP;P:x;;", &n) == WIFIQR_OK && n.kind == WIFIQR_UNSUPPORTED);

    CHECK(wifiqr_parse("https://example.com", &n) == WIFIQR_NOT_WIFI);
    CHECK(wifiqr_parse("WIFI:T:WPA;P:12345678;;", &n) == WIFIQR_NO_NAME);
    CHECK(wifiqr_parse("WIFI:S:123456789012345678901234567890123;;", &n) == WIFIQR_NO_NAME);
    CHECK(wifiqr_parse("WIFI:S:12345678901234567890123456789012;;", &n) == WIFIQR_OK);
    char longPassword[100] = "WIFI:T:WPA;S:x;P:";
    strcat(longPassword, "12345678901234567890123456789012345678901234567890123456789012345;;");
    CHECK(wifiqr_parse(longPassword, &n) == WIFIQR_TOO_LONG);

    // Writing, and reading it back.
    char text[256];
    CHECK(wifiqr_build("a;b", WIFIQR_WPA, "p:w\\", text, sizeof(text)));
    CHECK(strcmp(text, "WIFI:T:WPA;S:a\\;b;P:p\\:w\\\\;;") == 0);
    CHECK(wifiqr_parse(text, &n) == WIFIQR_OK && strcmp(n.ssid, "a;b") == 0 && strcmp(n.password, "p:w\\") == 0 && n.kind == WIFIQR_WPA);
    CHECK(wifiqr_build("Free", WIFIQR_OPEN, "ignored", text, sizeof(text)));
    CHECK(strcmp(text, "WIFI:T:nopass;S:Free;;") == 0);
    CHECK(!wifiqr_build("Free", WIFIQR_OPEN, "", text, 10));

    if(failures == 0) {
        printf("wifiqr_test: all passed\n");
    }

    return failures == 0 ? 0 : 1;
}
