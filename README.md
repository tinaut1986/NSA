# NSA

NSA is an open source title manager for the Nintendo 3DS: install and manage titles, browse and
view files, send games to another 3DS, and look after the console's own data. It is a fork of
[FBI](https://github.com/Steveice10/FBI) 2.6.1 by Steveice10 with a new interface, new tools and
its own updater. It installs as its own app, **next to FBI** (it does not replace it), and keeps its
data in `sdmc:/nsa/`.

**Download:** https://github.com/tinaut1986/NSA/releases (install the `.cia`, or run the `.3dsx`
from the Homebrew Launcher). NSA can then update itself from Settings, on the Stable or the Beta
channel. Some tools need Luma3DS, as FBI's did (listed below).

<p align="center">
  <img src="docs/images/main-menu.png" width="360" alt="Main menu">
  <img src="docs/images/store.png" width="360" alt="The Store">
</p>

## What it does

### Install

- Install CIAs and tickets from the SD card, from a computer on your network, over the Internet
  with a URL or a QR code. Title seeds are imported on the way.
- **Remote install** keeps a history of what you installed, to install it again. Its tiles, the History bar and the FTP and
  Web buttons can be moved or hidden (SELECT, or hold one).
- A **download queue** that keeps going in the background, one download after another: pause, resume or reorder it
  from the main menu or the web page.
