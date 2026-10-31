# NSA plan

Start every session here. It holds where things stand, what comes next and why
things are the way they are. Keep it current in the same change that alters it.

## Status (2026-10-31)

`release` holds everything below and is pushed. On 2026-10-31 the owner asked to merge the work
of that session into `release` **before testing it** (and to push directly): what is merged but
not yet tried on a console is listed in "Pending tests on the console" below, which is where to
start. Merged topic branches are kept on the remote and can be deleted.

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
12. **Files: cut/move, pictures and text** (`feat/file-viewers`): "Cut (move)" on files and
    folders (`clipboard_set_cut`; pasting on the same medium renames, `action_paste_contents_in`;
    on another one copies then deletes the source). Tapping a picture (PNG, JPEG, BMP, GIF, TGA,
    camera MPO in 3D) opens `imageview.c` (zoom, drag, L/R through the folder; up to 5
    megapixels / 12 MB, shrunk to a 1024 px texture; the MPO's second picture is found through its
    MP index and shown to the right eye, `screen_get_eye`). Text files (and "Open as text" for any
    file) open `textview.c`: edits with its own keyboard on the bottom screen because the system
    one hides the text; keeps CRLF, reads non-UTF-8 as Latin-1 (saved as UTF-8), up to 256 KB
    (longer: read only). Tapping other files still opens the action sheet.
13. **Audio player** (`feat/audio-player`): MP3, WAV, FLAC and Ogg Vorbis (`audioview.c`).
    dr_mp3/dr_flac/dr_wav (vendored in `source/libs/dr_libs`, public domain or MIT-0) and Tremor
    (`-lvorbisidec -logg`) decode on a thread into four DSP buffers; the folder's next file
    follows, seek bar, volume, +-10 s. Needs `sdmc:/3ds/dspfirm.cdc`.
14. **Downloads, history, idle drawing** (`feat/fast-downloads`; issue #19 has the review
    points): curl on its own thread with a ring of buffers, big files in 512 KB segments over 8
    connections (2 on an Old 3DS) when the server sends `Accept-Ranges`, the system's HTTP
    service only as a fallback; `http:` speed lines in `transfer-log.txt`. Remote install keeps
    a history (`sdmc:/nsa/history.json`, 100 requests). `ui_update` draws only while something
    happens and for 1.5 s after, else ten times a second (`ui_wake()` exists, nobody calls it).
    DEBUG: `SITE CPU [ms]` / `SITE CPU OFF` measure each core.
15. **Interface sounds** (`feat/ui-sounds`, `source/core/sound.c`): tones made at run time (no
    assets) and played on DSP channels 8-11: tick (D-Pad, touch), click (A), back (B) from
    `ui_update`; success tune and error tones from any prompt named "Success" or
    "Failure"/"Error" (`prompt_display_multi_choice`), which covers transfers and installs.
    Settings: Sounds (Off / Alerts / All, default All) and Sound volume (25-100 %). Requests are
    bits set from any thread and played by `sound_update` on the UI loop. The DSP is shared with
    the audio player (`sound_dsp_start`; the player uses channel 0 and its own per-channel
    mix as volume).
16. **Files: previews and types** (`feat/image-thumbnails`, `feat/file-type-icons`,
    `feat/3dsx-launch`, `feat/cia-size-cell`, `feat/file-dates`):
    - `thumbs.c`: a worker thread decodes pictures (up to 8 MB / 3 megapixels) into a 96 px
      square cropped from the centre, and a .3dsx's SMDH (found through its header, usually at the
      END of the file) into its 48 px icon, name and author; the UI thread makes the texture;
      48 slots, least recently used first, never one drawn in the last 1.5 s.
    - `files.c` `files_type_of`: each extension has a type (text, code, data, picture, sound,
      video, compressed, program, ROM, save, binary, PDF) with its icon and name; usual folders
      (`3ds`, `roms`, `DCIM`, `luma`, `Nintendo 3DS`...) have their own icon (`files_folder_icon`).
    - A CIA's card shows file size / installed size in one cell and a **Modified** cell (also
      tickets and other files, SD only): libctru's `stat` leaves `st_mtime` at 0 but
      `archive_getmtime("sdmc:/path")` reads the date the file system keeps (last change only;
      the 3DS gives no creation date).
    - "Run program" on a .3dsx (SD): sets it as the target of Luma3DS's 3DSX loader (`hb:ldr`,
      commands 2 and 3, as hbmenu does) and exits into the Homebrew Launcher placeholder title
      (default `000400000D921E00`; `sdmc:/nsa/hbl_title.txt` can name another), or into itself when
      NSA runs as a 3DSX (`launch3dsx.c`). Says what is missing when the loader or the title is
      not there. **Not tried on a console yet.**


