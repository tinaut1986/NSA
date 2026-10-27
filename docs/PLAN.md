# NSA plan

Start every session here. It holds where things stand, what comes next and why
things are the way they are. Keep it current in the same change that alters it.

## Status (2026-10-27)

| Branch | State |
|---|---|
| `release` | v0.1.0 beta (published), the self-update restart fix and the whole UI redesign (`feat/ui-themes`, merged with the owner's OK). No new beta tagged yet. |
| `feat/system-status`, `feat/grouped-menu`, `fix/icon-sizes`, `fix/list-loading` | Merged into `release` with the owner's OK: top bar status and System entry; the grouped main menu (big tiles, Cleanup and data, Advanced with one warning per session); icons from 16/32 px atlases; whole-pixel 3D; Titles loading into placeholders, presses that follow their item, a selection that keeps its row. |
| `feat/remember-filters` | Merged into `release` with the owner's OK: Titles, Files and Ext save data remember their filters, sort and view (`settings.json`); Titles sorts live while loading with the selected row kept still; battery digits 8 px tall and centred (measured on the console's font), clean 2 px terminal. |
| `master` | v0.1.0 has not been promoted to stable yet. |

The UI redesign on `release` holds:

1. Themed interface: drawing primitives, Tabler icons, the "Cards" theme (day and
   night), 8 accent colours, night mode, Spanish, the new main menu and Settings,
   FTP remote control (`SITE SCREENSHOT`, `PRESS`, `TAP`).
2. Titles: rows with icons, icon grid, filter and sort popover, search, actions as
   a list; updates and DLC grouped under their game, kind and region filters.
3. Files: path in the title bar, tools (clipboard, filter and sort, search, folder
   actions), icon by kind (a CIA shows its title's icon), CIA details on top, the
   folder left selected when going up.
4. Touch and motion for every screen: pressed look, act on release, long press with
   a radial fill (opens an item's actions in Files and Titles), flick scrolling,
   sheets/popovers/prompts animate in, the top card's contents cross-fade.
5. Storage bars (`widget_storage`): Files, Titles and the main menu per entry.
6. Stereoscopic 3D on the top screen: background, bar and cards on three depths.

**On the console now:** the `fix/list-loading` build (DEBUG), installed over FTP.
Still to confirm there: the battery percentage (needs `mcu::HWC`), the Wi-Fi SSID and
the system version in System (Azahar gives none of them). Console at
`192.168.1.133`.

## What the owner asked for (the direction)

The look is right, but navigation still feels like FBI: lists, Select for options,
another list for actions. Every screen should move to the Titles pattern:

- Tool icons in the title bar (back, view, filter, search) instead of Select menus.
- A popover for filter and sort, a sheet (list of rows) for actions.
- Rows with the item's picture and secondary info; a grid where pictures matter.
- The top screen shows the selected item's card and its details in cells.
- Modal buttons with centred labels; one tap opens; D-Pad and touch everywhere.

## Next

1. **Apply the Titles pattern to the other screens**, one branch or commit each:
   Tickets, Pending titles, Extra save data, System save data, Remote install,
   Dump NAND (Files is done). Translate each screen as it
   moves (`_()`, `source/core/i18n_es.c`); many of FBI's strings are still English.
2. **Themes "Pixel" and "HOME"** (mockups were shown and liked):
   - HOME: a main menu of big icon tiles in pages, modals as a sheet from the
     bottom. Mostly layout work on pieces that exist (grid, sheet).
   - Pixel: needs a bundled pixel font (OFL or similar), square corners, thick
     borders, block progress bars, yellow highlight.
   - Today only "Cards" exists; Settings → Theme already lists themes.
3. **Themes from the SD card** (`sdmc:/nsa/themes/`), data only: palette, radii,
   borders, font, layout variants.
4. **NSA icon and banner** (HOME menu and in-app): still FBI's. Ask the owner for a
   direction first.
5. **Top-screen information**: the owner wants to restructure what the top screen
   shows in general; ask before designing it.
6. Possibly move Remote install off port 5000 so it does not pause the FTP server
   (the owner raised it; not decided).

## Decisions

- Own app (`0xFB1A0`), own data folder (`sdmc:/nsa/`, import from `/fbi/` offered
  once), own versions from `v0.1.0`. Upstream is noted as "based on FBI 2.6.1".
- Updates: Stable/Beta channels from GitHub releases; the changelog joins every
  release since the installed one. Ported from `../zelda-tmc-3ds` (host tests in
  `tests/`).
- FTP server on port 5000, SD card only; `SITE INSTALL` refuses NAND titles and never
  deletes a title to force a downgrade (it would delete the game's saves). Remote
  install's "Receive URLs" also uses 5000 and pauses the server while open.
- `DEBUG=1` builds start the FTP server; release builds only when turned on.
- FBI's PNG themes and `textcolor.cfg` were dropped (agreed) for drawn shapes.
- Strings go through `_("English")`; literals only (cached by address).
- One-line text turns line breaks into spaces (some titles' descriptions have them).
- Titles' kind comes from the title ID's upper half (`00040000` game, `0004000E`
  update, `0004008C` DLC, `00040002` demo, `00048xxx` DSiWare, else system); a game,
  its update and its DLC share the lower half.

## Known issues

- `SITE SCREENSHOT` on the console sometimes answers "not taken in time" after
  15 s, yet the files appear later. Suspected: NSA was in the background (HOME menu
  or lid closed), which pauses its main loop. Not confirmed; the reply now says how
  long a capture took.
- In Azahar, HTTPS (the updater) does not answer; its virtual SD has no updates or
  DLC, so grouping and kind filters can only be checked on the console.
- The system font has no "·": use the button glyphs or plain spacing.
