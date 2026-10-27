#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "section.h"
#include "../core/core.h"

// The console at a glance, for the main menu's System entry: model, owner,
// versions, network, battery, uptime and the space left on each medium.

static struct {
    bool loaded;
    char model[32];
    char user[64];
    char region[32];
    char language[32];
    char version[96];
} info_static;

static struct {
    u64 time;
    char wifi[40];
    char ip[24];
} info_dynamic;

static const char* systeminfo_model_name(u8 model) {
    switch(model) {
        case CFG_MODEL_3DS: return "3DS";
        case CFG_MODEL_3DSXL: return "3DS XL";
        case CFG_MODEL_N3DS: return "New 3DS";
        case CFG_MODEL_2DS: return "2DS";
        case CFG_MODEL_N3DSXL: return "New 3DS XL";
        case CFG_MODEL_N2DSXL: return "New 2DS XL";
        default: return "3DS";
    }
}

static const char* systeminfo_region_name(u8 region) {
    switch(region) {
        case CFG_REGION_JPN: return _("Japan");
        case CFG_REGION_USA: return _("America");
        case CFG_REGION_EUR: return _("Europe");
        case CFG_REGION_AUS: return _("Australia");
        case CFG_REGION_CHN: return _("China");
        case CFG_REGION_KOR: return _("Korea");
        case CFG_REGION_TWN: return _("Taiwan");
        default: return _("Unknown");
    }
}

static const char* systeminfo_language_name(u8 language) {
    switch(language) {
        case CFG_LANGUAGE_JP: return _("Japanese");
        case CFG_LANGUAGE_EN: return _("English");
        case CFG_LANGUAGE_FR: return _("French");
        case CFG_LANGUAGE_DE: return _("German");
        case CFG_LANGUAGE_IT: return _("Italian");
        case CFG_LANGUAGE_ES: return _("Spanish");
        case CFG_LANGUAGE_ZH: return _("Chinese (simplified)");
        case CFG_LANGUAGE_KO: return _("Korean");
        case CFG_LANGUAGE_NL: return _("Dutch");
        case CFG_LANGUAGE_PT: return _("Portuguese");
        case CFG_LANGUAGE_RU: return _("Russian");
        case CFG_LANGUAGE_TW: return _("Chinese (traditional)");
        default: return _("Unknown");
    }
}

// What does not change while NSA runs.
static void systeminfo_load_static() {
    u8 value = 0;
    snprintf(info_static.model, sizeof(info_static.model), "%s", R_SUCCEEDED(CFGU_GetSystemModel(&value)) ? systeminfo_model_name(value) : "3DS");
    snprintf(info_static.region, sizeof(info_static.region), "%s", R_SUCCEEDED(CFGU_SecureInfoGetRegion(&value)) ? systeminfo_region_name(value) : _("Unknown"));
    snprintf(info_static.language, sizeof(info_static.language), "%s", R_SUCCEEDED(CFGU_GetSystemLanguage(&value)) ? systeminfo_language_name(value) : _("Unknown"));

    // The user name set in System Settings: UTF-16, up to 10 characters.
    u16 name[0x1C / 2 + 1] = {0};
    info_static.user[0] = '\0';
    if(R_SUCCEEDED(CFGU_GetConfigInfoBlk2(0x1C, 0x000A0000, name))) {
        name[10] = 0;
        ssize_t len = utf16_to_utf8((uint8_t*) info_static.user, name, sizeof(info_static.user) - 1);
        info_static.user[len > 0 ? len : 0] = '\0';
    }

    char system[48] = "";
    OS_VersionBin nver;
    OS_VersionBin cver;
    if(R_FAILED(osGetSystemVersionDataString(&nver, &cver, system, sizeof(system)))) {
        system[0] = '\0';
    }

    // Luma3DS answers this with its version.
    char luma[32] = "";
    s64 out = 0;
    if(R_SUCCEEDED(svcGetSystemInfo(&out, 0x10000, 0)) && ((out >> 8) & 0xFFFFFF) != 0) {
        snprintf(luma, sizeof(luma), "Luma3DS %d.%d.%d", (int) ((out >> 24) & 0xFF), (int) ((out >> 16) & 0xFF), (int) ((out >> 8) & 0xFF));
    }

    if(system[0] != '\0' && luma[0] != '\0') {
        snprintf(info_static.version, sizeof(info_static.version), "%s %s     %s", _("System"), system, luma);
    } else if(system[0] != '\0') {
        snprintf(info_static.version, sizeof(info_static.version), "%s %s", _("System"), system);
    } else {
        snprintf(info_static.version, sizeof(info_static.version), "%s", luma);
    }

    info_static.loaded = true;
}

