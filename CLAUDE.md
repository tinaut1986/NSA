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

## Git and GitHub

- `origin` = `tinaut1986/NSA` (a fork of `Steveice10/FBI`), `upstream` = Steveice10/FBI.
  The push URL of `upstream` is `DISABLED`; `gh repo set-default tinaut1986/NSA`.
- `master` = stable, only moves by merging `release`. `release` = beta, the working line.
- A `vX.Y.Z` tag on `release` publishes a Beta; once `master` contains it, re-running
  the release workflow on the tag promotes it to stable (`.github/workflows/build-release.yml`).
  Write `docs/release-notes/<tag>.md` first (`docs/release-notes/README.md`).

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

## Interface

- `source/core/theme.c`: themes (palettes for day and night, radii, borders, menu layout),
  accent colours and night mode. Widgets (`source/core/ui/widgets.c`) draw everything
  through the theme; never hard-code a colour in a screen.
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
- The system font has the 3DS button glyphs (`\uE000` A, `\uE001` B, `\uE002` X,
  `\uE003` Y, `\uE004` L, `\uE005` R) but not "·": use the glyphs in hints.

## Seeing and driving the app (FTP remote control)

With the FTP server on (always in DEBUG builds), port 5021 (5000 until v0.3.0):
`SITE SCREENSHOT` (both screens to `/nsa/screenshots/top.bmp` and `bottom.bmp`),
`SITE PRESS <A|B|X|Y|L|R|ZL|ZR|START|SELECT|UP|DOWN|LEFT|RIGHT>[+...] [frames]`,
`SITE TAP <x> <y> [frames]` (bottom screen), `SITE DRAG <x> <y1> <y2> [frames]` (a finger sliding
from y1 to y2, to scroll). For example:
`curl -Q "SITE PRESS DOWN" ftp://IP:5021/`.

To look at the real console, wait a few seconds after `SITE SCREENSHOT` (it answers
"not taken in time" but the files appear) and measure on that image: Azahar's font and
data differ (see `docs/PLAN.md`, Known issues).

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
- The owner's console was at `192.168.1.133` (2026-10-27).
- A helper loop used in sessions: upload with `ALLO` and `SITE INSTALL` in one curl
  call, then `DELE` the uploaded file:
  `curl -Q "+ALLO $(stat -c %s output/3ds-arm/NSA.cia)" -Q "-SITE INSTALL /nsa/update.cia" -Q "-DELE /nsa/update.cia" -T output/3ds-arm/NSA.cia ftp://IP:5021/nsa/update.cia`
- Screenshots: `curl -Q "SITE SCREENSHOT" ftp://IP:5021/` (allow 20 s), then fetch
  `/nsa/screenshots/top.bmp` and `bottom.bmp`.

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
