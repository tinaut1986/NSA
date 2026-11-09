#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>
#include <jansson.h>

#include "ftpserver.h"
#include "section.h"
#include "webserver.h"
#include "../core/core.h"

// The console at a glance, for the main menu's System entry: model, owner,
// versions, network, battery, uptime and the space left on each medium.
// System opens a list of the details (Console, Network, Storage, Battery),
// each drawn on the top screen while its row is selected.

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
    screen_draw_icon(ICON_3DS, x + 8 + 9, cardY + (cardH - tile) / 2 + 9, 28, t->accent);
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
    widget_cell(x, row1, cellW, _("Region"), info_static.region);
    widget_cell(x + cellW + gap, row1, cellW, _("Language"), info_static.language);
    widget_cell(x + 2 * (cellW + gap), row1, cellW, _("Battery"), battery);
    widget_cell(x, row2, cellW, "Wi-Fi", info_dynamic.wifi);
    widget_cell(x + cellW + gap, row2, cellW, "IP", info_dynamic.ip);
    widget_cell(x + 2 * (cellW + gap), row2, cellW, _("Up for"), uptime);

    // The space left on each medium.
    float bars = row2 + 34 + 7;
    float half = (w - 12) / 2;
    widget_storage(SYSTEM_MEDIATYPE_SD, x + 2, bars, w - 4);
    widget_storage(SYSTEM_MEDIATYPE_CTR_NAND, x + 2, bars + 24, half);
    widget_storage(SYSTEM_MEDIATYPE_TWL_NAND, x + 2 + half + 8, bars + 24, half);
}

// ---- the details ----

// "2 h 5 min", or "5 min".
static void systeminfo_duration(char* out, size_t size, u64 seconds) {
    if(seconds >= 3600) {
        snprintf(out, size, "%llu h %llu min", seconds / 3600, (seconds / 60) % 60);
    } else {
        snprintf(out, size, "%llu min", seconds / 60);
    }
}

#define CELL_HEIGHT 34
#define CELL_GAP 6
#define HEADER_HEIGHT 44

// A card across the top: the icon in its tile, a title and a line under it.
// Returns where what follows starts.
static float systeminfo_header(float x, float y, float w, u32 icon, const char* title, const char* subtitle) {
    const theme_current* t = theme();
    widget_card(x, y, w, HEADER_HEIGHT);

    float tile = 36;
    float tileY = y + (HEADER_HEIGHT - tile) / 2;
    float depth = screen_get_depth();
    screen_set_depth(depth + 1);
    screen_fill_round_rect(x + 5, tileY, tile, tile, t->def->radius, t->accentSoft);
    screen_draw_icon(icon, x + 5 + 2, tileY + 2, 32, t->accent);
    screen_set_depth(depth);

    float tx = x + 5 + tile + 10;
    float right = x + w - 10;
    float titleH = widget_text_height(WIDGET_TITLE_SCALE);
    float textY = y + (HEADER_HEIGHT - titleH - 2 - widget_text_height(WIDGET_SMALL_SCALE)) / 2;
    widget_text_fit(title, tx, textY, WIDGET_TITLE_SCALE, t->palette->text, right - tx);
    widget_text_fit(subtitle, tx, textY + titleH + 2, WIDGET_SMALL_SCALE, t->palette->textMuted, right - tx);
    return y + HEADER_HEIGHT + CELL_GAP;
}

// A cell of a grid of three columns that starts at y; span: how many columns it takes.
static void systeminfo_grid(float x, float y, float w, u32 column, u32 row, u32 span, const char* label, const char* value) {
    float cellW = (w - 2 * CELL_GAP) / 3;
    widget_cell(x + column * (cellW + CELL_GAP), y + row * (CELL_HEIGHT + CELL_GAP), cellW * span + CELL_GAP * (span - 1), label, value);
}

static bool systeminfo_hwc() {
    static bool tried = false;
    static bool hwc = false;
    if(!tried) {
        hwc = R_SUCCEEDED(mcuHwcInit());
        tried = true;
    }

    return hwc;
}

// ---- Console ----

static struct {
    bool loaded;
    bool isNew;
    char country[8];
    char serial[24];
    char deviceId[16];
    char system[48];
    char kernel[24];
    char mcu[16];
    char luma[48];
    char screens[48];
} info_console;

static const char* systeminfo_panel_name(u8 vendor) {
    switch(vendor) {
        case 0x1: return "IPS";
        case 0xC: return "TN";
        default: return "?";
    }
}

