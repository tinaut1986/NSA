# NSA plan

Start every session here. It holds where things stand, what comes next and why
things are the way they are. Keep it current in the same change that alters it.

## Status (2026-10-28)

Everything the owner validated is merged into `release` and pushed; no topic branch is
open. Merged topic branches are kept on the remote and can be deleted.

| Branch / tag | State |
|---|---|
| `release` | The working line. Holds everything below. |
| `master` | Stable. Moves only by merging `release`. |
| tag `v0.3.0` | **Stable**, published 2026-10-28 (notes: `docs/release-notes/v0.3.0.md`). `v0.1.0` and `v0.2.0` were betas. |

What `release` holds, in the order it was built:

1. **Themed interface**: drawing primitives, Tabler icons, the "Cards" theme (day and
   night), 8 accent colours, night mode, Spanish, new main menu and Settings, FTP
   remote control (`SITE SCREENSHOT`, `PRESS`, `TAP`, `DRAG`).
2. **Titles**: rows with icons, icon grid, filter and sort popover, search, actions as
   a sheet; updates and DLC grouped under their game (count badge, chain view); kind
   and region filters; loads into placeholders, sorts live, loaded/total in the title bar.
3. **Files**: path in the title bar, tools (clipboard, filter and sort, search, folder
   actions), icon by kind, CIA details on top, the folder left stays selected going up.
4. **Touch and motion everywhere**: pressed look, act on release, long press with a
   radial fill opens an item's actions, flick scrolling, drag scrolling, animated
   sheets/popovers/prompts, the top card's contents cross-fade (boxes stay still).
5. **Storage bars** (`widget_storage`), **stereoscopic 3D** (whole-pixel shifts), grouped
   **main menu**, **System page**, status bar (battery, Wi-Fi bars, FTP address end).
6. **Icons** from 16 and 32 px atlases (`romfs/icons16.png`, `icons32.png`).
7. **Filters remembered** per screen in `settings.json` (titles, tickets, files, extsave).
8. **Dialogs and progress** redone and translated.
9. **Tickets** and **Ext Save Data** on the Titles pattern (ext data linked to the game by
   ID heuristics).
10. **Export an installed title as a CIA** (Titles' sheet; by another session, validated).
11. **Console to console** (below): send and receive files and titles, over the Wi-Fi or
    with no router, install on arrival, resume, log.
12. **Files: cut/move, pictures and text** (merged, see Pending tests): "Cut (move)" on files and folders (renamed when pasted on the same medium,
    else copied then deleted); tapping a picture (PNG, JPEG, BMP, GIF, TGA, camera MPO in 3D)
    opens `imageview.c` (zoom, drag, L/R through the folder; decoded up to 5 megapixels and
    shrunk to a 1024 px texture); text files (and "Open as text" for any file) open
    `textview.c`, which edits with its own keyboard on the bottom screen (the system one hides
    the text), keeps CRLF, reads non-UTF-8 as Latin-1, up to 256 KB (longer: read only).
13. **Audio player** (merged, see Pending tests): MP3, WAV, FLAC and
    Ogg Vorbis from Files (`audioview.c`). dr_mp3/dr_flac/dr_wav (vendored in `source/libs/dr_libs`,
    public domain) and Tremor decode on a thread into four DSP buffers; the next file of the
    folder follows, seek bar, volume, +-10 s. Needs `sdmc:/3ds/dspfirm.cdc`. Tested only on Azahar.

## Pending tests on the console (2026-10-31)

Merged into `release` on the owner's word, **not yet tried on a console** (only built, and the
viewers in Azahar). The owner will go through them one by one; tick each when validated.

