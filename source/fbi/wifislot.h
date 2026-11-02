#pragma once

// The console's three Wi-Fi slots (config blocks 0x80000-0x80002, read and
// written through cfg:i) and their copies on the SD card. Adapted from
// WiFiManager+ (github.com/tinaut1986/WifiManager, itself a fork of
// LiquidFenrir's WifiManager): the same slot layout and the same backup files,
// so the ones it saved in sdmc:/3ds/WifiManager/ are listed too.

#define WIFI_SLOT_COUNT 3
#define WIFI_SLOT_BLOCK 0x00080000

// NSA saves its copies here; WiFiManager's folder is read as well.
#define WIFI_BACKUP_DIR "sdmc:/nsa/wifi"
#define WIFI_LEGACY_DIR "sdmc:/3ds/WifiManager"

typedef enum {
    WIFI_ENCRYPTION_OPEN = 0,
    WIFI_ENCRYPTION_WEP_64,
    WIFI_ENCRYPTION_WEP_128,
    WIFI_ENCRYPTION_WEP_152,
    WIFI_ENCRYPTION_WPA_PSK_TKIP,
    WIFI_ENCRYPTION_WPA_PSK_AES,
    WIFI_ENCRYPTION_WPA2_PSK_TKIP,
    WIFI_ENCRYPTION_WPA2_PSK_AES,
    WIFI_ENCRYPTION_COUNT
} wifi_encryption;

typedef struct {
    char ssid[0x20];
    u8 ssidLength;
    u8 encryption;
    u8 padding1[2];
    char passwordText[0x40];
    u8 passwordKey[0x20];   // The WEP key, or the WPA PSK made from the password and the SSID.
    u8 padding2[0x20];
} __attribute__((packed)) wifi_network_raw;

// The block as the console keeps it (0xC00 bytes; the CRC covers 0x410 from offset 4).
typedef struct {
    u16 version;
    u16 crc;

    u8 networkEnable;
    u8 editableSecurity;    // 0: set up with WPS or AOSS, the network is in multissid[0].
    u8 padding1[2];
    wifi_network_raw network;

    u8 multissidEnable;
    u8 multissidType;
    u8 multissidAmount;
    u8 padding2[1];
    wifi_network_raw multissid[4];

    u8 dhcp;
    u8 autoDns;
    u8 padding3[2];

    u8 ip[4];
    u8 gateway[4];
    u8 netmask[4];
    u8 dnsPrimary[4];
    u8 dnsSecondary[4];

    u8 connectionTestSuccess;
    u8 groupPrivacyMode;
    u8 padding4[2];
    u8 ipToUse[4];
    u8 apMac[6];
    u8 channel;
    u8 padding5[1];

    u8 proxyEnable;
    u8 proxyAuth;
    u16 proxyPort;
    char proxyHost[0x30];
    u8 padding6[0x34];
    char proxyUser[0x20];
    char proxyPassword[0x20];

    u16 upnp;
    u16 mtu;

    u8 padding7[0x7EC];
} __attribute__((packed)) wifi_slot_raw;

typedef enum {
    WIFI_SECURITY_OPEN,
    WIFI_SECURITY_WEP,
    WIFI_SECURITY_WPA,
    WIFI_SECURITY_WPA2
} wifi_security;

// A slot or a backup, read for showing.
typedef struct {
    bool exists;
    bool wps;               // Set up with WPS or AOSS: the password may not be readable.
    char ssid[0x21];
    char password[0x41];    // "" when there is none or it cannot be read.
    u8 encryption;
    wifi_slot_raw raw;
} wifi_slot;

typedef struct {
    char path[256];
    char name[128];         // The alias, else the SSID, else the file's name.
    char alias[64];
    s64 saved;              // Unix time, 0 when unknown.
    bool legacy;            // In WiFiManager's folder.
    wifi_slot slot;
} wifi_backup;

const char* wifi_encryption_name(u8 encryption);
wifi_security wifi_security_of(u8 encryption);

bool wifi_slot_read(u32 index, wifi_slot* out);
// Writes the slot (CRC fixed) and saves the config; the console uses it after a restart.
Result wifi_slot_write(u32 index, const wifi_slot* in);
// Empties the slot.
Result wifi_slot_clear(u32 index);

// A slot from a name, password and security: on top of `base` (its IP, DNS and
// proxy settings) when it holds one, else with DHCP. Makes the WPA PSK or WEP key.
Result wifi_slot_from_credentials(wifi_slot* out, const wifi_slot* base, const char* ssid, const char* password, wifi_security security);

// The backups of both folders, newest first; free() the array.
u32 wifi_backup_list(wifi_backup** out);
// Saves a slot as a new backup in WIFI_BACKUP_DIR (named after the alias or the SSID).
Result wifi_backup_save(const wifi_slot* slot, const char* alias, char* pathOut, size_t pathSize);
// Writes a backup back to its file (an edit, or a new alias).
Result wifi_backup_rewrite(const wifi_backup* backup);

// The WIFI:S:...;T:...;P:...;; text of a phone's "share network" QR code, or a plain
// "SSID" / "SSID\npassword". False when it holds no SSID.
bool wifi_qr_parse(const char* payload, char* ssid, size_t ssidSize, char* password, size_t passwordSize, wifi_security* security);
void wifi_qr_encode(const wifi_slot* slot, char* out, size_t size);
