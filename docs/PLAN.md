# NSA plan

Start every session here. It holds where things stand, what comes next and why
things are the way they are. Keep it current in the same change that alters it.

## Where the last session stopped (2026-11-07, evening)

**v0.9.0 is out as stable** (2026-11-07): `staging` merged into `release` (release notes `docs/release-notes/v0.9.0.md`),
`master` and `advanced` (its Dump NAND screen kept; the empty suspend/restore callbacks dropped there too). DEBUG builds
for the consoles now use `VERSION_MINOR=9 VERSION_MICRO=0`. `staging` goes on for what comes next.

**The guide for 0.9** (`feat/guide-0.9`): the chapters new in 0.9.0 (Selecting several, Music in the background, Game mods)
were marked 0.8, so someone updating from 0.8.4 (`guide_known` 0.8) was never offered them. Now `since` 0.9, and chapters
have `updated`: a release that changes one (Store: your own stores; From your computer: the transfer toast and the web
server's QR code) sets it, and the chapter is offered again also to whoever saw it. New steps: repeat and the playing
song in Music in the background, the toast and the QR code in From your computer (all 11 languages). 0.9.0 keeps
`guide_known` at 0.8 (its newest chapter said 0.8), so 0.9.1 offers the five. Tried in Azahar with `guide_known` 8 and
Store and From your computer seen: "New in this version" listed the five, 27 steps.

Start here in a new session. `staging` is pushed (`b41f2f6`) and holds all of it; nothing is uncommitted.

- **Builds on the consoles**: the New 3DS XL (192.168.1.136) and the 2DS (192.168.1.140) run `staging` as of `9135288`
  (before the zoom in every grid). The newest CIA (`b41f2f6`, zoom in Ext Save Data and Mods too) was built and sent but
  **not installed**: ask before installing.
- **The owner has to test** what is listed under "Pending tests on the console", newest first: the zoom in every grid,
  the Store install of 3D Pinball with the web page closed, "Shared UI pieces" (keyboards, viewers' buttons, deleting one
  ext save data, install/paste/export with B), "Sort both ways", "Shorter filters", "Selecting several files", "Music in
  the background", "Transfer toast".
- **Open questions**: the 2DS's short sleeps look like its sleep
  switch (hardware), to confirm in HOME / other apps.
- **Left on the New 3DS XL by the tests**: the title "NSA Test" (000400000FB1AE00, our dummy CIA) is installed again;
  the owner can delete it. Nothing else of the tests is on either SD card.
- **Refactor (UI structure)**: done in three rounds (`refactor/ui-structure`, `-2`, `-3`), see "Shared UI pieces". Not
  unified on purpose: the "Loading..." phase of the actions (only per-action code is left) and each screen's settings
  store/restore. `tools/ui_snapshots.py run/compare` checks that a change draws the same (49 steps in Azahar).
- **Ideas for later**: issue #32 (named mods and a GameBanana mods store), what is left of item 11 (Next).
- After the owner's tests pass: `staging` -> `release` -> `advanced` (CLAUDE.md, "Branches in use").

## Status (2026-10-31)

`release` holds everything below and is pushed. On 2026-10-31 the owner asked to merge the work
of that session into `release` **before testing it** (and to push directly): what is merged but
not yet tried on a console is listed in "Pending tests on the console" below, which is where to
start. Merged topic branches are kept on the remote and can be deleted.

| Branch / tag | State |
|---|---|
| `staging` | **Untested work** (since 2026-11-07): topic branches merge here until the owner tries them on a console, then `staging` goes into `release`. Holds `release` plus what is under "Pending tests" from that date. |
| `release` | The working line. Holds everything below, but "Advanced". |
| `advanced` | **The risky tools**: Dump NAND as its own screen (space, progress, SHA-256 and a `.sha256`), Pending titles and System save data on the Titles pattern, Advanced's red look. Cut from `release` on 2026-10-31; the owner wants what is about the console's own data merged there first. Keep `release` merged into it (`git merge release`). |
| `master` | Stable. Moves only by merging `release`. |
| tag `v0.9.9` | **Stable**, published 2026-11-11 (notes: `docs/release-notes/v0.9.9.md`): lists open at the top and jump arrows (#45), themes of one's own with editors on the console and the web page, HOME's paged grids and sheets pulled down, the Pixel font's digits (#14). |
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
    Settings: Sounds (Off / Alerts / All, default All) and Sound volume (0-100 %, as on the console). Requests are
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
23. **Guide** (`feat/guide`, `source/fbi/guide.c` on `source/core/ui/coach.c`): how NSA works, as guided tours on its own
    screens (a bubble points at a button and waits for the user to press it), offered at start and kept in Settings -> Guide. See
    Decisions, "The guide's chapters", before adding one for a new feature.

## Pending tests on the console (2026-10-31)

- [ ] Flick and jump arrows in the menus (2026-11-11, `feat/menu-flick`; the owner: the lists coast after a drag and
  have the arrows to the top and the end, the menus do not). Settings and the action sheets drawn as a list now coast
  when a drag is let go, show the two round arrows at their bottom right while they scroll (only the ways they can still
  go; a tap selects the first or the last row) and take ZL / ZR for the same; the filter popovers coast too (no arrows:
  the card is small). One helper, `widget_scroller` (`widgets.c`), which the lists (`list.c`) now use too for theirs.
  Built only (DEBUG 0.9.9), not tried in Azahar or on a console.
- [x] Sheets pulled down to close (2026-11-11, `feat/sheet-drag`; issue #14, the last of HOME's mockup). An action sheet
  (`overlay.c`, every theme; it now has a handle) and a dialog drawn as a sheet (`prompt.c`, a theme with sheetPrompts)
  follow a finger put on their top (the handle and the header, not the close cross) and the scrim lightens; let go past
  30% of their height, or flicked down, they are put away as B does (a question takes its last answer: No, Cancel, Keep
  editing; never one that goes ahead), else they go back up. Claude, 2026-11-11, Azahar (HOME): a title's actions pulled
  away, the editor's "Save the changes?" pulled away (Keep editing), a short slow pull going back.
  Second round (the owner on the New 3DS XL: it vanished when let go, and a touch just above the handle closed it):
  let go low enough, it slides the rest of the way down before it goes (no key counts meanwhile); a touch up to 18 px
  above its top takes its top too (`fix/sheet-drag-slide`). Claude, Azahar: a tap just above the handle keeps it, a long
  pull closes it.
  The owner, 2026-11-11, New 3DS XL (`a92ec4e`): "it works".
- [ ] Pages of a grid, second round (2026-11-11, `feat/page-scrubber`; the owner: no way to go to a page from the dots,
  and a bar when there are too many). Under a grid in pages (18 px now): up to 20 pages, dots 12 px apart, a tap on one
  (or a finger along them) goes to it; more pages, a bar with a thumb and "N / M" beside it, and a finger along it
  shows a bubble with the page it lands on, gone to when it lifts (first item selected). Switching rows and grid shows the
  selection again (it went back to the top). The guide's step on HOME says it. Claude, 2026-11-11, Azahar: the Store's 54
  pages, a drag along the bar (bubble "30", let go at 51), Tickets' 2 dots (a tap on the second), the selection's page
  after Y Y.
- [ ] Guide chapter **Your own themes** (2026-11-11, `docs/guide-themes-chapter`, guide 0.13, after Look and sound): Settings
  -> Edit the theme, Start from, Background opened in the colour picker (anchors `colorpick.square` and `colorpick.hue`),
  B to leave it as it was, Corners and the rows below it, Save as a new theme, leaving, and the web page's editor. Look and
  sound keeps one new step (HOME's pages, and that themes have their own chapter). Claude, 2026-11-11, Azahar: played
  from Settings -> Guide in Spanish, 15 steps, nothing saved.
- [ ] Theme editor, second round (2026-11-11, `feat/theme-editor-2`; the owner's notes on the first one). **Corners** is one
  value: 0 made every corner square whatever the other said (`screen_set_square_corners`), so the corners of rows and
  buttons now follow it (`theme_small_radius`, three quarters, as the built-in themes; a file may still give
  `radiusSmall`). Left and Right step quickly while held (`keys_hold_repeat`; `keys.c` now reads NSA's input, so remote
  and guided presses reach the keyboards and keypads too). **Start from** (first row) opens a list of the themes and a
  **blank template** (`theme_blank`: greys, corners 8); with changes it asks first. A theme of the SD card has **Save
  the theme** (over its file), **Save as a new theme** and **Delete this theme** (red, `theme_delete_custom`; the first
  built-in theme is used if it was in use); a built-in one only Save as a new theme. **Accent colour** is a row too (the
  setting, not part of the theme). Settings' row "Save the theme as a file" is gone (the editor writes files). The web
  page has the same: Start from with the blank template, one Corners slider, Save / Save as a new theme / Delete
  (`/api/theme/edit?id=blank`, `/api/theme/save?new=1`, `/api/theme/delete`). Claude, 2026-11-11, Azahar: Left held
  from 10 to 0 in a second, Start from -> blank with the question, saved as new, Save and Delete shown, deleted (back to
  Cards); the page: blank, Corners 20 -> 15 on both, save as new, delete; web tests 0 failures.
- [ ] Theme editor on the web page (2026-11-11, `feat/web-theme-editor`; issue #14). Settings -> Appearance -> **Edit the
  theme** opens a card: name, the theme to start from, day or night, the ten colours (the browser's colour picker, and
  the opacity of the veil behind dialogs), corners, outline, font, bars, dialogs, main menu, icon grids, and "Show it
  on the console" (on by default). The page wears what is edited at once (it stops following the console's theme
  meanwhile); the console draws it too (`POST /api/theme/preview`, the last of a burst wins). **Save and use** writes it
  as the console's editor does (`/api/theme/save?editing=`) and puts it in use; Discard, leaving Settings or closing the
  tab go back to the theme in use. API in `webserver.h`; the body is a theme file's JSON, checked by `themefile_parse`
  on the web thread, drawn or saved by the main one. `tools/web_tests.py` checks the endpoints (and now accepts a
  theme from the SD card in /theme.json). Claude, 2026-11-11, Azahar and the built-in browser: a colour changed on the
  page seen on the console, "Arena" saved with pages sideways and in use on both, web tests 0 failures.
- [ ] Theme editor on the console (2026-11-11, `feat/theme-editor`; issue #14). Settings -> **Edit the theme**
  (`themeedit.c`): the theme in use as a list (list.c with `list_set_side_keys`: Left and Right change the selected
  value, A changes or opens it): its name (keyboard), which palette (day or night: the screens are drawn in that one
  meanwhile), its ten colours, the corners of cards and of rows, the outline width, font (System, Pixel), progress bars,
  dialogs (window or sheet), main menu (buttons or tiles), icon grids (scroll or pages), and Save. While it is open
  everything is drawn with the copy edited (`theme_preview`); the top screen shows a sample (card, rows, button, bar, a
  dialog on its veil). A colour opens `colorpick.c`: a square of saturation and brightness and a bar of hues (textures
  of 64x64 drawn larger, so linear filtering makes the gradients), and a bar of opacity for the veil behind dialogs, all
  for a finger; the D-Pad moves in the square, L/R through the hues, X types #RRGGBB, new over old with its code. A
  theme from the SD card is saved over its file; a built-in one as a new file named after its name ("HOME 2"), and
  is then in use. B with changes asks Save / Discard / Keep editing. New guide step (0.13). Claude, 2026-11-11, Azahar:
  the editor and the picker (Cards and HOME, day and night), a colour changed with a finger seen on both screens, the
  theme saved as `cards-2.json` and in use, B with changes and Discard.
- [ ] Icon grids in pages (2026-11-11, `feat/paged-grids`; issue #14): with the HOME theme (`theme_def.pagedGrids`), every
  list's grid (Titles, Tickets, Ext Save Data, Mods, the Store) is in pages that turn sideways: as many rows as fit,
  dots under them (or "3 / 51" past 20 pages), no scrollbar and no jump arrows. Up and Down stay in the page; Left and
  Right go on to the page beside at its edge, on the same row; ZL/ZR to the first and last item. A finger slides the
  pages and they settle on one (a flick turns one), and the selection comes to the first item of the page turned to.
  Rows are unchanged. New `SITE SWIPE <x1> <y> <x2> [frames]` for tests. Claude, 2026-11-11, Azahar: the Store's 406
  apps in 51 pages, the D-Pad across a page edge, swipes both ways, rows afterwards.
- [ ] Themes from the SD card (2026-11-11, `feat/sd-themes`; issue #14). Each `.json` in `sdmc:/nsa/themes/` is a theme
  (`themefile.c`: colours of day and night as `#RRGGBB[AA]`, radii, border, font (system/pixel), blockBars, sheetPrompts, menu
  `grid`/`tiles`, grid `scroll`/`pages`, over a built-in `base`), listed in Settings -> Theme after the built-in ones, by name, saved as
  `sd:<file>`; Settings reads the folder again each time it opens. A file that cannot be read is left out and the
  Theme row's top screen says which and why. New row **Save the theme as a file** writes the theme in use as
  `<name>-N.json` ("<Name> N"), every key, as a start. Format for users: `docs/themes.md`. New guide step (0.13) in
  Look and sound. Host test `tests/themefile_test.c` (parse, base, errors, write and read back). Claude, 2026-11-11,
  Azahar: a theme on HOME with its own colours chosen and kept after a restart, a broken file reported, the export
  (`ocean-2.json`). To try on a console: a file copied over FTP while NSA runs shows when Settings opens again, the web
  page's theme list, Pixel and HOME as bases, night mode.
- [ ] Pixel theme: digits and B told apart (2026-11-11, `fix/pixel-font-digits`; issue #14, a user saw the 5 as an S).
  Baked without anti-aliasing at 10, 12 and 16 px, Pixelify Sans's 5 is an S, its 2 a mirrored S, and B, 8, 6, 9, 3
  differ by a pixel or two. `tools/make_pixelfont.py` now draws 2, 3, 5, 6, 9 and B (and 8 at 12 and 16 px) by hand
  (`REDRAWN`), each in the box of the font's own glyph: flat-topped 5 and 3, a 2 with a flat bottom, 6 and 9 open, B
  square on the left. The atlas in the repository was patched in place (only those cells): rebaking with this PC's
  Pillow moves pixels in every glyph, as the script now says. Claude, 2026-11-11, Azahar: main menu and Store in Pixel.
  To see on a console: versions, sizes and dates at the three sizes. The Z too (`fix/pixel-font-z`): the font's looked
  like its old 2; now a flat top and bottom with a diagonal (seen in Azahar on the Store's Zelda apps).
- [x] Lists open at the top, and jump to the top or the end (2026-11-11, `fix/list-selection-on-load`; issue #45). The Store
  (and Titles) "started in the middle": while a list loaded, the selection stayed on the first item listed (a CIA) and
  followed it wherever the live sort took it. Now `list.c` keeps the selection on its **index** until it is chosen (D-Pad,
  a touch, A, ZL/ZR, an arrow, `list_select`), and only then follows its item (sorting, deleting); an emptied list starts
  over. The Store keeps the app by title across a newer catalogue only when it was chosen (`list_selection_chosen`).
  While a list scrolls (and 1.5 s after), two round arrows float at its bottom right (`widget_float_button`): the top one
  selects the first item, the bottom one the last; each shows only the way the list can still go, and they fade out.
  ZL and ZR do the same on a New 3DS. The guide says it (a step of guide version 0.13 in Getting around, on Titles'
  list; `docs/guide-list-jump`), so whoever updates is offered that chapter again. While that step plays, the list shows
  both arrows even if it does not scroll, and the guide points at them (`coach_points_at("list.jump")`). Claude, 2026-11-11, Azahar: the Store opens on its first app, the arrows after a
  drag, the end arrow and ZL. Not tried: a console with many titles (Titles' live sort), the Store's newer catalogue
  arriving after the copy on the SD, ZR, whether the arrows hide a row's badge too long.
  The owner, 2026-11-11, New 3DS XL (0.9.8 DEBUG, `a41cbab`, with the guide step that shows the arrows): "it works".
  Issue #45 is closed when the next stable version comes out.
- [ ] A .3dsx installed in the HOME Menu (2026-11-10, `feat/forwarder`; issue #46). SD card -> a .3dsx's actions ->
  "Install in the HOME Menu" opens a screen: the top screen shows the banner (drawn from the icon, the name and the
  publisher, or a picture picked with "Choose a picture for the banner", fitted whole on its average colour; "Make the
  banner from the icon" goes back), A installs it behind a progress bar (B cancels). The .3dsx is **converted**, not
  forwarded (the first version, a stub that chained into the Homebrew Launcher title, quit into that launcher and showed
  as it in HOME, as every forwarder does: Luma marks a .3dsx "chainload itself on exit"). See Decisions, "A .3dsx as a
  title". DEBUG `SITE HOMEICON /path.3dsx[|/picture.png]` writes the CIA to `/nsa/homeicon-test.cia`.
  Claude, 2026-11-10, Azahar (Windows): NSA's own .3dsx converted (3.4 MB, RomFS of 0.5 MB, 2.3 s): every hash and the
  three RomFS hash levels checked on the PC, its NCCH booted as a title and ran (fonts and icons from its RomFS);
  installed through the screen, the installed banner decoded on the PC (the drawn one and a picture's); a 9-megapixel
  picture and a file that is not a picture refused before decoding. Azahar cannot install a title over itself
  (`BeginImportProgramForOverWrite` unimplemented: it aborts), so a second install there needs the title deleted first.
  The owner, 2026-11-11, New 3DS XL (0.9.8-dev.7 DEBUG): "it seems to work". Not tried yet: an Old 3DS / 2DS, a big
  .3dsx's time, a .3dsx without an SMDH.
- [ ] System in three groups and hardware tests (2026-11-10, `feat/system-hardware-tests`; issue #43): System's list
  is now Information (Console, Network, Storage, Battery: they only show), Console settings (Date and time, Wi-Fi
  connections, which A on Console and on Network used to open) and Hardware tests (`hwtest.c`). **Buttons and touch**:
  a drawing on the top screen lights each key while held and keeps a soft colour for the ones already pressed
  ("Pressed N of M": 17 keys and directions plus touch on an Old 3DS, 23 with ZL, ZR and the C-Stick on a New 3DS);
  the Circle Pad and the C-Stick show where they are, the 3D and volume sliders their level; the bottom screen draws
  where it is touched (a fading trail, the coordinates). B is under test too: hold it 1 s to leave, or tap the back
  arrow. **Motion sensors**: accelerometer (in g, 512 raw units each) and gyroscope (in degrees/s, HID's coefficient)
  with a graph of the last 2.5 s; A or the button records every frame to `sdmc:/nsa/logs/motion-<date>-<time>.csv`
  (ms, raw values, g and dps), with sleep mode off while it records (it goes on with the lid closed); leaving stops it.
  The guide's Wi-Fi and Date and time chapters go through Console settings; the System step of `cleanup` is new (0.12).
  Claude, 2026-11-10, Azahar only (fake sensors: still, Y at -1 g; about 30 rows a second there): the groups, both
  screens in English and Spanish, a recording. To try on the consoles: the C-Stick and ZL/ZR on the New 3DS XL and their
  absence on the 2DS, real sensor values and their signs, a recording with the lid closed, 60 rows a second, the guide
  chapters Wi-Fi connections and Date and time from Settings -> Guide.
  Second round (the owner on the New 3DS XL: "22 of 23" without saying which was missing, A counted on entering, a
  way to see drift): the touch screen has its own pill on the top screen (it was counted but not drawn); keys held when
  the screen opens count only once let go and pressed again; each stick has a cross at its centre, hollow direction
  marks until pushed that way and a box beside it with the centre magnified (25 raw units to the edge, a green ring of
  10, the last 1.5 s as dots, the dot amber outside the ring and "Drift N" under it); the 3D slider (not on a 2DS or New
  2DS XL) and the volume count once taken from one end to the other, with marks at the lowest and highest reached.
  New guide chapter `hwtest` (0.12): System -> Hardware tests and what the screens show. The Chinese texts were
  worded with characters already in the atlas (see Known issues). Claude, 2026-11-10, Azahar: 0 of 25 on entering with
  A, the touch pill, the drift boxes, the chapter's first four steps.
- [ ] Web PIN (2026-11-10, `feat/web-pin-modes`; issue #44 by Phibonacci): Settings -> Web PIN steps Random (a new PIN at each start, as before), Fixed (kept between launches) and None (the page opens without a PIN; what changes something still needs the `X-Pin` header, even empty). A row Fixed PIN shows only in Fixed and opens NSA's own keypad (`pinpad.c`: six digits on the top screen, a key with turning arrows makes a random one). When a browser asks for the PIN and has no valid one, the bottom screen shows it for 15 s (until the page signs in at /api/info). The web page tries to sign in with no PIN when it has none saved. Guide chapter `computer` (new steps, 0.12: 0.11 went out with v0.9.4) and the 10 languages. Claude, 2026-11-10, New 3DS XL: the toast, the keypad and Fixed worked (the owner tried the rows and the keypad by touch). Not tried: None with the page open from a browser; the Old 3DS.
- [ ] Boot logo (2026-11-10, `feat/boot-logo`, `tools/make_logo.py`; issue #15): NSA's own logo while the HOME menu
  starts it, signed with the HOME menu's HMAC key (kept out of the repository, `~/.3ds/logo_hmackey_text`). The owner
  saw it on the 2DS and the New 3DS XL: the square fades in, day turns to night with the app's bars. Left to look at:
  the 3D depth with the slider up on the New 3DS XL (the square comes from Z -80 and rests at +8).
- [ ] "Extract ExeFS" in a title's sheet (2026-11-10, `feat/extract-exefs`): files in `sdmc:/nsa/exefs/<title ID>/`.
  Claude, 2026-11-10: the HOME menu's `.code` (1393164 bytes) through `SITE EXEFS` on the 2DS; the sheet's action and a
  game (with icon, banner and logo) not tried after the fix (one whole read per file).
- [ ] FTP from Dolphin (2026-11-10, `fix/ftp-list-file`; issue #36): browsing hung or loaded for a long time. Seen
  through a logging proxy on the 2DS with Dolphin for Windows (KIO's ftp worker, which opens up to 5 connections per
  server): (1) `LIST <file>` answered 550, and KIO stats every file that way: now one line for the file (`NLST`, `MLSD`
  too). (2) With three sessions opening their data connections in the same few ms, `accept()` failed after a ready
  `poll()` and NSA gave up at once (425, the connection reset): now it keeps at it until the 15 s are up. (3) An idle
  session sent away for a new client could hold an unread command (lost, the client's job hung): it looks once more
  and stays. (4) 5 sessions instead of 3 (KIO fails the job of every 421: a hundred of them per folder), and (5) the
  SOC buffer at 2 MB instead of 1 (with 5 sessions listing at once, socket() failed: "425 Cannot open a data port").
  Claude and the owner, 2026-11-10, 2DS: Dolphin browsed without hanging after (5); 64 MB with music, FTP up/down
  819/606 KB/s, web 910/612 (as before). Left: Dolphin on Linux and on the New 3DS XL; the URL download speed (the PC's
  firewall blocked the test's server); FileZilla and WinSCP as before.
- [ ] Web server and FTP sessions (2026-11-09, `fix/web-tls-hang`; issues #37 and #36 by Phibonacci): (1) forcing `https://` on port 8080 closes at once (a TLS hello is dropped) and a plain `http://` in another tab loads without waiting; a silent connection gives up after 4 s (was 30 s). (2) When all 3 FTP places are taken and one has waited 3 s or more for a command, a 4th client takes its place (the idlest is closed); one under way is never closed, and a 4th within 3 s still gets 421. To try from Dolphin on Linux (browsing a folder opens several connections) and on an Old 3DS, where the owner thinks the real problem is the console saturating with open connections. Claude, 2026-11-09, New 3DS XL: https refused in 0.08 s; with 3 idle FTP connections, a 4th to 7th each got the greeting in 0.1-0.4 s and the server closed the evicted ones.
- [ ] Scrollbars (2026-11-09, after v0.9.4; the owner: taps near a list's right end did not press the row or button): the bar is grabbed only 4 px to its left (the gap to the rows; it was 16) and 14 px to its right. Every list, sheet, popover, prompt and Settings.

**Date and time** (2026-11-09, `feat/date-time`, `source/fbi/datetime.c`, `source/core/datefmt.c`; issue #33 asks for it,
left open until Cosmosx007 says whether the format was the problem). System -> Console opens it (A):
- [ ] The format: Clock (by region / 24 h / 12 h) and Date order (by region / day/month / month/day); by region is
  America month first and 12 h, Japan/China/Korea/Taiwan month first, Europe/Australia day first (the HOME Menu of a
  European console writes 9/11). It changes the status bar, the files' dates, Remote install's history, the Wi-Fi
  copies and the web page's dates (the page now shows the console's times as they are, not converted from UTC).
  Claude, 2026-11-09: on the New 3DS XL 12 h + month/day gave "4:07 PM 11/09/2026" on the top screen, `/api/settings`
  clock24/monthFirst follow; back to by region.
- [ ] Set the time from the Internet (pool.ntp.org, UTC + the zone + summer time; PTMSYSM_SetUserTime as Rosalina; ptm:sets
  is not in the service list and is not needed: time() follows). Claude, 2026-11-09: the New 3DS XL was 93 s slow, set
  right ("El reloj se ha movido +93 segundos"), and it stayed right after NSA restarted.
- [ ] "From the Internet when NSA opens" (off by default): once the Wi-Fi is up (up to 2 minutes), one try.
- [ ] Time zone (31, by country by default: ES -> Madrid) and summer time (by the zone's dates: EU, US, south-east
  Australia, New Zealand rules; never; always). Claude: Auckland in November showed +1 h (right: NZ summer); the
  zone list opens on the zone chosen (the sheets held 24 actions at most: now 40). Each choice list opens with the focus
  on the choice made (the owner's request).
- [ ] Date and time picked by hand (`source/fbi/datepicker.c`, the owner's idea instead of typing): a Date tab with the
  month's calendar (D-Pad a day / a week, L / R or the arrows the month, a tap a day; weeks start on Monday with
  day/month, Sunday with month/day; Sundays in red) and a Time tab with wheels for hours, minutes and seconds (+ AM/PM on
  12 h): drag, tap above / below, or Up / Down; Left / Right pick a wheel; A or Save saves, B goes back. The wheels follow
  the clock until one is turned. New icon `calendar` (only its cell added to the atlases: tools/make_icons.py renders
  with Linux Chrome). Claude on the 2DS, 2026-11-09: calendar and wheels drawn right, a drag of 48 px moved the minutes
  two, the time ticks, the hour turned down and saved gave 15:44, set back from the Internet (+3602 s).
- [ ] "From the Internet when NSA opens" ran on the 2DS at a start (on by a stray press, then off): "moved +0 s".
- [ ] The guide (2026-11-09, `feat/guide-whats-new`): steps carry their own version (INFO_SINCE); a chapter seen before
  plays only its new steps (marked "New" in the bubble) and the steps that move between screens; Settings -> Guide plays
  it whole with "New" on the newest version's steps. New chapter Date and time (0.11); new steps: extract .zip/.7z (SD
  card), resume a saved download, install over keeping the save, the notification light (Remote install), Console opens
  Date and time (Cleanup and System). The owner saw the offer on the New 3DS XL (17 steps: 5 + 5 + 1 + 6).
**The owner's tests of 2026-11-07, evening** (the items ticked below, all on `staging` 6760a75). Changed after them, on
`staging` and tried on the New 3DS XL (DEBUG) unless said:
- Store scripts (`fix/store-script-paths`; tried on the New 3DS XL on 2026-11-08 with `fix/store-empty-extract`): ndsForwarder and YANBF (extract a pack to the card's
  root) and Vapecord-ACNL/ACHHD (extract to `/luma/plugins`) were left out of Universal-DB's list; now they show and
  install. Every file an archive puts on the card must be where a script may write (one in `/Nintendo 3DS`, `/nsa`, a
  `.firm`... fails the extraction before anything is written), entries with `..` are refused, and an install or an
  extraction only takes a file the script wrote. Then (`fix/store-empty-extract`) an extraction that finds nothing is
  not an error (ACHHD's `^VapecordACHHD`, which its zip lacks, failed the install), and apps that are neither a CIA nor a
  3DSX show as installed (green badge) when one of the catalogue's `installed_files` is on the card, or the last file the
  Store's run left. The four installed, showed as installed, and were removed again (the SD card compared equal).
- Transfer toast (`feat/toast-two-lines`): two lines, "FTP, receiving: 3.2 / 12.0 MiB, 650 KiB/s" over the name (it was one line,
  cut at the edge), and a piece of the bar moves when the size is not known: FileZilla says no size before an upload (no
  ALLO), so there is no percentage then. Not seen on a console: the web page's screen view waits while a transfer runs.
- Viewers' buttons (`fix/viewer-drag-focus`): a finger sliding along the row lights the button under it, as the menus do, and
  that one is pressed on release; a finger that went down on the picture or a bar lights nothing. Not tried by hand.
- Audio player (`feat/player-seek-repeat`): an MP3's seek table is made by a second decoder at the lowest priority, so the
  seek bar answers at once (it read the track from the start: 4-5 s of silence on the New 3DS XL; 3:11 of a 4 min, 5.7 MB
  MP3 now plays 0.6 s after the tap, and back to 0:24 too). Repeat with Y or the button right of the volume: off, the list,
  the track (`player_repeat` in settings.json; both ends tried). The track that plays in the background chosen again in Files
  opens its player where it is.
- Test mods for the Game mods items, on the New 3DS XL: `/luma/titles/000400000FB1AE00/` (NSA Test: code.ips, an empty
  patch, and romfs/nsa-test.txt), `0004000000030700/romfs/nsa-test-mod/` (Mario Kart 7: a 2.4 MB file and a text, files the
  game never opens), `0004000000FFFF00/` (no such game: code.ips and romfs/) and the empty `0004000000FFFE00/`.
- [x] The toast during a FileZilla upload (two lines, the moving piece) and a download (the bar fills).
- [x] Slide a finger along a viewer's buttons: the one under it lights; let go on it and it acts.
- [x] The player: the seek bar on a long MP3 (right away and after a few seconds), repeat in its three states, and A on the
  track that plays in the background.
- FileZilla waited 20 s ("waiting for the welcome message", then a timeout) before each transfer: it browses on one
  connection and opens another to transfer, and the FTP server served one session at a time, so the second was accepted
  but not greeted until the first closed. Now (`feat/ftp-sessions`) up to 3 sessions, each on its own thread with its own
  buffer, turn and ALLO; a fourth gets "421 Too many connections" at once. Data still moves one transfer at a time (450
  Busy), listings and SITE answer meanwhile; installs (SITE INSTALL, the web page's) and the main thread's errands (SITE
  SCREENSHOT, TEST) go one at a time. Tried on both consoles (`multi.py` in the session's scratchpad: 11 checks, three
  greeted at once, the fourth refused, a listing during an upload, a second upload refused, a dropped client), a restart
  by SITE INSTALL with an idle session open, and the web page's install. Speed, the two builds one after the other on the
  New 3DS XL (32 MB, twice): up 871/971 before, 943/947 after; down 685/724 before, 773/771 after. 2DS after: 840 up,
  640 down.
- [ ] FileZilla with its default settings (2 connections): browse, upload and download several files; no wait before a
  transfer.

**Music in the background** (2026-11-07, `feat/player-background`; asked on Reddit): the audio player left with **B** while a
track plays goes on behind the other screens; **X** stops it and closes (the hint says so). The main loop calls
`audioview_tick` (the next track when one ends; at the end of the list it lets go). While it plays the top bar shows a play
icon (`UI_STATUS_PLAYER`), and the SD card's folder sheet (X) starts with *Back to the player* (`audioview_resume` pushes the
kept view again). Opening another track replaces it; NSA leaving stops it (`audioview_exit` in the cleanup). The background
music already ducks while channel 0 plays (bgmusic.c). Guide chapter `player` (since 0.8). **Not tried anywhere**: Azahar has
no DSP firmware (sdmc:/3ds/dspfirm.cdc comes from a console).
- [x] Play a folder of MP3s, B: it goes on; the play icon shows; browse Files, Titles, the Store; the next track starts by itself.
- [x] *Back to the player* in a folder's sheet opens it where it is; X there stops it and the icon goes.
- [ ] On the **2DS / Old 3DS**: an FTP upload and a URL download with music in the background against without (`tools/speed_tests.py`,
      back to back). If it costs much, pause the background player while a transfer holds the turn (`xferturn_get`) on an Old 3DS.
  Claude on the 2DS, 2026-11-09: with Settings -> Background music (Shop) on and off, not the audio player: FTP up 64 MB 782 / 946 KB/s (16 MB 859 / 856), down 637 / 624 (608 / 643), URL save 32 MB 961 / 847 (noise). Only the big upload lost (~17 %).

**FTP: REST, and appends that start mid-block** (2026-11-08, `feat/ftp-rest`; tested on the New 3DS XL):
- `REST <offset>` (in FEAT as `REST STREAM`): the next RETR starts there, the next STOR keeps the file and writes from
  there (554 past the end, 501 for a bad offset). A cut download picks up with `curl -C -`, FileZilla resumes too; the
  turn, toast and trace count what is left. Tested with ftplib: an 8 MB file sent and fetched in two parts, SHA-256 equal.
- An APPE or a STOR after REST from an offset that is not a multiple of 64 KB first writes up to one, then flushes:
  SD writes that start mid-block ran at 34 KB/s (APPE onto a 3,158,073-byte file), now 720-800 KB/s (930-1190 aligned).
- [x] curl -C - on the 2DS (2026-11-08): an upload cut at 3.3 MB picked up with SIZE + APPE, a download cut at 2.4 MB
  with REST + RETR; the 20 MB file came back with the same SHA-256. FileZilla sends the same commands; not tried itself.

**Time left in the transfer toast** (2026-11-07, `feat/toast-eta`; built, not seen on a console):
- The FTP and web page toast's second line starts with the time left when the size is known: "FTP, receiving: 3.2 / 12.0 MiB,
  650 KiB/s", then "00:00:14 left · game.cia" (from the average speed since the start, so it does not jump; none in the
  first 2 s). The other ways already showed it on their own screens: console to console, URL and QR downloads and installs,
  copies (data_op), Export CIA and Dump NAND.
- [ ] On the console: the time left in the toast during an FTP upload with ALLO (curl), an FTP download and a web page upload,
  in Spanish and in Japanese or Chinese; a FileZilla upload (no size) shows none.

**Transfer toast and the web page's QR code** (2026-11-07, `feat/transfer-toast`; asked on Reddit: a progress bar while files
come in or go out, and an easier way than an FTP app to reach the console from a phone):
- Any FTP or web page transfer that holds the turn (uploads, and downloads of 1 MB or more) shows a toast at the bottom:
  "FTP, receiving, 650 KB/s: game.cia" with a bar when the size is known (else how much so far). `xferturn_incoming` says the
  direction; `xferturn_notice` (core/xferturn.c) comes before the FTP server's own notice, which now only shows while it
  installs. Tried in Azahar (a 160 MB upload through the web page: "Web, receiving, 3.7 MiB/s").
- Settings -> Web server, while it runs, draws a QR code of `http://IP:8080/?pin=PIN` beside a shorter help text. The page
  signs in with `?pin=` and takes it out of the address bar at once. The encoder is NSA's own (`core/qrencode.c`: byte mode,
  level M, versions 1-10, mask by penalty); `tests/qrencode_test.c` reads back every length up to version 10 with quirc, and
  the code drawn in Azahar was read back from a screenshot. Tried in Azahar: the page opened signed in from the link.
- [ ] On the console: the toast during an FTP upload and download (FileZilla) and a web page upload; it goes away at the end.
- [x] A phone's camera opens the page from the QR code, signed in; the address bar has no PIN afterwards (the owner, 2026-11-07:
  the QR code and the web page are good).
- [x] Night mode: the QR code keeps its white square (quiet zone) on the dark card.

**Selecting several files** (2026-11-07, `feat/file-selection`; asked on Reddit by two users and in issue #31): Files has a
selection mode: the check tool in the title bar, **Y**, or *Select several* in a file's or folder's sheet (that item ticked).
Inside it A or a tap ticks or unticks (a folder does not open), Y ticks all or none, X (or a hold) opens the sheet for the
ticked items (*Install the selected CIAs* / *and delete* when CIAs are ticked, *Copy*, *Cut (move)*, *Select all*,
*Select none*, *Delete*), B leaves (after clearing a search). Ticked rows have a box and a soft accent; the top screen says how
many are ticked, how many are folders and the size of the files. Going to another folder leaves the mode.
How: `file_info.marked`; `populate_files_data.filterTopOnly` (the filter picks among the base folder's own entries, what a
kept folder holds is all listed); the clipboard holds a folder plus names (`clipboard_set_names`, "contents only"): a paste
copies only those (`clipboard_filter_names`), a cut on the same medium renames each, on another medium deletes only those after
the copy. Copying a selection into the folder it came from is refused (the paste would delete each file before copying it onto
itself). `action_delete_selected`, `action_install_cias_selected`. Guide chapter `select` (since 0.8).
- [x] Y, the check tool and *Select several* enter the mode; A ticks, Y all/none, B leaves; the title says "N selected".
- [x] Copy 3 files and a folder, paste in another folder: only those arrive, the folder with all it holds.
- [x] Cut and paste on the SD (rename) and from the SD to the CTR NAND or back (copy then delete only those).
- [x] Delete a selection with a folder in it; install 2 ticked CIAs (and "and delete").
- [x] Pasting a copied selection into its own folder says so and changes nothing. (It says so as an error; the owner: fine.)

**Game mods** (2026-11-07, `feat/layeredfs-cleanup`; asked on r/3dshomebrew: "I always forget to delete LayeredFS stuff when I
uninstall games"): Cleanup -> *Game mods* (`source/fbi/mods.c`, `task/listmods.c`) lists the folders of `sdmc:/luma/titles/`
named by a 16-digit title ID, with the game's icon and name, size, files and contents (romfs, code.ips/bps/bin, exheader.bin,
locale.txt, other). Each says whether its game is still there: *Installed* (SD or NAND), *Game card* (the CTR card in the slot),
*Update only* (not installed but its update or DLC is: most likely a card that is out) or *Not installed*. Filter (game on the
console / not installed), sort (name, size, ID), grid, search, remembered in `settings.json` (`mods`). Sheet: *Delete mods*
(that folder) and, once the list is loaded, *Delete mods of games not installed* (never a system title's: Luma patches some, and
a card that is out looks uninstalled, which the question says). Deleting a game or demo from Titles (also from the web page)
that has a folder there asks whether to delete its mods too. Guide chapter `mods` (since 0.8: a console already on 0.8 is not offered it at start, it is in Settings -> Guide). Translated into the 10 languages
(zh only with characters already in `hanfont.png`).
- [ ] The list on a console with mods: names and icons, sizes, the states (install a game, delete it, take a card out).
- [x] *Delete mods* and *Delete mods of games not installed* delete only those folders; the list updates.
  Claude on the New 3DS XL, 2026-11-09: *Delete mods* on a test mod (romfs/ + code.ips) and *Delete mods of games not installed* deleted only that folder, the empty ones stayed; the list updated. Its question says "los mods de 1 juegos" (plural for one).
  Claude, 2026-11-09 (fixed, 0.9.4-dev): one game now says "los mods de 1 juego que no está instalado" (a singular string in the 10 languages); seen on the 2DS with one fake mod, answered No.
- [x] Deleting a game with mods from Titles asks, and Yes deletes `/luma/titles/<ID>/`; a game without mods asks nothing.
- [x] No `/luma/titles` (or empty): the screen says "No mods in /luma/titles." and nothing breaks.
  Claude on the New 3DS XL, 2026-11-09: /luma/titles empty: "No hay mods en /luma/titles.", nothing broke.

**Empty mods folders** (2026-11-07, `feat/mods-empty-folders`; the owner's New 3DS XL has 22 folders in `/luma/titles`, all
empty, so every game seemed to have mods): a folder with no files is no mods. They are hidden by default and one row at the top
stands for them all ("22 empty folders", folder-off icon; its sheet: *Show empty folders*, *Delete empty folders*). Filter ->
Show has a third chip, *Empty folders* (settings `mods.states` bit 4). Shown, they carry the tag *Empty* and the sheet of any
row has *Hide* / *Delete empty folders*. *Delete mods of games not installed* leaves the empty ones out, and deleting a game
from Titles no longer asks about an empty folder. The owner decides whether hidden or shown reads better.
- [x] The New 3DS XL: the list shows only the summary row (or the real mods); A on it: show them, delete them all.
  Claude on the New 3DS XL, 2026-11-09: two empty test folders and one mod: the row "2 carpetas vacías" on top; A on it, *Delete empty folders* deleted both.
- [x] Filter -> Empty folders on and off; the setting is kept when the screen opens again.
  Claude on the New 3DS XL, 2026-11-09: off, the summary row; on, the folders tagged *Vacío*; kept after reopening.

**Deleting a folder that holds folders** (2026-11-07, `fix/delete-dir-trailing-slash`; the owner, deleting the mods of a
game not installed: "Failed to delete the mods", C8804478 not found): the console's FS refuses
`FSUSER_DeleteDirectoryRecursively` on a path that ends in `/` once the folder holds another (an empty one or one with only
files goes; tried on the New 3DS XL through the web API). `fs_delete_dir_recursively` drops the slash; the mods, a folder
cut and pasted onto another medium, the web page's delete and a store script's `rmdir` use it.
- [x] Mods: delete the test mods of 0004000000FFFF00 (romfs/ and code.ips), alone and with *Delete mods of games not installed*.
  Claude on the New 3DS XL, 2026-11-09: 0004000000FFFF00 (romfs/ with a file, code.ips) deleted alone and, made again, with *Delete mods of games not installed*.
- [ ] Files: cut a folder with folders in it and paste it onto another medium (the NAND, a save): the source goes.

**Shared UI pieces** (2026-11-07, `refactor/ui-structure`; the owner: screens, dialogs and bars each re-implemented the
same things): nothing should change on screen. What was copied now lives once: the items of a list (`widgets.c`: row and
tile frames, loading skeleton, fade-in, count badge, right tag, label/value cells, the "Loading..." card), the title bar's
search and grid/rows tools with R/B/Y (`list.c`), the "N of M" count, a list's loading task (`task/uitask.c`), the chain of
a grouped game (`chain.c`), the keyboards' keys (`core/ui/keys.c`), key repeat in popovers and sheets, the viewers'
buttons, and the progress views of data operations (`info_task_finished`, `data_op_progress_items/bytes`).
`tools/ui_snapshots.py` gave the same 49 pictures in Azahar before and after (but for blinking carets), and a folder with
subfolders was deleted from Files there (progress, "Deleted.").
- [x] Titles and Tickets grouped: a game with an update or DLC opens its chain; A on each row; delete one from there.
- [x] Install CIAs and tickets, paste a copied folder, delete a selection, export a CIA: the progress, B cancels, the end.
- [x] The search keyboard and the text editor's (pages, shift, held D-Pad and X; a key held with a finger repeats).
- Then (`refactor/ui-structure-2`): the viewers' buttons, their touches and card, the keyboards' touches, the icon of
  an item without one, the scroll clamp of popovers and dialogs. Same 49 pictures in Azahar.
- [x] The image, audio and text viewers: tap their buttons, slide off one before letting go (nothing happens).
- Then (`refactor/ui-structure-3`): deleting one ext save data or system save data runs through `action_delete_one`
  (`action/deleteone.c`); the 27 data operation suspend/restore callbacks that only returned 0 are gone (a NULL one is
  skipped, the result is the same). Copy, paste and delete a folder tried in Azahar.
- [ ] Delete one ext save data: Cleanup and data -> Extra save data, A on one you do not need (or a title's sheet ->
  Extra data -> Delete): red question, "Deleting...", gone from the list, notice; No keeps it. System save data (Advanced)
  is the same code: only *No* is worth trying there.

**The owner's tests of 2026-11-07, afternoon** (`fix/owner-feedback-1107`): grouped views, deleting titles and held
D-Pads in sheets are good (ticked above). Changed after them:
- Tickets has the tile size tool (zoom icon, SELECT), now shared with Titles and the Store (`list_add_tile_tool`); SELECT
  in Titles no longer opens the filter too.
- The Store marks an app installed by a script (a CIA from a .7z) once the install has ended: it looked too early, while
  the install still ran in its own screens.
- Keyboards: caps lock fills the shift key; the caret does not blink while it moves; X and Y repeat while held.
- Deleting a big folder or its contents: only the top level is read (each folder goes in one call), every entry of a
  folder is read (it stopped at 1024), no extra card call per file; B while reading stops without asking.
- speed-log.txt now has a line per second of each copy/install of a data operation (`copy`) and the time of each Store
  extraction (`extract`): the Store install of 3D Pinball (a .7z with a 963 KB CIA) was slow and the cause is not known.
- [x] Install 3D Pinball (or another .7z CIA) from the Store again: is it still slow? Then fetch /nsa/speed-log.txt.
  (Slow while the Store's status scan ran; fixed below.)
- [x] Big folders read and delete fast now (the owner, 2026-11-07).
- [x] The zoom icon and SELECT change the tile size in every grid: Titles, Tickets, Ext Save Data, Game mods, Store
  (`feat/tile-tool-all`); kept when the screen opens again; the mods summary tile at the smallest size.
- The Store's 3D Pinball install (New 3DS XL, 15:31-15:34): download 1.7 s, extraction 5.4 s, then 66 s before the
  install started (reading the CIA's info, AM) and 59.6 s to install 963 KB (AM_StartCiaInstall 4.9 s, the first write
  23.9 s, AM_FinishCiaInstall 26.5 s), with the UI at ~10 frames a second. The same Files install of a 570 KB CIA right
  after: 1.6 s (2.0 s reinstalling), 60 fps. So not AM or the install code: something loaded the console then; the web
  page had been used just before (music, an 8 MB upload). To try: the same Store install with the web page closed.
- **Why it was slow, found** (2026-11-07, `fix/store-scan-sd`): the Store's background scan of which apps are on the console
  called `stat()` twice for each app with a .3dsx (`/3ds/<name>/<name>.3dsx`, `/3ds/<name>.3dsx`): 574 calls of ~185 ms
  each on the New 3DS XL, 107 s after opening the Store. All that time the SD card answered everything else slowly (a
  6-byte FTP read 323 ms instead of 5). Pinball installed during the scan: extraction 5.2 s, 24 s before the install
  began, install 17.5 s (~40 s in all); after the scan: 0.6 s, 2 s, 2.2 s (~6 s). The scan now reads `/3ds/` once (30
  entries, 98 ms) and only calls `stat()` for an app whose folder is there: 1.07 s for 411 apps, and Pinball installed
  right after opening the Store took 6.7 s. Tried on the New 3DS XL (DEBUG); the "on this console" filter still shows
  the same 8 apps.
- [x] The Store on the 2DS: the scan's time in `store-log.txt` ("status of N apps found in ... ms") and an install soon
  after opening it.
- The 2DS's screen going off and on, Wi-Fi gone for a moment: sleep-log.txt has 26 sleeps since it started, 12 of them
  under 3 s (sleep, wake 1.2 s later). The system sends them (NSA never asks to sleep): on a 2DS that is the sleep
  switch. Wi-Fi is down for a moment after any wake, then NSA reconnects (acInit). Likely a worn or bumped switch.
- [x] Delete the contents of the big Metroid Zero Mission port's debug folder: how long the reading and the deleting take
  (the owner, 2026-11-07: good).
- [x] An install or a copy while the console goes to sleep or HOME (suspend and resume) goes on afterwards.

**Sort both ways, hold A, the clipboard, the picture viewer** (2026-11-07, `feat/sort-both-ways`; the owner asked; Files'
hint, the reversed name sort and Left/Right in the viewer seen in Azahar):
- Every *Sort by* (Titles, Tickets, Files, Ext Save Data, Game mods) turns round when its option is chosen again, as the
  Store's did: a chevron on the chip says which way, `sortReverse` in each screen's settings. Only the sort's own key turns:
  folders stay first, titles without a name last, the summary row of mods on top, and ties keep their order.
- Files' hint reads *A open   hold A actions   X folder   B up* (L and R are the title bar's filter and search icons), and
  the guide says it: hold A on a file, or a finger.
- The guide says the clipboard stays after leaving the SD card (it was always so: it empties only after pasting a cut, or
  when NSA closes), to paste on the CTR NAND for example.
- The picture viewer: Left and Right go to the folder's other pictures when not zoomed in (zoomed, the D-Pad still moves it).
- [x] Each screen's sort, both ways, kept when the screen opens again.
- Titles listed every title again when the sort changed (the owner noticed the wait); it now sorts what is loaded in place,
  as Tickets, Files, Ext Save Data, Game mods and the Store already did (`fix/titles-sort-in-place`; seen on the New 3DS XL:
  196 titles by size at once, the selection kept).
- [x] The hint fits in German and Russian; the viewer with Left/Right and the Circle Pad.

**Delete a title's mods from Titles** (2026-11-07, `feat/title-delete-mods`; the owner asked for it): a title with something
in `/luma/titles/<ID>/` has *Delete mods* in its sheet, right before *Delete* (a game card's too). To try it, a dummy CIA
"NSA Test" (`000400000FB1AE00`, devkitPro's hello world) with a test mods folder.
- [x] Titles -> NSA Test -> *Delete mods*: asks, deletes, and the action is gone the next time.
  Claude on the 2DS, 2026-11-09: on the test title 000400000FB1AF00 (installed by URL; NSA Test is not on the 2DS): "¿Borrar estos mods? El juego se queda, sin ellos." with the folder, "Mods borrados.", /luma/titles empty, the title kept; the sheet no longer has it.
- [x] Deleting NSA Test with mods asks whether to delete them too (the owner, 2026-11-07).
- [x] Now asked *before* deleting, right after *Delete the selected title?*, in red (`feat/title-mods-ask-first`; the
  owner was unsure which order reads better): Yes deletes the title and then the mods; No keeps the mods.

**Shorter filters, clearer sheets, D-Pad held** (2026-11-07, `feat/compact-overlays`; the owner: the filter card grew too long
on the small screen, the folder's and the item's sheets looked alike, and a held D-Pad did not run through the actions):
- Every filter option is an icon now (`popover_display` makes a group icon-only when all its options have one; new icons for
  Type, Region, Group, In use, Hidden, the Store's Type and Updated). The card is as wide as the screen (304 px); a group's name
  and its icons share a line when they fit (Titles' five groups fit with no scroll). The focused icon's name is in a tooltip
  above it (room is kept over the first line for it).
- Files' sheets have a header that says what they act on: *This folder (where you are)* with the folder's name (or *Root of the
  SD card*), *File* / *Folder* with the item's name and type icon, or *The ticked items* with the count
  (`sheet_display_list_for`, a kicker in the accent colour over the name, and a rule under it).
- In a sheet and in the filter card a held direction moves on and on (400 ms, then every 90 ms, as in the lists) and stops at
  the ends; a fresh press still goes round.
- Game mods: A opens the sheet (only a tap did), and its hint bar has the button glyphs again.
- Region shows one-colour flags (NSA's own `tools/icons/flag-eu|us|jp.svg`: the ring of stars, the canton and stripes, the
  red disc) instead of letters (`feat/region-flags`, the owner's idea).
Tried in Azahar (Titles' and Mods' filters, the sheets of the root, a file and the empty folders, a held Down).
- [x] The filter cards of Titles, Tickets, Files, Store, Ext Save Data, Pending titles: readable, the tooltips say the right names
  (the owner, 2026-11-07).
- [x] Files: the three sheets (X in a folder, an item's, the ticked items') tell themselves apart at a glance.
- [x] Holding Up/Down/Left/Right in a long sheet and in a filter card.

**Updates start sooner** (2026-11-06, `fix/update-start`): the first request of a download (`http_probe`: where the file is,
how big) ran on the caller's thread, on the system core, which an Old 3DS shares at 5% while a transfer turn is held; it now runs
on the app's core, as the fetches do, and the update check too. Measured on the 2DS (DEBUG built as 0.8.3-beta, offered v0.8.4):
"Yes" to the CIA downloaded 44 s -> 10 s (the probe 36 s -> 2.5 s); the check 6.3 s -> 4.7 s (reading the releases 1.1 s ->
0.36 s). `transfer-log.txt` now says how long each probe took and the update check's times.
- [ ] On the New 3DS XL: an update from Settings and an install from a URL start as before (no slower).
- [x] An install from a URL on the 2DS starts within a few seconds too (the same probe).
  Claude on the 2DS, 2026-11-09: a 2.5 MB CIA from this PC (`speed_tests.py --only url --cia`): probe 40 ms, downloaded and installed in 5.8 s.

**No seed lookup for titles without one, no probe for small downloads** (2026-11-06, `fix/seed-probe`): after installing a CIA,
NSA asked Nintendo's server for its seed whatever it was (as FBI did: a 404 for anything not from the eShop, 2.4 s on the 2DS);
now it asks only when the first content's NCCH header says the title uses one, or when the header could not be read (an
encrypted content). A download into a small buffer (under 4 MB: the releases, the Store's info, a seed) skips the first request
that looks for several connections. Measured with DEBUG builds offered v0.8.4: the update check 4.7 s -> 3.0 s on the 2DS (0.7 s
on the New 3DS XL), and no seed request after the install on either.
- [ ] A game from the eShop that uses a seed (installed from a CIA with readable contents) still gets its seed and starts.

**Guide and scrolling dialogs** (2026-11-06, `feat/guide`; DEBUG 0.7.3 tried in Azahar, in Spanish, Cards theme: the offer at
start, Getting around, Your main menu, Store, SD card and From your computer played through, a chapter started from Settings -> Guide,
START on the main menu, and a long message scrolled with the D-Pad and a finger)
- [ ] A console that ran NSA before (settings.json without `guide_known`) is asked once at start "New in this version": Your
  main menu and Store, with Getting around chained before them while not seen; the top screen lists what it will play with the steps
  of each, and X "Everything" adds every other chapter not seen. Any answer, Later too, is not asked again.
  Claude on the 2DS, 2026-11-09: 2DS (from 0.9.2-dev.116 with guide_known 9): at start "Novedades de esta versión: - Conexiones Wi-Fi" with Verlo / Todo / Luego; the top card lists Moverse por NSA (10 steps, not seen) before Conexiones Wi-Fi (8) and "X Todo: también los otros 14"; Luego moved guide_known to 10 (a console with `guide_known`, not one without).
- [ ] A first start (no settings.json) is welcomed with the five essential chapters (Getting around, Your main menu, Titles, SD card,
  Remote install).
- [ ] The tours: each chapter first goes back to the main menu by itself (B until only it is open); the rest of the screen is dimmed
  and only what the bubble asks for gets through (the button, a tap on what is framed, the D-Pad to bring the focus there); an
  explanation goes on with A or a tap on the bubble (a tap elsewhere does nothing). A bubble goes next to what it points at, or on the other screen when that fills its own
  (the actions sheet, a list, the top screen). START leaves at any time without reaching the screen (it would exit NSA on the main
  menu). Something not on the screen (an entry hidden from the menu, a title with no save) says so after a moment and A goes on.
  A chapter counts as seen when its last step is done; the update check runs after the guide.
- [ ] Opening the actions of a game that has updates or DLC (2026-11-06, the owner hit it: the list of updates opened and the
  tour waited for the actions): the bubble now asks to open the game itself, and to go back from that list after the actions.
  Not tried: Azahar has no such game (there the extra steps are skipped, which works).
- [ ] Every chapter on the console, in both themes with a different menu order (the anchors follow the layout) and with the HOME theme
  (tiles drawn differently): Titles (L popover, Y view), Remote install, Saves (a game with a save), Console to console, Look and sound,
  Cleanup and System, Updates.
- [ ] The Store with no copy of the catalogue on the SD card (`sdmc:/nsa/store-cache.json` deleted): a ring of dots turns in
  the middle of the bottom screen, "Loading the catalogue..." under it, until the catalogue is here (2026-11-06; seen turning in
  Azahar, which never gets it). `list_set_busy` gives the same to any list that loads in the background; the same wheel
  (`widget_busy`) turns in the "Loading" dialogs before their bar (it was three blinking dots) and over "Loading..." in the
  picture viewer: both too quick in Azahar to see.
- [ ] The 0.8 texts (Store, guide, menu editing, receive choices...) in the nine other languages (2026-11-06, translated in one go; only
  Simplified Chinese seen in Azahar: the guide's offer and a bubble). Simplified Chinese's font now has 587 characters in a 512x924 atlas
  (cells of 28 px: `tools/make_hanfont.py` must keep it within 1024 high, or NSA freezes at start in that language).
- [x] Settings -> Guide: "Everything not seen yet (N)", every chapter with New or a tick, "Start again" (asks); the top screen shows the
  summary, steps and the version it came with. The Settings row shows "N of 12 chapters seen".
  Claude on the New 3DS XL, 2026-11-09: "Todo lo que no has visto (15)", New on each chapter and a tick on the one seen, the top screen's summary, steps and "desde NSA 0.8"; "Empezar de nuevo" asks; the Settings row says "1 de 16 capítulos vistos" (16 now).
- [ ] Every dialog whose message is taller than its card scrolls it above the buttons (D-Pad / Circle Pad up and down, a finger
  dragging the text, or its scrollbar); short ones look as before. Check a sheet (HOME theme) and the Pixel theme.
- [ ] The texts of the chapters fit their card on the console's own font, in English and Spanish (only Spanish has the new strings).
- [ ] `tools/console_tests.py` on a fresh `sdmc:/nsa/`: the guide's question comes up at start; answer it once before the run.

**Editable menus and sliding focus** (2026-11-05, `feat/store`; tried on the New 3DS XL by the owner)
- [x] Main menu: three big tiles, the rest as one row of icons; the focused icon shows its name in a tooltip that slides in. Advanced is red as an icon and as a tile.
- [x] SELECT or a long press edits the menu: entries jiggle; A picks one up and the D-Pad moves it; X hides, Y puts it back as it came; hidden ones wait in a tray at the bottom (tap one to bring it back). By touch an entry is dragged and the layout follows the finger live: the others make way, and it is a tile over the tiles and an icon over the icons. System and Settings can not be hidden. Kept in `settings.json` (`menu`: ids shown, `|`, ids hidden).
- [x] Remote install: the four tiles can be reordered the same way (the history row stays below); `remote_menu` in `settings.json`.
- [x] A finger sliding over the buttons of the main menu, Remote install, a list with nothing to scroll or an action sheet moves the focus to what is under it; letting go there opens nothing.
- Only Spanish has the new strings.

**Store** (2026-11-05, branch `feat/store`, `source/fbi/store.c`; builds, not run in Azahar or on a console)
- [ ] Main menu -> Store downloads Universal-DB's UniStore (`https://db.universal-team.net/unistore/universal-db.unistore`, ~0.9 MB, kept as `sdmc:/nsa/store.unistore` and used when there is no connection) and lists the 3DS apps that publish a CIA or a .3dsx with a plain "downloadFile" first step (about 320 of 447 on 2026-11-05: 260 CIA, 280 .3dsx). Zips and multi-step scripts are left out. The top screen shows the selected app (author, version, description, sizes, category); R or the tool searches.
- [ ] A opens it: a CIA only -> asks and installs through the Remote install screens (`action_install_url`); a .3dsx only -> downloads to `/3ds/<name>/<name>.3dsx`; both -> a sheet to choose.
- [x] Memory: parsing ~1 MB of JSON on an Old 3DS / 2DS; leaving the screen while it loads (B).
  Claude on the 2DS, 2026-11-09: Universal-DB, 426 apps: downloaded and read in 8.4 s, on screen at 18 s with the icon sheets built; B while "Cargando el catálogo..." leaves at once, memory as before. Next time the SD copy: 4.6 s. The first copy read gave -1 (written by 0.9.2-dev.116, older STORE_CACHE_FORMAT: expected).
- Not done: DS apps, `downloadRelease` (the latest GitHub release: 13 apps, left out).
- [ ] **Universal-DB's scripts** (2026-11-06, `feat/store-scripts`, `source/fbi/storescript.c`, `source/core/unarchive.c`): the
  Store follows each file's steps as Universal-Updater does (downloadFile, extractFile from a .zip through zlib or a .7z through the
  LZMA SDK in `source/libs/lzma`, installCia, deleteFile, move). Tried on the New 3DS XL (DEBUG): a plain .3dsx goes straight to
  `/3ds/<name>.3dsx` after a yes/no (no Install/Save sheet), 3D Pinball's CIA out of a .7z installed and its files deleted, 3DS
  Quick Reboot's .3dsx out of a .zip. Still to try: an app whose CIA and data both come out of one zip, Cancel during
  "Extracting...", and an archive that does not hold what the script names (Cirrus's 3DSX zip: it must say it failed and delete
  the download).
- [ ] **Stores of your own** (2026-11-07, `feat/unistores`): the Store opens on a list of stores (Universal-DB, with its credit to
  Universal-Team, and the user's, in `sdmc:/nsa/stores.json`, files in `sdmc:/nsa/stores/<key>/`); add by QR code or link,
  change the link, delete. Every entry of a store shows now, for any console, with each download it offers (a third type in the
  filter, "Other"), and scripts run step by step: promptMessage (No skips `count` steps or stops), skip, mkdir, rmdir (asks
  first; only a folder below the card's root, and of /luma only /luma/titles/<id>/...), downloadRelease (the latest GitHub
  release's file, found by its pattern through the API). Scripts may now write in /luma/titles/ (game mods), nowhere else in
  /luma, and no *.firm. Tried on the New 3DS XL (DEBUG): the list and Universal-DB's card, a store checked and deleted, the QR
  scanner opens, LinuxCat's Store (icons from its one sheet, a .gba into /roms/gba/, a zip extracted and cleaned up),
  opentitus-3ds's 3DSX answering No to its question (the release found and downloaded, extracted, moved). A zip now extracts
  60 times faster (writes gathered in 256 KB: 5.4 MB in 110 s -> 1.8 s), .7z files too. Still to try: scanning a real QR
  code, a mod store (Minecraft or Paper Mario: rmdir's question, /luma/titles), a store's CIA through a script (RetroArch
  Cores), TWiLight Menu++ Skins (517 entries) on the 2DS for memory, and the guide's Store chapter.
- [ ] **Big .7z** (2026-11-06, v0.8.3): a block packed with LZMA, LZMA2 or stored is decoded a piece at a time and each file written
  as it comes out (its CRC checked), so only the dictionary is in memory (no bigger than the block; on the heap, else the linear
  heap): HoriHD-Gallery (36 MB .7z, a 43 MB block, 16 MB dictionary) ran out of memory before on the New 3DS XL and now works on
  it and on the 2DS (the 2DS: about 1 min 30 s to extract). Other methods (BCJ, PPMd) still unpack the whole block in memory.
  The bar follows the archive's bytes read. The extraction runs below the UI's priority (0x38; core 2 on a New 3DS): the owner
  saw the screen stop during the extraction before that change; check it now moves smoothly.
- [x] Hints (2026-11-06, `fix/hints-i18n`): the main menu's " open   START exit" had lost its A glyph in the code, so it
  showed in English with no button in every language; Settings' hint on a row with a volume slider had no translation. Both seen in
  Spanish in Azahar.
- [ ] What is on the console is marked (a check on the row, the grid and the top card; "On this console: CIA, 3DSX"), with no
  version and **no "update available"** (removed 2026-11-06 at the owner's request): the console does not keep Universal-DB's version
  number, so the Store could only compare with what it installed itself (`sdmc:/nsa/store-installed.json`), which an app installed another way
  or updated by its own updater leaves behind. That file now only tells whether an app with no unique ID was installed by the Store.

- [x] **Dates, sort both ways, category icons** (2026-11-06, v0.8.4, `feat/store-date`; a Reddit user asked to find apps by date,
  and "Newest" was there but no date showed): the top card says "Updated YYYY-MM-DD" and the rows put the date first while sorted
  by date or filtered by it; the filter has *Updated*: All / 1 month / 6 months / 1 year (`store.updated` in `settings.json`, the
  first day let through worked out from the console's clock); choosing the chosen sort again turns it round (a chevron beside its
  icon, `popover_group.directional` + `isReversed`; `store.sortReverse`); the categories are icons with their name in a tooltip
  (a multiple-choice icon group never shows the "chosen" tooltip). Tried by Claude on the Old 2DS (DEBUG): 35 apps in the last month,
  oldest first turned round, the tooltips. Simplified Chinese's atlas: 589 characters (月 and 年 added).
- [x] **Leaving with the Store open** (2026-11-06, v0.8.4): NSA restarting after an install (`SITE INSTALL`, the updater) or exiting
  while the Store was on screen left its threads running on freed memory: a data abort reading `store->cancelled` in the icon thread
  (Luma dump on the Old 2DS, v0.8.3 at 0x13ee08). `store_exit()` now cancels them and waits (at most 3 s) before the cleanup; tried on
  the Old 2DS: a `SITE INSTALL` from inside the Store restarted cleanly (61 ms waited).

**Transfer cards and readable URLs** (2026-11-04, `feat/transfer-info`; DEBUG on both consoles; seen on a real 40 MB send New 3DS XL -> 2DS, except the first three):
- [x] Receiving: the top card shows the file, "From <console> (size)" and "Saving in <folder>" (or "Installing it on this console" / "Importing it into the title"); sending shows the file, "To <console>" and the size. Seen on the consoles.
- [x] The question to a QR / typed / received URL shows the file name whole (over several lines) and, for one link, the whole address, with %XX turned into characters. Seen on the New 3DS XL with a link made of %20.
- [x] History: names and links on its rows and on the top card are decoded and the top card shows each link whole, over several lines (the stored address keeps its escapes, so "install again" asks for the real one); the summary on Remote install keeps its lines inside the card. Seen on the New 3DS XL; the 2DS still has the build before this.
- [x] Wrapped text: a word that fills a whole line (an address with no spaces) is cut where the line ends; before it left an empty first line, so a link with no spaces started one line lower than one with spaces (`screen_wrap_string`, used by every wrapped text). Seen on the New 3DS XL in History; other wrapped texts were not compared.
- [ ] The install progress screen shows the decoded name.
- [x] `tools/console_tests.py`: its rules answer "Incoming file" / "Existing files" as prompts; they are list sheets now (named after the screen under them), so the rules need updating before the next run.
  Claude, New 3DS XL -> 2DS, 2026-11-09: SITE TEST UI now describes sheets (kind "sheet"; sheet_set_id names the two), the script picks the row with the D-Pad and A and reads buttons and texts through the i18n tables (the consoles are in Spanish). All 33 tests pass.
- [ ] Not done: a preview of the icon of an incoming CIA / .3dsx. A CIA keeps its icon at the end and a .3dsx near it, so the sender has to send it: a protocol 9 (hello + icon) that both consoles must run.
- [ ] A 15 s "io: nothing for 15 s" stall killed one send while two screenshots were fetched over FTP mid-transfer; it did not come back in the reruns. Watch for it.

**Receive menus as lists** (2026-11-04, `feat/receive-list`; DEBUG 0.7.0 installed on the New 3DS XL only, the 2DS was off the network; the screen itself was not seen: `fake_peer.py send` got its connection reset before NSA logged anything):
- [ ] An incoming file / CIA / folder / save from another console shows a list sheet ("Install now", "Save", "Reject", "Import now", "Save in a folder", "Original path") with a detail line per row, the file name and size as its title, and the transfer card on the top screen; A or a tap chooses.
  Claude, New 3DS XL -> 2DS, 2026-11-09: through console_tests: the sheet for a file (Guardar / Ruta original / Rechazar, each with its detail line, title "payload.bin (16.00 MiB)", the card on the top screen), a folder and a title package (.cia, saved). Install now and the save package not tried.
- [ ] B or a tap above the list rejects: the sender sees "declined" and NSA goes back to waiting.
- [x] "Existing files" (Original path with files already there) is a list too: Overwrite, Skip existing (folders), Cancel (= reject).
  Claude, New 3DS XL -> 2DS, 2026-11-09: origin_folder_overwrite / skip / cancel and origin_file: the sheet comes up and each answer does what it says.
- [ ] Only Spanish has the new strings (the other ten languages show the English ones until translated).

**Web System tab** (2026-11-04, `feat/web-system`; DEBUG build on the New 3DS XL, page opened in the browser: the four cards showed the console's real data):
- [ ] The page has a **System** tab (Tools): Console (model, user, region, country, language, serial, device ID, screens, system version, kernel, MCU, Luma3DS, CIA/3DSX, uptime), Network (SSID, IP, netmask, gateway, DNS, signal, MAC, DHCP lease, FTP and web ports), Storage (SD card made/speed/writable/cluster and the space of SD, CTR NAND, TWL NAND, TWL Photo) and Battery (percent, voltage, temperature, charger, rate, time left, level, history graph). It refreshes every 10 s and keeps its scroll. The cards are all the same size, in two columns at most (one when the window is too narrow for two), with at most four columns of data inside each and can be dragged by their header to change the order (kept in this browser; the refresh waits while dragging). Same data as the console's System screens: `GET /api/system`, read on the main thread (`systeminfo_json`). The battery rate and graph need a couple of minutes of the page open (the history grows only while someone asks). Texts such as "High speed" come in the console's language.

**Web music** (2026-11-04, `feat/web-music`; decoded and looped against a mock server in the browser with a real `music-cache-3.pcm`):
- [ ] The page plays the console's background music (the last song of the style set on the console, from `/nsa/music-cache-N.pcm`, 22050 Hz mono) at the console's music volume; Off on the console (or the page switch "Console music here" in Settings -> This page) silences it; changing the style or volume on the console shows up within 30 s; a new song made by the console is fetched and crossfaded in (3.5 s). The song is kept in the browser (IndexedDB) per style and file date. Needs the console to have made that style once (the cache file exists) and a click or key on the page first (browser rule). It is the same audio, not a second synthesis: the page just reads the file.

**Web page** (2026-11-04, `feat/web-toast-cache`; tried against a mock server in the browser, not on a console):
- [ ] While the console transfers (FTP, a download, another 3DS), the page shows a toast at the bottom with the kind, name, percentage and KB/s and a progress bar, in place of the old text at the top right; it goes away when the transfer ends.
- [ ] Titles: start loading, switch to another tab and back: the names and icons still missing keep loading (they used to stay frozen).
- [ ] Titles: the filter menu has "Group updates and DLC" (Yes by default, remembered), as on the console: an update or DLC whose game is listed hides behind it (while Games or All is shown), the game shows a small round badge with how many hang from it, on the icon's corner like the console, and opening it lists the game and its updates and DLC as cards (icon, name, publisher, description, a medal for Game / Update / DLC, version and size), each opening its own sheet. `/api/title` now also returns `description`.
- [ ] Titles: names, publisher and icons are kept in the browser per console and title version (localStorage + IndexedDB), so the list fills at once on the next visit. Titles screen menu -> Reload forgets them.
 The title's data kinds (save, extra, boss) are not kept: the sheet asks for them (`/api/title/data`) the first time it opens in a session, and the list only asks `/api/title?data=0`.

**Languages** (2026-11-04, `feat/languages`; DEBUG 0.6.5 build installed on the New 3DS XL and driven over FTP: the list, Japanese, Russian and German looked right in the Cards theme with the console's own font; the console was put back on System afterwards. The rest is for the owner's hands)
- [ ] Settings -> Language opens a list (System, English, Español, Català, Français, Deutsch, Italiano, Português, Русский, 日本語, 简体中文, 繁體中文); the highlighted one shows its name and a few words in it on the top screen; A or a tap picks it, everything changes at once and it is still set after a restart; B goes back without changing.
- [ ] The Pixel theme in German (and the other languages with umlauts): at 10 px the pixel font draws the diaeresis faint (seen in Azahar). Russian has no yo: the Russian table writes it as e, because Azahar's font has no glyph for it.
- [ ] Every screen in each language: texts fit their rows and buttons (German and Russian run longest), no tofu boxes. Cyrillic and kanji come from the console's own font: check Russian and Japanese on both consoles, and on a console set to another region if there is one.
- [ ] Chinese: **Traditional** (繁體中文) draws completely with the console's font on the New 3DS XL (an EUR console), because its table only uses characters of JIS X 0208 (checked with `shift_jis`; a few Taiwan forms are written as their Japanese variants, 内 録 緑 ...). **Simplified** (简体中文) needs its own font: about a quarter of its characters (设 软 单 远 请 ...) are not in that console's Japanese character set, so while it is the language `screen.c` draws all of its Chinese from `romfs/hanfont.png` (Noto Sans CJK SC, 528 characters at one size, scaled like the system font; seen working on the New 3DS XL, Pixel theme not looked at). In any other language that font only fills in what the console's font lacks (the name 简体中文 in the language list). Check how it reads at the small sizes and in the Pixel theme.
- [ ] Japanese lines break between characters (dialogs, the top screen's descriptions, the update changelog) and never leave 、 。 」 at the start of a line.
- [ ] System follows the console's language (Settings of the console in French, German, Italian, Portuguese, Russian or Japanese); a console in Korean or Dutch shows English (Catalan is never picked by System: the 3DS has no Catalan).
- [ ] Advanced -> Dump NAND (only on `advanced`): its texts in every language (21 strings: sizes, time left, "Fits, with ... left over", "Saved as", the hints); built, not looked at on a console.
- [ ] The web page's Language drop-down lists all twelve and changes the console; the page itself is still English or Spanish.

**PKSM backups** (2026-11-05, branch `feat/pksm-import`, builds; not tried on a console)
- [ ] A title's Save data list and the import list show PKSM's backups (`sdmc:/3ds/PKSM/backups/0x<unique ID, 5 digits>/<date>/main`, or `sav.dat` for a Game Boy title) with PKSM's icon badge (its SMDH, shrunk to 20x20) and the file's modification time as the name (the date folder's name changed between PKSM versions: `2019112821573`, `2026-11-5_9-7-3`). Restoring one asks the red question; the file lands in the save archive as `main`. Source read from FlagBrew/PKSM (`loader.cpp`: `backupSave`, `idToSaveName`, `load`, `saveToTitle`); the owner's console has such folders (0x01B50/<date>/main, 445440 bytes = 0x6CC00, an Ultra Sun/Moon save).
- Not covered: GBA Virtual Console saves (`00000001.sav`, with their own CMAC), DS cartridge saves (`POKEMON X.sav`), PKSM's banks (below).
- **PKSM banks** (the boxes), researched, not built: `sdmc:/3ds/PKSM/banks/<name>.bnk` (a raw header then 30 entries per box, each a generation byte and the Pokemon's bytes, 0xFF for an empty slot) and `<name>.json` (the box names), plus the index `sdmc:/3ds/PKSM/banks.json` (name -> number of boxes). PKSM lists banks only from that index: copying the two files to another console is not enough, the bank must also be added to its `banks.json`. No whole-bank export in PKSM's UI; individual Pokemon go to `.pkx` in `/3ds/PKSM/dumps` (Storage screen, DUMP). Magic and header size not verified (they live in PKSM-Core). Nothing in the container ties it to a console.

**JKSM backups** (2026-11-04, branch `feat/jksm-import`, builds; not tried in Azahar or on a console)
- [ ] With a JKSM backup (`sdmc:/JKSM/User Saves|Extra Data|BOSS Extra Data/<game name>/<backup>/` with the tree inside, made by JKSM for 3DS), the title's Save data / Extra data list and the import list show it with the violet "J" badge and only the name. Restoring it asks the red question like Checkpoint's. The game's folder is found by its SMDH short description in any language (with `. , / \ < > : " | ? *` turned into spaces, `?` into `_`, as JKSM does), or by the 16 digit ID. Source read from J-D-K/JKSM (`FS.cpp`, `BackupMenuState.cpp`, `StringUtil.cpp`), never run. Also looks in the older folders `Saves`, `ExtData`, `Boss`.
- Not done: JKSM's `.zip` backups; the `._secure_value` file JKSM can write in a backup would be copied into the archive; two games with the same name share a folder; the badge is JKSM's own icon (SMDH of the installed CIA) shrunk to 20x20. Delete from the web page / sheet warns "JKSM will not see it any more".

**Badges of the backups** (2026-11-03; seen in Azahar and on the web page with fake NSA and Checkpoint backups)
- [ ] A title's Save data / Extra data list and the import list show NSA's backups with NSA's badge (the blue window with an N) and only the date, and Checkpoint's with its flag badge (pale flag on navy) and only the name, no "[NSA]" / "[Checkpoint]"; readable selected and not, in day and night themes.

**Red confirmations and going up a folder** (2026-11-03; built, not run)
- [ ] Every delete / erase confirmation (Files: delete, delete contents, folder, CIAs, tickets; Titles: delete, with ticket, erase save, secure value; Tickets; Ext save data; Pending titles) shows the red prompt with the warning icon.
- [ ] Files: B in a folder selects that folder in the parent at once, also in a big folder full of CIAs; a D-Pad or A pressed meanwhile does nothing until it is selected.

**System's details** (issue #23; `source/fbi/systeminfo.c`; only the layout was checked, in Azahar, whose data is fake)
- [x] Main menu -> System opens a list (Console, Network, Storage, Battery); each row shows its page on the top screen; B goes back.
  Claude on the New 3DS XL, 2026-11-09: Consola, Red, Almacenamiento, Batería; each page on the top screen; B back.
- [ ] Console: serial number (read from `SecureInfo_A` on the CTR NAND) matches the sticker; device ID, country, system and kernel versions, MCU firmware (shown as major - 0x10 . minor: check it is plausible), Luma3DS version and commit, "Running as" CIA / 3DSX.
  Claude on the New 3DS XL, 2026-11-09: QEF10498537, device ID, ES, 11.17.0-50E, kernel 2.58-0, MCU 3.65, Luma 13.4.0 (b3282131), running as CIA: all plausible; the sticker was not compared.
  Claude on the 2DS, 2026-11-09: 2DS: AEM12193646, ES, 11.17.0-50E, kernel 2.58-0, MCU 2.37, Luma 13.4.0, CIA: plausible; sticker not compared.
- [ ] Console on the New 3DS XL: the screen types (IPS / TN from `GSPLCD_GetVendors`: vendor 1 = IPS, 0xC = TN is a guess to confirm).
  Claude on the New 3DS XL, 2026-11-09: it says top IPS, bottom IPS: to check against the console.
  Claude on the 2DS, 2026-11-09: 2DS: TN (plausible for a 2DS).
- [ ] Network: MAC, gateway, DNS, DHCP lease and proxy have real values (all empty in Azahar).
  Claude on the New 3DS XL, 2026-11-09: **wrong**: gateway 127.0.0.1 and DNS "-" (the router is 192.168.1.1); MAC (as the router's ARP table), netmask, DHCP lease 23 h 42 min and proxy No are right.
  Claude on the 2DS, 2026-11-09: 2DS: the same **wrong** gateway 127.0.0.1 and DNS "-"; IP, mask, MAC, lease and the servers' ports right.
  Claude, 2026-11-09 (fixed, 0.9.4-dev): the routing table has routes through 127.0.0.1 (the console's own address, 127/8) before the default route (0.0.0.0/0 via 192.168.1.1, the last row): the default route's gateway is taken, never a loopback one. The DNS table wants a 336-byte buffer (EINVAL with 160). Both consoles now show 192.168.1.1 and "8.8.8.8, 206.67.222.222" (the page's System tab too). DEBUG `SITE NETDUMP` writes the raw tables and the CID to /nsa/netdump.txt.
- [ ] Storage: the SD card's maker, product name and date from its CID (the byte order is guessed: both are tried), speed, writable, cluster size.
  Claude on the New 3DS XL, 2026-11-09: **wrong**: "0x15 a8h0T", made 2049-11 (the CID read in the wrong order); speed (high), writable, cluster 32 KiB and the four media's space are right.
  Claude, 2026-11-09 (fixed, 0.9.4-dev): the 3DS hands the CID reversed on both consoles; the unreversed bytes of this Samsung card happened to read as text in the product name. The reversed order goes first and the other needs bytes 1-7 as text: New 3DS XL "Samsung BC2QT, 2017-05", 2DS still "Kingston SD32G, 2016-07".
  Claude on the 2DS, 2026-11-09: 2DS: "Kingston SD32G", made 2016-07, right; so the guessed byte order fails only for the New 3DS XL's card.
- [ ] Battery: percent with a decimal, voltage and temperature from MCU registers 0x0A-0x0D (plausible: about 3.6-4.2 V, room temperature); charging / on the charger / on battery; after 2 minutes on the page the rate and the time left appear and the graph fills; plugging or unplugging restarts the measure.
  Claude on the New 3DS XL, 2026-11-09: 100.0 %, 4.06 V, 23 °C, on the charger and not charging; after 2 min "+0.0 %/h" and the graph starting. Plugging and unplugging not tried.
  Claude on the 2DS, 2026-11-09: 2DS: 100.0 %, 4.04 V, 28 °C, plugged in, level 5/5, "Midiendo...". Plugging and unplugging not tried.

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
  Claude on the New 3DS XL, 2026-11-09: the row shows http://192.168.1.135:8080, the PIN and the QR code; the PIN changed at every start of the day's builds and the server started with them. On/Off not toggled.
- [ ] From the PC: open the address, the PIN screen, wrong PIN (and the lock after 10), browse the SD card, download, upload (a big file, several at once), new folder, rename, delete.
  Claude, 2026-11-09 (0.9.3-dev.38): web_tests.py passes on both consoles (52 checks: no / wrong PIN 401, mkdir, copy, rename, delete, upload, settings, titles, tickets, extra data, screens, input); it needed volume 101 for "out of range" (5 is valid now: it had set the music to 5 %) and to wait out the import still pending (409). The page in a browser and the lock after 10 not tried.
- [ ] Files tab: copy a big folder (the page waits; the console stays usable?), move to another folder, edit and save a text file (UTF-8, CRLF kept as typed).
- [ ] Tickets and Extra data tabs on a real console: counts, names and icons arrive (hundreds of tickets: how long), the text filter.
- [ ] Install a `.cia` from the page (an SD title; NSA itself restarts); the toast on the bottom screen shows it.
- [x] Speed of a big download and upload on the New 3DS XL and the 2DS (the FTP server's for comparison); the console stays usable meanwhile (UI, music).
  Claude, 2026-11-09: 64 MB with speed_tests.py, FTP and the page back to back (byte for byte). The page went 64-85 % of FTP (each send()/recv() moved 5-6 KB): New 3DS XL up/down 797/780 KB/s (FTP 1155/921), 2DS 554/427 (FTP 869/629). With 32 KB SO_RCVBUF/SO_SNDBUF on the listening socket (WEB_SOCKET_BUFFER, as the FTP's passive socket): New 3DS XL 1029/1289 (FTP 1001/1007), 2DS 892/633 (FTP 901/633), 15-20 KB per call on the 2DS; web_tests pass on both. Whether the console stays usable meanwhile is for the owner.
  The owner and Claude on the 2DS, 2026-11-09: 256 MB up 900 KB/s, down 623 KB/s through the page (byte for byte) while the owner went through the menus: no delay, the UI drew 10-13 frames/s in the traces and the music played throughout.
- [ ] The local wireless (Send / Receive without a router) stops the web server and brings it back after; nothing else breaks while it serves.
- [ ] The page in a phone's browser and in Firefox / Chrome (dark and light).
- [ ] Download a save / extra data / SpotPass package from the page on the 2DS: the console shows "Packing for the web page" with its bar, B there cancels and the page says so, the page's card shows packing then downloading, and NSA closes normally afterwards.
- [ ] Cancel *Back up now* with B: no new backup in the list; send an old backup to the other console and import it there.
- [ ] Titles sheet -> Save data / Extra data / SpotPass data (`feat/save-manager`): the list shows the backups (readable in night mode), *Back up now* adds one at the top, restoring asks in red, deleting asks and removes it (a folder holding save and extra data keeps the other); the same from the page.
- [ ] The redesigned page (`feat/web-ux`): Settings from the page change the console at once (theme, accent, night, language, sounds, music and volumes) and stay after a restart; dragging rows onto folders and breadcrumbs moves them; dropping a folder from the PC uploads the tree; *Install a CIA from this PC*; how the titles list feels with many titles on a real console.

**Original path** (`feat/keep-original-path`, issue #24; `tools/console_tests.py --only origin` (8 tests: new, overwrite, skip, cancel, cut and resumed, a single file, unsafe paths not offered) pass on the New 3DS XL <-> 2DS over the Wi-Fi; the owner has not tried it by hand)
- [ ] Files -> a folder's ⋯ -> *Send to another 3DS*; on the receiver *Original path*: with nothing there the folder appears at the same path; with files there it asks "N of M files already exist" and Overwrite / Skip existing / Cancel do what they say; try it with PKSM's folder.
  Claude, New 3DS XL -> 2DS, 2026-11-09: the 8 origin tests pass (sent with SITE TEST SENDDIR, not from Files); PKSM's folder not tried.
- [ ] The same with a single file, and a CIA or a save package (they must not offer *Original path*).

**Sending folders** (`feat/send-folders`; code in `transferfolder.c` and `foldermanifest.c`; `tests/foldermanifest_test.c` on the host and `tools/console_tests.py --only send_folder` (10 tests: clean, name taken, cuts on both sides, write and read failures, 250 small files, no files, unsafe paths, another destination) pass on the New 3DS XL <-> 2DS over the Wi-Fi; the owner has not tried it by hand)
- [ ] Files -> a folder's ⋯ (or the open folder's sheet) -> *Send to another 3DS*: a small folder (a few files, subfolders, an empty file, an empty subfolder) arrives in `sdmc:/nsa/received/<name>/` with the same tree; the receiver is asked *Save* / *Reject* and picks the destination folder.
- [ ] A folder with several CIAs (hundreds of MB): speed like a single file, nothing duplicated on the sender's card; cancel with B on each side; a name that already exists arrives as `<name> (2)`.
- [ ] A cut transfer of a folder goes on from where it stopped (the `<name>.part` folder and `.part.pos` stay on the receiver); an empty folder and one with only empty subfolders are refused by the sender ("no files"); a folder with thousands of small files.
  Claude, New 3DS XL -> 2DS, 2026-11-09: send_folder_cut_sender / cut_receiver / write_fails / read_fails, the empty folder refused and 250 small files in 20 folders pass; thousands of files not tried.
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
  Claude, New 3DS XL -> 2DS, 2026-11-09: console_tests (A New 3DS XL, B 2DS): all 33 pass, so sending and receiving work with the turn; a send while the receiver downloads not tried.
  Claude, 2026-11-09 (0.9.3-dev.38): turn_tests.py on both: all pass but, once each, reset-down on the New 3DS XL (the turn freed after 31-32 s, one answer 3.2 s: the reset not seen, as in Decisions) and web-vanish on the 2DS (one request failed, the page back at 10-14.7 s, close to the 15 s limit). Rerun twice each: both passed.
- [ ] Settings -> FTP server (and Web server) off and on again with a transfer under way: nothing freezes, the turn is free.
- [ ] The page: the top bar shows a transfer (name, %, KB/s) of FTP or a download; an upload while one runs says so and sends
      nothing; the live screens say they are paused; browsing Titles and Files keeps working.
- [ ] A QR / URL install while an FTP upload runs: the console says another transfer is in progress; after it ends it works.
  Claude, 2026-11-09 (0.9.3-dev.38): turn_tests busy-ftp on both consoles: /api/install-url gets 503 while an FTP upload holds the turn, the upload ends and the turn is free at once. The console's own message (QR) not seen.
- [x] The 2DS: the same speed tests (`speed_tests.py --wait-mode poll|nap`) to see whether the short poll helps there too.
  Claude on the 2DS, 2026-11-09: 64 MB, music off: poll 921 up / 619 down KB/s, nap 930 / 637, hybrid 849 / 627: no difference beyond noise (left on POLL).

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
- [x] Found (2026-11-04): the share of the system core NSA keeps (`APT_SetAppCpuTimeLimit(30)`, from FBI). On an Old 3DS
      the network runs on that core. The fork with the same trace: ~23 ms a send() for ~18.5 KB (NSA ~33 ms). 2DS, 30% ->
      5% (`SITE CPULIMIT`, DEBUG), one right after the other: FTP down 480-488 -> 643-657, up 230 (a noisy round) -> 715;
      a URL download (Save) 562-680 -> 880-929; a URL install 650-654 -> 758-763. But an install from the SD card (32.5 MB,
      Files -> Install) took 42.7-43.4 s -> 49.7-50.8 s. So a held transfer turn lowers it to 5% and the release puts 30%
      back (Old 3DS only, `xferturn_cpu_limits`). Then on the 2DS: FTP up 837, down 649-651, URL 802 KB/s.
- [ ] The New 3DS XL is unchanged (no limit changes there): check that its FTP, URL and console to console speeds stay.
- [x] Whether the New 3DS XL would gain from 5% too (2026-11-04, 64 MB, `SITE CPULIMIT`, two rounds each, alternated):
      30% up 954 / 955, down 1066 / 996, URL 1006 / 1066 KB/s; 5% up 875 / 881, down 1121 / 1149, URL 957 / 965. Down
      +8%, but up and URL -8%: no gain overall, so the New 3DS keeps 30%.
- [x] Music during FTP on the 2DS with the limit at 5% (`ftp_pauses_music` off, 32 MB, two rounds): playing up 741 / 821,
      down 647 / 653 KB/s; paused 745 / 782, 667 / 670. A download right after NSA starts, while the music is made on
      core 1: 637 KB/s (675 after), and the music took 16.7 s to make, as usual.
- [x] "Music and FTP", asked once at start on an Old 3DS that still has "Pause music for FTP" on (its old default; the
      new default is off): Yes turns the pause off, No keeps it; either way `ftp_music_asked` is saved and it is not asked
      again. A New 3DS, or a console with the pause off, only saves `ftp_music_asked`. On the 2DS (2026-11-04) it showed
      after installing the build and the owner's Yes saved the pause off.
- [ ] Its order with the other start-up questions: on a console with FBI's folder and no /nsa/, the import first, then
      this, then the update check; and a translated language (the text has explicit line breaks: check none overflows).
- [ ] Old 3DS, with the turn lowering the limit: console to console (both ways), the web page's upload and download, and
      the menus during a transfer (the app's core-1 threads get 5% then); whether the music sounds clean meanwhile.
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
  Claude on the 2DS, 2026-11-09: the traces say the UI draws 10 frames/s during FTP and 11-12 during a URL download (with or without music); how it feels is for the owner.
- [x] Console to console: the speed on screen no longer jumps between 0 and twice the real one (bytes counted every 32 KB,
      not per 256 KB chunk, and averaged over the last seconds); the owner saw it steady (2026-11-02).
- [ ] Remote Install's confirmation (QR, typed URL, history, received URLs, the web page's install from URL): Install /
      **Save** / Cancel. Save downloads the file to `sdmc:/nsa/received/` (with what comes from another 3DS; not the SD root, which is the owner's) as it is (name from the URL, decoded; an extension from
      the content when it has none; a name in use gets " (2)"), with the same progress, connections and errors (issue #22).
      Azahar: two saves of a 100 MB CIA byte for byte, Install unchanged. 2DS (2026-11-02): a 100 MB save from the web
      page at 557 KB/s, byte for byte. Left: a QR and a typed URL on a console.
- [ ] Settings -> "Pause music for FTP" (`ftp_pauses_music`, off by default since the 5% system core during transfers;
      it was on on an Old 3DS): the music fades out once an FTP
      transfer has lasted a second and comes back 3 s after the last one (from the SD cache, no new song is made). On the
      2DS an upload with music went at ~375 KB/s with it (254-316 before). The top bar's server icon blinks while the FTP
      server moves data, and a computer icon shows while the web server runs.
- [ ] Music during transfers on an Old 3DS: off made the 2DS's FTP 10-25% faster (New 3DS: 3-6%); a URL install on the
      2DS 520 -> 530 KB/s (2%, the install is its limit); a 16 MB send New 3DS -> 2DS 29.0 / 35.7 s with music on both,
      29.8 / 30.8 s off (~6%, noisy). Owner to decide whether NSA pauses it during FTP transfers on an Old 3DS.
  Claude on the 2DS, 2026-11-09: 2026-11-09 again: FTP up 64 MB 782 KB/s with music, 946 without; down and URL the same (see the 2DS item above).
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
  Claude on the New 3DS XL, 2026-11-09: seen only empty (0) on the New 3DS XL too.
- [ ] System save data: rows with the ID, search, sheet (browse, delete in red), the delete
      confirmation in the red warning. Azahar crashes on this screen (also on the previous
      `release`, before this change): not seen even once; readable names of the modules were not
      added (no source found for the IDs).

**Dump NAND screen** (branch `advanced`)
- [x] On a New 3DS XL (2026-11-07): the raw NAND opens, size and free space are right, a dump runs to the end,
      the SHA-256 shown matches the file and the `.sha256` passes `sha256sum -c`.
- [x] On the 2DS (2026-11-08, 0.9.2-dev.112): B during the dump stops it at once; the half file (199 MB) is deleted
      once the notice is closed and the screen is back at the start. With 859 MiB free (a filler plus a preallocated
      SITE SDBENCH) the screen is red: "Faltan 84.75 MiB: libera espacio primero", only Back (A goes back too).
- [x] B (or the button) asks first, red: "Stop the NAND dump? What was copied is deleted." (No goes on, Yes stops);
      the notice after is titled "Dump NAND": "NAND dump cancelled." (`feat/progress-eta`; seen on the 2DS 2026-11-08).

**Dump NAND on three threads** (2026-11-07, `feat/dumpnand-pipeline`): read (core 1), write (core 0) and hash
(core 2, or core 0 on an Old 3DS) in 1 MB blocks, four of them in flight, with `osSetSpeedupEnable`. On the New 3DS XL,
1.98 GB: 0.8.5 without a hash 647 s, 0.9.1 hashing on the copy thread 1105 s, this 557 s (3.4 MB/s); the dump's
SHA-256 checked with `sha256sum -c`, and it differs from the earlier dumps only in a few CTRNAND sectors. The SD write
is the limit now (slower while the NAND is read at the same time). The dump used to turn the clock and L2 off when it
ended (`fix/dumpnand-speedup`: FTP fell from 1.3 MB/s to 0.46 after a dump).
- [x] On the 2DS (2026-11-08, hash on core 0): 1.98 GB in 558 s against 1153 s on 0.8.5.
- [x] Cancelling and the full SD again on this build (see above).

**Wi-Fi connections** (2026-11-08, `feat/wifi-slots`; System -> Network, A): the console's three connections and the
copies in /3ds/WifiManager (WifiManager's folder and format: 0xC00 bytes, then "\nWMB1\t<time>\t<alias>\n"; both apps see
the same copies). A connection: save a copy, or restore one there; a copy: restore it to connection 1-3 (red, and asked,
when that replaces one), delete it; Y shows the passwords (8 stars otherwise).
- Why WifiManager's restores did nothing until System Settings: the ac service reads the connections from the config
  once, at boot, and connects with its own copy; writing the config blocks (CFG_SetConfigInfoBlk8) does not reach it,
  and ac's next flush can even write its old copy back. NSA writes through ac:i as System Settings does:
  LoadNetworkSetting (or InitializeNetworkSetting when empty), one setter per field, UpdateNetworkSetting,
  FlushNetworkSetting (ac saves the config itself). Checked: the block read back is byte for byte what was meant (0x4-0x414,
  the CRC's range, ac computes the CRC), also with a changed passphrase and back.
- The setters' static buffer numbers are not on 3dbrew; found by sending empty buffers (ac checks the number, not the
  size): name 2, key 3, BSSID 4, passphrase 8, IP 9, gateway 10, netmask 11, DNS 12, proxy host 13, user 14, password
  15. **A buffer larger than what ac set aside for that number crashes the console** (kernel prefetch abort while
  serving ac; seen with 0x40 bytes to number 2): wifislots.c sends each field with its own size, never more.
- NSA's RSF (buildtools' template) lists neither cfg:i nor ac:i; Luma3DS lets NSA open them anyway (libctru tries
  them first). DEBUG: SITE WIFIREAD (names, CRCs, what ac says), SITE WIFIROUNDTRIP <slot> [1] (write through ac:i and
  read back; refuses the connection in use).
- [x] On the New 3DS XL: the list, a copy saved (same bytes as WifiManager's copy of that network, but the last DHCP
  address), restored to its connection (the three read the same after, Wi-Fi stayed up), deleted; restore here -> No.
- [ ] Restore a copy of another network into a connection and connect with it without opening System Settings;
  a WPS/AOSS connection; Old 3DS/2DS; the 10 languages.
- Later the same day: saving asks for an optional alias (WifiManager's, up to 96 characters, in the trailer; empty = the
  network), and a copy can have it renamed (same file and date) (`feat/wifi-alias`); the questions name a copy by it.
  A guide chapter, `wifi` (marked 0.10: the guide tells minor versions apart only) (`feat/wifi-guide`). The list
  crashed after a save when the top screen drew the selected item the reload had freed: `fix/list-freed-selection`
  (list.c: the top screen no longer draws a selected item that is not in the list).
- Editing by hand (`feat/wifi-edit`, wifiedit.c): Edit on a connection or a copy, Set up by hand on an empty
  connection: the network's name, security, password, address (DHCP or typed: IP, netmask, gateway), DNS (automatic
  or typed). The key is worked out as the console does (WPA: PBKDF2-HMAC-SHA1 of the password, the name as salt, 4096
  rounds; checked on the New 3DS XL with SITE WIFIKEYCHECK against both connections set up in System Settings; copies
  made by WifiManager's editor hold a zero key). A WPS/AOSS connection edited becomes a plain one.
  - [x] Saving connection 2 untouched: the three read the same after, keys still match.
  - [ ] Change a password and a typed address, and connect with them; set up an empty connection by hand.
- Connecting (`feat/wifi-connect`, 2026-11-08). Tried on the New 3DS XL with SITE WIFICONNECT (`feat/wifi-test`, not
  merged): while the console is connected, ac:u ConnectAsync with the connection in use answers 0 and with another one
  C8809FFA; CloseAllAsync answers 0 but the connection stays (the system's own, ndm), and handing the Wi-Fi back after
  that left it half dead (no signal strength, the FTP server not answering) until HOME. So NSA never switches or tests
  a connection while connected; writing the one in use changes nothing until the console connects again, which the
  owner found only sleep mode does (not HOME, not reopening NSA): the notice says so. ac's AP types are 1, 2, 4 for
  connections 1-3 (as GetWifiStatus; libctru's AC_AP_TYPE_SLOT* are one bit off): mask 4 connected through connection 3.
- With NSA in front the system does not connect by itself (the HOME Menu does: the owner saw a connection restored
  while offline taken up only after HOME). NSA now connects as a game does (ndm infrastructure exclusive state,
  ConnectAsync, GetConnectResult, leave; the connection stays): after restoring, editing or setting up a connection
  while not connected ("Connecting with connection N...", then connected or why not), with Connect in a connection's
  actions while not connected, and in the background when NSA starts without a connection.
- The FTP and web servers stop when the Wi-Fi goes while they serve (they looked on but answered nothing until HOME)
  and start again once it is back, as at a start without Wi-Fi.
- Delete connection (`feat/wifi-delete`): ac:i RemoveNetworkSetting + FlushNetworkSetting, red and asked; the one in
  use keeps the console connected until sleep mode (said). On the New 3DS XL: connection 2 saved, deleted (empty in the
  config and in ac), restored from that copy: the three read as before, keys match.
- [ ] Offline (out of reach, or the connection in use replaced and the console put to sleep): restore or edit a
  reachable connection, and Connect; open NSA with no connection (it connects by itself, the servers start); the Wi-Fi
  lost and back while NSA is open (the FTP and web icons go and come back).

**B asks before it stops** (2026-11-08, `feat/confirm-cancel`): under every progress bar that says how to stop it
(installs, copies, pastes, deletes, imports, exports, console to console, Store files), B asks first, red: "Stop now?
What is left is not done." No goes on; yes hands the update the B it reads (injected, as the web page's keys are). The
notice after is titled "Cancelled", not "Failure" (data_op and the save packer). A paste stopped half way deleted
nothing: the file it was writing stayed, at its full size (the copy sets it first) but partly written; now it goes.
- [x] On the New 3DS XL: a 150 MB paste, B -> No went on (38 %, "Quedan 00:00:30"), B -> Yes stopped it, "Cancelado",
      the half file gone.
- [ ] A CIA install and console to console: B asks, No goes on, Yes stops.
  Claude, New 3DS XL -> 2DS, 2026-11-09: receiving: B -> "¿Parar ahora? Lo que falta no se hará.", Sí stops and the sender says the other console cancelled (receiver_cancels). No, and a CIA install, not tried.

**Time left under every progress bar** (2026-11-08, `feat/progress-eta`): the progress view (info.c, every copy,
paste, install, delete, import, export, console to console, Store files) shows "00:01:23 left" under its bar, from
the average since the bar started to move (it starts again when the bar goes back, as at each file of a copy; none in
the first 2 s). The texts that had their own ETA (data_op, Export CIA, console to console) lost it, so it is not twice.
Dump NAND's bar has "00:03:29 left · 3.76 MiB/s" under it too. The URL download screen kept its own.
- [x] On the 2DS: the dump's line; a 40 MB paste ("Quedan 00:00:07" under the bar).
- [ ] A long install, Export CIA and console to console: the time left under the bar, in Japanese or Chinese too.

**Dump NAND up to the last partition** (2026-11-08, `feat/dumpnand-trim`): the system says 1.84 GiB for the raw NAND on
every model, but after the last NCSD partition only zeros come; the dump stops there, as GodMode9's SysNAND backups do
(943 MiB on the 2DS, 1.21 GiB on the New 3DS XL). Tested on both: 2DS 254 s (from 558), New 3DS XL 366 s (from 557);
`.sha256` passes, same size as the owner's GodMode9 backups of 2026-09-27, which differ only in CTRNAND and TWL sectors
the system wrote since (header and FIRMs equal).

**Pixel theme** (Settings -> Theme, `tools/make_pixelfont.py`)
- [x] On a console: the pixel font (Pixelify Sans 10/12/16 px, `romfs/pixelfont.png`) is readable on
      both screens, Spanish accents included (the acute i is built from a dotless i), the button
      glyphs come out of the system font, texts fit in tiles, rows, prompts and the top card; day and
      night, every accent (the new Yellow gets dark text). The owner checked it on a console (said 2026-11-11).
- [ ] Cards is unchanged (same positions as before the text layer was split in two).
- Bundled font: Pixelify Sans, SIL OFL 1.1 (`tools/fonts/OFL-PixelifySans.txt`). HOME theme next.

**HOME theme** (Settings -> Theme)
- [x] On a console: the main menu as four big tiles per row with the name under each (two lines
      fit), D-Pad moves through the grid, a tap on the name opens too; dialogs rise as a sheet from
      the bottom (red for Advanced and deletes), day and night, every accent. The owner checked it on a console
      (said 2026-11-11).
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
  Claude on the 2DS, 2026-11-09: 2DS: songs of Shop take 14.8-29 s to make (music-log); turning it off while Calm was being made (going from Shop to No passes Calm and Lift): "style 2: stopped in 3196 ms" and the note left the status bar. Not heard.
- [ ] The animation: a note blinks in the status bar while a song is made, and Settings shows the progress,
      a progress bar and an equalizer (quick while making, slow while playing). Seen in Azahar only; the fade
      cannot be heard there.

**Remote install history**
- [x] History list: install again, forget, long-press sheet, top card with the links.
  Claude on the New 3DS XL, 2026-11-09: rows and top card with the whole link; A asks Install / Save... ; held A: Install / Forget; Forget removes the row (42 -> 41).
- [ ] Migration of the old `lasturls` (now deleted through an opened SD archive); 100 request limit.

**Files: cut / move**
- [ ] Cut a file and a folder, paste on the same medium (instant rename) and on another one
      (SD to NAND: copy then delete the source); same folder; folder into itself; name taken.
  Claude on the New 3DS XL, 2026-11-09: a file cut in /nsa/ptest and pasted in /nsa: "Movido", gone from the source. Copying a folder and pasting it inside itself is not refused: it made one copy inside (no loop). Cut into itself, name taken and SD -> NAND not tried.
  Claude, 2026-11-09 (fixed, 0.9.4-dev): copying (or cutting, where it is not a rename) a folder into itself or one of its folders is refused: "Una carpeta no se puede copiar dentro de sí misma" (the copy kept no archive ID: the archive handles are compared). On the 2DS: /0selftest/A pasted in A refused, nothing copied; pasted in /0selftest/C it asked and copied.
- [x] Cut, then paste after the source's list was left (clipboard survives navigating).
  Claude on the New 3DS XL, 2026-11-09: cut in /nsa/ptest, pasted from /nsa's sheet (the clipboard row showed it).

**Files: pictures**
- [ ] PNG, JPEG, BMP, GIF, TGA open; L/R through the folder; zoom, drag, D-Pad/Circle Pad pan.
  Claude on the New 3DS XL, 2026-11-09: all five open, L/R and Left/Right go through the folder ("4 / 7"), zoom 200 % and the D-Pad pans. A GIF crashed NSA until the main stack was raised (c5e0637). Dragging and the Circle Pad not tried.
- [ ] A camera's **MPO in 3D** (pair order, depth, slider at 0 shows the left picture): not
      tried even in Azahar (no sample).
- [x] More than 5 megapixels / 12 MB refuses with a message; a 1024+ px picture is shrunk.
  Claude on the New 3DS XL, 2026-11-09: a 3000x2000 JPEG: "Demasiado grande (5 megapíxeles como máximo)."; a 1600x1200 PNG shown shrunk to fit.
- [x] Memory on an Old 3DS with a 4-5 megapixel JPEG.
  Claude on the 2DS, 2026-11-09: 2DS: a 2520x1900 JPEG (4.8 MP, 382 KB) shown fitted after 6-9 s of "Cargando..."; SITE MEM the same before and after.

**Files: text**
- [x] Open `.txt`/`.json`/...; "Open as text" on a file with no known extension; binary files refuse.
  Claude on the New 3DS XL, 2026-11-09: .txt (accents, ñ) and .json open; README (no extension) opens as text; a binary file with no extension: "Esto no parece un archivo de texto."
- [ ] Edit with the custom keyboard: letters, shift/caps, symbols, accents, backspace/arrows
      repeat, D-Pad cursor, X/Y/A shortcuts, START saves, close with changes asks.
  Claude on the New 3DS XL, 2026-11-09: a letter typed, START saves ("Guardado."), closing with a change asks Guardar / Descartar / Cancelar and Descartar keeps the file. Shift, symbols, accents and repeats not tried.
- [x] CRLF kept; a Latin-1 file is read and saved as UTF-8; a file over 256 KB is read only.
  Claude on the New 3DS XL, 2026-11-09: CRLF kept byte for byte; a Latin-1 file shown right ("No es UTF-8...") and saved as UTF-8; 293 KB: "Solo se muestra el principio (solo lectura)", Editar greyed.
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
- [x] A .3dsx's preview, name and author on both consoles; a .3dsx without SMDH keeps the generic icon.
  Claude on the New 3DS XL, 2026-11-09: New 3DS XL: boot.3dsx shows its icon, "Homebrew Menu v2.4.3", "hbmenu team". 2DS and a .3dsx without SMDH not tried.
  Claude on the 2DS, 2026-11-09: 2DS: boot.3dsx with its icon, "Homebrew Menu v2.4.3", "hbmenu team"; a .3dsx with no SMDH: the generic program icon, its file name, "Programa".
- [ ] **Run program** on a real .3dsx: the owner's NSA is a CIA and they have a Homebrew Launcher CIA.
      Possible failures: its title ID is not `000400000D921E00` (write it in `sdmc:/nsa/hbl_title.txt`),
      or NSA's exheader does not allow `svcConnectToPort` ("hb:ldr", error shown). Also from a 3DSX build.
- [x] File type names and icons on files of each kind; "Open as text" is now offered only on
      files with no known extension (README...), not on programs, ROMs, pictures, archives.
  Claude on the New 3DS XL, 2026-11-09: Imagen, Texto, Archivo, Comprimido, Programa, ROM de juego, Carpeta with their icons; a picture's sheet has *Ver imagen* and no *Open as text*, a file with no extension has it.

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
   corners, thick borders, block progress bars, yellow highlight. Then ~~**themes from the
   SD card**~~ done (2026-11-11, `feat/sd-themes`, see Pending tests and `docs/themes.md`). Left: HOME's icon grids in
   pages that turn sideways (done, `feat/paged-grids`), and dragging a sheet down to close it (done, `feat/sheet-drag`).
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
    - ~~A mini player that keeps the song playing while browsing~~ done on `staging` (`feat/player-background`).
    - Date of creation: not available (the 3DS only gives the last change); NSA could record its own.

11. **A .3dsx in the HOME Menu** (asked on Reddit by rhinofinger in the first thread, 2026-10; issue #46): done, see
    "Pending tests on the console" and Decisions, "A .3dsx as a title". After-Pop-2000's
    [3DSX-Forwarder / MULTI-Forwarder](https://github.com/LBpartition/3DSX-Forwarder) (MIT) makes forwarders (the .3dsx
    stays, quits into the Homebrew Launcher). Left for later: a jingle of the user's (a .wav as the banner's CWAV), a .3dsx
    that changes itself (an updater writing its own .3dsx: the title keeps the old copy), and the owner's ID questions
    (2026-11-10): the official CIA's title ID when a known homebrew has one (a list of known homebrew IDs in NSA, so saves
    match and installing the official CIA replaces the copy), avoiding the known IDs otherwise, and when NSA installs a CIA
    whose ID is a converted title's, warning, copying its system save data to a free ID and moving the title there first.

- **Named mods and a GameBanana mods store** (issue #32, for later; the owner, 2026-11-07): Luma keeps one folder per game and
  mods mix in it, so a mod's name and files exist only if NSA installs it. A library in `sdmc:/nsa/mods/<ID>/<name>/` with a
  manifest (enable, disable, delete, conflicts), *Install as mod* from a .zip or folder in Files, a game's mods from Titles,
  then a store over GameBanana's API (check its terms, mapping its games to title IDs, .7z/.rar archives).

## Decisions

- **A .3dsx as a title** (`source/fbi/homeicon.c`, `homeiconview.c`, `ciatemplate/`; issue #46, 2026-11-10): the owner
  chose converting over forwarding (a forwarder quits into the Homebrew Launcher and shows as it in HOME; a title of its
  own quits to HOME and needs neither that title nor Luma's loader). The price: the program is copied (SD space; updating
  the .3dsx means installing again). How:
  - `romfs:/ciatemplate.cia` (`make -C ciatemplate`, committed) gives certificates, ticket, TMD, NCCH header, exheader (a
    homebrew title's permissions, 64 MB on an Old 3DS and 124 MB on a New one, stack 0x40000), logo, the banner's model and
    sound (a chime, `ciatemplate/chime.wav`) and the meta section.
  - .code: the three segments at a title's addresses (text at 0x100000) with the relocations Luma's loader applies
    (`sysmodules/loader/source/3dsx.c`); one page after rodata holds argv (argv[0] the .3dsx's path) and _prm's argument
    list points at it; nothing else in _prm, so libctru sees a title (mounts the title's RomFS, quits to HOME). The data
    goes up to its last non-zero word, the rest is BSS.
  - RomFS: the .3dsx's (libctru's format is level 3) with IVFC levels made on the console. It is read three times (level 2
    hashes; the content's SHA-256, as the TMD comes before the content; the install). Hashes made again everywhere;
    signatures stay invalid (Luma's patches, as for exported CIAs).
  - Banner: bannertool's model has one 256x128 RGBA4444 texture (8x8 Morton tiles, top row first); NSA writes the picture
    there with an ordered dither and stores the CGFX again as LZ11 of literals only (an eighth bigger). The drawn banner is
    made with the usual drawing calls into a VRAM texture (`screen_render_offscreen`), and the top screen shows that
    picture, so the preview is what gets installed. Pictures: up to 12 MB and 5 megapixels, checked before decoding.
  - Title ID: as the forwarder's (FNV hash of the path, unique ID 0xC8000-0xEFFFF, past titles not ours, product code
    `CTR-P-NSAF`): the same path updates its title.
- **The guide's chapters** (`source/fbi/guide.c`, the owner asked 2026-11-06 for a tutorial where an update plays only the new
  feature, chains the earlier ones not seen, and does not overwhelm): each chapter has a stable `id`, the version it came with
  (`since`, `GUIDE_VERSION(0, 8)`), `essential` (offered on a first start) and `needs` (a chapter shown first while unseen).
  `settings.json` keeps `guide_seen` (ids) and `guide_known` (the newest `since` offered; missing = a console from before the guide,
  taken as v0.7; 0 = a first start). At start, after the other questions and before the update check, NSA asks once about the
  chapters with `since` > `guide_known`; any answer moves `guide_known` on. **For a new feature**: append a chapter with the
  release's version as `since` and the chapter it builds on as `needs`, add its Spanish to `i18n_es.c`, and never rename an id.
- **Guided tours** (`source/core/ui/coach.c`; the owner chose 2026-11-06 a tutorial on the real screens where the user does each
  action, not a slide show): a chapter is a list of `coach_step` (macros in `guide.c`: `INFO` explains, A goes on; `DO` waits for a
  button or a tap on the anchor, then for `until`; `OPEN` a main menu entry; `REACH` a row with the D-Pad). Screens say where they draw
  what a step may point at with `coach_anchor(id, rect, selected)`: `menu.<entry id>`, `menu.tiles`, `menu.icons`, `menu.editing`,
  `tool.<search|filter|grid|rows|size|more|clipboard|nearby>`, `list`, `list.selected`, `sheet` and each sheet row by its label,
  `popover`, `settings.<option>`, `remote.<3ds|pc|qr|url|history>`, `back`, `hints`, `top`. A new screen a chapter visits needs its
  anchors. `until`: `v:<view id or English name>` on top, `<anchor>` drawn, `!<anchor>` gone, `@<anchor>` with the focus. All input
  goes through `coach_filter` (in `input_scan`): only the step's keys (A only while the anchor has the focus), keys pressed after
  the step began, and a touch that went down on the anchor; START ends the tour and is kept from the views until let go. Steps
  should never reach anything that changes data: point at it with `INFO` instead.
  A step with `only` runs just when that anchor is on the screen, for a branch the last one may have taken (`until` takes
  alternatives with `|`): a game with updates or DLC opens their list (`titles.chain`) instead of its actions, so "Getting around"
  and "Saves" then ask to open the game itself and, after the actions, to go back from that list.
- **Dialogs scroll** (`prompt.c`): a message taller than the card scrolls between the header and the buttons, so a long question
  never hides behind them; no dialog needs its text cut for that any more.
- **Sleep and HOME** (`net_sleep_hook` in `main.c`): the app's `ac` session does not survive the console's sleep
  (`ACU_GetWifiStatus` fails afterwards, so NSA saw no Wi-Fi and the servers never came back, and HOME froze the console).
  On sleep/suspend the FTP and web servers are paused; on wake/restore `acExit(); acInit()` runs and they resume. DEBUG
  builds write `sdmc:/nsa/sleep-log.txt` (hooks, Wi-Fi status, heartbeat, exit steps). If it ever fails again, SOC is the next
  suspect (reinit it on wake).
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
  because the screen reorders it meanwhile. Until the user chooses an item the selection keeps its index (the
  top), then the list keeps the selected row where it was on screen when items move (`list_validate`, issue #45), and `list_rebuild_visible` never writes past
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
  unsent data when a non-blocking socket closes. The FTP serves up to 5 sessions, each on its own thread (3 since
  2026-11-07, `feat/ftp-sessions`, 5 since 2026-11-10 for Dolphin's KIO, with the SOC buffer at 2 MB, `main.c`; it
  served one at a time, one silent for 30 s gave way to a new connection). Second requests for a segment only for the one the install waits for (near the end, any), once it has
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

- The Simplified Chinese atlas (`romfs/hanfont.png`, `tools/make_hanfont.py`) is full: 644 glyphs, 36 rows of 18, 1008
  pixels of the GPU's 1024 (2026-11-10, after the hardware tests' strings). The next new characters in `i18n_zh.c` need a
  second atlas or smaller cells; the script's comment ("up to 594 glyphs") is out of date.
- `SITE SCREENSHOT` on the console takes 17-21 s (2026-11-06, both consoles, 0.8.1; 0.17 s in Azahar) and the screen freezes
  meanwhile; it answers "not taken in time" after 15 s and the files appear later. Not the music (off: the same) nor core 1 (idle:
  the same); an FTP upload of 512 KB to the same card takes under a second. Still to measure where it goes (open, copy, write,
  close). A second `SITE INSTALL` right after one left the 2DS stuck on the FTP install bar at its end (no HOME, touch or
  buttons; the owner had to hold the power button), on the same afternoon: maybe the same slow SD writes.
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