## Pending tests on the console (2026-10-31)

Merged into `release` on the owner's word, **not yet tried on a console** unless said otherwise
(built, and the screens driven in Azahar, which shows layout but not sound, 3D, hb:ldr or real
dates). The owner will go through them one by one; tick each only when they say it works.

**Downloads** (issue #19 has the review points)
- [ ] Big download (>= 4 MB) from a server with `Accept-Ranges`: New 3DS (8 connections) and
      2DS (2 connections): speed, cancel half way, dropped connection (segment retries), hash.
- [ ] Small download and a server without ranges (one connection, ring of buffers).
- [ ] HTTPS curl cannot connect to: fallback to the system's HTTP service.
- [ ] `transfer-log.txt` has the `http:` speed lines.
- [ ] Idle drawing: nothing that moves by itself looks choppy (toasts, progress, cross-fades,
      Receive screens); `ui_wake()` exists but nobody calls it yet.
- [ ] `SITE CPU` (DEBUG) answers per core without disturbing the FTP server.

**NSA icon and banner** (`tools/make_branding.py` draws `meta/icon_3ds.png` and `banner_3ds.png`)
- [ ] After installing the CIA the HOME menu shows the new icon and the banner (the CIA's
      version is still 0.3.1 for every dev build; if HOME keeps the old art, bump the version).
- [ ] Banner on the console (3D rotation, tiny text readable). The boot logo animation no longer
      is FBI's (`LOGO := plain`: the CIA carries the generic logo of buildtools/makerom); look at what
      shows on launch. A logo of NSA's own is left open in issue #15. The banner sound is still FBI's.

**Advanced in a soft red** (`widget_button_danger`, `prompt_display_warning`)
- [ ] On the console: the Advanced button, its top card and the warning prompt in red, day and
      night, in several accents (seen only in Azahar, night mode, blue accent).

**Pending titles and System save data on the Titles pattern**
- [ ] Pending titles: rows (kind, ID, SD/NAND tag, version), SD/NAND filter remembered, search, sheet
      with both deletes in red, long press, X refreshes. Only seen empty in Azahar (no pending titles).
- [ ] System save data: rows with the ID, search, sheet (browse, delete in red), the delete
      confirmation in the red warning. Azahar crashes on this screen (also on the previous
      `release`, before this change): not seen even once; readable names of the modules were not
      added (no source found for the IDs).

**Pixel theme** (Settings -> Theme, `tools/make_pixelfont.py`)
- [ ] On a console: the pixel font (Pixelify Sans 10/12/16 px, `romfs/pixelfont.png`) is readable on
      both screens, Spanish accents included (the acute i is built from a dotless i), the button
      glyphs come out of the system font, texts fit in tiles, rows, prompts and the top card; day and
      night, every accent (the new Yellow gets dark text). Seen only in Azahar.
- [ ] Cards is unchanged (same positions as before the text layer was split in two).
- Bundled font: Pixelify Sans, SIL OFL 1.1 (`tools/fonts/OFL-PixelifySans.txt`). HOME theme next.

**HOME theme** (Settings -> Theme)
- [ ] On a console: the main menu as four big tiles per row with the name under each (two lines
      fit), D-Pad moves through the grid, a tap on the name opens too; dialogs rise as a sheet from
      the bottom (red for Advanced and deletes), day and night, every accent. Seen only in Azahar.
- [ ] Not done yet from the mockup: icon grids (Titles, Tickets, Ext Save Data) **in pages with dots**
      (they still scroll), and the sheet's drag-to-dismiss (it has the handle only).

**Remote use of the FTP server** (`sdmc:/nsa/remote.flag`; the server itself is on by default since v0.4.1)
- [ ] In a production build the FTP server serves files but `SITE ...` answers 502; with the file on
      the SD card `SITE SCREENSHOT/PRESS/TAP/DRAG/INSTALL` work; delete it and they stop (checked in
      Azahar only). DEBUG builds always allow them.

**Background music** (Settings -> Background music: Off / Lift / Calm / Shop, own volume; `source/core/bgmusic.c`)
- [ ] On both consoles: it starts a few seconds after turning it on (DEBUG builds write how long the loop
      took to `sdmc:/nsa/music-log.txt`; Azahar: 6.4 s), the join of the loop cannot be heard, it is
      soft enough under the interface sounds, it goes quiet while the audio player plays and comes back,
      nothing stutters in a heavy screen (Titles loading, a transfer) on the Old 3DS/2DS. The chords and timbre are the
      Python sketch's; the melody notes are other ones (another random generator). Shop (a bossa: marimba, electric
      piano, bass, shaker; 37 s) was written from `shopmusic.py`; DEBUG builds also write the loop to
      `sdmc:/nsa/music-style-N.wav` to listen to it on a computer.

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
- [ ] Open `.txt`/`.json`/...; "Open as text" on a file with no known extension; binary files refuse.
- [ ] Edit with the custom keyboard: letters, shift/caps, symbols, accents, backspace/arrows
      repeat, D-Pad cursor, X/Y/A shortcuts, START saves, close with changes asks.
