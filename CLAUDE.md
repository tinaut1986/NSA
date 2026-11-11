# Project notes for Claude

**NSA** is the owner's fork (`tinaut1986/NSA`) of Steveice10's FBI 2.6.1, a 3DS title
manager. It installs as its own app (product code `CTR-P-CNSA`, unique ID `0xFB1A0`)
next to FBI, keeps its data in `sdmc:/nsa/` and numbers its own releases from `v0.1.0`.

**Start every session by reading [`docs/PLAN.md`](docs/PLAN.md)**: where things stand, how the
console to console transfer works, what comes next, the decisions taken and the known issues. Work in progress lives on a
topic branch until the owner validates it; PLAN says which.

## Language

- Chat with the owner in **Spanish (castellano)**.
- Code, comments, identifiers, commit messages and docs: **English**.

## How we work

1. **Every change goes on a new branch**, unless the owner says otherwise. Topic
   branches (`feat/...`, `fix/...`, `chore/...`, `docs/...`) are cut from `release`.
2. **No pull requests.** The owner tests by hand on the console.
3. When the change is ready, **build the CIA and send it to the owner** (or install
   it, see below). Do not commit yet.
4. The owner tests it. Only when they say it is good: **commit, `git merge --no-ff`
   into `release`, and push**.

If a branch the owner is still testing is needed by the next change, cut the new one
from it and say so.

**Since 2026-10-31** the owner lets finished work be merged into `release` and **pushed straight
away** (commit on the topic branch, `merge --no-ff` into `release`, push both) without waiting
for their test, as long as it builds. What was not tried on a console then goes in
`docs/PLAN.md` -> "Pending tests on the console"; tick an item only when the owner says it
works (and they may say not to note something they already tested). The build's CIA is sent with
SendUserFile; installing it on a console still waits for them to ask (they usually say
"envía el cia" / "instálalo"). If the main checkout has uncommitted work of another task, make a
`git worktree` for yours (it needs `git submodule update --init` for `buildtools`).

## Git and GitHub

- `origin` = `tinaut1986/NSA` (a fork of `Steveice10/FBI`), `upstream` = Steveice10/FBI.
  The push URL of `upstream` is `DISABLED`; `gh repo set-default tinaut1986/NSA`.
- `master` = stable, only moves by merging `release`. `release` = beta, the working line.
- A `vX.Y.Z` tag on `release` publishes a Beta; once `master` contains it, re-running
  the release workflow on the tag promotes it to stable (`.github/workflows/build-release.yml`).
  Write `docs/release-notes/<tag>.md` first (`docs/release-notes/README.md`).

## Branches in use

- **`staging`** (since 2026-11-07, kept for good): the owner's **internal** line, for work that is finished but **not to be
  published yet, not even as a beta** (anything on `release` can be tagged and published). Topic branches are cut from
  `staging` and merged (`--no-ff`) into `staging`, not `release`; push both. `staging` goes into `release` only when the
  owner says so (after their console tests: "Pending tests on the console"). Never delete it, even when it equals `release`.
  **Once a version is published, merge `master` back into `staging`** (`--no-ff`; only merge commits, no file changes):
  the tag must be in `staging`'s history, or `tools/version.sh` counts from the previous tag and a staging build
  calls itself the published version plus `-dev` (0.9.2-dev.N after v0.9.2 instead of 0.9.3-dev.N).