// The serial number printed under the console, from SecureInfo_A (or _B) on the CTR NAND.
static void systeminfo_load_serial(char* out, size_t size) {
    static const char* const files[] = {"/rw/sys/SecureInfo_A", "/rw/sys/SecureInfo_B"};
    snprintf(out, size, "-");

    // Opened as Files does: Azahar has no CTR NAND and crashes on OpenFileDirectly.
    FS_Archive archive = 0;
    if(R_FAILED(FSUSER_OpenArchive(&archive, ARCHIVE_NAND_CTR_FS, fsMakePath(PATH_EMPTY, "")))) {
        return;
    }

    for(u32 i = 0; i < sizeof(files) / sizeof(*files); i++) {
        Handle handle = 0;
        if(R_FAILED(FSUSER_OpenFile(&handle, archive, fsMakePath(PATH_ASCII, files[i]), FS_OPEN_READ, 0))) {
            continue;
        }

        char serial[0x10] = {0};
        u32 read = 0;
        Result res = FSFILE_Read(handle, &read, 0x102, serial, 0xF);
        FSFILE_Close(handle);
        if(R_SUCCEEDED(res) && read > 0 && serial[0] != '\0') {
            snprintf(out, size, "%s", serial);
            break;
        }
    }

    FSUSER_CloseArchive(archive);
}

static void systeminfo_load_console() {
    if(!info_static.loaded) {
        systeminfo_load_static();
    }

    u8 model = 0;
    info_console.isNew = R_SUCCEEDED(CFGU_GetSystemModel(&model)) && (model == CFG_MODEL_N3DS || model == CFG_MODEL_N3DSXL || model == CFG_MODEL_N2DSXL);

    // The country: block 0xB0000 holds its code in the fourth byte.
    u8 block[4] = {0};
    u16 code = 0;
    if(R_SUCCEEDED(CFGU_GetConfigInfoBlk2(sizeof(block), 0x000B0000, block)) && R_SUCCEEDED(CFGU_GetCountryCodeString(block[3], &code)) && code != 0) {
        snprintf(info_console.country, sizeof(info_console.country), "%c%c", code & 0xFF, code >> 8);
    } else {
        snprintf(info_console.country, sizeof(info_console.country), "-");
    }

    systeminfo_load_serial(info_console.serial, sizeof(info_console.serial));

    s32 internal = 0;
    u32 deviceId = 0;
    if(R_SUCCEEDED(AM_GetDeviceId(&internal, &deviceId))) {
        snprintf(info_console.deviceId, sizeof(info_console.deviceId), "%lu", deviceId);
    } else {
        snprintf(info_console.deviceId, sizeof(info_console.deviceId), "-");
    }

    OS_VersionBin nver;
    OS_VersionBin cver;
    if(R_FAILED(osGetSystemVersionDataString(&nver, &cver, info_console.system, sizeof(info_console.system)))) {
        snprintf(info_console.system, sizeof(info_console.system), "-");
    }

    u32 kernel = osGetKernelVersion();
    snprintf(info_console.kernel, sizeof(info_console.kernel), "%d.%d-%d", (int) GET_VERSION_MAJOR(kernel), (int) GET_VERSION_MINOR(kernel), (int) GET_VERSION_REVISION(kernel));

    // The MCU answers its major version plus 0x10.
    u8 high = 0;
    u8 low = 0;
    if(systeminfo_hwc() && R_SUCCEEDED(MCUHWC_GetFwVerHigh(&high)) && R_SUCCEEDED(MCUHWC_GetFwVerLow(&low))) {
        snprintf(info_console.mcu, sizeof(info_console.mcu), "%d.%d", high >= 0x10 ? high - 0x10 : high, low);
    } else {
        snprintf(info_console.mcu, sizeof(info_console.mcu), "-");
    }

    s64 version = 0;
    s64 commit = 0;
    if(R_SUCCEEDED(svcGetSystemInfo(&version, 0x10000, 0)) && ((version >> 8) & 0xFFFFFF) != 0) {
        int length = snprintf(info_console.luma, sizeof(info_console.luma), "%d.%d.%d", (int) ((version >> 24) & 0xFF), (int) ((version >> 16) & 0xFF), (int) ((version >> 8) & 0xFF));
        if(R_SUCCEEDED(svcGetSystemInfo(&commit, 0x10000, 1)) && commit != 0 && length > 0) {
            snprintf(info_console.luma + length, sizeof(info_console.luma) - length, "  (%08lx)", (u32) commit);
        }
    } else {
        snprintf(info_console.luma, sizeof(info_console.luma), "%s", _("Not found"));
    }

    // Only a New 3DS may have an IPS panel; the others are all TN.
    u8 vendors = 0;
    if(!info_console.isNew) {
        snprintf(info_console.screens, sizeof(info_console.screens), "TN");
    } else if(R_SUCCEEDED(gspLcdInit())) {
        if(R_SUCCEEDED(GSPLCD_GetVendors(&vendors))) {
            snprintf(info_console.screens, sizeof(info_console.screens), _("Top %s, bottom %s"), systeminfo_panel_name(vendors >> 4), systeminfo_panel_name(vendors & 0xF));
        } else {
            snprintf(info_console.screens, sizeof(info_console.screens), "-");
        }

        gspLcdExit();
    } else {
        snprintf(info_console.screens, sizeof(info_console.screens), "-");
    }

    info_console.loaded = true;
}

