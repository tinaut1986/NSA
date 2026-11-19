// DS game forwarders: filling a template SRL for a game (source/fbi/ndsfwd.c). The template and the games are made here:
// a header with its checksum, an icon block, the path's placeholder.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../source/fbi/ndsfwd.h"

static int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if(!(cond)) {                                                   \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                 \
        }                                                               \
    } while(0)

#define TEMPLATE_SIZE 0x30000
#define TEMPLATE_BANNER 0x29E00
#define TEMPLATE_PATH 0x1DD74
#define TEMPLATE_PATH_LEN 252

static uint16_t get16(const uint8_t* p) { return (uint16_t) (p[0] | (p[1] << 8)); }
static void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8); }
static void put32(uint8_t* p, uint32_t v) { put16(p, (uint16_t) v); put16(p + 2, (uint16_t) (v >> 16)); }

static void seal_header(uint8_t* h) { put16(h + 0x15E, ndsfwd_crc16(h, 0x15E)); }

static void put_name(uint8_t* banner, int language, const char* text) {
    memset(banner + 0x240 + language * 0x100, 0, 0x100);
    for(int c = 0; text[c]; c++) {
        put16(banner + 0x240 + language * 0x100 + c * 2, (uint8_t) text[c]);
    }
}

// The template: "HOMEBREW" "####", a version 0x103 icon block, "sd:/<<<..." where the path goes.
static uint8_t* make_template(void) {
    uint8_t* srl = calloc(1, TEMPLATE_SIZE);
    memcpy(srl, "HOMEBREW\0\0\0\0####", 16);
    put32(srl + 0x68, TEMPLATE_BANNER);
    srl[0x230] = '#';
    srl[0x231] = '#';
    srl[0x232] = '#';
    srl[0x233] = '#';
    srl[0x234] = 0x04;
    srl[0x236] = 0x03;
    uint8_t* banner = srl + TEMPLATE_BANNER;
    put16(banner, 0x0103);
    memcpy(srl + TEMPLATE_PATH - 4, "sd:/<<<Start NDS Path", 21);
    seal_header(srl);
    return srl;
}

// A game: `version` icon block at 0x4000 with an English name of two lines and a publisher.
static uint8_t* make_game(const char* code, uint16_t version, size_t* size) {
    *size = 0x8000;
    uint8_t* nds = calloc(1, *size);
    memcpy(nds, "MYGAMETITLE", 11);
    memcpy(nds + 0x0C, code, 4);
    memcpy(nds + 0x10, "01", 2);
    put32(nds + 0x68, 0x4000);
    seal_header(nds);

    uint8_t* banner = nds + 0x4000;
    put16(banner, version);
    for(int i = 0; i < 0x200; i++) {
        banner[0x20 + i] = (uint8_t) (i * 7);
    }

    put16(banner + 0x220 + 2, 0x7FFF);   // Colour 1: white.
    for(int l = 0; l < 6; l++) {
        put_name(banner, l, "Game\nSubtitle\nMaker");
    }

    put_name(banner, 2, "Jeu\nMaker");
    put16(banner + 2, ndsfwd_crc16(banner + 0x20, 0x820));
    return nds;
}

