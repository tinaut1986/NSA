#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ndsfwd.h"

// Cartridge header (GBATEK, "DS Cartridge Header").
#define H_TITLE 0x000
#define H_CODE 0x00C
#define H_MAKER 0x010
#define H_BANNER 0x068
#define H_CRC 0x15E
#define H_TID_LOW 0x230   // DSi: the title ID's low half (its high half follows).

// Icon/title block (GBATEK, "DS Cartridge Icon/Title").
#define B_VERSION 0x000
#define B_CRC 0x002       // Four CRCs: versions 1, 2 and 3 (from 0x20) and the animated icon's (from 0x1240).
#define B_ICON 0x020
#define B_PALETTE 0x220
#define B_NAMES 0x240
#define B_ANIM_ICONS 0x1240
#define B_ANIM_PALETTES 0x2240
#define B_ANIM_SEQUENCE 0x2340

#define PLACEHOLDER "<<<"

static uint32_t get32(const uint8_t* p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static uint16_t get16(const uint8_t* p) {
    return (uint16_t) (p[0] | (p[1] << 8));
}

static void put16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t) v;
    p[1] = (uint8_t) (v >> 8);
}

uint16_t ndsfwd_crc16(const uint8_t* data, size_t size) {
    uint16_t crc = 0xFFFF;
    for(size_t i = 0; i < size; i++) {
        crc ^= data[i];
        for(int bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (uint16_t) ((crc >> 1) ^ 0xA001) : (uint16_t) (crc >> 1);
        }
    }

    return crc;
}

// How many bytes of the icon block a version has.
static size_t banner_size(uint16_t version) {
    switch(version) {
        case 0x0001: return 0x840;
        case 0x0002: return 0x940;
        case 0x0003: return 0xA40;
        case 0x0103: return 0x23C0;
        default: return 0;
    }
}

bool ndsfwd_parse_template(const char* text, ndsfwd_template* out) {
    memset(out, 0, sizeof(*out));
    const char* line = text;
    while(line != NULL && *line) {
        const char* end = strchr(line, '\n');
        size_t len = end != NULL ? (size_t) (end - line) : strlen(line);
        char buf[128];
        if(len < sizeof(buf)) {
            memcpy(buf, line, len);
            buf[len] = '\0';
            char* cr = strchr(buf, '\r');
            if(cr != NULL) {
                *cr = '\0';
            }

            char* eq = strchr(buf, '=');
            if(eq != NULL) {
                *eq = '\0';
                unsigned long value = strtoul(eq + 1, NULL, 0);
                if(strcmp(buf, "gamepath_location") == 0) {
                    out->pathOffset = (uint32_t) value;
                } else if(strcmp(buf, "gamepath_length") == 0) {
                    out->pathLength = (uint32_t) value;
                }
            }
        }

        line = end != NULL ? end + 1 : NULL;
    }

    return out->pathOffset != 0 && out->pathLength > 1;
}

uint32_t ndsfwd_banner_offset(const uint8_t* header) {
    return get32(header + H_BANNER);
}

bool ndsfwd_check_template(const uint8_t* srl, size_t size, const ndsfwd_template* t) {
    if(size < NDSFWD_HEADER_SIZE || ndsfwd_crc16(srl, H_CRC) != get16(srl + H_CRC)) {
        return false;
    }

    uint32_t banner = ndsfwd_banner_offset(srl);
    if(banner == 0 || banner > size || size - banner < NDSFWD_BANNER_MAX || get16(srl + banner + B_VERSION) != 0x0103) {
        return false;
    }

    if(t->pathOffset > size || size - t->pathOffset < t->pathLength) {
        return false;
    }

    return memcmp(srl + t->pathOffset, PLACEHOLDER, strlen(PLACEHOLDER)) == 0;
}