void systeminfo_draw_console(float x1, float y1, float x2, float y2) {
    if(!info_console.loaded) {
        systeminfo_load_console();
    }

    float x = x1 + 10;
    float w = x2 - x1 - 20;
    float y = systeminfo_header(x, y1 + 8, w, ICON_3DS, info_static.model, info_static.user[0] != '\0' ? info_static.user : info_console.system);

    systeminfo_grid(x, y, w, 0, 0, 1, _("Region"), info_static.region);
    systeminfo_grid(x, y, w, 1, 0, 1, _("Country"), info_console.country);
    systeminfo_grid(x, y, w, 2, 0, 1, _("Language"), info_static.language);
    systeminfo_grid(x, y, w, 0, 1, 1, _("Serial number"), info_console.serial);
    systeminfo_grid(x, y, w, 1, 1, 1, _("Device ID"), info_console.deviceId);
    systeminfo_grid(x, y, w, 2, 1, 1, _("Screens"), info_console.screens);
    systeminfo_grid(x, y, w, 0, 2, 1, _("System version"), info_console.system);
    systeminfo_grid(x, y, w, 1, 2, 1, _("Kernel"), info_console.kernel);
    systeminfo_grid(x, y, w, 2, 2, 1, _("MCU firmware"), info_console.mcu);
    systeminfo_grid(x, y, w, 0, 3, 2, "Luma3DS", info_console.luma);
    systeminfo_grid(x, y, w, 2, 3, 1, _("Running as"), envIsHomebrew() ? "3DSX" : "CIA");
}

// ---- Network ----

static struct {
    u64 time;
    bool connected;
    char ssid[40];
    char signal[16];
    char mac[24];
    char ip[24];
    char netmask[24];
    char gateway[24];
    char dns[40];
    char lease[24];
    char proxy[16];
    char ftp[48];
    char web[48];
} info_network;

static void systeminfo_address(char* out, size_t size, struct in_addr addr) {
    snprintf(out, size, "%s", addr.s_addr != 0 ? inet_ntoa(addr) : "-");
}

