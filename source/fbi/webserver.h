#pragma once

// A small web server running in the background while NSA is open (and the owner turned it on in
// Settings): a page to manage the console from a computer's browser.
//
//   http://<console IP>:8080/   the page (romfs:/web/index.html), asks for the PIN shown in Settings.
//
// Everything under /api needs the PIN: the cookie `pin` or the header `X-Pin` (anything that
// changes something needs the header, which a page of another site cannot send). The PIN is made
// again each time the server starts. After 10 wrong ones in a row the server refuses for a minute.
// It is plain HTTP on the local network: anyone who can watch the Wi-Fi sees the PIN.
//
// Only the SD card is reachable ("/" is its root), never the NAND. One request at a time.
//
//   GET    /api/info                       {console, version, free, total}
//   GET    /api/list?path=/dir             [{name, dir, size, mtime}]
//   GET    /api/file?path=/f               the file (Range supported)
//   PUT    /api/file?path=/f               the body is the file (written as .part, then renamed)
//   DELETE /api/file?path=/f               a file, or a folder with everything in it
//   POST   /api/mkdir?path=/dir
//   POST   /api/rename?path=/a&to=/b
//   POST   /api/install?path=/f.cia        installs a CIA from the SD card (SD titles only)
//   GET    /theme.json                     (public) the console's theme, for the page to wear
//   GET    /font.ttf                       (public) the Pixel theme's font
//   GET    /api/titles                     [{id, media, kind, version, size}]: SD and NAND, no DSiWare
//   GET    /api/title?id=&media=           {name, publisher, code, save, ext, boss}
//   GET    /api/icon?id=&media=            the 48x48 icon as a BMP
//   POST   /api/save/pack?id=&media=&kind=save|ext|boss&name=   makes the .nsasave package: {path, size}
//   POST   /api/save/receive?path=/nsa/received/x.nsasave&import=1   the console takes it like a sent one and asks there
//
// Port 8080 (the ports 5000-5085 are Remote Install, the transfers and the FTP server).

#define WEB_SERVER_PORT 8080

// False with a reason in webserver_address() (no Wi-Fi, port busy...).
bool webserver_start();
void webserver_stop();
bool webserver_running();

// As the FTP server's: the radio is taken by the local wireless, and comes back later.
void webserver_pause();
void webserver_resume_later();

// Main thread, once per frame: brings the server back after a pause.
void webserver_poll_main();

// "http://192.168.1.50:8080", or why it is not serving.
void webserver_address(char* out, size_t size);

// The PIN to type in the page ("" when not serving).
void webserver_pin(char* out, size_t size);
