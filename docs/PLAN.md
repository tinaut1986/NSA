# NSA plan

Start every session here. It holds where things stand, what comes next and why
things are the way they are. Keep it current in the same change that alters it.

## Status (2026-10-27, end of the day's session)

Everything the owner validated is merged into `release` and pushed; no topic branch is
open. `origin/release` = local `release`. The branches `feat/*` and `fix/*` that were
merged are kept on the remote and can be deleted.

| Branch / tag | State |
|---|---|
| `release` | v0.1.0 (beta, published) plus everything below. v0.2.0 (beta) published 2026-10-27. |
| `master` | v0.1.0 has not been promoted to stable. |
| tag `v0.2.0` | Beta published (CIA, 3DSX, QR). Not promoted to stable. |

What `release` holds, in the order it was built:

1. **Themed interface**: drawing primitives, Tabler icons, the "Cards" theme (day and
   night), 8 accent colours, night mode, Spanish, new main menu and Settings, FTP
   remote control (`SITE SCREENSHOT`, `PRESS`, `TAP`).
2. **Titles**: rows with icons, icon grid, filter and sort popover, search, actions as
   a sheet; updates and DLC grouped under their game (count badge, chain view); kind
   and region filters; loads into placeholders, sorts live, loaded/total in the title bar.
3. **Files**: path in the title bar, tools (clipboard, filter and sort, search, folder
   actions), icon by kind, CIA details on top, the folder left stays selected going up.
4. **Touch and motion everywhere**: pressed look, act on release, long press with a
   radial fill opens an item's actions, flick scrolling, animated sheets/popovers/prompts,
   the top card's contents cross-fade (boxes stay still).
5. **Storage bars** (`widget_storage`), **stereoscopic 3D** (whole-pixel shifts), grouped
   **main menu** (three big tiles; Cleanup and data; Advanced with one warning per
   session; System; Settings), **System page**, status bar (battery with percentage,
   Wi-Fi bars, FTP address end).
6. **Icons** from 16 and 32 px atlases (`romfs/icons16.png`, `icons32.png`).
7. **Filters remembered** per screen in `settings.json` (titles, tickets, files, extsave).
8. **Dialogs and progress** redone and translated (icon header, percentage, cards with
   cells on the top screen).
9. **Tickets** and **Ext Save Data** on the Titles pattern; tickets show the title they
   belong to; ext data show their size and are linked to the installed game by ID or by
   ID without the last byte; Titles' sheet offers Browse/Delete extra data for a game that
   has some; browsing leaves the sheet open so B comes back to it.

