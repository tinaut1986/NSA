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
//   curl -T NSA.cia ftp://IP:5000/nsa/update.cia -Q "-SITE INSTALL /nsa/update.cia"
//
// Remote control (debugging):
//   SITE SCREENSHOT          -> both screens to /nsa/screenshots/top.bmp, bottom.bmp
//   SITE PRESS <keys> [n]    -> holds buttons for n frames (A, B, X, Y, L, R, ZL,
//                               ZR, START, SELECT, UP, DOWN, LEFT, RIGHT; "A+R")
//   SITE TAP <x> <y> [n]     -> touches the bottom screen
//
// Port 5000, shared with Remote Install's "Receive URLs": that screen pauses
// the server and resumes it when it closes.

#define FTP_SERVER_PORT 5000

// False with a reason in ftpserver_address() (no Wi-Fi, port busy...).
bool ftpserver_start();
void ftpserver_stop();
bool ftpserver_running();

// Give port 5000 to someone else for a while; resume restarts the server only
// if it was running when paused.
void ftpserver_pause();
void ftpserver_resume();

// Main thread, once per frame after drawing: serves screenshot requests.
void ftpserver_poll_main();

// "192.168.1.50:5000", or why it is not serving.
void ftpserver_address(char* out, size_t size);

// "FTP 192.168.1.50:5000" while serving, else empty: for ui_set_status.
void ftpserver_corner(char* out, size_t size);

// While a CIA arrives or installs: the line to show and how far along it is
// (0..1, or < 0 when unknown). False otherwise. The restart that follows an
// install of NSA itself is relaunch.h's.
bool ftpserver_notice(char* out, size_t size, float* progress);
