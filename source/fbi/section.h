#pragma once

void mainmenu_open();

void dumpnand_open();
void extsavedata_open();
void files_open(FS_ArchiveID archiveId, FS_Path archivePath);
void files_open_sd();
void files_open_ctr_nand();
void files_open_twl_nand();
void files_open_twl_photo();
void files_open_twl_sound();
void pendingtitles_open();
void remoteinstall_open();
// Adds URLs to the history of Remote Install (the web page installs through the same screens).
void remoteinstall_note_urls(const char* urls);
void systemsavedata_open();
void tickets_open();
void titles_open();
void startup_run();
void settingsmenu_open();
void systeminfo_draw_top(float x1, float y1, float x2, float y2);