**On the console now:** the last build of `feat/action-dialogs` (= `release`, DEBUG),
installed over FTP at `192.168.1.133`. Still to confirm there:
- Ext data link (Mario Kart 7: title `…30700`, data `…307`): does Titles offer Browse
  extra data, and does the Ext Save Data top screen name the game? How many ext data
  stay unlinked? (If many: read the real ExtData ID from each game's exheader; see Next.)
- The Wi-Fi SSID and the system version in System (Azahar gives neither).

## Export an installed title as a CIA (`feat/export-cia`, merged)

Titles' sheet has "Export as CIA" (CTR titles on SD or NAND, not cards or DSiWare); the
file goes to `sdmc:/nsa/cias/<name> [<title ID>] v<version>.cia`
(`source/fbi/action/exportcia.c`). fs:USER does not give a title's raw contents, so it
borrows fs's own `PxiFS0` session through Luma3DS's `svcControlService` (0xB0) and reads
each content with FSPXI on archive `0x2345678A`, path type 2 (raw content by TMD index):
the NCCH as installed, without the SD and title key layers. Certificates come from
`nand:/dbs/certs.db` (found by issuer and name); ticket and TMD are made here, unsigned,
contents marked not encrypted, hashes computed while copying; the SMDH is the meta.
Installing it needs signature patches; installing it on a console that has the title's
real ticket replaces that ticket with the made one. The copy reads the next block while
the last one is written (system core) and hashed (core 2 on a New 3DS, else the app core).
The owner validated it on the console (export, then NSA and FBI take the CIA). Azahar
cannot run it: it has no Luma SVCs.

## Console to console (`feat/send-between-3ds`, merged; the consoles run it, 2026-10-28)

Send a file to another 3DS on the same Wi-Fi (`source/fbi/transfer.c`, protocol in
`transfer.h`): Files' file sheet has "Send to another 3DS", Remote install has "Receive
from another 3DS". UDP broadcast on 5001 finds the receivers, TCP 5002 carries the file;
the receiver allows it, then picks: Install now (only for `.cia`; installed from
`sdmc:/nsa/received/`, then deleted), Save (a folder picker, starting in
`sdmc:/nsa/received/`) or Reject. Sending works between two consoles (owner, 2026-10-27).
Protocol 4 sends over **four connections at once** (`transferconn.c`; one is slow on a 3DS,
as 3dsDL and FBI Reloaded found): chunks of 256 KB go to whichever is free, with 48 KiB
socket buffers set before connecting; a file is written in place (the space taken first)
and `<name>.part.pos` says how far it is whole; a title package is hashed after it is all
there ("Checking the contents"). Receiving and sending verified in Azahar (extra consoles
are listed in `/nsa/peers.txt`, one address a line, which also helps where broadcasts do
not pass), including a cut transfer, resuming and the retry prompt. **Not verified on
consoles**; the owner's 133 (New 3DS) and 139 (2DS) still run the build before this.
Azahar aborts on a thread for core 2 on an Old 3DS: asked only on a New 3DS. Before it,
protocol 2 resumed: the receiver writes `<name>.part`, and when the same file comes
again (CRC-32 of its first 4 KB in the hello, and at least 1 MiB there) it answers the
byte to go on from, the last MiB discarded. The sender reads the next 256 KB block in a
thread while it sends one. Resuming verified in Azahar (receiver side, byte for byte);
the speed gain and the sender's side are not measured. The sender connects blocking: a non-blocking connect did not report its outcome on the
3DS. Titles' sheet has "Send to another 3DS": no CIA is made first. The sender reads the
contents through PXI as the export does and streams them as a *title package*
(`exportcia.c`, "title packages"; hello kind 1, protocol 3); the receiver's sink builds the
CIA in `<name>.part` (hashes while writing, the TMD last), then renames it and offers Install
now / Save as for any CIA. A title package does not resume (a stopped one leaves nothing).
The receiver side is verified in Azahar with a fake package (contents, hashes, title ID,
version and save size right; `nand:/dbs/certs.db` is missing there, so that check was
bypassed in a temporary build); the sender needs Luma3DS and two consoles. Not done:
folders.

### Local wireless (nearby) transfer, 2026-10-27

No router needed, and nothing to set: a console **without a Wi-Fi network** that opens Receive creates its own
(UDS: `udslink.c`, named after the console); a sender with no Wi-Fi scans for it, and one with Wi-Fi that finds
nobody on the router for 7 s gets a row "Search nearby (no Wi-Fi)" (a choice: it takes the console off the Wi-Fi). UDS has no IP, so the transfer runs over `relstream.c`, a reliable
stream (window of 48 frames, selective acks, retransmission; host test `tests/relstream_test.c`, 440 to 530 KiB/s
simulated at 30 % loss) with one connection instead of four; the hello, chunks, resume, title packages and
screens are the same (`transfer_link` in `transferconn.c`). Using it drops the console's Wi-Fi: the FTP server is
paused and comes back by itself (`ftpserver_resume_later`). Measured on the consoles: **4 MiB in 10 s (about
400 KiB/s), no frame resent, identical file**. A receiver that has Wi-Fi uses the router, so a sender without Wi-Fi
will not find it (switch the receiver's Wi-Fi off). Not tried: a title package over it, and the fallback from
Wi-Fi to nearby.
Developer flag: `sdmc:/nsa/nearby.flag` makes the console behave as if it had no Wi-Fi; with `auto` inside, the
sender picks the first console found and the receiver accepts, and `/nsa/nearby-log.txt` gets the results.

## What the owner asked for (the direction)

The look is right, but navigation still feels like FBI: lists, Select for options,
another list for actions. Every screen should move to the Titles pattern:

- Tool icons in the title bar (back, view, filter, search) instead of Select menus.
- A popover for filter and sort, a sheet (list of rows) for actions.
- Rows with the item's picture and secondary info; a grid where pictures matter.
- The top screen shows the selected item's card and its details in cells.
- Modal buttons with centred labels; one tap opens; D-Pad and touch everywhere.

## Next

0. **Still open from console to console**: a title package over the local wireless (only
   files were tried there, and a CIA received over it installed), cutting and resuming over the
   local wireless, long transfers over it, sending a title from the New 3DS, the *Super Mario
   Land 2* CIA received before the certificate fix (its root certificate is wrong: replace the
   certificate chain, 0xA00 bytes after the header, with the standard one), and a release
   (`v0.3.0` beta) with its notes. The CIAs it makes install only on a console with signature
   patches (Luma3DS); every send and receive is logged in `/nsa/transfer-log.txt`. Receive has a
   button (and X) to change between the Wi-Fi and its own network, the send list a tool icon (and
   X); both ways were tried on the consoles, and the list drag fix too.

1. ~~Publish v0.2.0 beta~~ done. Promoting to stable needs the owner's OK (`master` must contain the tag first).
2. **Remote install**, the owner agreed it needs the same treatment. Proposal, to show as
   a mockup first: three big cards (QR code, enter URLs, receive from a computer), the
   receive screen with the console's IP and port large and what has arrived, the last
   request as a history row, progress like the new dialogs. Translate its list items.
   Possibly move "Receive URLs" off port 5000 (it pauses the FTP server): the owner
   raised it, not decided.
3. **Pending titles, System save data, Dump NAND** on the same pattern (Dump NAND as
   its own screen: space needed against free on the SD card, progress with ETA, where
   the file ends up). Pending titles and System save data still have FBI's action lists
   and English item names ("Delete Pending Title"...); System save data would show
   readable names of the known modules.
4. **Exact ext data link**: read the ExtData ID from each game's exheader (NCCH
   `ExHeader`, ARM11 local system capabilities, offset 0x230 by 3dbrew; check with the
   Program ID at 0x200) instead of the ID heuristics. The file path type for the exheader
   on `ARCHIVE_SAVEDATA_AND_CONTENT` is not confirmed: probe it on the console and write
   the result to a file the FTP server can fetch, since Azahar cannot answer it.
5. **Themes "Pixel" and "HOME"** (mockups were shown and liked): HOME = big icon tiles
   in pages, modals as a sheet from the bottom; Pixel = bundled pixel font (OFL), square
   corners, thick borders, block progress bars, yellow highlight. Then **themes from the
   SD card** (`sdmc:/nsa/themes/`), data only.
6. **NSA icon and banner** (HOME menu and in-app): still FBI's. Ask the owner for a
   direction first.
7. **Top-screen information**: the owner wants to restructure what the top screen shows
   in general; ask before designing it.
8. **A help window** (SELECT opens the shortcuts of the screen plus notes) was built and
   the owner discarded it for now (branch deleted). If it comes back: a card over the
   screen listing keys and notes, per-screen entries, hints ending in "SELECT help".
9. Tickets: rows show the title name only for installed titles; an update or DLC ticket has
   no icon. Fine for now.

## Decisions

- Own app (`0xFB1A0`), own data folder (`sdmc:/nsa/`, import from `/fbi/` offered
  once), own versions from `v0.1.0`. Upstream is noted as "based on FBI 2.6.1".
- Updates: Stable/Beta channels from GitHub releases; the changelog joins every
  release since the installed one. Ported from `../zelda-tmc-3ds` (host tests in `tests/`).
- FTP server on port 5000, SD card only; `SITE INSTALL` refuses NAND titles and never
  deletes a title to force a downgrade (it would delete the game's saves). Remote
  install's "Receive URLs" also uses 5000 and pauses the server while open.
- `DEBUG=1` builds start the FTP server; release builds only when turned on.
- FBI's PNG themes and `textcolor.cfg` were dropped for drawn shapes.
- **Strings**: `_("English")` for literals (cached by address). Text that may be built
  at run time (dialog messages, hints, list names) goes through `i18n_translate_text`
  (looked up by content, never cached): `prompt_*`, `info_display`, `error_display*`,
  `kbd_display`, `list_display` and the default list row do it themselves. Format strings
  of progress text are wrapped in `_()` at the call site. Spanish table: `i18n_es.c`.
- **Lists load in two passes** (Titles, Tickets, Ext Save Data): the task adds a
  placeholder row for every item (ID only), then fills each one's details in place and
  marks it `ready`; the screen draws grey blocks until then, fades the contents in,
  sorts as items become ready (ready first, the rest by ID) and shows loaded/total in the
  title bar (`list_set_progress`). The loader goes over the list until none is missing,
  because the screen reorders it meanwhile. The list keeps the selected row where it was
  on screen when items move (`list_validate`), and `list_rebuild_visible` never writes past
  its array (a populate task adds items concurrently: that was a crash).
- A press (and its long-press fill) follows the item it began on, not its index.
- Filters, sort and view of Titles, Tickets, Files and Ext Save Data are stored in
  `settings.json` on every change (`titles_store` and the like).
- Deleting actions are drawn in red in the sheets (`sheet_action.danger`); the sheet's
  `stay` flag runs an action with the sheet still open (browsing: B returns to it).
- Titles' kind comes from the title ID's upper half (`00040000` game, `0004000E` update,
  `0004008C` DLC, `00040002` demo, `00048xxx` DSiWare, else system); a game, its update
  and its DLC share the lower half.
- A ticket is "in use" when a title with its ID is installed (SD or NAND); otherwise it
  is left from a deleted title or one never installed.
- Ext data → game: the data ID equals the title's low half, or its low half without the
  last byte (Mario Kart 7: `30700` and `307`). A hint, not proof: the wording says
  "Probably", and the delete confirmation shows the data's own name and icon.
- Icons: 16 px atlas for rows and buttons, 32 px for tiles; drawn on whole pixels at their
  own size. Textures minify linearly (nearest dropped rows of texels).
- 3D: each eye shifts a whole number of pixels per depth layer (a fraction made one eye
  see thin lines the other did not); layers: background 0, bar 1, content 2, card icons 3.
- Battery digits are placed by measurement on the console's system font (Azahar's
  stand-in font sits differently): 8 px tall, one free row above and below.
- Motion: only text, icons and images move when the selection changes; boxes stay.
- No SELECT shortcut is used for options any more: L opens the filter everywhere.

## Known issues

- `SITE SCREENSHOT` on the console answers "not taken in time", yet the files appear a few
  seconds later: fetch `/nsa/screenshots/top.bmp` after waiting. Suspected: NSA paused
  in the background; not confirmed.
- **Azahar cannot judge** the console's data: its battery is 0, its Wi-Fi/SSID/system
  version are empty, its ticket list repeats one ticket, its ext data is a dummy with ID 0
  and size 0, its font differs (measure text and digits on the console). HTTPS (the
  updater) does not answer there. Use it for layout and flow, never for data.
- Azahar remembers the filters in its own `settings.json`: an earlier test left "Games"
  off and Titles looked empty; upload a clean `/nsa/settings.json` to reset it.
- Azahar's virtual SD holds test files left by sessions (`/test`, renamed ones): harmless.
- The system font has no "·": use the button glyphs or plain spacing.
- The `Failed to allocate ...` messages and the error code descriptions (`error.c`) stay
  in English on purpose.
