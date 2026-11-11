# Your own themes

NSA reads themes from `sdmc:/nsa/themes/`: each `.json` file there is a theme, listed in
Settings -> Theme after Cards, Pixel and HOME (by name). Settings reads the folder again each time
it opens, so a file copied over FTP or the web page shows up without restarting NSA.

The easiest way is **Settings -> Edit the theme**, on the console: every value below, the colours
picked with a finger, seen at once on both screens. A built-in theme is saved as a new file named
after the name you give it.

Or from a computer's browser: the web page (Settings -> Web server on the console) has **Edit the
theme** under Settings -> Appearance, with the same values; the page and the console show the theme
while you change it, and **Save and use** stores it on the SD card.

To edit a file on a computer instead, start from **Settings -> Save the theme as a file**: it writes the theme in use as
`/nsa/themes/<name>-2.json` (named "<Name> 2"), with every key. Change its colours on a computer
and pick it under Theme.

A theme is only data: colours, corners and a few switches. It cannot change how a screen is laid
out. The accent colour and night mode are still chosen in Settings, and work with any theme.

## The file

```json
{
  "name": "Ocean",
  "base": "home",
  "day": {
    "background": "#D8ECF4",
    "surface": "#F4FBFE",
    "border": "#9CC8DA",
    "text": "#0B2A3A",
    "textMuted": "#3F6577",
    "scrim": "#0B2A3A70",
    "success": "#0F7A56",
    "successSoft": "#DDF3EA",
    "danger": "#C83232",
    "warning": "#9A6200"
  },
  "night": { "...": "the same keys, for night mode" },
  "radius": 14,
  "radiusSmall": 12,
  "border": 1,
  "font": "system",
  "blockBars": false,
  "sheetPrompts": true,
  "menu": "tiles",
  "grid": "pages"
}
```

Every key may be left out: it then comes from `base`.

| Key | What it is |
|---|---|
| `name` | The name in Settings. Without it, the file's name. |
| `base` | `cards` (the default), `pixel` or `home`: the built-in theme that gives what the file leaves out. |
| `day`, `night` | The palette for each mode. Colours are `#RRGGBB`, or `#RRGGBBAA` with transparency. |
| `background` | Behind everything. |
| `surface` | Cards, buttons, rows and bars. |
| `border` (colour) | Outlines and lines. |
| `text`, `textMuted` | Text, and secondary text such as hints. |
| `scrim` | The veil behind a dialog (usually with transparency). |
| `success`, `successSoft` | Done and in use, and the background of their badges. |
| `danger`, `warning` | Deleting and errors, and warnings. |
| `radius`, `radiusSmall` | Rounded corners of cards and dialogs, and of buttons and rows, in pixels (0 to 24; 0 is square). |
| `border` (number) | The width of outlines, in pixels (0 to 4). |
| `font` | `system` (the console's) or `pixel` (the Pixel theme's). Only the fonts NSA carries: a font is baked into a picture on a computer (`tools/make_pixelfont.py`), so a new one comes with an update of NSA. |
| `blockBars` | `true`: progress bars in blocks. |
| `sheetPrompts` | `true`: dialogs rise from the bottom as a sheet (HOME). |
| `menu` | `grid` (buttons in two columns) or `tiles` (big icon tiles, four to a row, as HOME). |
| `grid` | How the icon grids of Titles, Tickets, the Store... go: `scroll` (down, with a scrollbar) or `pages` (pages that turn sideways, with dots, as HOME). |

Keys NSA does not know are left alone. A file that is not JSON, or has a wrong value, is not
listed: Settings -> Theme says which file and why, under its sample. Up to 32 themes are read,
each up to 32 KB.
