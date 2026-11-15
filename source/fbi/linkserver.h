#pragma once

// Links sent from a web page or another device to this console, the way 3dsDL's Receive takes them, so that a store's
// "Send to 3DS" button reaches NSA too (store.brewology.com links to http://3ds.local/send?links=...):
//
//   GET  http://3ds.local/                 a page to paste links into (also nsa.local, or the console's IP)
//   GET  http://3ds.local/send?links=...   the links (URL-encoded, separated by spaces or new lines), at most 20
//   POST http://3ds.local/send             the same as a form (links=...) or as plain text, one link a line
//
// Nothing is downloaded until the owner answers on the console: the links go to Remote install's question (install,
// save...), which also offers to extract an archive. 3dsDL's other fields (folder, replace, extract) are accepted and
// not needed: NSA asks about each of those itself. A browser gets a page back that says to look at the console (a page
// that sends with the header X-Requested-With gets {"count": n}).
//
// Port 80, while NSA is open and the setting is on (Settings, on by default). The console answers mDNS for "3ds.local"
// and "nsa.local" (another device already using one of them: "3ds-2.local"...), so the page needs no address.

#define LINK_SERVER_PORT 80

bool linkserver_start();
void linkserver_stop();
bool linkserver_running();
// Starts now, or once there is Wi-Fi.
void linkserver_start_when_online();
bool linkserver_waiting();
// The radio is taken (sleep, the local wireless), and comes back later.
void linkserver_pause();
void linkserver_resume_later();
// Main thread, once per frame: brings the server back, and asks about links that came.
void linkserver_poll_main();
// "3ds.local, nsa.local, 192.168.1.50", or why it is not running.
void linkserver_address(char* out, size_t size);