**Downloads** (issue #19 has the review points)
- [ ] Big download (>= 4 MB) from a server with `Accept-Ranges`: New 3DS (8 connections) and
      2DS (2 connections): speed, cancel half way, dropped connection (segment retries), hash.
- [ ] Small download and a server without ranges (one connection, ring of buffers).
- [ ] HTTPS curl cannot connect to: fallback to the system's HTTP service.
- [ ] `transfer-log.txt` has the `http:` speed lines.
- [ ] Idle drawing: nothing that moves by itself looks choppy (toasts, progress, cross-fades,
      Receive screens); `ui_wake()` exists but nobody calls it yet.
- [ ] `SITE CPU` (DEBUG) answers per core without disturbing the FTP server.

**Remote install history**
- [ ] History list: install again, forget, long-press sheet, top card with the links.
- [ ] Migration of the old `lasturls` (now deleted through an opened SD archive); 100 request limit.

**Files: cut / move**
- [ ] Cut a file and a folder, paste on the same medium (instant rename) and on another one
      (SD to NAND: copy then delete the source); same folder; folder into itself; name taken.
- [ ] Cut, then paste after the source's list was left (clipboard survives navigating).

**Files: pictures**
- [ ] PNG, JPEG, BMP, GIF, TGA open; L/R through the folder; zoom, drag, D-Pad/Circle Pad pan.
- [ ] A camera's **MPO in 3D** (pair order, depth, slider at 0 shows the left picture): not
      tried even in Azahar (no sample).
- [ ] More than 5 megapixels / 12 MB refuses with a message; a 1024+ px picture is shrunk.
- [ ] Memory on an Old 3DS with a 4-5 megapixel JPEG.

**Files: text**
- [ ] Open `.txt`/`.json`/...; "Open as text" on any file; binary files refuse.
- [ ] Edit with the custom keyboard: letters, shift/caps, symbols, accents, backspace/arrows
      repeat, D-Pad cursor, X/Y/A shortcuts, START saves, close with changes asks.
- [ ] CRLF kept; a Latin-1 file is read and saved as UTF-8; a file over 256 KB is read only.
- [ ] Saving on the NAND / save-data archives (commit).

**Files: audio**
- [ ] MP3, WAV, FLAC, Ogg Vorbis really heard (Azahar only showed the position moving).
- [ ] Old 3DS decodes MP3/FLAC in real time; CPU use while the UI draws.
- [ ] Seek (+-10 s, bar) in a long MP3 (slow without a seek table); next track on its own.
- [ ] No `dspfirm.cdc`: the message shows instead of crashing.

## Export an installed title as a CIA (`source/fbi/action/exportcia.c`)

Titles' sheet has "Export as CIA" (CTR titles on SD or NAND, not cards or DSiWare); the
file goes to `sdmc:/nsa/cias/<name> [<title ID>] v<version>.cia`. fs:USER does not give a
title's raw contents, so it borrows fs's own `PxiFS0` session through Luma3DS's
`svcControlService` (0xB0) and reads each content with FSPXI on archive `0x2345678A`,
path type 2 (raw content by TMD index): the NCCH as installed, without the SD and title key
layers. The **certificate chain is the standard one** embedded in `ciacerts.h` (the copies in
a console's `nand:/dbs/certs.db` are not reliable: a New 3DS's root certificate differed
from the standard in 490 bytes and AM refused it, "cert signature or hash check failed",
`0xD8E0806A`; a 2DS keeps it in another layout). The ticket and TMD are made here, unsigned,
contents marked not encrypted, hashes computed while copying; the SMDH is the meta.
**Installing such a CIA needs signature patches** (Luma3DS has them); on a console that
has the title's real ticket it replaces that ticket with the made one. Azahar cannot run
the export: it has no Luma SVCs.

## Console to console

Files' file sheet and Titles' sheet have "Send to another 3DS"; Remote install has "Receive
from another 3DS". Code: `transfer.c` (screens, hello, answers, resume, install),
`transferconn.c` (chunks over the connections), `udslink.c` (local wireless), `relstream.c`
(reliable stream, pure C, host test `tests/relstream_test.c`), the title package in
`exportcia.c` ("title packages").

**How a transfer goes**
- Discovery: on the Wi-Fi, UDP broadcast on 5001 (the receiver answers `NSA!` and its
  name; `/nsa/peers.txt` lists addresses to ask directly, one per line). Nearby: UDS
  beacons (below).
- Control connection (TCP 5002, or the UDS stream): the sender sends a 316 byte hello
  (protocol 5: magic, version, sender name, file name, size, `check` = CRC-32 of the first
  4 KB of the file or of a package's header, `kind` 0 file / 1 title package, `head` = the
  package header's size). The receiver asks its user (Allow, then Install now / Save /
  Reject; Install only for a CIA; Save opens a folder picker starting in `sdmc:/nsa/received/`)
  and answers 13 bytes: status, the byte to go on from, a transfer number.
- Data: the payload is cut in chunks of 256 KB. Over the Wi-Fi four TCP connections (the
  control one and three more that quote the transfer number) carry chunks, each taken by
  whichever is free; a record is offset (8), length (4), bytes; a connection ends with a
  terminator record and an **extra connection is closed only after the receiver acknowledges
  it** (closing a socket on the 3DS throws away what it has not sent and resets the
  connection: that cost the last bytes of a chunk before). 48 KiB socket buffers are set
  before connecting (what 3dsDL and FBI Reloaded found: it widens the TCP window; a single
  connection did about 100 KiB/s, four about 500). On New 3DS the third and fourth
  connection try core 2.
- Files are written in place (the size is taken first, so a full card fails at the start);
  `<name>.part.pos` (position, check, size) says how far everything is whole. A title
  package is a header (title ID, version, save size, contents list, meta) plus each
  content as read; the receiver's sink builds the CIA in `<name>.part`, hashes the written
  contents at the end ("Checking the contents") and writes the TMD last. A cut transfer
  (files and titles) goes on from the last whole place minus 1 MiB; the sender sends the
  package header again.
- The receiver answers the end with 1 byte: 1 whole, 0 failed, **2 the user cancelled** (the
  sender then says "The other console cancelled the transfer" and does not offer to retry;
  after a failure it offers "Retry?" and the receiver accepts the same file again by itself
  with what was chosen).
- "Install now" installs from `sdmc:/nsa/received/` and deletes the CIA, with no second
  question (`action_install_cia_delete_now`).

**Local wireless (no router)**
- UDS (`udslink.c`): a console **without Wi-Fi** that opens Receive creates a network named
  after itself; a sender without Wi-Fi scans and lists it. With Wi-Fi, Receive uses the
  router and the screen has a button (and X) "Receive without Wi-Fi" (and back, "Receive over
  Wi-Fi"), plus a hint after 15 s with nobody; the send list shows a row "Search nearby (no
  Wi-Fi)" after 7 s with nobody, and a tool icon (and X) to change either way. Changing
  takes the radio and holds the console for some seconds: the screen says "Switching..." first.
- UDS has no IP, so the stream is `relstream.c`: window of 48 frames of 1400 bytes,
  selective acknowledgements, retransmission, keepalive. One connection, not four.
  Measured on the consoles: **4 MiB in about 10 s (about 400 KiB/s), no frame resent, identical
  file**; a CIA received over it installed.
- Using UDS drops the Wi-Fi and the FTP server; leaving it pauses/restarts the FTP
  (`ftpserver_resume_later`, retried from `ftpserver_poll_main` for 2 minutes).

**Log and developer tools** (the tests on the consoles are in their own section below)
- `sdmc:/nsa/transfer-log.txt` (every send and receive; the reason of every failure, with
  errno; starts over at 96 KB). Read it from the PC through the FTP server after an error.
- `sdmc:/nsa/nearby.flag`: `1` or `auto` makes the console behave as if it had no Wi-Fi;
  `auto` also skips every question (the sender takes the first console found, the receiver
  accepts and saves) and closes UDS after 150 s; `autosend` skips the questions but keeps the
  Wi-Fi (the sender still has to pick "Search nearby"). The file is read again every 3 s.
  **Delete it when a test is over**: an old copy kept a console without Wi-Fi once.

**Verified on the consoles** (a New 3DS and a 2DS, 2026-10-27/28): files and titles over the
Wi-Fi (title sizes from 4 to 300 MB), resume after a cut (file and title), the retry after a
failure, the cancel message, files over the local wireless, a CIA received over it installed,
changing the mode both ways, the list drag fix.

## Tests on the consoles (`tools/console_tests.py`, `source/fbi/selftest.c`)

Run them after any change to exporting or transfers: the consoles do the steps that are hard to
make happen by hand (a write that fails, a connection that drops, a retry). Both consoles run a
DEBUG build, sit on the main menu, on the same Wi-Fi as the PC:

    tools/console_tests.py --a <sender/exporter> --b <receiver>   (--only <prefix>, --list, --title)

A failed test leaves both transfer logs and screenshots in `test-results/<time>/`. Nothing is
installed; it writes only in `/nsa/tests/`, `/nsa/received/` and the title's CIA in `/nsa/cias/`.

What DEBUG builds have for it (FTP, port 5021; release builds answer 502 or ignore them):
- `SITE TEST UI`: the top view as JSON: depth (1 = main menu), untranslated name (`ui_view.id`),
  kind (prompt, info, list), a prompt's untranslated text, options and their buttons, an info's
  line and bar, a list's count and selection. The tests answer dialogs by name, not by position.
- `SITE TEST EXPORT|SENDTITLE <title ID> [nand]`, `SEND <path>`, `RECEIVE`, `TITLES` (SD titles,
  smallest first, in `/nsa/tests/titles.txt`): open the same screens as the menus.
- `SITE FAULT <point> [skip] [count]` (`CLEAR`, `LIST`): the next hits of `point` after `skip`
  fail `count` times, as the real failure would (an I/O error). Points: `export.read`,
  `export.write`, `title.read` (sending a title), `send.read`, `send.chunk` (a sender connection
  drops), `recv.chunk` (a receiver connection drops), `recv.write` (the receiver's SD write).
  A fired fault writes a line in `transfer-log.txt`.
- `SITE SHA256 <path>`: the file's hash, computed on the console.

The runner also checks every CIA (TMD hashes and each content's SHA-256) and makes stray
connections to the receiver's port 5002 itself. Not covered: the local wireless (the FTP server
is off there) and installing.

## What the owner asked for (the direction)

The look is right, but navigation still feels like FBI: lists, Select for options,
another list for actions. Every screen should move to the Titles pattern:

- Tool icons in the title bar (back, view, filter, search) instead of Select menus.
- A popover for filter and sort, a sheet (list of rows) for actions.
- Rows with the item's picture and secondary info; a grid where pictures matter.
- The top screen shows the selected item's card and its details in cells.
- Modal buttons with centred labels; one tap opens; D-Pad and touch everywhere.

## Next

Every item below also has a GitHub issue (tinaut1986/NSA, #3 to #16); the issues are kept
current, this list is the summary. What only the owner can test (local wireless) is #8.

0. **Still open from console to console**:
   - A **title package over the local wireless** (only files were tried there), cutting and
     resuming over it, long transfers over it (hundreds of MB: does the stream hold for
     minutes?), sending a title **from the New 3DS** (all titles so far came from the 2DS).
   - The *Super Mario Land 2* CIA that the New 3DS received before the certificate fix has the
     wrong root certificate: replace the certificate chain (0xA00 bytes after the header,
     CIA-aligned) with the one in `ciacerts.h`. Only the owner's test file.
   - Folders cannot be sent, only files and titles. Tickets, saves and extra data could use the
     same channel (saves and extra data export without the CIA problem).
   - The root certificate fix means CIAs made before it (in `/nsa/cias/`) may carry a wrong chain.
   - Sound/vibration when a transfer ends, if the owner wants it.
1. ~~Publish v0.2.0 beta~~ done; **v0.3.0 stable published**.
2. **Remote install**, the owner agreed it needs the same treatment. Proposal, to show as
   a mockup first: three big cards (QR code, enter URLs, receive from a computer), the
   receive screen with the console's IP and port large and what has arrived, the last
   request as a history row, progress like the new dialogs. Translate its list items.
   Four cards: "Receive from another 3DS" is the fourth. (The FTP server moved off
   port 5000 so Receive URLs no longer pauses it.)
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
- FTP server on port 5021 (passive 5022-5085), SD card only; `SITE INSTALL` refuses NAND
  titles and never deletes a title to force a downgrade (it would delete the game's saves).
  It was on 5000 until v0.3.0 and paused while Remote install's "Receive URLs" was open;
  it moved instead of Receive URLs because FBI's PC tools (`servefiles.py`) only send to
  5000. Ports: 5000 Receive URLs, 5001 discovery (UDP), 5002 transfers, 5021 FTP.
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

- **Transfers** (details in "Console to console"): four TCP connections over the Wi-Fi (one is
  capped on a 3DS), one reliable stream over the local wireless; an extra connection is closed
  only after the receiver acknowledges; the CIA's certificate chain is the standard one,
  never the console's; a cancel by the receiver is told to the sender (it is not a failure to
  retry); the receiver that chose "Install now" does not get a second question; changing the
  radio mode shows "Switching..." first because it holds the console for seconds; the local
  wireless is chosen by the Wi-Fi state (receiver) or by the user (sender's row, tool, X), never
  taken by the app on its own while there is a Wi-Fi network.
- Every send and receive writes `sdmc:/nsa/transfer-log.txt`; nothing in the transfer code
  should fail without a line there.
- Receive and send show the state of the radio in their own screens; buttons under an info
  card are `info_set_button` (touch or X).

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
- **The two consoles' addresses swap** after the local wireless (DHCP gives them again): say which
  is which by the Receive card's name (`New 3DS XL - ...` / `2DS - ...`), by the files on the
  SD, or by `transfer-log.txt`; the owner's: a New 3DS XL and a 2DS (2026-10-28 they were
  `.133` and `.139`, in either order).
- **Over the local wireless the FTP server is off**: the console cannot be driven or seen until
  it leaves that mode. Plan such tests with `nearby.flag` `auto` (no questions, UDS closes
  itself after 150 s) and read `transfer-log.txt` afterwards. Right after the Wi-Fi returns the
  FTP server may cut a big download: retry a few seconds later.
- `SITE SCREENSHOT` on a busy console may serve an old image: look at the clock in the status
  bar of the image. The console's clock is an hour ahead of the PC's.
- Azahar: an emulated Old 3DS aborts on a thread for core 2 (asked only on a New 3DS now);
  `nand:` archive `0x567890AB` is not supported; opening Receive twice within ~60 s of closing
  it can answer "Address already in use" (TIME_WAIT); UDS exists but takes the FTP server off.
  `tools/fake_peer.py` plays the other console (see its header).
- **`pkill -f <text>` kills the shell that ran it when the command line contains the text**
  (exit 144): use `pkill -x azahar` or a PID.
- The title package needs Luma3DS on the **sending** console (PXI), and the made CIAs need
  signature patches on the **installing** one.
- Checking a big title after it arrives (SHA-256 of every content, "Checking the contents")
  takes a while on an Old 3DS; the sender waits for it ("Waiting for ... to finish").