// The network, every couple of seconds.
static void systeminfo_load_dynamic() {
    if(info_dynamic.time != 0 && osGetTime() < info_dynamic.time + 2000) {
        return;
    }

    info_dynamic.time = osGetTime();

    u32 status = 0;
    char ssid[0x21] = {0};
    if(R_SUCCEEDED(ACU_GetWifiStatus(&status)) && status != 0 && R_SUCCEEDED(ACU_GetSSID(ssid)) && ssid[0] != '\0') {
        snprintf(info_dynamic.wifi, sizeof(info_dynamic.wifi), "%s", ssid);
    } else {
        snprintf(info_dynamic.wifi, sizeof(info_dynamic.wifi), "%s", status != 0 ? _("Connected") : _("Not connected"));
    }

    struct in_addr addr = {.s_addr = status != 0 ? (u32) gethostid() : 0};
    snprintf(info_dynamic.ip, sizeof(info_dynamic.ip), "%s", addr.s_addr != 0 ? inet_ntoa(addr) : "-");
}

static void systeminfo_cell(float x, float y, float w, const char* label, const char* value) {
    const theme_current* t = theme();
    float h = 34;
    screen_frame_round_rect(x, y, w, h, t->def->radiusSmall, t->def->border, t->palette->border, t->palette->surface);
    screen_draw_text(label, x + 8, y + 3, WIDGET_SMALL_SCALE, t->palette->textMuted);
    float scale = widget_text_width(value, WIDGET_TEXT_SCALE) <= w - 14 ? WIDGET_TEXT_SCALE : WIDGET_SMALL_SCALE;
    widget_text_fit(value, x + 8, y + 3 + widget_text_height(WIDGET_SMALL_SCALE), scale, t->palette->text, w - 14);
}

void systeminfo_draw_top(float x1, float y1, float x2, float y2) {
    if(!info_static.loaded) {
        systeminfo_load_static();
    }

    systeminfo_load_dynamic();

    const theme_current* t = theme();
    float x = x1 + 10;
    float w = x2 - x1 - 20;

    // The console: model, user and versions.
    float cardY = y1 + 8;
    float cardH = 62;
    widget_card(x, cardY, w, cardH);

    float tile = 46;
    float cardDepth = screen_get_depth();
    screen_set_depth(cardDepth + 1);
    screen_fill_round_rect(x + 8, cardY + (cardH - tile) / 2, tile, tile, t->def->radius, t->accentSoft);
    screen_draw_icon(ICON_DEVICE_NINTENDO, x + 8 + 9, cardY + (cardH - tile) / 2 + 9, 28, t->accent);
    screen_set_depth(cardDepth);

    float tx = x + 8 + tile + 12;
    float right = x + w - 10;
    widget_text_fit(info_static.model, tx, cardY + 7, WIDGET_TITLE_SCALE, t->palette->text, right - tx);
    if(info_static.user[0] != '\0') {
        float modelW = widget_text_width(info_static.model, WIDGET_TITLE_SCALE);
        widget_text_fit(info_static.user, tx + modelW + 10, cardY + 7 + (widget_text_height(WIDGET_TITLE_SCALE) - widget_text_height(WIDGET_TEXT_SCALE)), WIDGET_TEXT_SCALE, t->palette->textMuted, right - tx - modelW - 10);
    }

    widget_text_fit(info_static.version, tx, cardY + 12 + widget_text_height(WIDGET_TITLE_SCALE), WIDGET_SMALL_SCALE, t->palette->textMuted, right - tx);

    // Details in cells.
    int percent;
    u8 level;
    bool charging;
    widget_battery_state(&percent, &level, &charging);
    char battery[48];
    if(percent >= 0) {
        snprintf(battery, sizeof(battery), charging ? "%d %%  %s" : "%d %%", percent, _("charging"));
    } else {
        snprintf(battery, sizeof(battery), charging ? "%d/5  %s" : "%d/5", level, _("charging"));
    }

    u64 seconds = svcGetSystemTick() / SYSCLOCK_ARM11;
    char uptime[32];
    if(seconds >= 3600) {
        snprintf(uptime, sizeof(uptime), "%llu h %llu min", seconds / 3600, (seconds / 60) % 60);
    } else {
        snprintf(uptime, sizeof(uptime), "%llu min", seconds / 60);
    }

    float gap = 6;
    float cellW = (w - 2 * gap) / 3;
    float row1 = cardY + cardH + 6;
    float row2 = row1 + 34 + gap;
    systeminfo_cell(x, row1, cellW, _("Region"), info_static.region);
    systeminfo_cell(x + cellW + gap, row1, cellW, _("Language"), info_static.language);
    systeminfo_cell(x + 2 * (cellW + gap), row1, cellW, _("Battery"), battery);
    systeminfo_cell(x, row2, cellW, "Wi-Fi", info_dynamic.wifi);
    systeminfo_cell(x + cellW + gap, row2, cellW, "IP", info_dynamic.ip);
    systeminfo_cell(x + 2 * (cellW + gap), row2, cellW, _("Up for"), uptime);

    // The space left on each medium.
    float bars = row2 + 34 + 7;
    float half = (w - 12) / 2;
    widget_storage(SYSTEM_MEDIATYPE_SD, x + 2, bars, w - 4);
    widget_storage(SYSTEM_MEDIATYPE_CTR_NAND, x + 2, bars + 24, half);
    widget_storage(SYSTEM_MEDIATYPE_TWL_NAND, x + 2 + half + 8, bars + 24, half);
}
