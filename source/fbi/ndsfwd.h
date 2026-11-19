#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// DS game forwarders: a HOME Menu icon that starts a .nds from the SD card. The icon is a tiny DSiWare title made from a
// template (an SRL whose only job is to start sd:/_nds/ntr-forwarder/sdcard.nds, nds-bootstrap's forwarder, with the
// game's path); NSA copies the game's name, code and icon into it, writes the path, and installs it to the console's
// DSiWare storage. The .nds stays on the SD card: a DS game reads its data from the cartridge while it runs, and
// nds-bootstrap is what turns those reads into reads of the file.
//
// The formats are GBATEK's (cartridge header, icon/title). Plain C (no 3DS calls): tests/ndsfwd_test.c runs it on the
// host. The CIA around the SRL and the install are in ndsfwdview.c.

#define NDSFWD_HEADER_SIZE 0x240    // What is read of the .nds: the cartridge header and the DSi title ID.
#define NDSFWD_BANNER_MAX 0x23C0    // The biggest icon/title block (version 0x103: the DSi's animated icon).
#define NDSFWD_NAMES 8              // Japanese, English, French, German, Italian, Spanish, Chinese, Korean.
#define NDSFWD_NAME_CHARS 0x80

// The template's description (its .fwd file: "key=value" lines).
typedef struct {
    uint32_t pathOffset;   // Where the game's path goes, after "sd:/".
    uint32_t pathLength;   // Room for it, the ending zero included.
} ndsfwd_template;

typedef struct {
    uint8_t gameTitle[12];
    uint8_t gameCode[4];
    uint8_t maker[2];
    uint8_t emagCode[4];   // The title ID's low half as the DSi header stores it: the game code reversed.
    uint8_t icon[0x200];   // 32x32, 4 bits a pixel in 8x8 tiles.
    uint8_t palette[0x20]; // 16 colours, BGR555; colour 0 is transparent.
    uint16_t names[NDSFWD_NAMES][NDSFWD_NAME_CHARS];   // UTF-16, lines split by '\n' (name, then publisher).
} ndsfwd_game;

typedef enum {
    NDSFWD_OK,
    NDSFWD_NOT_NDS,         // The header's checksum is wrong: not a .nds (or a damaged one).
    NDSFWD_NO_BANNER,       // The game has no icon and title.
    NDSFWD_BAD_BANNER,      // Its icon and title are damaged.
    NDSFWD_PATH_TOO_LONG,   // The path does not fit in the template (or is not on the SD card).
    NDSFWD_BAD_TEMPLATE,    // The template is not one NSA knows how to fill.
} ndsfwd_result;

// Reads the template's .fwd text; false when it does not say where the path goes.
bool ndsfwd_parse_template(const char* text, ndsfwd_template* out);

// Whether `srl` (`size` bytes) is a template NSA can fill: a valid header, its icon block, the path's room inside it and
// still holding the template's placeholder.
bool ndsfwd_check_template(const uint8_t* srl, size_t size, const ndsfwd_template* t);

// Where the game's icon and title are (the header's offset), 0 when it has none.
uint32_t ndsfwd_banner_offset(const uint8_t* header);

// The game from its first NDSFWD_HEADER_SIZE bytes and its icon block (`bannerSize` bytes read from the banner offset,
// up to NDSFWD_BANNER_MAX: fewer when the file ends first).
ndsfwd_result ndsfwd_read_game(const uint8_t* header, const uint8_t* banner, size_t bannerSize, ndsfwd_game* out);

// Fills a copy of the template (`srl`, `size` bytes) for `game` at `path` (on the SD card: "/roms/nds/game.nds").
// A game without a code of its own (homebrew: "####") gets one made from the path, so that each has its own title.
ndsfwd_result ndsfwd_make_srl(uint8_t* srl, size_t size, const ndsfwd_template* t, const ndsfwd_game* game, const char* path);

// The title ID the filled SRL installs as (0x00048004xxxxxxxx: DSiWare).
uint64_t ndsfwd_title_id(const uint8_t* srl);
// The same, known before the template is at hand (the template's own high half, 0x00048004, assumed).
uint64_t ndsfwd_game_title_id(const ndsfwd_game* game, const char* path);

// The game's name in `language` (an index into names; English when that one is empty), its first line or two (the
// publisher left out), as UTF-8.
void ndsfwd_name(const ndsfwd_game* game, int language, char* out, size_t size);

// The icon as 32x32 pixels, 4 bytes each: R, G, B, A.
void ndsfwd_icon_rgba(const ndsfwd_game* game, uint8_t* out);

// CRC-16 as the DS computes its header and icon checksums.
uint16_t ndsfwd_crc16(const uint8_t* data, size_t size);