static void systeminfo_load_network() {
    if(info_network.time != 0 && osGetTime() < info_network.time + 2000) {
        return;
    }

    info_network.time = osGetTime();

    u32 status = 0;
    info_network.connected = R_SUCCEEDED(ACU_GetWifiStatus(&status)) && status != 0;
    char ssid[0x21] = {0};
    if(info_network.connected && R_SUCCEEDED(ACU_GetSSID(ssid)) && ssid[0] != '\0') {
        snprintf(info_network.ssid, sizeof(info_network.ssid), "%s", ssid);
    } else {
        snprintf(info_network.ssid, sizeof(info_network.ssid), "Wi-Fi");
    }

    snprintf(info_network.signal, sizeof(info_network.signal), info_network.connected ? "%d/3" : "-", osGetWifiStrength());

    u8 mac[6] = {0};
    socklen_t length = sizeof(mac);
    if(SOCU_GetNetworkOpt(SOL_CONFIG, NETOPT_MAC_ADDRESS, mac, &length) == 0 && (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) != 0) {
        snprintf(info_network.mac, sizeof(info_network.mac), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        snprintf(info_network.mac, sizeof(info_network.mac), "-");
    }

    struct in_addr ip = {0};
    struct in_addr netmask = {0};
    struct in_addr broadcast = {0};
    if(!info_network.connected || SOCU_GetIPInfo(&ip, &netmask, &broadcast) != 0) {
        ip.s_addr = 0;
        netmask.s_addr = 0;
    }

    systeminfo_address(info_network.ip, sizeof(info_network.ip), ip);
    systeminfo_address(info_network.netmask, sizeof(info_network.netmask), netmask);

    // The gateway: the default route's (0.0.0.0/0, the last row on a real console). The console's own address and
    // 127.0.0.0/8 also have the G flag, through 127.0.0.1: never take a loopback one.
    struct in_addr gateway = {0};
    SOCU_RoutingTableEntry routes[16];
    length = sizeof(routes);
    if(info_network.connected && SOCU_GetNetworkOpt(SOL_CONFIG, NETOPT_ROUTING_TABLE, routes, &length) == 0) {
        for(u32 i = 0; i < length / sizeof(*routes); i++) {
            u32 via = ntohl(routes[i].gateway.s_addr);
            if((routes[i].flags & ROUTING_FLAG_G) && via != 0 && (via >> 24) != 127) {
                gateway = routes[i].gateway;
                if(routes[i].dest_ip.s_addr == 0) {
                    break;
                }
            }
        }
    }

    systeminfo_address(info_network.gateway, sizeof(info_network.gateway), gateway);

    // The DNS servers (two at most are set). The service wants room for its whole table (336 bytes, 16.8 entries):
    // with less it fails with EINVAL.
    SOCU_DNSTableEntry dns[17];
    length = sizeof(dns);
    info_network.dns[0] = '\0';
    if(info_network.connected && SOCU_GetNetworkOpt(SOL_CONFIG, NETOPT_DNS_TABLE, dns, &length) == 0) {
        for(u32 i = 0; i < length / sizeof(*dns) && i < 2; i++) {
            if(dns[i].ip.s_addr != 0) {
                size_t used = strlen(info_network.dns);
                snprintf(info_network.dns + used, sizeof(info_network.dns) - used, "%s%s", used > 0 ? ", " : "", inet_ntoa(dns[i].ip));
            }
        }
    }

    if(info_network.dns[0] == '\0') {
        snprintf(info_network.dns, sizeof(info_network.dns), "-");
    }

    // What is left of the address the router lent, in seconds.
    u32 lease = 0;
    length = sizeof(lease);
    if(info_network.connected && SOCU_GetNetworkOpt(SOL_CONFIG, NETOPT_DHCP_LEASE_TIME, &lease, &length) == 0 && lease != 0) {
        systeminfo_duration(info_network.lease, sizeof(info_network.lease), lease);
    } else {
        snprintf(info_network.lease, sizeof(info_network.lease), "-");
    }

    bool proxy = false;
    if(info_network.connected && R_SUCCEEDED(ACU_GetProxyEnable(&proxy))) {
        snprintf(info_network.proxy, sizeof(info_network.proxy), "%s", proxy ? _("On") : _("Off"));
    } else {
        snprintf(info_network.proxy, sizeof(info_network.proxy), "-");
    }

    // The address is the IP above: only the port.
    if(ftpserver_running()) {
        snprintf(info_network.ftp, sizeof(info_network.ftp), _("Port %d"), FTP_SERVER_PORT);
    } else {
        snprintf(info_network.ftp, sizeof(info_network.ftp), "%s", _("Off"));
    }

    if(webserver_running()) {
        snprintf(info_network.web, sizeof(info_network.web), _("Port %d"), WEB_SERVER_PORT);
    } else {
        snprintf(info_network.web, sizeof(info_network.web), "%s", _("Off"));
    }
}

void systeminfo_draw_network(float x1, float y1, float x2, float y2) {
    systeminfo_load_network();

    float x = x1 + 10;
    float w = x2 - x1 - 20;
    float y = systeminfo_header(x, y1 + 8, w, info_network.connected ? ICON_WIFI : ICON_WIFI_OFF, info_network.ssid, info_network.connected ? _("Connected") : _("Not connected"));

    systeminfo_grid(x, y, w, 0, 0, 1, "IP", info_network.ip);
    systeminfo_grid(x, y, w, 1, 0, 1, _("Netmask"), info_network.netmask);
    systeminfo_grid(x, y, w, 2, 0, 1, _("Gateway"), info_network.gateway);
    systeminfo_grid(x, y, w, 0, 1, 2, "DNS", info_network.dns);
    systeminfo_grid(x, y, w, 2, 1, 1, _("Signal"), info_network.signal);
    systeminfo_grid(x, y, w, 0, 2, 2, _("MAC address"), info_network.mac);
    systeminfo_grid(x, y, w, 2, 2, 1, _("DHCP lease"), info_network.lease);
    systeminfo_grid(x, y, w, 0, 3, 1, _("FTP server"), info_network.ftp);
    systeminfo_grid(x, y, w, 2, 3, 1, _("Proxy"), info_network.proxy);
    systeminfo_grid(x, y, w, 1, 3, 1, _("Web server"), info_network.web);
}

// ---- Storage ----

static struct {
    bool loaded;
    char maker[32];
    char product[16];
    char made[16];
    char speed[32];
    char writable[32];
    char cluster[16];
} info_storage;

static const char* systeminfo_sd_maker(u8 id) {
    switch(id) {
        case 0x01: return "Panasonic";
        case 0x02: return "Toshiba";
        case 0x03: return "SanDisk";
        case 0x1B: return "Samsung";
        case 0x1D: return "ADATA";
        case 0x27: return "Phison";
        case 0x28: return "Lexar";
        case 0x31: return "Silicon Power";
        case 0x41: return "Kingston";
        case 0x74: return "Transcend";
        case 0x76: return "Patriot";
        case 0x82: return "Sony";
        case 0x9C: return "Angelbird";
        default: return NULL;
    }
}

// The card's CID (manufacturer, product name, date) in the SD order: byte 0 is
// the manufacturer. The 3DS hands it over reversed and without the CRC byte
// (both consoles, 2026-11-09), so that order is tried first; the other only if
// its OEM ID and product name (bytes 1-7) read as text. Checking the product
// name alone took a Samsung card's unreversed bytes ("a8h0T", made 2049-11).
static bool systeminfo_sd_cid(u8* cid) {
    u8 raw[0x10] = {0};
    if(R_FAILED(FSUSER_GetSdmcCid(raw, sizeof(raw)))) {
        return false;
    }

    u8 reversed[0x10] = {0};
    for(u32 i = 0; i < 15; i++) {
        reversed[i] = raw[14 - i];
    }

    const u8* candidates[] = {reversed, raw};
    for(u32 c = 0; c < 2; c++) {
        bool text = true;
        for(u32 i = 1; i < 8; i++) {
            if(candidates[c][i] < 0x20 || candidates[c][i] > 0x7E) {
                text = false;
            }
        }

        if(text) {
            memcpy(cid, candidates[c], 0x10);
            return true;
        }
    }

    return false;
}

static void systeminfo_load_storage() {
    u8 cid[0x10];
    if(systeminfo_sd_cid(cid)) {
        const char* maker = systeminfo_sd_maker(cid[0]);
        if(maker != NULL) {
            snprintf(info_storage.maker, sizeof(info_storage.maker), "%s", maker);
        } else {
            snprintf(info_storage.maker, sizeof(info_storage.maker), "0x%02X", cid[0]);
        }

        snprintf(info_storage.product, sizeof(info_storage.product), "%.5s", (const char*) &cid[3]);

        u32 year = 2000 + (((cid[13] & 0x0F) << 4) | (cid[14] >> 4));
        u32 month = cid[14] & 0x0F;
        if(month >= 1 && month <= 12) {
            snprintf(info_storage.made, sizeof(info_storage.made), "%04lu-%02lu", year, month);
        } else {
            snprintf(info_storage.made, sizeof(info_storage.made), "-");
        }
    } else {
        snprintf(info_storage.maker, sizeof(info_storage.maker), "-");
        snprintf(info_storage.product, sizeof(info_storage.product), "-");
        snprintf(info_storage.made, sizeof(info_storage.made), "-");
    }

    FS_SdMmcSpeedInfo speed;
    if(R_SUCCEEDED(FSUSER_GetSdmcSpeedInfo(&speed))) {
        snprintf(info_storage.speed, sizeof(info_storage.speed), "%s", speed.highSpeedModeEnabled ? _("High speed") : _("Normal speed"));
    } else {
        snprintf(info_storage.speed, sizeof(info_storage.speed), "-");
    }

    bool writable = false;
    if(R_SUCCEEDED(FSUSER_IsSdmcWritable(&writable))) {
        snprintf(info_storage.writable, sizeof(info_storage.writable), "%s", writable ? _("Yes") : _("No (locked)"));
    } else {
        snprintf(info_storage.writable, sizeof(info_storage.writable), "-");
    }

    FS_ArchiveResource resource = {0};
    if(R_SUCCEEDED(FSUSER_GetArchiveResource(&resource, SYSTEM_MEDIATYPE_SD)) && resource.clusterSize != 0) {
        snprintf(info_storage.cluster, sizeof(info_storage.cluster), "%.0f %s", ui_get_display_size(resource.clusterSize), ui_get_display_size_units(resource.clusterSize));
    } else {
        snprintf(info_storage.cluster, sizeof(info_storage.cluster), "-");
    }

    info_storage.loaded = true;
}

void systeminfo_draw_storage(float x1, float y1, float x2, float y2) {
    if(!info_storage.loaded) {
        systeminfo_load_storage();
    }

    float x = x1 + 10;
    float w = x2 - x1 - 20;
    char title[64];
    if(info_storage.maker[0] != '-') {
        snprintf(title, sizeof(title), "%s %s", info_storage.maker, info_storage.product);
    } else {
        snprintf(title, sizeof(title), "%s", _("SD card"));
    }

    char subtitle[64];
    snprintf(subtitle, sizeof(subtitle), "%s: %s", _("Cluster size"), info_storage.cluster);
    float y = systeminfo_header(x, y1 + 8, w, ICON_DEVICE_SD_CARD, title, subtitle);

    systeminfo_grid(x, y, w, 0, 0, 1, _("Made"), info_storage.made);
    systeminfo_grid(x, y, w, 1, 0, 1, _("Speed"), info_storage.speed);
    systeminfo_grid(x, y, w, 2, 0, 1, _("Writable"), info_storage.writable);

    // The space of each medium.
    float bars = y + CELL_HEIGHT + 10;
    widget_storage(SYSTEM_MEDIATYPE_SD, x + 2, bars, w - 4);
    widget_storage(SYSTEM_MEDIATYPE_CTR_NAND, x + 2, bars + 24, w - 4);
    widget_storage(SYSTEM_MEDIATYPE_TWL_NAND, x + 2, bars + 48, w - 4);
    widget_storage(SYSTEM_MEDIATYPE_TWL_PHOTO, x + 2, bars + 72, w - 4);
}

// ---- Battery ----

#define BATTERY_SAMPLES 180      // Half an hour, one every ten seconds.
#define BATTERY_EVERY 10         // Seconds between samples.
#define BATTERY_RATE_AFTER 120   // Seconds watched before a rate is shown.

static struct {
    u64 time;
    bool fine;          // The MCU's registers were read.
    float percent;
    int temperature;
    float voltage;
    u8 level;           // PTM's 0..5.
    bool charging;
    bool adapter;

    // The percentage over time while the page is shown, for the rate and the graph.
    float samples[BATTERY_SAMPLES];
    u64 sampleTimes[BATTERY_SAMPLES];
    u32 count;
    bool samplesCharging;
} info_battery;

static void systeminfo_load_battery() {
    u64 now = osGetTime();
    if(info_battery.time != 0 && now < info_battery.time + 1000) {
        return;
    }

    info_battery.time = now;

    // MCU registers 0x0A..0x0D: temperature (degrees C), percentage and its
    // fraction (1/256), voltage (5/256 V).
    u8 regs[4] = {0};
    info_battery.fine = systeminfo_hwc() && R_SUCCEEDED(MCUHWC_ReadRegister(0x0A, regs, sizeof(regs)));
    if(info_battery.fine) {
        info_battery.temperature = (s8) regs[0];
        info_battery.percent = regs[1] + regs[2] / 256.0f;
        if(info_battery.percent > 100) {
            info_battery.percent = 100;
        }

        info_battery.voltage = regs[3] * 5.0f / 256.0f;
    }

    if(R_FAILED(PTMU_GetBatteryLevel(&info_battery.level))) {
        info_battery.level = 0;
    }

    if(!info_battery.fine) {
        info_battery.percent = info_battery.level * 20.0f;
    }

    u8 state = 0;
    info_battery.charging = R_SUCCEEDED(PTMU_GetBatteryChargeState(&state)) && state;
    if(R_FAILED(PTMU_GetAdapterState(&info_battery.adapter))) {
        info_battery.adapter = false;
    }

    // A new sample; the history starts again after a gap or when charging starts or stops.
    u64 seconds = now / 1000;
    u32 last = info_battery.count - 1;
    if(info_battery.count > 0 && (seconds > info_battery.sampleTimes[last] + 3 * BATTERY_EVERY || info_battery.charging != info_battery.samplesCharging)) {
        info_battery.count = 0;
    }

    if(info_battery.count == 0 || seconds >= info_battery.sampleTimes[info_battery.count - 1] + BATTERY_EVERY) {
        if(info_battery.count == BATTERY_SAMPLES) {
            memmove(info_battery.samples, info_battery.samples + 1, (BATTERY_SAMPLES - 1) * sizeof(*info_battery.samples));
            memmove(info_battery.sampleTimes, info_battery.sampleTimes + 1, (BATTERY_SAMPLES - 1) * sizeof(*info_battery.sampleTimes));
            info_battery.count--;
        }

        info_battery.samples[info_battery.count] = info_battery.percent;
        info_battery.sampleTimes[info_battery.count] = seconds;
        info_battery.count++;
        info_battery.samplesCharging = info_battery.charging;
    }
}

// The percentage over the samples, with the time it covers on top.
static void systeminfo_battery_graph(float x, float y, float w, float h, const char* watched) {
    const theme_current* t = theme();
    screen_frame_round_rect(x, y, w, h, t->def->radiusSmall, t->def->border, t->palette->border, t->palette->surface);

    float labelH = widget_text_height(WIDGET_SMALL_SCALE);
    char label[64];
    snprintf(label, sizeof(label), "%s: %s", _("Watched for"), watched);
    screen_draw_text(label, x + 8, y + 3, WIDGET_SMALL_SCALE, t->palette->textMuted);

    float gx = x + 8;
    float gy = y + 3 + labelH + 3;
    float gw = w - 16;
    float gh = h - (gy - y) - 6;
    if(info_battery.count < 2 || gh < 8) {
        widget_text_centered(_("Measuring..."), x, x + w, gy + (gh - labelH) / 2, WIDGET_SMALL_SCALE, t->palette->textMuted);
        return;
    }

    float low = 100;
    float high = 0;
    for(u32 i = 0; i < info_battery.count; i++) {
        low = info_battery.samples[i] < low ? info_battery.samples[i] : low;
        high = info_battery.samples[i] > high ? info_battery.samples[i] : high;
    }

    low = low > 1 ? low - 1 : 0;
    high = high < 99 ? high + 1 : 100;

    char range[32];
    snprintf(range, sizeof(range), "%.0f - %.0f %%", low, high);
    screen_draw_text(range, x + w - 8 - widget_text_width(range, WIDGET_SMALL_SCALE), y + 3, WIDGET_SMALL_SCALE, t->palette->textMuted);

    // One column per sample, from the left as they come.
    float step = gw / BATTERY_SAMPLES;
    float column = step < 1 ? 1 : (float) (int) step;
    for(u32 i = 0; i < info_battery.count; i++) {
        float fraction = (info_battery.samples[i] - low) / (high - low);
        float top = gy + gh - gh * fraction;
        float cx = (float) (int) (gx + i * step);
        screen_fill_rect(cx, top, column, gy + gh - top, t->accentSoft);
        screen_fill_rect(cx, top, column, 2, t->accent);
    }
}

void systeminfo_draw_battery(float x1, float y1, float x2, float y2) {
    systeminfo_load_battery();

    float x = x1 + 10;
    float w = x2 - x1 - 20;

    char percent[16];
    snprintf(percent, sizeof(percent), info_battery.fine ? "%.1f %%" : "%.0f %%", info_battery.percent);
    const char* state = info_battery.charging ? _("Charging") : info_battery.adapter ? _("On the charger, not charging") : _("On battery");
    u32 icon = info_battery.charging ? ICON_BATTERY_CHARGING : info_battery.level >= 5 ? ICON_BATTERY_4 : info_battery.level >= 4 ? ICON_BATTERY_3 : info_battery.level >= 2 ? ICON_BATTERY_2 : ICON_BATTERY_1;
    float y = systeminfo_header(x, y1 + 8, w, icon, percent, state);

    char voltage[16];
    char temperature[16];
    char level[16];
    if(info_battery.fine) {
        snprintf(voltage, sizeof(voltage), "%.2f V", info_battery.voltage);
        snprintf(temperature, sizeof(temperature), "%d °C", info_battery.temperature);
    } else {
        snprintf(voltage, sizeof(voltage), "-");
        snprintf(temperature, sizeof(temperature), "-");
    }

    snprintf(level, sizeof(level), "%d/5", info_battery.level);

    // How fast it goes, once it has been watched for a while.
    char rate[24];
    char left[32];
    char watched[24];
    u32 first = 0;
    u32 last = info_battery.count > 0 ? info_battery.count - 1 : 0;
    u64 span = info_battery.count > 1 ? info_battery.sampleTimes[last] - info_battery.sampleTimes[first] : 0;
    systeminfo_duration(watched, sizeof(watched), span);
    snprintf(left, sizeof(left), "-");
    if(span >= BATTERY_RATE_AFTER && info_battery.fine) {
        float perHour = (info_battery.samples[last] - info_battery.samples[first]) * 3600.0f / span;
        snprintf(rate, sizeof(rate), "%+.1f %%/h", perHour);
        if(info_battery.charging && perHour > 0.05f) {
            systeminfo_duration(left, sizeof(left), (u64) ((100 - info_battery.percent) / perHour * 3600));
        } else if(!info_battery.charging && perHour < -0.05f) {
            systeminfo_duration(left, sizeof(left), (u64) (info_battery.percent / -perHour * 3600));
        }
    } else {
        snprintf(rate, sizeof(rate), "%s", _("Measuring..."));
    }

    systeminfo_grid(x, y, w, 0, 0, 1, _("Voltage"), voltage);
    systeminfo_grid(x, y, w, 1, 0, 1, _("Temperature"), temperature);
    systeminfo_grid(x, y, w, 2, 0, 1, _("Charger"), info_battery.adapter ? _("Plugged in") : _("Unplugged"));
    systeminfo_grid(x, y, w, 0, 1, 1, info_battery.charging ? _("Charging rate") : _("Draining rate"), rate);
    systeminfo_grid(x, y, w, 1, 1, 1, info_battery.charging ? _("Full in") : _("Empty in"), left);
    systeminfo_grid(x, y, w, 2, 1, 1, _("Level (0-5)"), level);

    float graphY = y + 2 * (CELL_HEIGHT + CELL_GAP);
    systeminfo_battery_graph(x, graphY, w, y2 - 8 - graphY, watched);
}


// ---- for the web page ----

static json_t* systeminfo_media_json(const char* id, FS_SystemMediaType media) {
    FS_ArchiveResource resource = {0};
    if(R_FAILED(FSUSER_GetArchiveResource(&resource, media)) || resource.totalClusters == 0) {
        return NULL;
    }

    return json_pack("{s:s,s:I,s:I}", "id", id, "free", (json_int_t) ((u64) resource.freeClusters * resource.clusterSize),
                     "total", (json_int_t) ((u64) resource.totalClusters * resource.clusterSize));
}

// Everything the System screens show, as raw values where it is easy (the page words them in its own language). The battery's
// history only grows while someone asks, from the console's System screen or from here every few seconds.
json_t* systeminfo_json() {
    if(!info_static.loaded) {
        systeminfo_load_static();
    }

    if(!info_console.loaded) {
        systeminfo_load_console();
    }

    if(!info_storage.loaded) {
        systeminfo_load_storage();
    }

    systeminfo_load_network();
    systeminfo_load_battery();

    json_t* root = json_object();
    json_object_set_new(root, "console", json_pack("{s:s,s:s,s:s,s:s,s:s,s:s,s:s,s:s,s:s,s:s,s:s,s:s,s:b}", "model", info_static.model, "user", info_static.user,
                                                    "region", info_static.region, "language", info_static.language, "country", info_console.country,
                                                    "serial", info_console.serial, "deviceId", info_console.deviceId, "screens", info_console.screens,
                                                    "system", info_console.system, "kernel", info_console.kernel, "mcu", info_console.mcu,
                                                    "luma", info_console.luma, "cia", !envIsHomebrew()));
    json_object_set_new(root, "uptime", json_integer((json_int_t) (svcGetSystemTick() / SYSCLOCK_ARM11)));

    json_object_set_new(root, "network", json_pack("{s:b,s:s,s:s,s:s,s:s,s:s,s:s,s:s,s:s,s:i,s:i}", "connected", info_network.connected, "ssid", info_network.ssid,
                                                    "signal", info_network.signal, "mac", info_network.mac, "ip", info_network.ip,
                                                    "netmask", info_network.netmask, "gateway", info_network.gateway, "dns", info_network.dns,
                                                    "lease", info_network.lease, "ftp", ftpserver_running() ? FTP_SERVER_PORT : 0,
                                                    "web", webserver_running() ? WEB_SERVER_PORT : 0));

    json_t* media = json_array();
    const struct {
        const char* id;
        FS_SystemMediaType media;
    } kinds[] = {{"sd", SYSTEM_MEDIATYPE_SD}, {"ctr", SYSTEM_MEDIATYPE_CTR_NAND}, {"twl", SYSTEM_MEDIATYPE_TWL_NAND}, {"photo", SYSTEM_MEDIATYPE_TWL_PHOTO}};
    for(u32 i = 0; i < sizeof(kinds) / sizeof(*kinds); i++) {
        json_t* one = systeminfo_media_json(kinds[i].id, kinds[i].media);
        if(one != NULL) {
            json_array_append_new(media, one);
        }
    }

    json_object_set_new(root, "storage", json_pack("{s:s,s:s,s:s,s:s,s:s,s:s,s:o}", "maker", info_storage.maker, "product", info_storage.product,
                                                    "made", info_storage.made, "speed", info_storage.speed, "writable", info_storage.writable,
                                                    "cluster", info_storage.cluster, "media", media));

    u32 last = info_battery.count > 0 ? info_battery.count - 1 : 0;
    u64 span = info_battery.count > 1 ? info_battery.sampleTimes[last] - info_battery.sampleTimes[0] : 0;
    json_t* samples = json_array();
    for(u32 i = 0; i < info_battery.count; i++) {
        json_array_append_new(samples, json_real(info_battery.samples[i]));
    }

    json_t* battery = json_pack("{s:b,s:f,s:i,s:b,s:b,s:I,s:o,s:i}", "fine", info_battery.fine, "percent", (double) info_battery.percent, "level", (int) info_battery.level,
                                "charging", info_battery.charging, "adapter", info_battery.adapter, "watched", (json_int_t) span, "samples", samples,
                                "every", BATTERY_EVERY);
    if(info_battery.fine) {
        json_object_set_new(battery, "voltage", json_real(info_battery.voltage));
        json_object_set_new(battery, "temperature", json_integer(info_battery.temperature));
    }

    if(span >= BATTERY_RATE_AFTER && info_battery.fine) {
        json_object_set_new(battery, "perHour", json_real((info_battery.samples[last] - info_battery.samples[0]) * 3600.0f / span));
    }

    json_object_set_new(root, "battery", battery);
    return root;
}
