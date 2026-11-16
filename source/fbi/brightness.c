#include <math.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "brightness.h"
#include "../core/appinfo.h"

// Block 0x00050001 holds the power saving flag and the level (1 to 5); 0x00050009 (a New 3DS) has auto-brightness on in
// its 5th byte. Block 0x00050002, "Backlight PWM" (0x38 bytes): nine floats (the curve from a brightness to the backlight's PWM), the
// number of levels (5) and a byte, seven u16 brightnesses (levels 1 to 5, then two more: 142 and 172 on a New 3DS XL),
// the PWM's base (0x200) and its least (10). Found on the owner's New 3DS XL, 2026-11-16 (docs/PLAN.md, Next).
#define BLOCK_ID 0x00050002
#define BLOCK_SIZE 0x38
#define OFFSET_COUNT 0x24
#define OFFSET_LEVELS 0x26
#define LEVEL_COUNT 5
#define VALUE_COUNT 7

#define BACKUP_PATH "sdmc:" APP_DATA_DIR "brightness-factory.bin"

// What a console that has never been changed holds, when there is no backup to read.
static const u16 usual[VALUE_COUNT] = {16, 28, 48, 82, 142, 142, 172};

static bool brightness_read(u8* block) {
    return R_SUCCEEDED(CFG_GetConfigInfoBlk8(BLOCK_SIZE, BLOCK_ID, block)) && block[OFFSET_COUNT] == LEVEL_COUNT;
}

static u16 brightness_value(const u8* block, u32 index) {
    return (u16) (block[OFFSET_LEVELS + index * 2] | (block[OFFSET_LEVELS + index * 2 + 1] << 8));
}

static void brightness_put(u8* block, u32 index, u16 value) {
    block[OFFSET_LEVELS + index * 2] = (u8) (value & 0xFF);
    block[OFFSET_LEVELS + index * 2 + 1] = (u8) (value >> 8);
}

static bool brightness_backup_read(u8* block) {
    FILE* f = fopen(BACKUP_PATH, "rb");
    if(f == NULL) {
        return false;
    }

    bool ok = fread(block, 1, BLOCK_SIZE, f) == BLOCK_SIZE && block[OFFSET_COUNT] == LEVEL_COUNT;
    fclose(f);
    return ok;
}

// The console's own values: the backup's, else the usual ones.
static void brightness_factory_values(u16* values) {
    u8 backup[BLOCK_SIZE];
    for(u32 i = 0; i < VALUE_COUNT; i++) {
        values[i] = usual[i];
    }

    if(brightness_backup_read(backup)) {
        for(u32 i = 0; i < VALUE_COUNT; i++) {
            values[i] = brightness_value(backup, i);
        }
    }
}

bool brightness_supported() {
    u8 block[BLOCK_SIZE];
    return brightness_read(block);
}

bool brightness_levels(u16* levels) {
    u8 block[BLOCK_SIZE];
    if(!brightness_read(block)) {
        return false;
    }

    for(u32 i = 0; i < LEVEL_COUNT; i++) {
        levels[i] = brightness_value(block, i);
    }

    return true;
}

void brightness_factory_levels(u16* levels) {
    u16 values[VALUE_COUNT];
    brightness_factory_values(values);
    for(u32 i = 0; i < LEVEL_COUNT; i++) {
        levels[i] = values[i];
    }
}

bool brightness_custom() {
    u16 now[LEVEL_COUNT];
    u16 factory[LEVEL_COUNT];
    if(!brightness_levels(now)) {
        return false;
    }

    brightness_factory_levels(factory);
    return memcmp(now, factory, sizeof(now)) != 0;
}

void brightness_spread(u32 lowest, u32 highest, u16* levels) {
    if(lowest < BRIGHTNESS_LOWEST) {
        lowest = BRIGHTNESS_LOWEST;
    }

    if(highest > BRIGHTNESS_HIGHEST) {
        highest = BRIGHTNESS_HIGHEST;
    }

    if(highest < lowest) {
        highest = lowest;
    }

    // The console's own levels grow by about the same ratio each step (16, 28, 48, 82, 142): so do these.
    float ratio = powf((float) highest / (float) lowest, 1.0f / (LEVEL_COUNT - 1));
    for(u32 i = 0; i < LEVEL_COUNT; i++) {
        levels[i] = (u16) (lowest * powf(ratio, (float) i) + 0.5f);
    }

    levels[0] = (u16) lowest;
    levels[LEVEL_COUNT - 1] = (u16) highest;
}

