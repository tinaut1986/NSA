#pragma once

// A small FTP server running in the background while NSA is open, to drop
// files on the SD card or install CIAs from a computer.
// Ported from ../zelda-tmc-3ds platform/3ds/source/ftp_server_3ds.c.
//
// Anonymous (any user and password), one client at a time, passive mode only.
// "/" is the root of the SD card and nothing outside it is reachable: never
// the NAND or system save data. Commands: USER PASS SYST FEAT TYPE MODE STRU
// OPTS NOOP ALLO PWD CWD CDUP PASV EPSV LIST NLST MLSD RETR STOR APPE SIZE MDTM
// DELE MKD RMD RNFR RNTO QUIT, enough for FileZilla, WinSCP, curl and lftp.
//
// SITE INSTALL <path>: installs that CIA from the SD card. Only titles that
// install to the SD card; when it is NSA itself (CIA build), NSA restarts
// into it 10 s later (relaunch.h). With curl, upload then install:
//   curl -T NSA.cia ftp://IP:5021/nsa/update.cia -Q "-SITE INSTALL /nsa/update.cia"
//
// Remote control (debugging):
//   SITE SCREENSHOT          -> both screens to /nsa/screenshots/top.bmp, bottom.bmp
//   SITE PRESS <keys> [n]    -> holds buttons for n frames (A, B, X, Y, L, R, ZL,
//                               ZR, START, SELECT, UP, DOWN, LEFT, RIGHT; "A+R")
//   SITE TAP <x> <y> [n]     -> touches the bottom screen
//
// Port 5021, passive data ports 5022-5085: clear of Remote Install's "Receive
// URLs" (5000, where FBI's PC tools send) and of the console to console ports
// (5001 discovery, 5002 transfers).

#define FTP_SERVER_PORT 5021

// False with a reason in ftpserver_address() (no Wi-Fi, port busy...).
bool ftpserver_start();
void ftpserver_stop();
bool ftpserver_running();

// While the radio is taken by the local wireless: the server stops, and
// resume_later starts it again only if it was running (or about to) when paused.
// The network may not be back yet: it tries every couple of seconds (from
// ftpserver_poll_main) for two minutes.
void ftpserver_pause();
void ftpserver_resume_later();

// Main thread, once per frame after drawing: serves screenshot requests.
void ftpserver_poll_main();

// "192.168.1.50:5021", or why it is not serving.
void ftpserver_address(char* out, size_t size);

// "FTP 192.168.1.50:5021" while serving, else empty: for ui_set_status.
void ftpserver_corner(char* out, size_t size);

// While a CIA arrives or installs: the line to show and how far along it is
// (0..1, or < 0 when unknown). False otherwise. The restart that follows an
// install of NSA itself is relaunch.h's.
bool ftpserver_notice(char* out, size_t size, float* progress);