- **Links from web pages**: a store's "Send to 3DS" button (like Brewology's, made for 3dsDL) or a phone on your Wi-Fi
  can send links to `http://3ds.local/` (also `nsa.local`, or the console's address), the same way 3dsDL's Receive takes
  them (`/send?links=...`). The console asks before it downloads anything. On by default (Settings).
- **Send a file or an installed title to another 3DS** that runs NSA, over your Wi-Fi (four
  connections at once) or with no router at all. The other console picks what to do: install now,
  or save it in a folder (a save sent this way is imported or kept with the other exports). A cut transfer goes on where it stopped. Every send and receive is logged
  in `sdmc:/nsa/transfer-log.txt`.
- **Send several titles at once**: mark them in Titles and press START; the other console takes them all with one answer.
- **Export an installed title as a CIA** (installing it again needs Luma3DS).
- **A `.3dsx` in the HOME Menu** as a real title (not a forwarder), with its icon and a banner made from it or from a
  picture of yours, and **a DS game (`.nds`) in the HOME Menu**: an icon that starts it with nds-bootstrap. NSA checks
  that nds-bootstrap and its forwarder are on the SD card first, and downloads them when they are missing.

<p align="center">
  <img src="docs/images/remote-install.png" width="360" alt="Remote install, with the History bar on top">
  <img src="docs/images/remote-install-edit.png" width="360" alt="Remote install's layout being edited: a tile hidden">
  <img src="docs/images/receive-3ds.png" width="360" alt="Receiving from another 3DS">
</p>

### Store

- Browse the homebrew of [Universal-DB](https://db.universal-team.net/) with its icons: rows or a
  grid, search with NSA's own keyboard, filters by type and category, and sorting.
- Install an app's CIA or download its `.3dsx` to `/3ds/`. A mark shows what is already
  on the console.
- Add other stores in the Universal-Updater format (a `.unistore`) by their QR code or link: skins for TWiLight Menu++,
  mods for 3DS games (into `/luma/titles/`), games for other consoles, RetroArch cores... NSA follows each file's steps
  as Universal-Updater does, asking before it deletes a folder. Universal-DB is made by
  [Universal-Team](https://universal-team.net/): thank you.
- **Remove what the Store installed**: NSA notes every file an app's steps write, so it can take exactly those away
  again (texture packs, mods, plugins...). Apps that write to `boot.firm`, `/luma` or `/gm9` show a red warning first.

<p align="center">
  <img src="docs/images/store-actions.png" width="360" alt="An app of the Store and its actions">
  <img src="docs/images/keyboard.png" width="360" alt="Searching with NSA's own keyboard">
</p>

### Titles, tickets and data

<p align="center">
  <img src="docs/images/titles.png" width="360" alt="The list of titles">
  <img src="docs/images/titles-grid.png" width="360" alt="Titles as a grid of icons">
  <img src="docs/images/titles-filter.png" width="360" alt="Filters: memory, kind and region">
  <img src="docs/images/title-actions.png" width="360" alt="A title's actions">
</p>

- **Titles**: rows or a grid of icons, filters by memory, kind and region, search and sorting;
  updates and DLC are grouped under their game. Launch, delete (with or without its ticket),
  extract the icon, browse the save data and the extra data, import and export secure values.
- **Save data, extra data and SpotPass data** of a 3DS game can be exported to a folder on the SD card, imported back (the current
  data is exported first) and sent to another 3DS. Importing also finds [Checkpoint](https://github.com/FlagBrew/Checkpoint)'s
  backups, each one tagged with the app it comes from. Each kind of data has its own list in the title's
  actions: back up now, send to another 3DS, browse, and every backup (NSA's and Checkpoint's) newest first;
  a backup can be restored, sent to another 3DS or deleted.
- **Tickets**, **Ext Save Data** and **Pending titles** (interrupted installs and downloads) on the
  same pattern: pictures, filters, search, and actions in a sheet, with what deletes in red.
- **The game card** in the console shows up in Titles, and its save can be backed up.
- The console's own software is marked in red, and deleting it asks twice.
- Export, import and erase the save data of DS cartridges.
- **Web page** (off until you turn it on in Settings): manage the console from your computer's browser (the SD card, titles with icons, saves,
  installing from files or URLs, saves and their backups to download, restore or delete, and the console's screens live with its buttons), in the console's own theme, protected by a PIN shown on the console. Plain HTTP, for your own network only.

<p align="center">
  <img src="docs/images/web-titles.png" width="560" alt="The web page: titles with their icons">
  <img src="docs/images/web-files.png" width="560" alt="The web page: the SD card's files">
  <img src="docs/images/web-screen.png" width="560" alt="The web page: the console's screens live, with its buttons">
</p>

### Files

- Browse the SD card (and the NAND under Advanced) with a file card on the top screen: size, type,
  dates, a CIA's contents. Copy, cut (move), rename, delete, new folder.
- **Picture viewer** (PNG, JPEG, BMP, GIF, TGA, and the 3DS camera's 3D photos), **text viewer and
  editor** with its own keyboard, and an **audio player** (MP3, WAV, FLAC, Ogg Vorbis) that plays the
  folder in order. Pictures and `.3dsx` programs show a preview in the list.
- Run a `.3dsx` from the list (needs Luma3DS).
- **Select several** files or folders to copy, move, install or delete at once; extract `.zip` and `.7z` on the console.
- **Calculate size** in a folder's actions counts everything in it and shows **what takes the room**: its contents
  biggest first, each with its share, and its folders to go into. Folders counted show their size in the list.

<p align="center">
  <img src="docs/images/what-takes-the-room.png" width="360" alt="What takes the room: a folder's contents, biggest first">
  <img src="docs/images/folder-sizes.png" width="360" alt="Folders with their size in the list">
</p>

<p align="center">
  <img src="docs/images/files.png" width="360" alt="Browsing the SD card">
  <img src="docs/images/files-photos.png" width="360" alt="A folder of pictures">
  <img src="docs/images/file-actions.png" width="360" alt="A file's actions">
  <img src="docs/images/image-viewer.png" width="360" alt="The picture viewer">
  <img src="docs/images/text-viewer.png" width="360" alt="The text viewer">
  <img src="docs/images/text-editor.png" width="360" alt="Editing with NSA's own keyboard">
  <img src="docs/images/audio-player.png" width="360" alt="The audio player">
</p>

### Utilities

What Luma3DS's Rosalina menu or GodMode9 were opened for, in one list:

- **DSP firmware** (`/3ds/dspfirm.cdc`, the sound of homebrew): dump the console's, or put
  [OpenDSP](https://github.com/Cellenseres/OpenDSP), an open source one, from GitHub or from the copy NSA carries.
- **Game patching**: Luma3DS's option, on or off.
- **What takes the room** on the whole SD card.
- **Button remapping** with [Rehid](https://github.com/hax0kartik/rehid): other buttons in every game, or in one
  (turbo, buttons that touch the screen, the Circle Pad as D-Pad...). NSA installs Rehid from its GitHub release.
- **Restart** and **Power off**.

<p align="center">
  <img src="docs/images/utilities.png" width="360" alt="Utilities">
</p>

### Advanced (only with NSA installed as a CIA, or run as a 3DSX under Luma3DS)

The console's own memory and data, behind a warning: browse and modify the CTR NAND, TWL NAND,
TWL photos and TWL sounds, browse and delete system save data, and dump the raw NAND image to the
SD card. These are drawn in a soft red because a wrong change can stop the console from starting.

<p align="center">
  <img src="docs/images/advanced-warning.png" width="360" alt="The warning before Advanced">
  <img src="docs/images/advanced.png" width="360" alt="Advanced">
</p>

### Look and feel

- **Your own main menu**: a grid six cells across, each entry big or small, wherever you want it. Hold an entry, or
  press SELECT, to drag it anywhere (the others make room), make it big or small (R, or the button on its corner) or
  hide it.
- **A guide** that shows how NSA works in short tours on the real screens: a bubble points at each
  button and you do each step yourself. After an update it offers only what is new; Settings →
  Guide plays any chapter again.
- **NSA's own keyboard** for searches, names, URLs and the text editor.
- **Three themes**: *Cards*, *Pixel* (a pixel font, square corners and bars in blocks) and *HOME*
  (big icon tiles and dialogs as a sheet from the bottom), in day and night colours with eight
  accent colours and a yellow one. Settings → Theme. **Your own themes**, colour by colour, on the console or on the
  web page (kept in `/nsa/themes/`).
- English, Spanish, Catalan, French, German, Italian, Portuguese, Russian, Japanese and Chinese, simplified and traditional (a list in Settings), stereoscopic 3D on the top screen, touch and button controls everywhere
  (pressed look, long press for an item's actions, flick scrolling, draggable scrollbars), and optional
  interface sounds, each with its own volume slider (sounds and music).
- Optional **background music** made by the console itself, with no files: soft lift music, a calm
  variation, or a bossa with marimba. The last song of each style is kept and plays at once next time
  while a new one is made, and they crossfade.
- A banner and a short jingle of its own in the HOME menu.
- A **System** page with the console's model, versions, network, battery and storage; **Console settings** (Wi-Fi
  connections saved, edited or set up from a QR code, Date and time from the Internet, custom screen brightness) and
  **Hardware tests** (buttons, touch screen, sticks, sliders and motion sensors).
- **Changelog** in your language (Settings), and NSA's own boot logo when it starts from the HOME menu.

<p align="center">
  <img src="docs/images/menu-edit.png" width="360" alt="Editing the main menu: a big entry with its size button, one hidden">
  <img src="docs/images/guide-tour.png" width="360" alt="A tour of the guide on the main menu">
  <img src="docs/images/guide.png" width="360" alt="The guide's chapters">
  <img src="docs/images/theme-pixel.png" width="360" alt="The Pixel theme">
  <img src="docs/images/theme-home.png" width="360" alt="The HOME theme">
  <img src="docs/images/settings.png" width="360" alt="Settings">
  <img src="docs/images/system.png" width="360" alt="The System page">
</p>

### For developers

- With the **FTP server** on (Settings → FTP server, on by default) the SD card is served on port 5021. Driving
  the console from a computer (`SITE SCREENSHOT`, `SITE PRESS`, `SITE TAP`, `SITE DRAG`, and
  `SITE INSTALL` to update NSA itself) is off in normal builds: it turns on while the file
  `sdmc:/nsa/remote.flag` exists on the SD card (DEBUG builds always have it on). See
  [CLAUDE.md](CLAUDE.md).
- Plans, decisions and what is still to test on a console are in [docs/PLAN.md](docs/PLAN.md).

> The pictures are from a New 3DS XL and a 2DS. Some of the newer features have so far run only in an
> emulator, not on every console; the "Pending tests" list in `docs/PLAN.md` says which. Report
> what does not work as an [issue](https://github.com/tinaut1986/NSA/issues).

## Building

Requires [devkitARM](https://devkitpro.org/wiki/Getting_Started) with the 3DS libraries `citro3d`,
`curl`, `mbedtls`, `zlib`, `jansson`, `libvorbisidec` and `libogg` from the devkitPro pacman
repository. `bannertool` and `makerom` are vendored in `tools/bin/`.

```sh
export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
git submodule update --init          # buildtools
make -j"$(nproc)" DEBUG=1            # output/3ds-arm/NSA.cia and 3ds/NSA/NSA.3dsx
make -C tests                        # host tests (needs libjansson-dev)
```

On Windows, build from devkitPro's MSYS2 shell, not from Git Bash (devkitPro's `make` gets no environment there).

`DEBUG=1` starts the FTP server with the app and shows DEBUG next to the version. Without it the
version comes from the git tags (`tools/version.sh`).

## Credits

- **FBI** by [Steveice10](https://github.com/Steveice10): the base this is built on (MIT, see
  `LICENSE.txt`). Original banner by
  [OctopusRift](http://gbatemp.net/members/octopusrift.356526/) and logo by
  [PabloMK7](http://gbatemp.net/members/pablomk7.345712/); NSA draws its own icon and banner.
- SPI protocol information: [TuxSH](https://github.com/TuxSH/)
  ([TWLSaveTool](https://github.com/TuxSH/TWLSaveTool)).
- Icons: [Tabler Icons](https://tabler.io/icons) (MIT) and a few of NSA's own.
- Simplified Chinese font: [Noto Sans CJK SC](https://github.com/notofonts/noto-cjk) (SIL Open Font License 1.1), baked by `tools/make_hanfont.py`
- Pixel theme font: [Pixelify Sans](https://github.com/eifetx/Pixelify-Sans) (SIL Open Font
  License 1.1, `tools/fonts/`).
- Audio decoding: [dr_libs](https://github.com/mackron/dr_libs) (public domain / MIT-0) and
  Tremor; pictures: [stb_image](https://github.com/nothings/stb); QR codes:
  [quirc](https://github.com/dlbeer/quirc).
- Open source DSP firmware: [OpenDSP](https://github.com/Cellenseres/OpenDSP) by Cellenseres (BSD 2-Clause, the
  licence in `romfs/opendsp/LICENSE.txt`). NSA carries a copy of its `opendsp.cdc` for consoles without internet.
- Button remapping: [Rehid](https://github.com/hax0kartik/rehid) by hax0kartik, downloaded from its GitHub release (not
  carried by NSA).
- DS games in the HOME Menu: [nds-bootstrap](https://github.com/DS-Homebrew/nds-bootstrap) and its
  [forwarder pack](https://github.com/RocketRobz/NTR_Forwarder) by the DS-Homebrew team and RocketRobz, and Olmectron's
  forwarder template, all downloaded when needed.
- `.7z` archives: 7-Zip's LZMA SDK by Igor Pavlov (public domain).