ndsfwd_result ndsfwd_read_game(const uint8_t* header, const uint8_t* banner, size_t bannerSize, ndsfwd_game* out) {
    memset(out, 0, sizeof(*out));
    if(ndsfwd_crc16(header, H_CRC) != get16(header + H_CRC)) {
        return NDSFWD_NOT_NDS;
    }

    if(ndsfwd_banner_offset(header) == 0) {
        return NDSFWD_NO_BANNER;
    }

    if(bannerSize < 0x840) {
        return NDSFWD_BAD_BANNER;
    }

    uint16_t version = get16(banner + B_VERSION);
    size_t size = banner_size(version);
    if(size == 0 || bannerSize < size || ndsfwd_crc16(banner + B_ICON, 0x820) != get16(banner + B_CRC)) {
        // A block that says it is newer but is damaged past version 1 still has a good version 1.
        if(bannerSize < 0x840 || ndsfwd_crc16(banner + B_ICON, 0x820) != get16(banner + B_CRC)) {
            return NDSFWD_BAD_BANNER;
        }

        version = 1;
    }

    memcpy(out->gameTitle, header + H_TITLE, sizeof(out->gameTitle));
    memcpy(out->gameCode, header + H_CODE, sizeof(out->gameCode));
    memcpy(out->maker, header + H_MAKER, sizeof(out->maker));
    // The title ID from the game code, also for a DSi-enhanced game: its DSi header would hold the same, but some leave it
    // empty (Pokémon Black and White: unit code 2, title ID 0), and a forwarder installed as 0004800400000000 hangs on
    // white screens.
    for(int i = 0; i < 4; i++) {
        out->emagCode[i] = out->gameCode[3 - i];
    }

    memcpy(out->icon, banner + B_ICON, sizeof(out->icon));
    memcpy(out->palette, banner + B_PALETTE, sizeof(out->palette));

    // Version 1 has six names, 2 adds Chinese, 3 Korean; the missing ones are the English one.
    int count = version >= 3 ? 8 : version == 2 ? 7 : 6;
    for(int i = 0; i < NDSFWD_NAMES; i++) {
        const uint8_t* name = banner + B_NAMES + (i < count ? i : 1) * NDSFWD_NAME_CHARS * 2;
        for(int c = 0; c < NDSFWD_NAME_CHARS; c++) {
            out->names[i][c] = get16(name + c * 2);
        }
    }

    return NDSFWD_OK;
}