- `release`: beta, what may be published (a tag on it publishes a Beta); `master`: stable (only moves by merging `release`).
- `advanced` no longer exists (2026-11-08: its Dump NAND screen came into `staging`).
- `feat/website` is the GitHub Pages source (`/docs` of that branch): do not delete it.
- DEBUG builds for the owner's consoles: build with the **micro version of the latest tag** (after a `make clean`):
  `tools/version.sh` gives that tag's major and minor, so after `v0.6.1` that is `make DEBUG=1 VERSION_MICRO=1` (title
  0.6.1, what the consoles hold once they update). `SITE INSTALL` and the web page's install take the **same** version
  (`AM_StartCiaInstallOverwrite`; only HOME's icon, banner and sound stay old); a lower one is refused (D8E08027). **Never
  go above it** (and never 15): the micro version is 4 bits and a console that holds a higher one refuses the next releases.
  Check first (`/api/titles`, id `000400000FB1A000`). A console on a release build needs `sdmc:/nsa/remote.flag` (an
  FTP upload works without it) for `SITE INSTALL`; delete it afterwards. See `docs/PLAN.md`, Decisions, "Updating a console"

## Build

```sh
export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
git submodule update --init          # buildtools
make -j"$(nproc)" DEBUG=1            # output/3ds-arm/NSA.cia and 3ds/NSA/NSA.3dsx
make -C tests                        # host tests (needs libjansson-dev)
```

`DEBUG=1` starts the FTP server with NSA and shows DEBUG next to the version; the
version itself comes from `tools/version.sh`. `bannertool` and `makerom` are in `tools/bin/`.
`make` does not rebuild on a version or `DEBUG` change: `make clean` first.

**On Windows** (devkitPro's msys2 shell, `C:/devkitpro/msys2/usr/bin/bash.exe -l`, with Git's `usr/bin` on the PATH for
`xxd`): pass `VERSION_MAJOR=0 VERSION_MINOR=6 VERSION_MICRO=N` (version.sh's parts are not picked up there), and package the
CIA with the Linux `tools/bin/makerom` from WSL: devkitPro's `makerom.exe` ignores `-minor` (0.6.2 comes out as 0.0.2, which a
console refuses as older, D8E08027). Take the line from `make -n ... output/3ds-arm/NSA.cia` (with the CIA moved away) and run
it in WSL from the repo with `./tools/bin/makerom`; check the TMD's title version (98 = 0.6.2) before installing.

## Interface

- `source/core/theme.c`: themes (palettes for day and night, radii, borders, menu layout),
  accent colours and night mode. Widgets (`source/core/ui/widgets.c`) draw everything
  through the theme; never hard-code a colour in a screen.
  Themes from the SD card (`sdmc:/nsa/themes/*.json`, data only) are read by `source/core/themefile.c`; the format
  is in `docs/themes.md`.
- `source/core/i18n.c`: every user-facing string goes through `_("English")` (literals
  only: lookups are cached by address) or, for text that may be built at run time,
  `i18n_translate_text` (by content, never cached; the dialogs and lists already do it).
  Spanish is `source/core/i18n_es.c`; a missing entry shows the English.
- Icons: Tabler (MIT), listed in `tools/icons.txt`, plus NSA's own in `tools/icons/<name>.svg`
  (24 px grid, 2 px strokes, like Tabler; `3ds` is the console, for everything console to console); `python3 tools/make_icons.py` rebuilds
  `romfs/icons16.png`, `romfs/icons32.png` and `source/core/icons.h` (needs Chrome and
  Pillow). Draw at 16 or 32 px so they land on whole pixels.
- Screens: lists are `source/core/ui/list.c` (rows or grid, tools, filter, long press),
  floating layers `overlay.c` (popover, action sheet), dialogs `prompt.c`/`info.c`/`error.c`.
  Titles, Tickets and Ext Save Data are the model for a screen (placeholders, live sort,
  remembered filters in `settings.json`): see `docs/PLAN.md`, Decisions.
- Reuse before copying: a list's items draw with `widget_item_frame` / `widget_tile_frame` / `widget_skeleton` /
  `widget_fade_in` / `widget_count_badge` / `widget_badge_right`, the top screen's details with `widget_cell` and
  `widget_loading_card`; the title bar's search and grid/rows tools are `list_add_search_tool` / `list_add_view_tools`
  (with `list_open_search`, `list_clear_search`, `list_toggle_mode` for R, B and Y); a list's loading task starts,
  stops and clears with `task_start_populate` / `task_stop_populate` / `task_clear_list` (`task/uitask.c`).
  A viewer's buttons: `widget_row_button` / `widget_button_row_hit` / `widget_button_row_hold`, its card
  `widget_info_card`; an item without icon `widget_icon_placeholder`; sizes `ui_format_size`; keyboards `core/ui/keys.c`;
  a task's progress view `info_task_finished` with `data_op_progress_items` / `data_op_progress_bytes`; a grouped game's
  list `chain.c`; deleting one item `action_delete_one` (`action/deleteone.c`). A data operation needs no suspend /
  restore callbacks that only return 0 (NULL is skipped).
- `tools/ui_snapshots.py` takes pictures of every screen in Azahar on a fixed SD card (`run`) and compares two builds
  (`compare`): run it before and after a change that should not show (a refactor). The search keyboard's caret blinks,
  so that step may differ by a few pixels.
- Background music: `source/core/bgmusic.c` (circular synthesis on a low-priority thread, DSP channels 14 and 15,
  cache per style on the SD, crossfade); DEBUG builds log how long a loop took to `sdmc:/nsa/music-log.txt` and write the
  loop as `music-style-N.wav`. The pixel font is baked by `tools/make_pixelfont.py`, the icon and banner by
  `tools/make_branding.py`.
- Sound: `source/core/sound.c` (interface tones, Settings "Sounds"/"Sound volume"; a dialog named
  "Success" / "Failure" / "Error" plays its tune by itself); the DSP is started only through
  `sound_dsp_start`. The audio player (`source/fbi/audioview.c`) owns DSP channel 0.
- Files screen: `files.c` (list, card, sheets, `files_type_of` / `files_folder_icon`), viewers opened
  by extension from `viewers.c`: `imageview.c`, `textview.c` (own keyboard), `audioview.c`;
  `thumbs.c` makes picture and .3dsx previews on a worker thread; `launch3dsx.c` starts a .3dsx
  through Luma's `hb:ldr`. Cut/move is `clipboard_set_cut` + `action_paste_contents_in`.
  Dates: `archive_getmtime("sdmc:/...")` (libctru's `stat` gives 0).
- The system font has the 3DS button glyphs (`\uE000` A, `\uE001` B, `\uE002` X,
  `\uE003` Y, `\uE004` L, `\uE005` R) but not "·": use the glyphs in hints.

## Seeing and driving the app (FTP remote control)

With the FTP server on (always in DEBUG builds), port 5021 (5000 until v0.3.0). In a production build
the FTP server (on by default) only serves files; the `SITE` commands below (and installing with `SITE INSTALL`) answer
`502` unless the file `sdmc:/nsa/remote.flag` exists (create it to turn remote use on, delete it to
turn it off; DEBUG builds ignore it):
`SITE SCREENSHOT` (both screens to `/nsa/screenshots/top.bmp` and `bottom.bmp`),
`SITE PRESS <A|B|X|Y|L|R|ZL|ZR|START|SELECT|UP|DOWN|LEFT|RIGHT>[+...] [frames]`,
`SITE TAP <x> <y> [frames]` (bottom screen), `SITE DRAG <x> <y1> <y2> [frames]` (a finger sliding
from y1 to y2, to scroll). For example:
`curl -Q "SITE PRESS DOWN" ftp://IP:5021/`.

To look at the real console, wait a few seconds after `SITE SCREENSHOT` (it answers
"not taken in time" but the files appear) and measure on that image: Azahar's font and
data differ (see `docs/PLAN.md`, Known issues).

A handy driver (not in the repo): for each `SITE PRESS`/`TAP` wait about a second, finish with
`SITE SCREENSHOT`, wait 2 s (Azahar) or 6 s (console), fetch `/nsa/screenshots/top.bmp` and
`bottom.bmp`, paste them into one 400x480 image (bottom at x=40, y=240) and scale x2 to read it. A
long press is `SITE PRESS A 60`. The list's first row is index 0; check the screen before
assuming where the selection is. To find a console: scan port 5021 on the subnet.

**Azahar** (flatpak `org.azahar_emu.Azahar`) runs the 3DSX for UI work without the
console: `flatpak run org.azahar_emu.Azahar "$PWD/output/3ds-arm/3ds/NSA/NSA.3dsx"`, then the
FTP server answers on `127.0.0.1:5021`. Close it with `pkill -9 -x azahar` before
launching another build. HTTPS (updater) does not work there; its NAND is virtual, but
still answer No to destructive prompts.

## Installing on the console

Ask before installing while the owner is using the console.

- **NSA's FTP server** (Settings → FTP Server, always on in DEBUG builds), port 5021:
  `curl -T output/3ds-arm/NSA.cia ftp://IP:5021/nsa/update.cia -Q "-SITE INSTALL /nsa/update.cia"`.
  NSA restarts into the new build 10 s later.
- **FBI's Remote Install → Receive URLs** (port 5000): serve the CIA over HTTP from
  this PC and send the URL with FBI's protocol (4-byte big-endian length, then the
  URL). The owner confirms on the console. Do not probe the port with an empty
  connection: FBI shows "Failed to read payload length".
- The owner's consoles: a New 3DS XL and a 2DS; `192.168.1.133` was the New 3DS XL on 2026-10-31
  (and a console of theirs on 2026-10-27); the addresses change, scan for port 5021.
- A helper loop used in sessions: upload with `ALLO` and `SITE INSTALL` in one curl
  call, then `DELE` the uploaded file:
  `curl -Q "+ALLO $(stat -c %s output/3ds-arm/NSA.cia)" -Q "-SITE INSTALL /nsa/update.cia" -Q "-DELE /nsa/update.cia" -T output/3ds-arm/NSA.cia ftp://IP:5021/nsa/update.cia`
- Screenshots: `curl -Q "SITE SCREENSHOT" ftp://IP:5021/` (allow 20 s), then fetch
  `/nsa/screenshots/top.bmp` and `bottom.bmp`.

Delete test files from the console's SD afterwards; the SD root is the owner's (Luma, boot9strap,
`boot.3dsx`, `retroarch`, `roms` ... are theirs: never touch what is not NSA's or a test's).

## Web server

`source/fbi/webserver.c` (port 8080, off until Settings -> Web server) and `romfs/web/index.html`: a page to manage the console
from a computer. Design, API, security and the phases left are in `docs/PLAN.md` ("Next" item 10); the API is also at the top of
`webserver.h`. To try it: Azahar with `"web_server": true` in its `sdmc:/nsa/settings.json`, then
`tools/web_tests.py --host 127.0.0.1` (a DEBUG build writes the PIN to `sdmc:/nsa/web-pin.txt`), and open
`http://127.0.0.1:8080/` in the built-in browser. Anything that changes something must be sent with the `X-Pin` header. `make` does not notice a change that is only in `romfs/` (the page): delete `output/3ds-arm/3ds/NSA/NSA.3dsx` and `output/3ds-arm/NSA.cia` and run it again.

## Speed tests

`tools/speed_tests.py --host IP` measures FTP, web and URL downloads (and `--server ftpd --ftp-port 5000` the FTP part against
ftpd), with NSA's trace of every second (`sdmc:/nsa/speed-log.txt`, `source/core/speedlog.h`; release builds need
`sdmc:/nsa/speed.flag`). See `docs/PLAN.md`, "Speed tests". Delete the flag on a user's console when done.

## Console to console: testing

Read "Console to console" in `docs/PLAN.md` first. In short:
- `tools/console_tests.py --a IP --b IP` runs the automated tests on the two consoles (DEBUG
  builds, on the main menu): exports, sends, failures made on purpose with `SITE FAULT`, retries.
  Run it after changing exporting or transfers; see "Tests on the consoles" in PLAN.
- `tools/fake_peer.py` plays the other console against Azahar or a console; every send and
  receive of the real app is logged in `sdmc:/nsa/transfer-log.txt` (fetch it over FTP after an
  error: it says why a transfer stopped).
- The owner has a New 3DS XL and a 2DS; their FTP addresses swap after the local wireless.
  Ask before installing while the owner is using a console, and say what the build holds.
- In the local wireless mode (no router) the FTP server is off: test it with `/nsa/nearby.flag`
  (`auto`) and read the log afterwards; **delete the flag when done**.
- `pkill -f` kills the shell that runs it when the text is in the command: use `pkill -x`.
