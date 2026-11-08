#pragma once

#include <time.h>

#include <3ds.h>

// The console's three Wi-Fi connections (System Settings -> Internet Settings -> Connection 1-3), and copies of them on
// the SD card in WifiManager's folder and format, so that both apps see the same copies.
//
// The system keeps the connections twice: in the config savegame (blocks 0x00080000 + n, 0xC00 bytes) and in the ac
// service, which reads them once at boot and connects with its own copy. Writing the blocks changes what is saved but
// not what ac uses, until System Settings saves again. So a connection is written through ac:i, as System Settings does
// it (wifislots_write): ac takes it up at once and saves it itself.

#define WIFISLOTS_COUNT 3
#define WIFISLOTS_BLOCK 0x00080000
#define WIFISLOTS_SIZE 0xC00
#define WIFISLOTS_BACKUP_DIR "/3ds/WifiManager"

typedef enum {
    WIFISLOTS_OPEN = 0,
    WIFISLOTS_WEP_64,
    WIFISLOTS_WEP_128,
    WIFISLOTS_WEP_152,
    WIFISLOTS_WPA_TKIP,
    WIFISLOTS_WPA_AES,
    WIFISLOTS_WPA2_TKIP,
    WIFISLOTS_WPA2_AES,
} wifislots_security;

typedef struct {
    char ssid[0x20];
    u8 ssidLength;
    u8 security;
    u8 padding1[2];
    char password[0x40];   // As typed; empty when the key was set some other way.
    u8 key[0x20];          // What the system connects with (the PSK for WPA, the key for WEP).
    u8 padding2[0x20];
} __attribute__((packed)) wifislots_network;

typedef struct {
    u8 dhcp;
    u8 autoDns;
    u8 padding1[2];
    u8 ip[4];
    u8 gateway[4];
    u8 netmask[4];
    u8 dns1[4];
    u8 dns2[4];
    u8 tested;   // The connection test passed once.
    u8 privacy;
    u8 padding2[2];
    u8 ipToUse[4];
    u8 bssid[6];
    u8 channel;
    u8 padding3;
    u8 proxyEnabled;
    u8 proxyAuth;
    u16 proxyPort;
    char proxyHost[0x30];
    u8 padding4[0x34];
    char proxyUser[0x20];
    char proxyPassword[0x20];
    u16 upnp;
    u16 mtu;
} __attribute__((packed)) wifislots_settings;

typedef struct {
    u16 version;          // 0: the connection is empty.
    u16 crc;              // CRC-16/ARC of bytes 0x4..0x414.
    u8 enabled;
    u8 editableSecurity;  // 0: set up with WPS or AOSS; the network is then multiSsid[0].
    u8 padding1[2];
    wifislots_network network;
    u8 multiSsidEnabled;
    u8 multiSsidType;
    u8 multiSsidCount;
    u8 padding2;
    wifislots_network multiSsid[4];
    wifislots_settings settings;
    u8 rest[0x7EC];       // Nothing the system reads: the CRC ends with the settings, at 0x414.
} __attribute__((packed)) wifislots_slot;

_Static_assert(sizeof(wifislots_network) == 0xA4, "a network is 0xA4 bytes");
_Static_assert(sizeof(wifislots_settings) == 0xD4, "the settings are 0xD4 bytes");
_Static_assert(sizeof(wifislots_slot) == WIFISLOTS_SIZE, "a connection is 0xC00 bytes");

// A copy on the SD card.
typedef struct {
    char path[256];      // sdmc:/3ds/WifiManager/<name>.bin
    char alias[100];     // The name it was given (up to 96 characters, as WifiManager), or empty.
    time_t saved;        // 0 when the file does not say.
    wifislots_slot slot;
} wifislots_backup;

Result wifislots_read(u32 index, wifislots_slot* out);
// Writes connection `index` through ac:i (see above). On failure `step` names the call that failed.
Result wifislots_write(u32 index, const wifislots_slot* in, const char** step);

bool wifislots_empty(const wifislots_slot* slot);
// The network it connects to: the main one, or the first WPS/AOSS one.
const wifislots_network* wifislots_network_of(const wifislots_slot* slot);
void wifislots_name(const wifislots_slot* slot, char* out, size_t size);
// The password as text: the typed one, else (WEP) the key, else "" (WPS, or set up by another app).
void wifislots_password(const wifislots_slot* slot, char* out, size_t size);
const char* wifislots_security_name(u8 security);

// The copies in WIFISLOTS_BACKUP_DIR, newest first. `out` is allocated (free it); returns how many.
u32 wifislots_list_backups(wifislots_backup** out);
// Saves `slot` as a new copy named after `alias` or the network; `path` gets where (when not NULL).
Result wifislots_save_backup(const wifislots_slot* slot, const char* alias, char* path, size_t pathSize);
Result wifislots_delete_backup(const wifislots_backup* backup);
// Gives a copy another alias (empty: none), in the same file and with the same date, as WifiManager's rename.
Result wifislots_set_alias(wifislots_backup* backup, const char* alias);

// Editing: a new, empty connection (DHCP, automatic DNS); the network typed by hand (its key worked out: the WPA PSK,
// or the WEP key), or the reason it cannot be (a text to translate); an address "a.b.c.d"; a copy's file rewritten
// with the same alias and date.
void wifislots_new(wifislots_slot* slot);
const char* wifislots_set_network(wifislots_slot* slot, const char* ssid, u8 security, const char* password);
bool wifislots_parse_ip(const char* text, u8* out);
Result wifislots_update_backup(wifislots_backup* backup, const wifislots_slot* slot);

// DEBUG: SITE WIFIREAD and SITE WIFIROUNDTRIP (ftpserver.c).
void wifislots_debug_read(char* out, size_t size);
void wifislots_debug_keycheck(char* out, size_t size);
bool wifislots_debug_roundtrip(u32 index, u32 change, char* out, size_t size);