// The configuration in memory, GSP told to read it again, and (save) the configuration written to the NAND.
static Result brightness_write(const u8* block, bool save) {
    Result res = CFG_SetConfigInfoBlk8(BLOCK_SIZE, BLOCK_ID, block);
    if(R_SUCCEEDED(res) && save) {
        res = CFG_UpdateConfigSavegame();
    }

    if(R_FAILED(res)) {
        return res;
    }

    // gsp::Lcd ReloadConfig (0x000C): without it auto-brightness keeps the old table until the level changes in HOME.
    Handle lcd = 0;
    res = srvGetServiceHandle(&lcd, "gsp::Lcd");
    if(R_SUCCEEDED(res)) {
        u32* cmdbuf = getThreadCommandBuffer();
        cmdbuf[0] = IPC_MakeHeader(0xC, 1, 0);
        cmdbuf[1] = 0;
        res = svcSendSyncRequest(lcd);
        if(R_SUCCEEDED(res)) {
            res = (Result) cmdbuf[1];
        }

        svcCloseHandle(lcd);
    }

    // Without auto-brightness the level is applied by whoever sets it (HOME calls SetBrightness with the level, 1 to 5,
    // and GSP takes that level's value from the table): ReloadConfig leaves the screens as they were, so the level in
    // use is set again. Auto-brightness on (block 0x00050009's 5th byte; an Old 3DS has no such block) needs nothing more.
    u8 controls[2];
    u8 newControls[8];
    bool autoOn = R_SUCCEEDED(CFG_GetConfigInfoBlk8(sizeof(newControls), 0x00050009, newControls)) && newControls[4] != 0;
    if(R_SUCCEEDED(res) && !autoOn && R_SUCCEEDED(CFG_GetConfigInfoBlk8(sizeof(controls), 0x00050001, controls))
       && controls[1] >= 1 && controls[1] <= LEVEL_COUNT && R_SUCCEEDED(gspLcdInit())) {
        res = GSPLCD_SetBrightness(GSPLCD_SCREEN_BOTH, controls[1]);
        gspLcdExit();
    }

    return res;
}

Result brightness_set(u32 lowest, u32 highest, bool save) {
    u8 block[BLOCK_SIZE];
    Result res = CFG_GetConfigInfoBlk8(BLOCK_SIZE, BLOCK_ID, block);
    if(R_FAILED(res)) {
        return res;
    }

    if(block[OFFSET_COUNT] != LEVEL_COUNT) {
        return MAKERESULT(RL_PERMANENT, RS_NOTSUPPORTED, RM_APPLICATION, RD_NOT_IMPLEMENTED);
    }

    // Before NSA changes it for the first time, a copy of the console's own block (only while it still is).
    u8 backup[BLOCK_SIZE];
    if(!brightness_backup_read(backup)) {
        bool usualNow = true;
        for(u32 i = 0; i < VALUE_COUNT; i++) {
            usualNow = usualNow && brightness_value(block, i) == usual[i];
        }

        if(usualNow) {
            FILE* f = fopen(BACKUP_PATH, "wb");
            if(f != NULL) {
                fwrite(block, 1, BLOCK_SIZE, f);
                fclose(f);
            }
        }
    }

    u16 factory[VALUE_COUNT];
    brightness_factory_values(factory);

    u16 levels[LEVEL_COUNT];
    brightness_spread(lowest, highest, levels);
    for(u32 i = 0; i < LEVEL_COUNT; i++) {
        brightness_put(block, i, levels[i]);
    }

    // The two values after the levels follow the fifth as the console's own do (142 and 172 for a level 5 of 142),
    // never above the highest the bar allows or the console's own.
    u16 top = levels[LEVEL_COUNT - 1];
    for(u32 i = LEVEL_COUNT; i < VALUE_COUNT; i++) {
        u32 value = factory[LEVEL_COUNT - 1] > 0 ? (u32) top * factory[i] / factory[LEVEL_COUNT - 1] : top;
        u32 limit = factory[i] > top ? factory[i] : top;
        brightness_put(block, i, (u16) (value > limit ? limit : value));
    }

    return brightness_write(block, save);
}

Result brightness_restore() {
    u8 block[BLOCK_SIZE];
    Result res = CFG_GetConfigInfoBlk8(BLOCK_SIZE, BLOCK_ID, block);
    if(R_FAILED(res)) {
        return res;
    }

    u16 factory[VALUE_COUNT];
    brightness_factory_values(factory);
    for(u32 i = 0; i < VALUE_COUNT; i++) {
        brightness_put(block, i, factory[i]);
    }

    return brightness_write(block, true);
}

u32 brightness_now() {
    u32 value = 0;
    if(R_SUCCEEDED(gspLcdInit())) {
        if(R_FAILED(GSPLCD_GetBrightness(GSPLCD_SCREEN_TOP, &value))) {
            value = 0;
        }

        gspLcdExit();
    }

    return value;
}