int main(void) {
    ndsfwd_template t;
    CHECK(ndsfwd_parse_template("name=DSi/3DS SD Card\r\ngamepath_location=0x1DD74\r\ngamepath_length=252\r\nbanner_location=0x29E00\r\n", &t));
    CHECK(t.pathOffset == TEMPLATE_PATH && t.pathLength == TEMPLATE_PATH_LEN);
    CHECK(!ndsfwd_parse_template("name=nothing\n", &t));
    ndsfwd_parse_template("gamepath_location=0x1DD74\ngamepath_length=252\n", &t);

    uint8_t* tpl = make_template();
    CHECK(ndsfwd_check_template(tpl, TEMPLATE_SIZE, &t));
    CHECK(!ndsfwd_check_template(tpl, TEMPLATE_BANNER + 0x100, &t));

    // A retail game.
    size_t size;
    uint8_t* nds = make_game("ASME", 1, &size);
    ndsfwd_game game;
    CHECK(ndsfwd_read_game(nds, nds + ndsfwd_banner_offset(nds), 0x840, &game) == NDSFWD_OK);
    char name[128];
    ndsfwd_name(&game, 1, name, sizeof(name));
    CHECK(strcmp(name, "Game Subtitle") == 0);
    ndsfwd_name(&game, 2, name, sizeof(name));
    CHECK(strcmp(name, "Jeu") == 0);
    ndsfwd_name(&game, 7, name, sizeof(name));   // Korean is missing in version 1: English.
    CHECK(strcmp(name, "Game Subtitle") == 0);

    uint8_t* srl = malloc(TEMPLATE_SIZE);
    memcpy(srl, tpl, TEMPLATE_SIZE);
    CHECK(ndsfwd_make_srl(srl, TEMPLATE_SIZE, &t, &game, "/roms/nds/My Game.nds") == NDSFWD_OK);
    CHECK(ndsfwd_crc16(srl, 0x15E) == get16(srl + 0x15E));
    CHECK(memcmp(srl, "MYGAMETITLE", 11) == 0 && memcmp(srl + 0x0C, "ASME01", 6) == 0);
    CHECK(ndsfwd_title_id(srl) == 0x0004800441534D45ULL);
    CHECK(ndsfwd_game_title_id(&game, "/roms/nds/My Game.nds") == 0x0004800441534D45ULL);
    CHECK(memcmp(srl + TEMPLATE_PATH - 4, "sd:/roms/nds/My Game.nds\0", 25) == 0);
    uint8_t* b = srl + TEMPLATE_BANNER;
    CHECK(get16(b) == 0x0103);
    CHECK(get16(b + 2) == ndsfwd_crc16(b + 0x20, 0x820));
    CHECK(get16(b + 4) == ndsfwd_crc16(b + 0x20, 0x920));
    CHECK(get16(b + 6) == ndsfwd_crc16(b + 0x20, 0xA20));
    CHECK(get16(b + 8) == ndsfwd_crc16(b + 0x1240, 0x1180));
    CHECK(memcmp(b + 0x20, nds + 0x4020, 0x200) == 0 && memcmp(b + 0x1240 + 7 * 0x200, nds + 0x4020, 0x200) == 0);
    CHECK(b[0x2340] == 0x01 && b[0x2343] == 0x01 && b[0x2344] == 0);

    // The filled SRL is no template any more; the copy is.
    CHECK(!ndsfwd_check_template(srl, TEMPLATE_SIZE, &t));
    CHECK(ndsfwd_check_template(tpl, TEMPLATE_SIZE, &t));

    // Paths: not on the card's root form, too long.
    memcpy(srl, tpl, TEMPLATE_SIZE);
    CHECK(ndsfwd_make_srl(srl, TEMPLATE_SIZE, &t, &game, "roms/x.nds") == NDSFWD_PATH_TOO_LONG);
    char longPath[300] = "/";
    memset(longPath + 1, 'a', 252);
    CHECK(ndsfwd_make_srl(srl, TEMPLATE_SIZE, &t, &game, longPath) == NDSFWD_PATH_TOO_LONG);
    longPath[252] = '\0';   // 251 characters after the slash: fits with its zero.
    CHECK(ndsfwd_make_srl(srl, TEMPLATE_SIZE, &t, &game, longPath) == NDSFWD_OK);

    // Homebrew ("####"): a code from the path, a different one for another path, the same for the same.
    free(nds);
    nds = make_game("####", 1, &size);
    CHECK(ndsfwd_read_game(nds, nds + 0x4000, 0x840, &game) == NDSFWD_OK);
    memcpy(srl, tpl, TEMPLATE_SIZE);
    CHECK(ndsfwd_make_srl(srl, TEMPLATE_SIZE, &t, &game, "/a.nds") == NDSFWD_OK);
    uint64_t a = ndsfwd_title_id(srl);
    memcpy(srl, tpl, TEMPLATE_SIZE);
    ndsfwd_make_srl(srl, TEMPLATE_SIZE, &t, &game, "/b.nds");
    uint64_t bId = ndsfwd_title_id(srl);
    memcpy(srl, tpl, TEMPLATE_SIZE);
    ndsfwd_make_srl(srl, TEMPLATE_SIZE, &t, &game, "/a.nds");
    CHECK(a != bId && a == ndsfwd_title_id(srl) && a == ndsfwd_game_title_id(&game, "/a.nds"));
    CHECK((a >> 32) == 0x00048004 && srl[0x0C] >= 'A' && srl[0x0C] <= 'Z');

    // Damage: the header, the icon block, no icon block.
    nds[0x20] ^= 1;
    CHECK(ndsfwd_read_game(nds, nds + 0x4000, 0x840, &game) == NDSFWD_NOT_NDS);
    nds[0x20] ^= 1;
    nds[0x4100] ^= 1;
    CHECK(ndsfwd_read_game(nds, nds + 0x4000, 0x840, &game) == NDSFWD_BAD_BANNER);
    nds[0x4100] ^= 1;
    CHECK(ndsfwd_read_game(nds, nds + 0x4000, 0x200, &game) == NDSFWD_BAD_BANNER);
    put32(nds + 0x68, 0);
    seal_header(nds);
    CHECK(ndsfwd_read_game(nds, nds + 0x4000, 0x840, &game) == NDSFWD_NO_BANNER);

    // DSi-enhanced (unit code 2) with an empty DSi title ID, as Pokémon Black and White: the ID still comes from the code.
    free(nds);
    nds = make_game("IRBS", 1, &size);
    nds[0x12] = 2;
    seal_header(nds);
    CHECK(ndsfwd_read_game(nds, nds + 0x4000, 0x840, &game) == NDSFWD_OK);
    CHECK(ndsfwd_game_title_id(&game, "/roms/nds/x.nds") == 0x0004800449524253ULL);
    memcpy(srl, tpl, TEMPLATE_SIZE);
    CHECK(ndsfwd_make_srl(srl, TEMPLATE_SIZE, &t, &game, "/roms/nds/x.nds") == NDSFWD_OK && ndsfwd_title_id(srl) == 0x0004800449524253ULL);

    // A version 3 block that says so but whose newer checksums do not match is read as version 1.
    free(nds);
    nds = make_game("BXYZ", 3, &size);
    CHECK(ndsfwd_read_game(nds, nds + 0x4000, 0xA40, &game) == NDSFWD_OK);

    // The icon: index 0 is transparent, colour 1 white.
    uint8_t rgba[32 * 32 * 4];
    memset(game.icon, 0x10, sizeof(game.icon));   // Even pixels 0, odd pixels 1.
    ndsfwd_icon_rgba(&game, rgba);
    CHECK(rgba[3] == 0 && rgba[4] == 255 && rgba[5] == 255 && rgba[6] == 255 && rgba[7] == 255);

    free(nds);
    free(srl);
    free(tpl);
    if(failures == 0) {
        printf("ndsfwd_test: all passed\n");
    }

    return failures == 0 ? 0 : 1;
}
