#pragma once

void mainmenu_open();

void dumpnand_open();
void wificonnections_open();
void extsavedata_open();
void files_open(FS_ArchiveID archiveId, FS_Path archivePath);
void files_open_sd();
void files_open_ctr_nand();
void files_open_twl_nand();
void files_open_twl_photo();
void files_open_twl_sound();
// The folders of /luma/titles (mods.c).
void mods_open();
void pendingtitles_open();
void remoteinstall_open();
// Adds URLs to the history of Remote Install (the web page installs through the same screens).
void remoteinstall_note_urls(const char* urls);
void store_open();
// The camera reads QR codes until one comes out: `found` gets its text (the scanner already closed). `title` and `hint`
// (what to point it at; NULL: links to CIAs) are English, translated when drawn.
void qr_scan(const char* title, const char* hint, void* data, void (*found)(void* data, const char* text));
// NSA is leaving (a restart after an install, or the exit) with the Store maybe open: stops its threads first, so none
// is left running on memory that is going away.
void store_exit();
void systemsavedata_open();
void tickets_open();
void titles_open();
void startup_run();
void settingsmenu_open();
// The web page's address and PIN as a QR code in a square of `side` pixels (Settings, and Remote install's web button).
void settingsmenu_draw_web_qr(float x, float y, float side);
void languagemenu_open();
void systeminfo_draw_top(float x1, float y1, float x2, float y2);
void systeminfo_draw_console(float x1, float y1, float x2, float y2);
void systeminfo_draw_network(float x1, float y1, float x2, float y2);
void systeminfo_draw_storage(float x1, float y1, float x2, float y2);
void systeminfo_draw_battery(float x1, float y1, float x2, float y2);
// The System details as JSON for the web page (main thread only: it reads what the System screens read).
struct json_t;
struct json_t* systeminfo_json();