static bool code_char(uint8_t c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

// A code for a game without one of its own: four letters from the path.
static void code_from_path(const char* path, uint8_t* code) {
    uint32_t hash = 2166136261u;
    for(const char* p = path; *p; p++) {
        hash = (hash ^ (uint8_t) *p) * 16777619u;
    }

    for(int i = 0; i < 4; i++) {
        code[i] = (uint8_t) ('A' + hash % 26);
        hash /= 26;
    }
}

// The code and title ID half the forwarder gets: the game's, or one from the path when it has none.
static void game_codes(const ndsfwd_game* game, const char* path, uint8_t* code, uint8_t* emag) {
    memcpy(code, game->gameCode, 4);
    memcpy(emag, game->emagCode, 4);
    if(!code_char(code[0]) || !code_char(code[1]) || !code_char(code[2]) || !code_char(code[3])) {
        code_from_path(path, code);
        for(int i = 0; i < 4; i++) {
            emag[i] = code[3 - i];
        }
    }
}

uint64_t ndsfwd_game_title_id(const ndsfwd_game* game, const char* path) {
    uint8_t code[4];
    uint8_t emag[4];
    game_codes(game, path, code, emag);
    return (0x00048004ULL << 32) | get32(emag);
}

ndsfwd_result ndsfwd_make_srl(uint8_t* srl, size_t size, const ndsfwd_template* t, const ndsfwd_game* game, const char* path) {
    if(!ndsfwd_check_template(srl, size, t)) {
        return NDSFWD_BAD_TEMPLATE;
    }

    // The template starts "sd:/" itself.
    if(path[0] != '/') {
        return NDSFWD_PATH_TOO_LONG;
    }

    const char* relative = path + 1;
    size_t len = strlen(relative);
    if(len == 0 || len >= t->pathLength) {
        return NDSFWD_PATH_TOO_LONG;
    }

    uint8_t code[4];
    uint8_t emag[4];
    game_codes(game, path, code, emag);

    // The header: the game's name, code and maker, the title ID from its code.
    memcpy(srl + H_TITLE, game->gameTitle, sizeof(game->gameTitle));
    memcpy(srl + H_CODE, code, sizeof(code));
    memcpy(srl + H_MAKER, game->maker, sizeof(game->maker));
    memcpy(srl + H_TID_LOW, emag, sizeof(emag));

    // The icon and names; the animated icon is the still one in each of its frames, shown for good.
    uint8_t* banner = srl + ndsfwd_banner_offset(srl);
    memcpy(banner + B_ICON, game->icon, sizeof(game->icon));
    memcpy(banner + B_PALETTE, game->palette, sizeof(game->palette));
    for(int i = 0; i < NDSFWD_NAMES; i++) {
        for(int c = 0; c < NDSFWD_NAME_CHARS; c++) {
            put16(banner + B_NAMES + (i * NDSFWD_NAME_CHARS + c) * 2, game->names[i][c]);
        }
    }

    for(int frame = 0; frame < 8; frame++) {
        memcpy(banner + B_ANIM_ICONS + frame * 0x200, game->icon, sizeof(game->icon));
        memcpy(banner + B_ANIM_PALETTES + frame * 0x20, game->palette, sizeof(game->palette));
    }

    static const uint8_t still[4] = {0x01, 0x00, 0x00, 0x01};   // Frame 0, palette 0, forever.
    memset(banner + B_ANIM_SEQUENCE, 0, 0x80);
    memcpy(banner + B_ANIM_SEQUENCE, still, sizeof(still));

    put16(banner + B_CRC, ndsfwd_crc16(banner + B_ICON, 0x820));
    put16(banner + B_CRC + 2, ndsfwd_crc16(banner + B_ICON, 0x920));
    put16(banner + B_CRC + 4, ndsfwd_crc16(banner + B_ICON, 0xA20));
    put16(banner + B_CRC + 6, ndsfwd_crc16(banner + B_ANIM_ICONS, 0x1180));

    // The path, after the template's "sd:/".
    memset(srl + t->pathOffset, 0, t->pathLength);
    memcpy(srl + t->pathOffset, relative, len);

    put16(srl + H_CRC, ndsfwd_crc16(srl, H_CRC));
    return NDSFWD_OK;
}

uint64_t ndsfwd_title_id(const uint8_t* srl) {
    return (0x00048000ULL << 32) | ((uint64_t) srl[H_TID_LOW + 4] << 32) | get32(srl + H_TID_LOW);
}

void ndsfwd_name(const ndsfwd_game* game, int language, char* out, size_t size) {
    if(size == 0) {
        return;
    }

    const uint16_t* name = language >= 0 && language < NDSFWD_NAMES && game->names[language][0] != 0 ? game->names[language] : game->names[1];

    // The last of two or three lines is the publisher.
    int lines = 1;
    for(int c = 0; c < NDSFWD_NAME_CHARS && name[c] != 0; c++) {
        if(name[c] == '\n') {
            lines++;
        }
    }

    int keep = lines > 1 ? lines - 1 : 1;
    size_t pos = 0;
    int line = 1;
    for(int c = 0; c < NDSFWD_NAME_CHARS && name[c] != 0; c++) {
        uint32_t ch = name[c];
        if(ch == '\n') {
            if(++line > keep) {
                break;
            }

            ch = ' ';
        }

        uint8_t bytes[3];
        size_t n;
        if(ch < 0x80) {
            bytes[0] = (uint8_t) ch;
            n = 1;
        } else if(ch < 0x800) {
            bytes[0] = (uint8_t) (0xC0 | (ch >> 6));
            bytes[1] = (uint8_t) (0x80 | (ch & 0x3F));
            n = 2;
        } else {
            bytes[0] = (uint8_t) (0xE0 | (ch >> 12));
            bytes[1] = (uint8_t) (0x80 | ((ch >> 6) & 0x3F));
            bytes[2] = (uint8_t) (0x80 | (ch & 0x3F));
            n = 3;
        }

        if(pos + n >= size) {
            break;
        }

        memcpy(out + pos, bytes, n);
        pos += n;
    }

    out[pos] = '\0';
}

void ndsfwd_icon_rgba(const ndsfwd_game* game, uint8_t* out) {
    for(int y = 0; y < 32; y++) {
        for(int x = 0; x < 32; x++) {
            int tile = (y / 8) * 4 + x / 8;
            int offset = tile * 32 + (y % 8) * 4 + (x % 8) / 2;
            uint8_t index = (uint8_t) ((game->icon[offset] >> ((x & 1) * 4)) & 0xF);
            uint16_t color = get16(game->palette + index * 2);
            uint8_t* p = out + (y * 32 + x) * 4;
            p[0] = (uint8_t) (((color & 0x1F) * 255) / 31);
            p[1] = (uint8_t) ((((color >> 5) & 0x1F) * 255) / 31);
            p[2] = (uint8_t) ((((color >> 10) & 0x1F) * 255) / 31);
            p[3] = index == 0 ? 0 : 255;
        }
    }
}
