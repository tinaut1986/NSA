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
| `release` | The working line. Holds everything below, but "Advanced". |
| `advanced` | **The risky tools**: Dump NAND as its own screen (space, progress, SHA-256 and a `.sha256`), Pending titles and System save data on the Titles pattern, Advanced's red look. Cut from `release` on 2026-10-31; the owner wants what is about the console's own data merged there first. Keep `release` merged into it (`git merge release`). |
| `master` | Stable. Moves only by merging `release`. |
| tag `v0.6.2` | **Stable**, published 2026-11-02 (notes: `docs/release-notes/v0.6.2.md`): a steady speed on screen for console to console transfers. |
| tag `v0.6.1` | **Stable**, published 2026-11-02 (notes: `docs/release-notes/v0.6.1.md`): v0.6.0 plus Remote Install's Save (issue #22). |
| tag `v0.6.0` | **Beta**, published 2026-11-02 (notes: `docs/release-notes/v0.6.0.md`): download, FTP and web speed (issue #21), titles cache, System details, row chevrons. Promoted with v0.6.1. |
| tag `v0.4.1` | **Stable**, published 2026-10-31 (notes: `docs/release-notes/v0.4.1.md`). `v0.4.0` (stable the same day) and `v0.3.0` came before; `v0.1.0` and `v0.2.0` were betas. |

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
13b. **Saves** (`feat/save-export-import`, `feat/send-saves`, `feat/checkpoint-import`, `feat/import-list`): export, import and
    **Since 2026-11-01** (`feat/save-manager`) the Titles sheet has one row per kind of data (*Save data*, *Extra data*,
    *SpotPass data*) opening a list: *Back up now* (export), *Send to another 3DS*, *Browse* (save and extra data), *Delete
    extra data*, then every backup (NSA's and Checkpoint's) newest first; a backup's sheet restores it (the red question) or
    deletes it (`action_save_delete_backup`: only that kind's folder of an NSA export, the whole folder with its manifest once
    empty; a Checkpoint backup is deleted whole). The list reloads by itself after an export, import or deletion
    (`action_save_generation`). The web page has the same: a sheet per kind with the download and the backups (restore / delete,
    `POST /api/save/backup/delete`). Rows drawn with `COLOR_TEXT` as a raw colour were invisible in night mode (import list, the
    transfer's folder picker): fixed. A backup's sheet also sends that backup to another 3DS (`action_send_save_backup`: the same package, read from the
    backup's folder on the SD card through `pack_job.root`); a cancelled or failed export deletes its folder, so it is not listed.
    send the save data and extra data of 3DS titles; see item 3b under "Next".
13c. **Web server** (`feat/web-server`, `source/fbi/webserver.c`, `romfs/web/index.html`; phase 1 of "Next" item 10): a page
    in the computer's browser to manage the console. See that item for the design, the API and what is left.
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

17. **Themes Pixel and HOME** (`feat/theme-pixel`, `feat/theme-home`): Pixel draws every text from Pixelify Sans
    (SIL OFL, `tools/fonts/`) baked at 10, 12 and 16 px by `tools/make_pixelfont.py` into `romfs/pixelfont.png`
    and `source/core/pixelfont.c` (the button glyphs fall back to the system font; the acute i is built from a
    dotless i), square corners, block bars, a Yellow accent with dark text on light accents. HOME draws the main menu as
    four big tiles per row and dialogs as a sheet from the bottom (`theme_def.sheetPrompts`, `menuLayout`).
    Icon grids in pages with dots (HOME) are **not done**.
18. **NSA icon and banner** (`tools/make_branding.py`): drawn in code (48x48 and a 256x128 PNG banner). The boot logo
    is now the generic one of `buildtools` and the banner jingle is NSA's own (`tools/make_jingle.py`, 2.5 s, synthesised; issue #15). An old title version in HOME
    keeps its icon until the CIA's version number goes up (see Decisions).
19. **FTP and remote use**: the server is on by default; outside DEBUG builds the `SITE` commands (screenshot, press, tap,
    drag, install) work only while `sdmc:/nsa/remote.flag` exists (`ftp_remote_allowed` in `ftpserver.c`).
20. **Background music** (`source/core/bgmusic.c`): Off / Lift / Calm / Shop, rendered at run time into a seamless loop
    (circular synthesis) on a low-priority thread, played from memory on DSP channels 14 and 15; a cache per style on the SD
    card (`music-cache-N.pcm`) plays at once next time while a new song is made, and the two crossfade over 3.5 s; a note blinks
    in the status bar and Settings shows progress and an equalizer. The seed comes from the clock: another melody each time.
    Measured: 4 to 5 s per loop on the New 3DS XL, 15 s on the 2DS (`music-log.txt` in DEBUG builds).
21. **Settings**: a volume slider under Sounds and Background music (one box each), draggable scrollbars in lists, Settings,
    action sheets and popovers, "Open as text" only on files with no known extension.
22. **README with pictures** taken on a real console (`docs/images/`), v0.4.0 and v0.4.1 published.

## Pending tests on the console (2026-10-31)

**Languages** (2026-11-04, `feat/languages`; DEBUG 0.6.5 build installed on the New 3DS XL and driven over FTP: the list, Japanese, Russian and German looked right in the Cards theme with the console's own font; the console was put back on System afterwards. The rest is for the owner's hands)
- [ ] Settings -> Language opens a list (System, English, Español, Català, Français, Deutsch, Italiano, Português, Русский, 日本語, 简体中文, 繁體中文); the highlighted one shows its name and a few words in it on the top screen; A or a tap picks it, everything changes at once and it is still set after a restart; B goes back without changing.
- [ ] The Pixel theme in German (and the other languages with umlauts): at 10 px the pixel font draws the diaeresis faint (seen in Azahar). Russian has no yo: the Russian table writes it as e, because Azahar's font has no glyph for it.
- [ ] Every screen in each language: texts fit their rows and buttons (German and Russian run longest), no tofu boxes. Cyrillic and kanji come from the console's own font: check Russian and Japanese on both consoles, and on a console set to another region if there is one.
- [ ] Chinese: **Traditional** (繁體中文) draws completely with the console's font on the New 3DS XL (an EUR console), because its table only uses characters of JIS X 0208 (checked with `shift_jis`; a few Taiwan forms are written as their Japanese variants, 内 録 緑 ...). **Simplified** (简体中文) needs its own font: about a quarter of its characters (设 软 单 远 请 ...) are not in that console's Japanese character set, so while it is the language `screen.c` draws all of its Chinese from `romfs/hanfont.png` (Noto Sans CJK SC, 528 characters at one size, scaled like the system font; seen working on the New 3DS XL, Pixel theme not looked at). In any other language that font only fills in what the console's font lacks (the name 简体中文 in the language list). Check how it reads at the small sizes and in the Pixel theme.
- [ ] Japanese lines break between characters (dialogs, the top screen's descriptions, the update changelog) and never leave 、 。 」 at the start of a line.
- [ ] System follows the console's language (Settings of the console in French, German, Italian, Portuguese, Russian or Japanese); a console in Korean or Dutch shows English (Catalan is never picked by System: the 3DS has no Catalan).
- [ ] Advanced -> Dump NAND (only on `advanced`): its texts in every language (21 strings: sizes, time left, "Fits, with ... left over", "Saved as", the hints); built, not looked at on a console.
- [ ] The web page's Language drop-down lists all twelve and changes the console; the page itself is still English or Spanish.

**Badges of the backups** (2026-11-03; seen in Azahar and on the web page with fake NSA and Checkpoint backups)
- [ ] A title's Save data / Extra data list and the import list show NSA's backups with NSA's badge (the blue window with an N) and only the date, and Checkpoint's with its flag badge (pale flag on navy) and only the name, no "[NSA]" / "[Checkpoint]"; readable selected and not, in day and night themes.

**Red confirmations and going up a folder** (2026-11-03; built, not run)
- [ ] Every delete / erase confirmation (Files: delete, delete contents, folder, CIAs, tickets; Titles: delete, with ticket, erase save, secure value; Tickets; Ext save data; Pending titles) shows the red prompt with the warning icon.
- [ ] Files: B in a folder selects that folder in the parent at once, also in a big folder full of CIAs; a D-Pad or A pressed meanwhile does nothing until it is selected.

**System's details** (issue #23; `source/fbi/systeminfo.c`; only the layout was checked, in Azahar, whose data is fake)
- [ ] Main menu -> System opens a list (Console, Network, Storage, Battery); each row shows its page on the top screen; B goes back.
- [ ] Console: serial number (read from `SecureInfo_A` on the CTR NAND) matches the sticker; device ID, country, system and kernel versions, MCU firmware (shown as major - 0x10 . minor: check it is plausible), Luma3DS version and commit, "Running as" CIA / 3DSX.
- [ ] Console on the New 3DS XL: the screen types (IPS / TN from `GSPLCD_GetVendors`: vendor 1 = IPS, 0xC = TN is a guess to confirm).
- [ ] Network: MAC, gateway, DNS, DHCP lease and proxy have real values (all empty in Azahar).
- [ ] Storage: the SD card's maker, product name and date from its CID (the byte order is guessed: both are tried), speed, writable, cluster size.
- [ ] Battery: percent with a decimal, voltage and temperature from MCU registers 0x0A-0x0D (plausible: about 3.6-4.2 V, room temperature); charging / on the charger / on battery; after 2 minutes on the page the rate and the time left appear and the graph fills; plugging or unplugging restarts the measure.

**Web server** (item 10; `source/fbi/webserver.c`; tested only in Azahar with `tools/web_tests.py` and the page in a browser)
- [ ] Screen tab on a real console: frames per second over the Wi-Fi on the New 3DS XL and the 2DS (and what it costs the console: UI, music, a transfer meanwhile); if too slow, try a smaller frame.
- [ ] The buttons, keys, taps and drags from the page drive the console (a tap on a row, scrolling a list, A/B on a prompt); a stuck press does not stay held.
- [ ] Import a backup from the page (an NSA export and a Checkpoint backup): the red question appears on the console, and accepting replaces the data (with the backup first and the rollback if it fails).
- [ ] Launch and delete (with and without ticket) a title from the page: the console asks first; a second request while the first question is open says the console is busy.
- [ ] Install from URLs from the page (and from the history): the console asks, downloads and installs; the URL is in Remote Install's history on the console.
- [ ] The page wears the console's theme: change theme (Cards, Pixel, HOME), accent and night mode on the console and see the page follow within seconds.
- [ ] Titles tab on a real console: the list, names and icons arrive (a console has many more titles than Azahar: how long, and does the console stay usable), filters.
- [ ] Download a save, extra data and SpotPass data of a game as a package; send that package back from the page: the console unpacks it and asks about the import (red) there.
- [ ] Settings -> Web server: On/Off; the row's top screen shows `http://IP:8080  PIN nnnnnn`; the PIN changes each time it starts; it starts at launch when it was left on.
- [ ] From the PC: open the address, the PIN screen, wrong PIN (and the lock after 10), browse the SD card, download, upload (a big file, several at once), new folder, rename, delete.
- [ ] Files tab: copy a big folder (the page waits; the console stays usable?), move to another folder, edit and save a text file (UTF-8, CRLF kept as typed).
- [ ] Tickets and Extra data tabs on a real console: counts, names and icons arrive (hundreds of tickets: how long), the text filter.
- [ ] Install a `.cia` from the page (an SD title; NSA itself restarts); the toast on the bottom screen shows it.
- [ ] Speed of a big download and upload on the New 3DS XL and the 2DS (the FTP server's for comparison); the console stays usable meanwhile (UI, music).
- [ ] The local wireless (Send / Receive without a router) stops the web server and brings it back after; nothing else breaks while it serves.
- [ ] The page in a phone's browser and in Firefox / Chrome (dark and light).
- [ ] Download a save / extra data / SpotPass package from the page on the 2DS: the console shows "Packing for the web page" with its bar, B there cancels and the page says so, the page's card shows packing then downloading, and NSA closes normally afterwards.
- [ ] Cancel *Back up now* with B: no new backup in the list; send an old backup to the other console and import it there.
- [ ] Titles sheet -> Save data / Extra data / SpotPass data (`feat/save-manager`): the list shows the backups (readable in night mode), *Back up now* adds one at the top, restoring asks in red, deleting asks and removes it (a folder holding save and extra data keeps the other); the same from the page.
- [ ] The redesigned page (`feat/web-ux`): Settings from the page change the console at once (theme, accent, night, language, sounds, music and volumes) and stay after a restart; dragging rows onto folders and breadcrumbs moves them; dropping a folder from the PC uploads the tree; *Install a CIA from this PC*; how the titles list feels with many titles on a real console.

**Original path** (`feat/keep-original-path`, issue #24; `tools/console_tests.py --only origin` (8 tests: new, overwrite, skip, cancel, cut and resumed, a single file, unsafe paths not offered) pass on the New 3DS XL <-> 2DS over the Wi-Fi; the owner has not tried it by hand)
- [ ] Files -> a folder's ⋯ -> *Send to another 3DS*; on the receiver *Original path*: with nothing there the folder appears at the same path; with files there it asks "N of M files already exist" and Overwrite / Skip existing / Cancel do what they say; try it with PKSM's folder.
- [ ] The same with a single file, and a CIA or a save package (they must not offer *Original path*).

**Sending folders** (`feat/send-folders`; code in `transferfolder.c` and `foldermanifest.c`; `tests/foldermanifest_test.c` on the host and `tools/console_tests.py --only send_folder` (10 tests: clean, name taken, cuts on both sides, write and read failures, 250 small files, no files, unsafe paths, another destination) pass on the New 3DS XL <-> 2DS over the Wi-Fi; the owner has not tried it by hand)
- [ ] Files -> a folder's ⋯ (or the open folder's sheet) -> *Send to another 3DS*: a small folder (a few files, subfolders, an empty file, an empty subfolder) arrives in `sdmc:/nsa/received/<name>/` with the same tree; the receiver is asked *Save* / *Reject* and picks the destination folder.
- [ ] A folder with several CIAs (hundreds of MB): speed like a single file, nothing duplicated on the sender's card; cancel with B on each side; a name that already exists arrives as `<name> (2)`.
- [ ] A cut transfer of a folder goes on from where it stopped (the `<name>.part` folder and `.part.pos` stay on the receiver); an empty folder and one with only empty subfolders are refused by the sender ("no files"); a folder with thousands of small files.
- [ ] Wi-Fi and the local wireless (no router); an older NSA (protocol 5) on the other console must refuse cleanly.

**Saves, extra data and SpotPass data** (item 3b; code in `source/fbi/action/savetransfer.c`; none of it tried on a console)
- [ ] Export a save (Titles sheet) to `sdmc:/nsa/saves/<TITLEID>/<date>/save/` with `manifest.txt` (`source=NSA`): with subfolders, an empty save, a big one; cancel with B midway.
- [ ] Same for extra data (`ext/`; fixed file layout) and for SpotPass data (`boss/`; the BOSS archive may refuse to create files or be emptied).
- [ ] Import: the scrolling list shows NSA's exports tagged `[NSA]` newest first (no cap), the red warning, the version warning when the game's version differs, the backup `<date>-before-import` made first, the data replaced, the game starts with it.
- [ ] Import rollback: make an import fail (cancel with B midway, a full save) and check the old data is back, the `-before-import` folder is gone and the message says it could not be imported; if the rollback itself fails the folder stays.
- [ ] Import from Checkpoint's backups (`sdmc:/3ds/Checkpoint/saves|extdata/0x<ID> <game>/<backup>/`) tagged `[Checkpoint]`: the format was checked against Checkpoint's source, not a real backup; extra data under `extdata/` with the same ID rule.
- [ ] Send a save, extra data and SpotPass data to the other console (New 3DS XL <-> 2DS): the package `sdmc:/nsa/saves/send/<TITLEID>-<kind>-<name>.nsasave` is made, goes through, and the receiver offers *Import now* / *Save in a folder* / *Reject*.
- [ ] Receiver without the title (or its extra data / SpotPass archive): only *Save in a folder* / *Reject*, with the reason; the folder appears in the import list as `[NSA]`.
- [ ] A cut transfer of a package goes on where it stopped; the package is deleted after unpacking; the sender's package is overwritten by the next one.
- [ ] Spanish texts of all of this (rows, list title, prompts, notices) and their fit on the real console screen.

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

**One transfer at a time** (`feat/one-transfer`; on the New 3DS XL `tools/turn_tests.py` passes and the speed is above)
- [ ] Console to console with the turn (New 3DS XL <-> 2DS, `tools/console_tests.py`): sending and receiving still work; a send
      while the receiver downloads something is refused with "Another transfer is in progress"; the retry question frees it.
- [ ] Settings -> FTP server (and Web server) off and on again with a transfer under way: nothing freezes, the turn is free.
- [ ] The page: the top bar shows a transfer (name, %, KB/s) of FTP or a download; an upload while one runs says so and sends
      nothing; the live screens say they are paused; browsing Titles and Files keeps working.
- [ ] A QR / URL install while an FTP upload runs: the console says another transfer is in progress; after it ends it works.
- [ ] The 2DS: the same speed tests (`speed_tests.py --wait-mode poll|nap`) to see whether the short poll helps there too.

**Downloads from Dropbox and in long pieces** (Decisions, "Saving comes in long pieces"; the New 3DS XL passed Save from
Dropbox, catbox and the LAN, byte for byte, and an install by URL of an 8 MB CIA over 8 connections)
- [ ] A QR install from a Dropbox link (a game not installed yet): it says "one connection" after the first segments and
      installs at ~1 MB/s.
- [ ] The whole Dragon Quest VIII with Save (3.4 GB): ends, and the file is the right size.
- [x] The 2DS: Save from Dropbox. In long pieces it went at 180-255 KB/s: each 256 KB write far into the 3.4 GB file took
      3.2-3.9 s (the trace's disk time), with a handle for each connection too; in order, into the same file set to its
      size, 444-457 KB/s (3dsDL from Dropbox ends on one connection, so it writes in order too).
- [x] The New 3DS XL too (2026-11-04, Google Drive, 534 MB): in long pieces the writes took 54-113 ms for 6 minutes,
      then 3.7-4.4 s each (60 KB/s) to the end, the SD card's cache full with several places written at once. So every
      model saves **in order**, as an install: segments over the connections that pay off, written in order (the same
      engine; long pieces only with `SITE SAVEORDER ANY`). Segments of 1 MB, not 512 KB: a request (and its round trip)
      every 512 KB started Google Drive at ~500 KB/s; at 1 MB it went at 963 KB/s from the start (3dsDL: ~1 MB/s). Drive
      whole on the New 3DS XL: identical bytes (SHA-256), saved as "EUR - Paper Mario.cia" (Content-Disposition), FTP
      answering throughout. The 2DS: 222-278 KB/s (3dsDL 127 KB/s there).
- [x] The ramp (ramp.c): at most 4 (eight saturated the New 3DS's network: its FTP could not send 6 bytes), 10 s
      measures from a sample a second, one more kept when its mean rose by more than twice the measures' error (and 5%),
      one fewer when the speed falls under half of where it settled. The in-order engine (installs too) uses it, from 2.
- [x] Links: Google Drive's share, open and uc links become `drive.usercontent.google.com/download?id=ID&export=download
      &confirm=t` (its page, or a virus scan warning for big files, otherwise); a server that sends a web page instead of
      the file fails with "The link leads to a web page, not to the file"; 429 hands an install over to one connection.
      The user agent is `NSA/x.y.z (Nintendo 3DS)`: FBI's "Mozilla/5.0 (Nintendo 3DS; Mobile; rv:10.0) Gecko/20100101"
      got 500 and 403 from Google Drive.
- [ ] NSA went back to the HOME Menu by itself once (no Luma dump), right after a SITE SHA256 of the 534 MB file had
      read it whole: find out what closed it.

**FTP against the original ftpd** (the owner, 2026-11-04, FileZilla on the New 3DS XL: ftpd ~850 KB/s up, ~1200 KB/s
down; NSA ~750 / ~800). ftpd sets 32 KB SO_RCVBUF/SO_SNDBUF on its passive socket on the 3DS (`ftpSession.cpp`, not on
the accepted one), which its data connections inherit; NSA left the system's. `SITE PASVBUF <KB>` (DEBUG), 64 MB with
`speed_tests.py` (byte for byte), New 3DS XL: the system's 802-836 up / 805-815 down KB/s; 16 KB 817 / 986; 24 KB 776 /
1137; **32 KB 974-1116 / 1148-1208**; 40 KB 953 / 1168. Now 32 KB by default. The owner's own copies with curl, two
minutes each: Dragon Quest VIII from /cias to the PC **1147 KB/s** (ftpd ~1200); Metal Gear Solid 3D (3.2 GB, from the
PC's O: drive) to /cias **641 KB/s** (ftpd ~850).
- [x] Right after that upload was cut, NSA seemed gone (FTP and web refused, pings answered): the New 3DS XL had lost
      the Wi-Fi and come back as 192.168.1.139, and a phone had .132. NSA was fine.
- [x] The slow real upload (641 KB/s): not the O: drive (88 MB/s), not the .cia name (curl to .bin and .cia alike,
      779-841 KB/s), but the Wi-Fi of that moment (it dropped minutes later).
- [x] Same moment, same console (New 3DS XL, 64 MB, two rounds each, one after the other): ftpd 794-849 up / 1090-1132
      down; NSA (32 KB) 782-845 / 1166-1205; the owner's fork as it was 681-728 / 749-772, fixed (tinaut1986/ftpd#1)
      921-977 / 1112-1142.
- [ ] **The 2DS** (32 MB, one after the other): the fixed fork 733-747 up / 560-597 down, NSA 601-685 / 454-484. Not the
      wait (`SITE POLL`), the size of each send (`SITE SENDCHUNK`, 16-64 KB), the FTP thread's priority (`SITE FTPPRIO`
      0x31 or 0x2F), the web server (off) or the CPU (core 0 at 15%). The trace: ~90% of each second inside send(), ~33 ms
      a call for ~19.5 KB. Sending first and polling only when nothing fits (as ftpd) brought 454-463 -> 478-484 (New 3DS
      XL unchanged: 808-896 / 1190-1192). The rest (~20%) is still to find.
- [ ] Whether NSA stops answering again right after "the rest over one connection" (it did once): if it does, a photo of
      the screen and `transfer-log.txt` at once.
- [ ] Remote Install's choices (#29): a list (Install, Save and install, Save install and delete, Save; X saves at once)
      with each one explained on the top screen. Save and install and Save install and delete tried on the New 3DS XL
      with NSA's own CIA: downloaded, saved, the install from the SD card started (and was refused, "Already exists", as
      the running NSA of the same version is); the file stays when the install fails. To try with a game not installed.
- [ ] `SITE SCREENSHOT` (DEBUG) is not taken in time (15 s) on 0.6.4 and the console stops answering its buttons until a
      B arrives; on 2026-11-02 it worked (console_tests' screenshots).

**Console to console: how many connections** (`SITE CONNS 1-4`, DEBUG, on the sender; 2026-11-03, New 3DS XL -> 2DS over
the Wi-Fi, 64 MB, two rounds, every copy identical; the 2DS's receive time, and its web page's slowest answer meanwhile):
1 connection 223-288 s (228-294 KB/s, 0.35-1.43 s); 2: 145-160 s (408-453, 0.63-1.69 s); 3: 136-155 s (424-482,
0.67-1.22 s); 4: 114-152 s (430-575, 2.0-4.1 s), and once all four failed together twice in 80 s (`io: poll failed,
errno 115` on the 2DS; Retry went on from where it was). So one connection is too few, and 2 to 4 are within the Wi-Fi's
noise. Then the ramp (`ramp.c`, the owner's idea: add while one pays off, drop the one that does not, try again two
minutes later, to the end; protocol 8): New 3DS XL -> 2DS 131 and 164 s, 2DS -> New 3DS XL 157 s, on 2 or 3 connections
(the third tried and dropped when it brought +0 to +14%, kept at +28%; a fourth -7%). Two New 3DS are not measured yet:
the log says what it chose ("sender: N KB/s over M connections...").
- [ ] Why all four connections fail at the same moment on the 2DS (errno 115 in poll): seen once in four sends.

**Download and FTP speed** (`fix/download-speed`, `fix/ftp-pasv-buffers`, issue #21). Measured on the New 3DS XL over
the LAN (numbers in Decisions); URL install, FTP and web up/down worked there, byte for byte.
- [ ] The catbox.moe QR CIA in 3dsDL on the 2DS, to compare with NSA's 432 KB/s there.
- [ ] The 2DS stays smooth during a download and an FTP transfer (menus, music; FTP threads now on the app core).
- [x] Console to console: the speed on screen no longer jumps between 0 and twice the real one (bytes counted every 32 KB,
      not per 256 KB chunk, and averaged over the last seconds); the owner saw it steady (2026-11-02).
- [ ] Remote Install's confirmation (QR, typed URL, history, received URLs, the web page's install from URL): Install /
      **Save** / Cancel. Save downloads the file to `sdmc:/nsa/received/` (with what comes from another 3DS; not the SD root, which is the owner's) as it is (name from the URL, decoded; an extension from
      the content when it has none; a name in use gets " (2)"), with the same progress, connections and errors (issue #22).
      Azahar: two saves of a 100 MB CIA byte for byte, Install unchanged. 2DS (2026-11-02): a 100 MB save from the web
      page at 557 KB/s, byte for byte. Left: a QR and a typed URL on a console.
- [ ] Settings -> "Pause music for FTP" (`ftp_pauses_music`, on by default on an Old 3DS): the music fades out once an FTP
      transfer has lasted a second and comes back 3 s after the last one (from the SD cache, no new song is made). On the
      2DS an upload with music went at ~375 KB/s with it (254-316 before). The top bar's server icon blinks while the FTP
      server moves data, and a computer icon shows while the web server runs.
- [ ] Music during transfers on an Old 3DS: off made the 2DS's FTP 10-25% faster (New 3DS: 3-6%); a URL install on the
      2DS 520 -> 530 KB/s (2%, the install is its limit); a 16 MB send New 3DS -> 2DS 29.0 / 35.7 s with music on both,
      29.8 / 30.8 s off (~6%, noisy). Owner to decide whether NSA pauses it during FTP transfers on an Old 3DS.
- [ ] FTP and web after the non-blocking change: FileZilla and a browser on a real console (big files both ways, several
      at once, a client that disappears mid-transfer, turning the servers off mid-transfer).

**NSA icon and banner** (`tools/make_branding.py` draws `meta/icon_3ds.png` and `banner_3ds.png`)
- [ ] After installing the CIA the HOME menu shows the new icon and the banner (the CIA's
      version is still 0.3.1 for every dev build; if HOME keeps the old art, bump the version).
- [ ] Banner on the console (3D rotation, tiny text readable). The boot logo animation no longer
      is FBI's (`LOGO := plain`: the CIA carries the generic logo of buildtools/makerom); look at what
      shows on launch. A logo of NSA's own is left open in issue #15. The banner jingle is NSA's own now: listen to it in HOME.

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

**Dump NAND screen** (branch `advanced`)
- [ ] On a console: the raw NAND opens (Azahar cannot, it closes), size and free space are right,
      a dump runs to the end, the SHA-256 shown matches the file (`sha256sum`) and the `.sha256` next to it
      passes `sha256sum -c`, cancelling deletes the
      half file, a full SD card shows the red "not enough space" screen. Seen in Azahar only with a
      fake source of zeros (UI and hash).

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

**Volume sliders in Settings** (a trial the owner asked for to see how it looks)
- [ ] On a console: Sounds and Background music are one box each, the selector on top and a draggable
      bar with its percentage under it (they replace the two "volume" rows); a finger anywhere along the bar sets the
      volume in steps of 5 and it applies while dragging; X / Y change it with the buttons.

**Draggable scrollbars** (lists, Settings, action sheets, filter popovers)
- [ ] On a console: a finger on the bar on the right (a wide area, not only the 4 px) makes the screen follow it, a
      tap on the bar jumps there, and dragging the content still works as before. Seen in Azahar with SITE DRAG.

**Music cache and crossfade** (`bgmusic.c`)
- [ ] On both consoles: the second time the music starts it plays the last song of that style at once (from
      `sdmc:/nsa/music-cache-N.pcm`, 1.3 to 1.6 MB each), a new one is made meanwhile (about 15 s on the 2DS) and it
      replaces it with a 3.5 s fade-out/fade-in; the new one is what is kept. Changing style fades the old one out
      and plays the cache of the new one if there is one. Turning it off cancels a song being made.
- [ ] The animation: a note blinks in the status bar while a song is made, and Settings shows the progress,
      a progress bar and an equalizer (quick while making, slow while playing). Seen in Azahar only; the fade
      cannot be heard there.

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
- Control connection (TCP 5002, or the UDS stream): the sender sends a 828 byte hello
  (protocol 8: magic, version, sender name, file name, size, `check` = CRC-32 of the first
  4 KB of the file or of a package's header, `kind` 0 file / 1 title package / 2 folder, `head` = the
  package header's or folder manifest's size, `origin` = where the file or folder is on the sender's
  SD card, "" when it is not there). For a folder the manifest (`head` bytes) follows the hello at
  once, before any answer. The receiver reads it, drops the transfer without asking when the
  manifest is damaged or unsafe, and asks its user (Allow, then Install now / Save /
  Reject; Install only for a CIA; Save opens a folder picker starting in `sdmc:/nsa/received/`)
  and answers 13 bytes: status, the byte to go on from, a transfer number.
- A **folder** (kind 2) is one payload: the manifest (`foldermanifest.h`: every path relative to the folder,
  type and size) and then each file's bytes in that order, one segment per file, so the chunks, the
  resume (`prefix`) and the connections are the same as for a file. The sender reads files as chunks ask;
  the receiver keeps the manifest chunks, parses and checks them (`folder_path_ok`: no `..`, no leading
  slash, no `:`), makes the folders and writes each chunk into `nsa/tmp/<name>.part/<path>` (a file's bytes
  wait, up to 60 s, for the manifest; `nsa/tmp/` is only NSA's, so clearing an old `.part` there never
  touches the user's folders), makes the empty files at the end and moves the folder to the chosen one
  (`<name>`, or `<name> (2)`... when it exists). Files and title packages do the same
  (`feat/tmp-for-files`): their `<name>.part` and `<name>.part.pos` are in `nsa/tmp/` too, and the file is
  moved to the chosen folder when whole. A `.part` that an older build left in another folder is not
  resumed (it is only a leftover to delete). This is why
  **no copy of the folder is made on the sender's card**.
- **Original path** (issue #24; `feat/keep-original-path`): for a file or a folder (not CIAs, titles or
  save packages) whose `origin` is safe (starts with `/`, no `..`, `:` or `\`, not in `/nsa/tmp`), the
  Incoming file question adds *Original path*. Nothing is written there until the transfer is whole in
  `nsa/tmp/`. If some of the files already exist at that path, a second question says how many and asks
  *Overwrite* / *Skip existing* (a single file: *Overwrite* / *Cancel*) / *Cancel*. Then each file is moved
  into place (a file where a folder is needed, or the reverse, is an error, never deleted); files that
  are only on the receiver stay. The files already moved stay if the move fails midway. A retry of
  the same transfer keeps the choices. Only Files on the SD card sends an origin.
- Data: the payload is cut in chunks of 256 KB. Over the Wi-Fi four TCP connections (the
  control one and three more that quote the transfer number) carry chunks, each taken by
  whichever is free (two send from the start and `ramp.c` finds how many of the four pay off as it goes: one that
  does not is kept aside, saying it is still there with a wait record every 5 s, and tried again two minutes later;
  protocol 8); a record is offset (8), length (4), bytes; a connection ends with a
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
- `SITE TEST BUILD`: the protocol and build time of the NSA on the console; the runner refuses to run
  when the two consoles differ (a console that kept an old build made every transfer fail once).
- `SITE TEST SENDDIR <folder>`: sends a folder (and its path as the origin).
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

## Speed tests (`tools/speed_tests.py`, `source/core/speedlog.c`)

After reports of slow transfers (issue: a QR install at ~1 MB/s with small files; FTP starting at 800+ KB/s and falling to
~500 on a 256 MB file; the web page at 300-550 KB/s; slower than ftpd), every transfer can leave a trace of each second in
`sdmc:/nsa/speed-log.txt`: DEBUG builds always, release builds while `sdmc:/nsa/speed.flag` exists (a user can be asked to
create it, transfer, and send the file). Traces are kept in memory and written when the transfer ends. Each `s,` line: KB/s,
time waiting for the network (`wait`: nothing to read, no room to send), inside the socket calls (`net`), inside the card or
the install (`disk`, and the slowest single call), bytes per socket call, UI frames drawn, a gauge (a download's segments
ahead), music on/off and, after `SITE CPU`, each core's load. An `e,` line sums the transfer (first and last 10 s, slowest
second). Traced: FTP RETR/STOR, the web page's file up/download, downloads by URL (one connection and segments; `net` there
is only bytes, `disk` is the install's time, `wait` the time with no segment ready).

    tools/speed_tests.py --host IP [--sizes 1,16,64,256] [--only ...] [--cpu] [--label 2ds-music-on]
    tools/speed_tests.py --host IP --server ftpd --ftp-port 5000          the same FTP tests against ftpd

Tests: `ftp-up`/`ftp-down` (checked byte for byte), `ftp-null`/`ftp-zero` (NSA's `STOR /dev/null` and `RETR /dev/zero/<MB>`:
the network alone), `ftp-small` (many small files: the cost per file), `sd` (`SITE SDBENCH <MB> [buffer KB] [PRE]`: the card
alone, with the 256 KB stdio buffer, with the size set first, unbuffered), `web-up`/`web-down`, `url` (the PC serves a file,
the page's install-url asks for it, the script presses X = Save; `--cia` installs instead). Results in
`test-results/speed-<time>-<server>-<label>/`: `summary.md`, `report.html` (a chart per test: KB/s of the PC and the console,
and each second's share of card / waiting / socket calls), CSVs of both sides and the fetched log.

First run (2026-11-02, the release build on 192.168.1.141, no traces): FTP up 363-405, down 224-336 KB/s; 20 files of 128 KB
took ~626 ms each up and ~494 ms down, so most of a small file's time is fixed cost per file.

Full battery, 2026-11-02 evening, both consoles on the LAN Wi-Fi, this branch's DEBUG build (`test-results/speed-20261102-*`),
KB/s from the PC:

| | 2DS | New 3DS XL |
|---|---|---|
| FTP up / down, 256 MB | 395 / 343 | 661 / 652 (16-64 MB: 624-649 / 715-728) |
| FTP network only (`/dev/null` / `/dev/zero`), 64 MB | 389 / 374 | 851 / 875 |
| Web page up / down, 256 MB | 360 / 323 | 731 / 766 |
| URL download, 64 MB, 8 connections | 559 | 1223 |
| 100 files of 256 KB, up / down (per file) | 1750 / 957 ms | 480 / 466 ms |
| Card alone (SDBENCH 256 MB, 256 KB buffer): write / read; size set first; unbuffered | 3926 / 7693; 5502; 2058 | 4105 / 9547; 5190; 2570 |
| Slowest card write (growing / size set first) | 1444 / 789 ms | 143 / 128 ms |

What the traces say:
- **No CPU limit**: the counting threads saw the cores ~3% busy on the New 3DS, ~15% (core 0) on the 2DS. The FTP and web
  threads wait for the network 62-85% of each second (uploads: nothing to read; downloads on the 2DS: ~65% inside `send()`).
  Each `recv()` brings ~2.9 KB on the New 3DS, ~6 KB on the 2DS: the data trickles in; the console drains it as it comes.
- **Not the card** for big files: the network alone is as fast (2DS) or only a little faster (New 3DS: 850-900 against
  620-730 with the card). One TCP connection is the limit: the URL download with 8 connections doubled it (1223 KB/s on the
  New 3DS, which is the "~1 MB/s" of the report and normal there).
- **The fall from 800+ to ~500 KB/s on a 256 MB file** shows in the network-only test too (`/dev/null`, 256 MB on the New 3DS:
  30 s averages 826 -> 396-640 -> 950, with 42 seconds under 200 KB/s in which the thread waited ~1000 ms with nothing coming
  and the card idle): the Wi-Fi delivered less, not NSA.
- **Small files on the 2DS**: the first ~40 uploads spent ~65 ms each in the card; from the 45th on, ~1.47 s each (writing
  and closing 256 KB), steadily; music off, CPU idle. On the New 3DS 480 ms a file. To compare with ftpd on the same console.
- Candidate fixes, to measure afterwards with the same battery: set the file's size first on uploads (FTP `ALLO`, the web
  page's length: +40% card speed, slowest write halved on the 2DS); and, under the owner's rule of **one transfer at a time**
  (control stays free; the web page browses and shows the transfer, its live screens pause), a short blocking `poll()` for
  the transfer that holds the turn instead of the naps. Next: the same FTP tests against ftpd-EX on both consoles.

2026-11-03, New 3DS XL (192.168.1.132), ftpd-EX 1.3.2 at 08:35 then NSA at 09:43, same morning's Wi-Fi, KB/s from the PC:

| | NSA | ftpd-EX |
|---|---|---|
| FTP up 16 / 64 / 256 MB | 530 / 525 / 509 | 527 / 531 / 490 |
| FTP down 16 / 64 / 256 MB | 566 / 652 / 625 | 573 / 795 / 635 |
| Network only up (NSA `/dev/null`, ftpd `/devZero`) 16 / 64 / 256 MB | 666 / 535 / 583 | 637 / 605 / 619 |
| Network only down 16 / 64 / 256 MB | 671 / 652 / 782 | 879 / 740 / 809 |
| 100 files of 256 KB up / down, per file | 506 / 500 ms | 475 / 457 ms |

Uploads level; ftpd 5-20% ahead on downloads and ~8% on small files, where NSA waits 75-86% of the time for room to send
and naps between asks while ftpd sleeps in `poll()`: what the one-transfer turn with a short blocking `poll()` would change.
Both show the same dips mid-transfer in the network-only tests: the Wi-Fi.

2000 MB on the same console (NSA, 10:45-13:15, `speed-20261103-104517-nsa-new3ds-2000`; traces whole, 3126 and 2817 lines):
FTP up 647 KB/s in 53 min (10-minute averages 670, 621, 599, 689, 657, 671; card 14-16% of the time, slowest write 206 ms),
down 720 KB/s in 47 min (714-737 throughout), byte for byte, 8 seconds under 200 KB/s in each. Card alone: 4261 KB/s write
(slowest 145 ms), 5363 with the size set first (94 ms), 2369 unbuffered (2186 in the first 10 s, 2348 in the last), read
9642. A long file neither slows the card nor makes the transfer sink on this console.

**One transfer at a time** (`feat/one-transfer`, cut from `feat/speed-tests`): the turn and the short `poll()` (Decisions,
"One transfer at a time"). Same console, 2026-11-03 afternoon, the three ways of waiting one after another three times
(`SITE POLL`, `speed_tests.py --wait-mode`), 64 MB and 50 files of 256 KB, KB/s or ms a file, average of the three:

| | short `poll()` (kept) | naps (before) | 1 ms naps then `poll()` |
|---|---|---|---|
| FTP up 64 MB | 634 (677, 687, 537) | 596 | 599 |
| FTP down 64 MB | 747 (706, 779, 756) | 649 | 724 |
| 50 files up, a file | 458 ms | 544 ms | 559 ms |
| 50 files down, a file | 479 ms | 477 ms | 496 ms |

The short `poll()` is ahead everywhere but small downloads (level): +6% up, +15% down, 16% less a small file up. The full
battery with it (`speed-20261103-134452-nsa-new3ds-turn`): FTP 693-757 up, 727-762 down (16-256 MB), waiting for the network
49-60% of the time going up (66-74% before). An earlier run that looked worse for small files was the Wi-Fi of the moment.
`tools/turn_tests.py` (busy replies, clients that vanish) passes on the New 3DS XL.

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
3. ~~**Pending titles, System save data, Dump NAND**~~ done on the `advanced` branch (not in `release`), on the same pattern (Dump NAND as
   its own screen: space needed against free on the SD card, progress with ETA, where
   the file ends up). Pending titles and System save data still have FBI's action lists
   and English item names ("Delete Pending Title"...); System save data would show
   readable names of the known modules.
3b. **Send saves and extra data**: built 2026-10-31 (code: `source/fbi/action/savetransfer.c`), untried on a console
   (see Pending tests). A save is whole: it cannot be merged, only replaced or kept apart.
   - **Export** (Titles sheet, flat rows *Export save/extra data*): copies the archive's files to
     `sdmc:/nsa/saves/<TITLEID>/<YYYYMMDD-HHMMSS>/{manifest.txt, save/ | ext/}`; the manifest has
     `source=NSA`, kind, title ID, version, name, date. A plain folder to browse or copy to a PC.
   - **Import** (*Import save/extra data*, red): a scrolling list (no cap) of NSA's exports tagged `[NSA]` and
     Checkpoint's backups tagged `[Checkpoint]` (`sdmc:/3ds/Checkpoint/saves|extdata/0x<unique ID, 5 digits> <game>/<backup>/`,
     the tree directly in the folder, no manifest). It first exports the current data to `<date>-before-import`
     (a suffix, not the `before-import-<date>` prefix first planned, so names still sort by date), empties the
     archive, copies the tree and commits. **If the copy fails or is cancelled, the old data is put back from that folder, and the
     folder is deleted once it is back**; if it cannot be put back, the folder stays and the message says so. Warns when the export's game version differs from the installed one
     (NSA's exports only). Only offered when the archive opens (title started once; ext data found).
   - **Send** (*Send save/extra data to another 3DS*): no protocol change. The data is packed on a worker thread into one
     file `sdmc:/nsa/saves/send/<TITLEID>-<save|ext>-<name>.nsasave` (format in `savetransfer.c`) and goes through the
     normal file transfer (resume included). The package stays on the sender's SD until the next one of that title
     overwrites it. The receiver recognises the name: *Import now* (only if the title, or the ext data archive, is
     installed) / *Save in a folder* / *Reject*; either way it unpacks into `sdmc:/nsa/saves/<TITLEID>/<date>/` (the same
     folder the import list shows) and deletes the package. Unpacking refuses paths with `..`, a leading `/`, `\` or `:`.
   - **SpotPass data** (the BOSS extra data archive, same ID as the extra data) works the same as the extra data: rows
     *Export/Import/Send SpotPass data* when the game has it, `boss/` in the export folder, `boss-` in the package name (kind 2),
     no Checkpoint equivalent so no Checkpoint backups are listed. Written like extra data: no commit is required.
   - Not built: exporting in Checkpoint's layout (the owner said it is not needed now);
     ext data to the right game still rests on the ID heuristics (4).
10. **Web server** (the owner asked for it 2026-10-31: "a page to do all of this from the PC"). Phase 1 is built:
    - **What**: an HTTP server on port 8080 while NSA is open, **off until turned on** in Settings ("Web server", saved as
      `web_server` in `settings.json`; the row's top screen shows `http://IP:8080  PIN nnnnnn`). `webserver.c` copies the FTP
      server's shape (own thread, polling with naps because libctru has one SOC session, pause/resume around the local wireless
      in `udslink.c`, retry for two minutes when the Wi-Fi is not back). One request at a time, `Connection: close`.
    - **Page**: `romfs/web/index.html`, one file with its CSS and JS (English or Spanish by `navigator.language`, dark by
      `prefers-color-scheme`): PIN screen, breadcrumbs, sorted list, download, upload (button or drop, one at a time, with progress),
      new folder, rename, delete (confirms), *Install* on `.cia`, free space. Test it in a desktop browser against Azahar.
    - **API** (all under `/api`, listed at the top of `webserver.h`): `info`, `list`, `file` (GET with Range / PUT as `.part` then rename /
      DELETE, folders recursive), `mkdir`, `rename`, `install` (reuses the FTP server's CIA install, `ftpserver_install`; SD titles only,
      NSA itself restarts). Paths are plain `/dir/file` inside the SD card (no `..`, no `\`, no `:`), never the NAND; the root cannot be deleted.
    - **Security**: a 6 digit PIN made when the server starts, kept only in memory (DEBUG builds also write it to `sdmc:/nsa/web-pin.txt`
      for the tests). Sent as the cookie `pin` (reads, so links download) or the header `X-Pin`; **anything that changes something
      needs the header**, which another site's page cannot send (no CORS), so no CSRF. 10 wrong PINs in a row lock it for a minute.
      It is plain HTTP: anyone who can watch the Wi-Fi sees the PIN (no HTTPS on the 3DS in practice). Not for untrusted networks.
    - **Tests**: `tools/web_tests.py --host 127.0.0.1` (Azahar with `"web_server": true`; PIN from `/nsa/web-pin.txt`) checks the page,
      the PIN rules, upload/download/range/list/rename/delete and the path rules. Last run 2026-10-31: all pass in Azahar.
    - **Theme in the page** (built): `GET /theme.json` (public, only colours and shapes) gives the console's current theme resolved for
      day or night: palette, accent, radii, border width, `pixel`. The page sets CSS variables from it and re-reads it every 5 s, so
      changing the theme, accent or night mode on the console changes the page. Pixel also loads the bundled font
      (`romfs/web/PixelifySans.ttf`, OFL, served at `/font.ttf`), square corners and thick borders. The HOME theme's big tiles
      are not copied (its colours and radii are). Checked in Azahar with Cards (night, blue) and Pixel (day, yellow).
    - **Phase 2** (built): a *Titles* tab beside *Files*. `GET /api/titles` is fast (ids, kind, version, size of the SD and NAND titles,
      no DSiWare); the page asks `GET /api/title` for each card's name, publisher, product code and which of save / extra /
      SpotPass data it has, one at a time (the server answers one request at a time), and `GET /api/icon` (the 48x48 icon as a BMP,
      untiled from the SMDH). Filters: text and kind (games and demos by default). Per title, *Download save / extra data /
      SpotPass*: `POST /api/save/pack` makes the same `.nsasave` package as sending (`action_save_pack_sync`, on the web
      thread) and the page downloads it from `/nsa/saves/send/`. *Send a save package to the console* uploads a package to
      `/nsa/received/` and `POST /api/save/receive?import=1` hands it to the main thread (`webserver_poll_main`), which runs
      `action_save_package_received` as if it had been sent: unpacked into `nsa/saves/<TITLEID>/<date>/`, and, if the title or
      its data is here, the red *import* question appears **on the console** (nothing is replaced from the page alone).
    - **Phase 2, the rest** (built): *Import* (⬆) beside each *Download* (⬇): `GET /api/save/backups` lists the title's NSA exports and Checkpoint's
      (the same scan as the console's import list, `action_save_backups`), the page shows them in a dialog and `POST /api/save/import`
      makes the console ask (red) there. *Launch*, *Delete* and *Delete with ticket* per title (`POST /api/title/launch|delete`): the
      console asks first. A third tab, *Install*: URLs (one per line, http/https only) go to `POST /api/install-url`, which opens
      Remote Install's own question on the console and adds them to its history; below, that history (`/nsa/history.json`, with *Install
      again*) and the end of `transfer-log.txt`.
    - **How a request that opens something on the console works**: `webserver.c` keeps one slot (`web_job`) that the main thread runs in
      `webserver_poll_main`; a second request meanwhile gets 409. The page never changes the console's data on its own: every import,
      launch, delete and install is confirmed on the console. Launch and delete keep a small title item alive for the question (not freed).
    - **Files tab extras** (built): *Copy* (`POST /api/copy?path=&to=`, recursive, 409 if the name is taken, 400 into itself), *Move* (the page uses
      `rename` with a path in another folder), *Edit* on text-like extensions up to 256 KB (a dialog; reads and writes with the existing `file` GET / PUT).
    - **Tickets and Extra data tabs** (built, read only): `GET /api/tickets` (ids, where the title is installed, kind; repeated ids folded),
      `GET /api/extdata` (SD and NAND archives), `GET /api/extdata/info|icon` (name, publisher and icon from the archive's SMDH). The page
      fills names and icons one request at a time and filters by text and name. Deleting stays on the console's own screens (they ask there).
    - **Left of phase 2**: actions on tickets and extra data from the page (delete asked on the console, like titles).
    - **Phase 3** (built): a *Screen* tab. `GET /api/screens` returns both screens as raw RGB565 (the top's 400x240, then the bottom's
      320x240: 345,600 bytes); the main thread reads the framebuffers in `webserver_poll_main` (`screen_capture_rgb565`, the left eye
      only) and the page paints two canvases, asking for the next frame as soon as the last arrives (10-12 fps in Azahar; the real
      speed is a test to do). Buttons: `POST /api/input/press?keys=A+R&frames=` (the names of SITE PRESS), `input/tap?x=&y=`, `input/drag?x=&y1=&y2=`
      (`input_inject_*`, the same as the FTP server's remote control). The page has an on-screen pad, keys (arrows, X=A, Z=B, S=X, A=Y,
      Q=L, W=R, Enter=Start, Backspace=Select), a click on the bottom canvas as a tap and a drag up or down as a scroll. The live view stops
      when the tab is left or the browser tab is hidden. **No `remote.flag` needed here**: turning the web server on and the PIN
      are the permission (whoever has the PIN can already install CIAs and delete files); the FTP server's flag is unchanged.
    - **Page redesign** (`feat/web-ux`, 2026-11-01; the owner asked for it to look and behave like the console): a menu on the left (a bar
      at the bottom on a phone), a status bar (console, SD space, battery from `info`, clock), screens by the URL's hash (`#titles`,
      `#files/dir/`, so the browser's back works). Lists are plain rows (or an icon grid) built once and filtered, sorted and filled in
      place; names and icons load one request at a time, the rows on screen first, icons kept as blobs. A click opens the item's
      **sheet** (its card with cells, then the actions, as on the console; a bottom sheet on a phone). Install is four tiles (URLs, a CIA
      from the PC: uploaded to `/nsa/web-install/`, installed, deleted; history; log). Files: selection (Ctrl/Shift click, the icon),
      right-click menus, a clipboard (copy / cut, paste bar), **drag rows onto a folder or a breadcrumb to move** (Ctrl copies), drop
      files or whole folders from the computer (on a folder row: into it), upload panel with speed, picture viewer, audio player, text
      editor (Ctrl+S), keys (Del, F2, Ctrl+A/C/X/V, Backspace). **Settings** tab: theme, accent, night mode, language, sounds and music
      with volumes, changed on the console (`GET/POST /api/settings`, applied and saved by the main thread), plus this page's own sounds
      (the console's tones played by WebAudio, as the console's setting says) and animations. The icons are the console's atlas
      (`/icons.png` and `/icons.js`, made by `tools/make_icons.py`) tinted with CSS masks. Checked in Azahar with the built-in browser.
    - **Save packages for the page** (`fix/web-pack-progress`): packing used to run on the web server's thread, which is on the system
      core (30 % of it on an Old 3DS / 2DS): a SpotPass package took minutes there and the 2DS could not be closed meanwhile (exiting
      waits for that thread). Now `POST /api/save/pack` only hands it to the main thread, which packs it with a worker on the
      application's core behind the console's own progress screen ("Packing for the web page", B cancels); the page follows
      `GET /api/save/pack/status` in a card (counting, packing with a bar, then the download with its speed) and can cancel it.
      **Left**: copying folders, uploads and downloads still run on the web thread (system core): slow on an Old 3DS, and closing NSA
      waits for one in progress.
    - **Phase 3 left**: sound is not sent; the 3D right eye; a smaller or JPEG frame if the Wi-Fi is slow (RGB565 at 345 KB is the first try);
      a text box that types into the console's keyboard; holding a button (press is a tap of N frames).
    - **Ideas**: a `remote.flag`-like switch for the destructive calls; drag whole folders (`webkitGetAsEntry`); download a folder
      as a zip; a QR code of the address on the console's top screen; HEAD and keep-alive; serving two requests at once.

4. **Exact ext data link**: read the ExtData ID from each game's exheader (NCCH
   `ExHeader`, ARM11 local system capabilities, offset 0x230 by 3dbrew; check with the
   Program ID at 0x200) instead of the ID heuristics. The file path type for the exheader
   on `ARCHIVE_SAVEDATA_AND_CONTENT` is not confirmed: probe it on the console and write
   the result to a file the FTP server can fetch, since Azahar cannot answer it.
5. **Themes "Pixel" and "HOME" (both built, see Pending tests; HOME lacks paged grids)** (mockups were shown and liked): HOME = big icon tiles
   in pages, modals as a sheet from the bottom; Pixel = bundled pixel font (OFL), square
   corners, thick borders, block progress bars, yellow highlight. Then **themes from the
   SD card** (`sdmc:/nsa/themes/`), data only.
6. ~~**NSA icon and banner**~~ done (the boot logo and the banner sound are left in issue #15). It was: still FBI's. Ask the owner for a
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
- **Languages** (`feat/languages`): one table per language, `source/core/i18n_<code>.c`, registered in `i18n.c`'s
  `languages[]` (code for settings.json and the web API, its own name, the console's `CFG_LANGUAGE_*` it answers to).
  Spanish is the reference for which keys exist; `python3 tools/check_i18n.py` checks every table against it (same keys,
  printf conversions in the same order, button glyphs, line breaks, edge spaces). Adding a language = a new table, one line in
  `languages[]`, one in `language_id`, one in the web page's `langSelect`. Settings -> Language is a list
  (`languagemenu.c`), not a button that steps through them. Chosen: Catalan, French, German, Italian, Portuguese (Brazilian), Russian,
  Japanese and Chinese (simplified `zh`, traditional `tw`). The console draws text with the system's shared font, which
  on a European, American or Japanese console is the Japanese character set: Latin, Cyrillic and kana are fine, Traditional
  Chinese works if the table only uses characters of JIS X 0208 (the table is rewritten to do so: variants like 内 録 緑,
  no 檔 卡 你; test it with `ch.encode('shift_jis')`), but Simplified Chinese is about a quarter boxes there. Not done:
  Korean (hangul is not in that font either) and Dutch (the 3DS has it; smaller audience). Simplified has a font of our own
  (`tools/make_hanfont.py` -> `romfs/hanfont.png` 86 KB + `source/core/hanfont.c`, from Noto Sans CJK SC, OFL; the OTF is not in the
  repository, the script says where to get it). It holds every CJK character that `i18n_zh.c` uses: **run the script again after
  changing that table**. Japanese is one standard written language (kanji and kana, polite form, common kanji only); `screen.c`
  breaks lines between its characters, since it has no spaces. The pixel theme falls back to the system font for what it lacks.
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
- **No thread waits inside the SOC service** (`source/core/netpoll.c`): libctru sends every socket call through one
  session that serves one request at a time, so a `poll()` with a timeout (curl calls it on every connection) holds up
  every other thread's network. The link wraps `poll()` (`-Wl,--wrap=poll`) to ask with no timeout and nap 1, 5, 20, then
  50 ms; the FTP and web servers nap the same way (`net_nap_ms`), so idle they cost little and need not be turned off
  during a download. Download sockets ask for 48 KB buffers (`net_set_buffers`; 3dsDL measured 48 KB fastest and 64 KB
  or more breaking connections). The FTP's passive socket takes **32 KB** (as the original ftpd: 48 KB there reset
  every data connection, and the owner's fork, which had dropped them, was the one leaving the system's; 2026-11-04,
  "FTP against the original ftpd"); the web server's sockets keep the system's. A download's connections share DNS
  answers and TLS sessions (curl share). `sslcInit` is called because devkitPro's mbedTLS takes its randomness from SSLC
  without checking. Learned from 3dsDL (2026-11-01).
- **FTP and web servers** run as ftpd's thread does (`net_server_thread`): the New 3DS's third core, else the app core
  just below the UI; not the system core (30% for NSA, shared with the tasks). Uploads go to the SD through a 256 KB
  stdio buffer (`setvbuf`), not one write per piece the network hands over. Measured on the New 3DS XL, LAN, 2026-11-01,
  v0.5.0 -> v0.5.14: FTP up 340 -> 866 KB/s, down 480 -> 850-940 KB/s; URL install of a 100 MB CIA 902 -> 1080 KB/s
  (8 connections) and 869 -> 1143 KB/s (one); web page up 910, down 1004 KB/s.
  Against ftpd-EX (the owner's fork, same 30 MB file, same evening): New 3DS up 758 / down 955 KB/s (NSA 866 / 850-940);
  2DS up 387 / down 418 KB/s (NSA 350 / 365, measured an hour earlier). So the New 3DS is level; the 2DS ~10-15% behind,
  near the ~400-500 KB/s both reach there.
- **Whether a server serves ranges** is asked with a GET of byte 0 (`Range: 0-0`, a 206 with the total in Content-Range),
  as 3dsDL does, not HEAD + `Accept-Ranges`: catbox.moe answers HEAD with no size and no Accept-Ranges yet serves ranges,
  so NSA got one connection there (~350 KB/s at 156 ms) while 3dsDL split it in four (~600 KB/s, the owner, 2026-11-01).
  A server that ignores the range is cut after the first byte and the download goes on one connection.
- **Segments in a window** (`http_parallel_download`): the connections fetch 512 KB segments into buffers of their own and
  copy each finished one into a window of 2 per connection (at least 6) ahead of the one the install needs next; a
  connection with nothing left to take asks again for the oldest segment on its way for 2 s (whichever copy comes first
  is used). Before, each connection owned one segment in turn and waited for the install to take it, so the download went
  at the slowest connection's pace: the catbox CIA on the New 3DS rose to ~600 KB/s and fell to ~100 again and again.
  After the window it finished at 459 KB/s with the network steady at 530-630 KB/s, stalling only on single slow
  segments, which is what the second request fixes. Both models use 8 connections now (an Old 3DS had 2 before the SOC
  and TLS fixes). On the 2DS: the catbox CIA at 432 KB/s with 8 (CPU 19% / 9%), 354 with 4 (partly sharing the internet
  with the New 3DS); over the LAN it tops out around 500 KB/s even with one connection, with the install keeping up (the
  window never fills), so its limit is the network side (the system's Wi-Fi and SOC work at 268 MHz), not the SD card.
  The New 3DS did the catbox CIA at 762 KB/s (3dsDL: 515-560).
  The install screen says how many connections ("From host, N connections"). The log says "segments ahead" every 10 s
  and how many segments were asked for twice.
- **Saving comes in long pieces; links good for one request** (2026-11-03, the owner's Dropbox link, Dragon Quest VIII
  3.4 GB, New 3DS XL). Dropbox redirects three times to `uc….dl.dropboxusercontent.com/cd/0/get/…`, which serves **one**
  request (even on the same connection) and answers 403 to the rest: the segments over 8 connections got 403 ("Forbidden"
  in NSA), and going through the redirects for each one (a new host and TLS handshake every 512 KB) ran the console out
  of sockets (curl 7 "Couldn't connect" ~100 ms after the redirect: each connection kept the old ones open). 3dsDL
  (`source/net/download_engine.cpp`) fetches long pieces, one request each, and when an extra connection gets an HTTP
  error it goes back to the link and keeps the connections still open as its most: from Dropbox it ended on **one**
  (its log's `connections=4` is the most ever open), 758 KB/s over the whole file.
  - **Save** (`http_download_file`, `data_op_data.dstAnyOrder`): the file is set to its size first (`FSFILE_SetSize`; a file
    FAT32 can not hold, or no room, fails at once and leaves nothing), split among the connections, each fetches its
    piece in one request and writes it where it goes; one that is done takes half of the biggest piece left. Refused
    where the redirects ended: the link given (free); refused there: no more connections than are open. Each request
    closes its connection (`FORBID_REUSE`: kept ones caused the curl 7). It **starts with 2 and finds how many pay
    off** (`ramp.c`, shared with console to console: one more while it makes it 10% faster, 5 s measures from 1.5 s
    after it opened; one that does not stops its piece where it is, which another takes, and one more is tried again
    two minutes later, to the end). Measured: Dropbox 2 connections, 1312 KB/s over 3 minutes (8 connections: 789 KB/s
    in minutes 1-3, and the console's ping at 150-200 ms with FTP data connections reset; with 2-3, ping ~50 ms, FTP
    1-3 s, web 0.3 s); catbox.moe 5 connections, 554 KB/s (34 MB, the same bytes); LAN 256 MB 2 connections, 1534 KB/s
    (with 4 fixed: 1311), the same bytes after two pieces stopped halfway; a cancelled save leaves nothing in
    `/nsa/received/`.
  - **Install** keeps the segments in order (the installer needs them so). When the address the redirects ended at
    refuses one (403/410), the rest comes over one connection from the link given, from where it was
    (`HTTP_RES_ONE_LINK`), as 3dsDL installs: Dropbox **1087 KB/s** (segments through the redirects: ~470). Tried with
    `SITE SAVEORDER INORDER` (DEBUG: Save takes the in-order path) and a local server whose addresses answer once or 30
    times (`onceserver`-style): the same bytes, 1112-1194 KB/s, also when the switch comes after 12 MB.
  - Once, on the first try of the switch to one connection, NSA stopped answering (an `abort` or ErrDisp: no Luma dump)
    and the log, which starts over at 96 KB, had just started over. Four runs after that and the Dropbox one went well.
- **One transfer at a time** (`source/core/xferturn.c`, the owner's rule, 2026-11-03): a download (URL, QR, updater, Save),
  an FTP RETR (1 MB and up, and `/dev/zero`) or STOR, a file up or down through the web page (down: 1 MB and up) and a console
  to console send (from connecting to the end; not while asking whether to retry) or receive (from accepting) take the turn.
  Whatever asks meanwhile is answered at once and never waits: FTP `450 Busy: <kind>, <name> (n%)`, the web page 503 with
  `busy` (its uploads, live screens, install from URL), a download or a send fails with "Another transfer is in progress".
  Free during a transfer: FTP control and LIST, small files, `speed-log.txt`, the page's browsing, settings and buttons.
  `/api/info` has `transfer` (kind, name, done, total, KB/s); the page shows it in the top bar, asks every 3 s meanwhile,
  checks before an upload and pauses the live screens. A turn not touched for 60 s is free again (a forgotten release never
  locks the rest out). The transfer with the turn waits in a real `poll()` of at most 20 ms (`net_poll_owner`, calling
  `__real_poll` past the wrap) instead of napping, as ftpd does: nothing else moves data, so a control request waits at most
  that. `SITE POLL POLL|NAP|HYBRID` (DEBUG) switches it to compare.
- **The 3DS does not always see a client's reset** (`tools/turn_tests.py`): a transfer whose client vanished waited its 30 s,
  the FTP session's give-way waited 30 s more, and the web page (one request at a time) sat behind it, then behind its answer
  to the dead client. Now a data wait stalled 10 s gives up when another client is waiting (`FTP_STALL_MS`, `WEB_STALL_MS`),
  an FTP session whose transfer broke lets a new connection in at once, `recv()` after a ready `poll()` waits 1 s, and a web
  upload whose client went away is not answered: the page answers again within ~13 s (10 s stall, 1 s, 2 s closing).
- **The servers' sockets never block** (`net_set_nonblocking`, `net_recv`, `net_close_gently` in `netpoll.c`): a blocking
  send to a client that had gone waited inside the SOC session until TCP gave up, holding every socket call of NSA, the
  shutdown() meant to wake it too; the 2DS stopped answering FTP and web during a download test and froze when the FTP
  server was turned off (2026-11-02). Closing waits (up to 2 s) for the other side to close, as the SOC service drops
  unsent data when a non-blocking socket closes. The FTP serves one session at a time: one silent for 30 s gives way to a
  new connection. Second requests for a segment only for the one the install waits for (near the end, any), once it has
  taken three times the average: with a fixed 2 s the 2DS asked for 158 segments twice (~80 MB wasted); now 1, and the
  same download went from 401 to 514 KB/s. The 2DS's install lags the network there (the window stays full).
- `SITE CPU` leaves counting threads at the lowest priority on every core until `SITE CPU OFF`: send it after measuring.
- **DEBUG builds can reinstall the same title version**: `SITE INSTALL` and the web page's install use
  `AM_StartCiaInstallOverwrite`, which takes the same version (only HOME's icon stays old); a lower one is refused
  (D8E08027, AM description 39). The ports install over and over because NSA's Install CIA deletes the title first (and
  its save). The micro version is 4 bits (max 15): the consoles hold 0.5.15 since 2026-11-01, so a published 0.5.x will
  not install over it there (0.6.x will, or delete NSA first; its data in `sdmc:/nsa/` stays).
- **2DS** (Old 3DS model, LAN, 2026-11-01), v0.4.14 -> v0.5.14: URL install 331 -> 459 KB/s (2 connections) and 364 ->
  531 KB/s (one); FTP up 185 -> 350, down 300 -> 365 KB/s. `tools/console_tests.py` passed (15/15) with v0.5.14 on both.
- **Drawing** only while something happens (`ui_should_draw`): CPU left to downloads and
  installs; animations that run with no input need `ui_wake()`.
- Vendored libraries: `source/libs/stb_image`, `source/libs/dr_libs` (dr_mp3, dr_flac, dr_wav),
  `source/libs/quirc`. Tremor and libogg come from devkitPro's portlibs.
- New icons: add the Tabler name to `tools/icons.txt` and run `python3 tools/make_icons.py` (needs
  network for Tabler's CSS; it can take two minutes: run it again if it times out). New ones are
  appended, so existing icon IDs do not move.

- **Updating a console during development**: a console refuses a CIA of NSA older than the installed one (D8E08027, AM
  description 39), but `SITE INSTALL` and the web page's install (`AM_StartCiaInstallOverwrite`) take the **same**
  version; only HOME keeps the old icon, banner and sound until the version goes up. The micro version is 4 bits (max
  15). **Since 2026-11-01 both consoles hold 0.5.15.** Build DEBUG installs with the micro version of the latest tag (after v0.6.1:
  `make DEBUG=1 VERSION_MICRO=1`, after `make clean`; `make` does not rebuild on a version change): the same version as
  the consoles once they update, never above it (the 2DS got 0.6.15 from another session once and had to be reinstalled
  through FBI, which deletes the title first). Check a console's NSA version
  first (`/api/titles`, id `000400000FB1A000`, `version` 95 = 0.5.15). A published 0.5.x will not install over 0.5.15
  on those consoles: the next release they can take is 0.6.0, or delete NSA first (its data in `sdmc:/nsa/` stays).
  Other titles (the ports) reinstall at any version because NSA's Install CIA deletes the installed title first.
- **Titles cache** (`listtitles.c`): `sdmc:/nsa/titles-cache.bin` keeps each CTR title's name, publisher, region, product code and
  icon, keyed by ID + version; only a small index is held in RAM and records are read one at a time. Version and size come from one
  `AM_GetTitleInfo` call per media. Titles without an SMDH (updates, DLC) are remembered too. X on Titles ignores the cache and
  rebuilds it; a full pass drops what is no longer installed. DEBUG builds log timings to `sdmc:/nsa/titles-log.txt`. On the
  owner's New 3DS (204 titles): ~33 s without cache, ~5.4 s with it (2026-11-01). Not tried on the 2DS.
- **Publishing a stable**: write `docs/release-notes/vX.Y.Z.md` (since the previous stable), merge it into `release`,
  `git merge --ff-only release` into `master` and push `master`, then tag `release`'s tip and push the tag: with
  `master` already containing the commit the workflow publishes it as stable straight away.
- **Pictures for the README** are taken on a real console through the FTP remote control (Settings in English and
  Sounds off, restored afterwards; the consoles' own names, IPs and Wi-Fi are painted over, and only a demo folder on the SD
  is shown). `docs/images/` is 800x960, both screens, 160 colours.
- **Music is generated, not shipped** (no assets in the CIA); the loop is made circular so its join is silent; what
  was planned and not done: endless music without a loop (it needs streaming synthesis, measure the 2DS first).

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

Pending (2026-11-01): the default background music is now Shop (was Off), for installs with no `settings.json`; the banner
jingle has four candidates from `tools/make_jingle.py --all DIR` (bossa, marimba, chime, pixel) waiting for the owner's pick.

Banner jingle (2026-11-01): the owner chose **bossa** (`meta/audio_3ds.wav`). **marimba** is kept in `meta/alt/audio_3ds_marimba.wav`
(copy it over `meta/audio_3ds.wav` to switch; both come from `tools/make_jingle.py --variant NAME`). Listen to it in HOME after installing the CIA.