- [ ] CRLF kept; a Latin-1 file is read and saved as UTF-8; a file over 256 KB is read only.
- [ ] Saving on the NAND / save-data archives (commit).

**Files: audio**
- [ ] MP3, WAV, FLAC, Ogg Vorbis really heard (Azahar only showed the position moving).
- [ ] Old 3DS decodes MP3/FLAC in real time; CPU use while the UI draws.
- [ ] Seek (+-10 s, bar) in a long MP3 (slow without a seek table); next track on its own.
- [ ] No `dspfirm.cdc`: the message shows instead of crashing.

**Interface sounds** (`source/core/sound.c`, Settings: Sounds Off/Alerts/All, Sound volume)
- [ ] Heard on both consoles: tick (D-Pad, touch), click (A), back (B), success tune (transfer
      or install ended), error tones (any Failure/Error dialog). Azahar only proved it runs.
- [ ] "Alerts" plays only the last two; "Off" is silent; volume steps 25/50/75/100 give a sample.
- [ ] No `dspfirm.cdc`: no sound and no crash. Sounds and the audio player share the DSP: play a
      song, then press buttons (the song's volume is its own, per channel).
- [ ] Are the tones pleasant and not tiring? Tune them in `sound_prepare` (tone tables).
- [ ] Possible next steps: sound on the held repeat of a list, on text-editor keys, a different
      tone per kind of action (delete, install).

**Files: previews, types, dates, 3DSX** (seen working on the New 3DS on 2026-10-31 and approved by
the owner: folder icons, the CIA size cell and the Modified cell)
- [ ] Picture previews in the rows and on the card: speed while scrolling a big folder, nothing
      freezes, Old 3DS memory.
- [ ] A .3dsx's preview, name and author on both consoles; a .3dsx without SMDH keeps the generic icon.
- [ ] **Run program** on a real .3dsx: the owner's NSA is a CIA and they have a Homebrew Launcher CIA.
      Possible failures: its title ID is not `000400000D921E00` (write it in `sdmc:/nsa/hbl_title.txt`),
      or NSA's exheader does not allow `svcConnectToPort` ("hb:ldr", error shown). Also from a 3DSX build.
- [ ] File type names and icons on files of each kind; "Open as text" is now offered only on
      files with no known extension (README...), not on programs, ROMs, pictures, archives.

**Next, item 3 (Pending titles, System save data, Dump NAND on the Titles pattern)**
- [ ] The owner thinks they tried it once but does not remember. The code on `release` still
      has FBI's action lists for those three (no sheet, no tool icons), so check on the console
      what state they are really in before working on them.

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
   - ~~Sound when a transfer ends~~ done with the interface sounds (see Pending tests); the 3DS has no vibration.
1. ~~Publish v0.2.0 beta~~ done; **v0.3.0 stable published**.
2. ~~**Remote install**~~ done (four cards, receive screen, history; see Status 14).
3. **Pending titles, System save data, Dump NAND** on the same pattern (Dump NAND as
   its own screen: space needed against free on the SD card, progress with ETA, where
   the file ends up). Pending titles and System save data still have FBI's action lists
   and English item names ("Delete Pending Title"...); System save data would show
   readable names of the known modules.
3b. **Send saves and extra data** (design agreed with the owner 2026-10-31, not started). A save is
   whole: it cannot be merged, only replaced or kept apart. Today a 3DS title only has "Save data"
   (browse the files) and "Browse extra data"; export/import exists for DS cartridge saves only.
   - **Local first**, on the Titles sheet, flat rows (no nested menus): *Export save*, *Import save*,
     *Send save to another 3DS*, and the same three for *extra data*. Export copies the archive's
     files to `sdmc:/nsa/saves/<TITLEID>/<date>/` (extra data under `ext/`, SpotPass under `boss/`)
     with a small manifest (title ID, version, name, date, kind); it is a plain folder the owner can
     browse and copy to a PC. Import picks one of those folders (newest first).
   - **Import replaces the whole archive** (delete its contents, copy the tree, commit), behind the
     red warning, and **first copies the current save to `before-import-<date>/`** next to the
     exports. It is only offered when the archive can be opened (title installed and started once;
     for extra data, the archive exists).
   - **Sending**: folders cannot be sent today, so a save/ext data goes as a package like the title
     package: a header (kind 2 save / 3 extra data, title ID, version, name, file count) and the files
     as read; protocol version up. The receiver unpacks into a folder under `sdmc:/nsa/saves/` first,
     whatever the choice, and "Import now" is the same local import over that folder.
   - **Receiving**, in the same Receive screen: *Import now* (red; only if the title or archive is
     there) / *Save in a folder* / *Reject*; with no title or archive, only *Save in a folder* / *Reject*
     and the text says why. Mockup: `saves-mock.png` (owner's chat, 2026-10-31).
   - When the sender's game version differs from the installed one (same title ID), the receiver shows
     both versions and asks whether to import anyway (the owner's choice, never blocked or forced).
   - Open: ext data to the right game depends on the exact ext data link (4).
4. **Exact ext data link**: read the ExtData ID from each game's exheader (NCCH
   `ExHeader`, ARM11 local system capabilities, offset 0x230 by 3dbrew; check with the
   Program ID at 0x200) instead of the ID heuristics. The file path type for the exheader
   on `ARCHIVE_SAVEDATA_AND_CONTENT` is not confirmed: probe it on the console and write
   the result to a file the FTP server can fetch, since Azahar cannot answer it.
5. **Themes "Pixel" and "HOME" (both built, see Pending tests; HOME lacks paged grids)** (mockups were shown and liked): HOME = big icon tiles
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

10. **Ideas the owner liked or that came up on 2026-10-31** (none started):
    - Hex viewer for files that are not text, and a file's SHA-256 (the FTP DEBUG command exists).
    - Browse and extract `.zip`; previews of `.smdh`, `.bcstm`/`.bcwav` audio, a `.cia`'s icon already exists.
    - More sound: the held repeat of a list, text-editor keys, a tone per kind of action.
    - ~~Hide "Open as text" on programs, ROMs, archives~~ done; open `.3dsx` by tap instead of through the sheet.
    - The NSA icon and banner (item 6) show now in every 3DSX preview: still FBI's.
    - A mini player that keeps the song playing while browsing (the audio view stops it on B).
    - Date of creation: not available (the 3DS only gives the last change); NSA could record its own.

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

- **Sounds** (`sound.c`): synthesized at the first use, mono 22 kHz, on DSP channels 8-11 (the
  audio player owns channel 0); one sound per frame, an alert wins over clicks; `sound_play` is
  safe from any thread (an atomic bit mask), playing happens in `sound_update` on the UI loop.
  Alerts are tied to the dialog titles "Success" / "Failure" / "Error": a new success or failure
  dialog gets its sound by using those names. Default mode All, volume 60 %.
- **DSP** is started once (`sound_dsp_start`) and ended by `sound_exit`; never call `ndspInit` or
  `ndspExit` elsewhere. Without `dspfirm.cdc` everything audio is silent and says so.
- **Viewers** are opened from Files by extension (`viewers_kind_of`: picture, text, audio);
  other files keep the action sheet, which also has "View picture", "Play", "Open text", "Open as
  text" (any file; binaries refuse) and, for a .3dsx, "Run program". Text and pictures are read
  through `viewers_read_file` (limits in each viewer).
- **Previews** never block the UI: decode on `thumbs.c`'s thread, textures on the UI thread,
  slots reused only when not drawn for 1.5 s (a texture freed while a frame still uses it is a
  crash). Pictures over 8 MB / 3 megapixels keep the icon.
- **File dates** come from `archive_getmtime` (SD only); `stat` is useless on the 3DS.
- **Starting a 3DSX** goes through Luma3DS's `hb:ldr` (as devkitPro's hbmenu does: SetTarget, SetArgv,
  then the placeholder title starts and loads it) and the libctru chainloader
  (`aptSetChainloader` / `aptSetChainloaderToSelf`) once NSA exits through the main loop
  (`launch3dsx_poll`); the loader forgets the target after one load.
- **Downloads**: one connection carries only what a 3DS keeps in flight (~50 KB/s at 130 ms
  from far away), hence segments over several connections; an Old 3DS starves with more than 2.
- **Drawing** only while something happens (`ui_should_draw`): CPU left to downloads and
  installs; animations that run with no input need `ui_wake()`.
- Vendored libraries: `source/libs/stb_image`, `source/libs/dr_libs` (dr_mp3, dr_flac, dr_wav),
  `source/libs/quirc`. Tremor and libogg come from devkitPro's portlibs.
- New icons: add the Tabler name to `tools/icons.txt` and run `python3 tools/make_icons.py` (needs
  network for Tabler's CSS; it can take two minutes: run it again if it times out). New ones are
  appended, so existing icon IDs do not move.

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
- **Azahar and the 2026-10-31 features**: it has no `hb:ldr` (Run program answers `D88007FA`), no
  file dates (a fixed 2000 date), plays audio and sounds silently (the position moves), has no 3DS
  camera MPO sample, and its SD (`~/.var/app/org.azahar_emu.Azahar/data/azahar-emu/sdmc`) holds
  test folders from this session (`/viewtest`, `/viewtest/types`, `/viewtest/snd`).
- Opening a software keyboard (`kbd_display`) in Azahar blocks its window; a test script that
  presses A on "New folder" must not.
- The SD card's root of the owner's console held CTR 3D debug dumps (`atlas_*`, `vramdump_*`,
  `cachemeta_*`, `tridump_*`, `ctr*.log`): deleted on 2026-10-31 at the owner's request. CTR 3D is
  their Crash Team Racing port; `/3ds/ctr_native` is its folder (left alone).
- The console that answered on the network on 2026-10-31 was the New 3DS XL at `192.168.1.133`
  (the 2DS was not on); a subnet scan of port 5021 (`/dev/tcp`) finds the open ones.
- `make` does not rebuild when only the version string changes: the screens show the commit of the
  last full build, not of `HEAD`.
