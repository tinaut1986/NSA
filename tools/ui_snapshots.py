#!/usr/bin/env python3
"""Screenshots of NSA's screens in Azahar, to check that a change (a refactor) draws everything as before.

    tools/ui_snapshots.py run --nsa output/3ds-arm/3ds/NSA/NSA.3dsx --out snaps/before
    tools/ui_snapshots.py run --nsa <the other build> --out snaps/after
    tools/ui_snapshots.py compare snaps/before snaps/after --out snaps/diff

`run` starts the .3dsx in Azahar (Windows: `C:\\Program Files\\Azahar\\azahar.exe`, its SD card in
`%APPDATA%\\Azahar\\sdmc`) on a fixed SD card: Azahar's own `sdmc:/nsa/` is put aside and replaced by fixed settings (web
server on, music and sounds off, the guide already seen, a fixed theme and language), plus a few test folders
(`/0-ui-fixture/`, first in the list, test mods in `/luma/titles/`). It walks every screen through the web server's input API (STEPS below),
saves both screens of each step as `<n>-<name>.png` (400x480, the bottom screen at x=40), then closes Azahar and puts
everything back as it was.

`compare` lists the steps whose pictures differ, ignoring the top bar (clock, battery, Wi-Fi), and writes for each a
picture with the differing pixels in red, and `report.html`. Run the same build twice first to see that it gives the same
pictures (`compare` of the two should say "same").

Needs Pillow. Only Azahar's own SD card is touched, never a console.
"""
import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.request

AZAHAR = r'C:\Program Files\Azahar\azahar.exe'
SDMC = os.path.join(os.environ.get('APPDATA', ''), 'Azahar', 'sdmc')
BASE = 'http://127.0.0.1:8080'
TOP_BAR = 22   # Rows of the top screen with the clock, the battery and the Wi-Fi.

# Each step: a name and what to do before the picture is taken. Actions: ('press', keys, frames), ('tap', x, y),
# ('wait', s). Every action is followed by a short pause; every step waits SETTLE seconds before its picture, for the
# animations (sliding focus, fades) to end.
SETTLE = 2.5


def P(key, times=1, frames=4):
    """A key pressed `times` times (a press held through the API moves only once)."""
    return [('press', key, frames)] * times


def T(x, y):
    return [('tap', x, y)]


def W(s):
    return [('wait', s)]


# Back to the main menu from anywhere this walk goes (B at the main menu does nothing).
HOME = (P('B') + W(0.8)) * 4

# Where things are on the bottom screen (Cards theme): the main menu's tiles, a list's rows.
MENU = {'sd': (56, 74), 'titles': (160, 75), 'remote': (264, 75), 'store': (36, 142), 'cleanup': (98, 142),
        'advanced': (160, 142), 'system': (222, 142), 'settings': (284, 142)}
ROW = [48, 86, 124, 162]   # The first four rows of a list.
NAME_SORT_CHIP = (177, 101)   # Files' filter: Sort by -> Name.

# Never an A on a question that deletes: questions are only looked at, and closed with B.
STEPS = [
    ('main-menu', []),
    ('main-menu-right', P('RIGHT')),
    ('sd-card', T(*MENU['sd']) + W(2)),
    ('sd-filter', P('L')),
    ('sd-filter-sort-reversed', T(*NAME_SORT_CHIP)),
    ('sd-filter-closed', T(*NAME_SORT_CHIP) + P('B')),
    ('sd-folder-sheet', P('X')),
    ('sd-search', P('B') + W(0.5) + P('R')),
    ('fixture-folder', P('B') + W(1) + T(160, ROW[0]) + W(2)),
    ('fixture-file-sheet', P('DOWN') + P('A', frames=70)),
    ('fixture-file-sheet-down', P('DOWN', 2)),
    ('fixture-select-mode', P('B') + W(1) + P('Y') + P('A') + P('DOWN') + P('A')),
    ('fixture-select-sheet', P('X')),
    ('fixture-folder-again', P('B') + W(0.5) + P('B') + W(1)),
    ('image-viewer', T(160, ROW[1]) + W(2)),
    ('image-viewer-next', P('RIGHT') + W(2)),
    ('image-viewer-zoom', P('A')),
    ('text-viewer', P('B') + W(1) + T(160, ROW[3]) + W(2)),
    ('main-menu-again', HOME),
    ('titles', T(*MENU['titles']) + W(4)),
    ('titles-filter', P('L')),
    ('titles-rows', P('B') + P('Y')),
    ('titles-sheet', P('A')),
    ('titles-sheet-end', P('UP')),
    ('titles-grid', P('B') + W(0.5) + P('Y')),
    ('cleanup-group', HOME + T(*MENU['cleanup']) + W(1)),
    ('tickets', T(160, 47) + W(4)),
    ('tickets-filter', P('L')),
    ('extsave', P('B') + W(0.5) + P('B') + W(1) + T(160, 115) + W(4)),
    ('extsave-filter', P('L')),
    ('mods', P('B') + W(0.5) + P('B') + W(1) + T(160, 149) + W(3)),
    ('mods-filter', P('L')),
    ('mods-summary-sheet', P('B') + W(0.5) + P('A')),
    ('mods-rows', P('B') + W(0.5) + P('Y') + P('DOWN')),
    ('mods-sheet', P('A')),
    ('mods-grid', P('B') + W(0.5) + P('Y')),
    ('pending', P('B') + W(1) + T(160, 81) + W(3)),
    ('store', HOME + T(*MENU['store']) + W(3)),
    ('system', HOME + T(*MENU['system']) + W(2)),
    ('system-network', P('DOWN')),
    ('system-storage', P('DOWN')),
    ('settings', HOME + T(*MENU['settings']) + W(2)),
    ('settings-down', P('DOWN', 3)),
    ('remote-install', HOME + T(*MENU['remote']) + W(2)),
    ('advanced-warning', HOME + T(*MENU['advanced']) + W(1)),
    ('advanced', P('A') + W(1)),
    ('main-menu-end', HOME),
]

FIXTURE_SETTINGS = {
    'channel': 'beta', 'auto_update': False, 'ftp_server': True, 'web_server': True,
    'ftp_pauses_music': False, 'ftp_music_asked': True,
    'theme': 'cards', 'accent': 'green', 'night_mode': 'off', 'language': 'es',
    'sounds': 0, 'sound_volume': 0, 'music': 0, 'music_volume': 0,
    'guide_seen': '', 'guide_known': 0xFFFF,
}

FIXTURE_MODS = ['0004000000030700', '0004000000055D00']   # One with a file, one empty.


def log(*a):
    print(*a, flush=True)


# ---- the SD card ----

class Fixture:
    """Puts Azahar's sdmc:/nsa/ aside and lays out the test SD card; restore() puts it all back."""

    def __init__(self, lang):
        self.nsa = os.path.join(SDMC, 'nsa')
        # Kept outside the SD card, so that no screen lists it.
        self.saved = os.path.join(os.environ.get('LOCALAPPDATA', os.path.expanduser('~')), 'nsa-ui-snapshots-saved-nsa')
        self.folder = os.path.join(SDMC, '0-ui-fixture')
        self.luma = os.path.join(SDMC, 'luma')
        self.lumaExisted = os.path.exists(self.luma)
        self.lang = lang

    def setup(self):
        if os.path.exists(self.saved):
            sys.exit(f'{self.saved} is there: an earlier run did not finish. Put it back as {self.nsa} first.')

        if os.path.exists(self.nsa):
            shutil.move(self.nsa, self.saved)

        os.makedirs(self.nsa)
        settings = dict(FIXTURE_SETTINGS, language=self.lang)
        with open(os.path.join(self.nsa, 'settings.json'), 'w', encoding='utf-8', newline='\n') as f:
            json.dump(settings, f, indent=2)

        from PIL import Image, ImageDraw
        os.makedirs(os.path.join(self.folder, 'sub folder'))
        for name, colour in (('a-picture.png', (200, 60, 60)), ('b-picture.png', (60, 120, 200))):
            img = Image.new('RGB', (320, 200), colour)
            ImageDraw.Draw(img).rectangle((40, 40, 280, 160), outline=(255, 255, 255), width=6)
            img.save(os.path.join(self.folder, name))
        with open(os.path.join(self.folder, 'c-notes.txt'), 'w', encoding='utf-8', newline='\n') as f:
            f.write('NSA UI snapshot fixture.\n' + ''.join(f'Line {i}: the quick brown fox jumps over the lazy dog.\n' for i in range(1, 40)))
        with open(os.path.join(self.folder, 'sub folder', 'inside.bin'), 'wb') as f:
            f.write(bytes(range(256)) * 16)

        titles = os.path.join(self.luma, 'titles')
        for i, tid in enumerate(FIXTURE_MODS):
            path = os.path.join(titles, tid)
            os.makedirs(os.path.join(path, 'romfs') if i == 0 else path, exist_ok=True)
            if i == 0:
                with open(os.path.join(path, 'romfs', 'ui-fixture.txt'), 'w') as f:
                    f.write('fixture\n')

    def restore(self):
        if os.path.exists(self.saved):
            if os.path.exists(self.nsa):
                shutil.rmtree(self.nsa)
            shutil.move(self.saved, self.nsa)

        if os.path.exists(self.folder):
            shutil.rmtree(self.folder)

        if self.lumaExisted:
            for tid in FIXTURE_MODS:
                shutil.rmtree(os.path.join(self.luma, 'titles', tid), ignore_errors=True)
        elif os.path.exists(self.luma):
            shutil.rmtree(self.luma)


# ---- Azahar and the web server ----

def pin():
    with open(os.path.join(SDMC, 'nsa', 'web-pin.txt')) as f:
        return f.read().strip()


def call(method, path, timeout=20):
    req = urllib.request.Request(BASE + path, method=method, headers={'X-Pin': pin()})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def kill_azahar():
    subprocess.run(['taskkill', '/F', '/IM', 'azahar.exe'], capture_output=True)
    time.sleep(1.5)


def start_azahar(nsa):
    kill_azahar()
    subprocess.Popen([AZAHAR, os.path.abspath(nsa)])
    # Azahar asks about its own update first: dismiss it.
    ps = ('$w = New-Object -ComObject WScript.Shell; for($i=0;$i -lt 20;$i++){ '
          'if($w.AppActivate("Actualización disponible") -or $w.AppActivate("Update available")){ $w.SendKeys("{TAB}{ENTER}"); break }; '
          'Start-Sleep -Milliseconds 700 }')
    subprocess.run(['powershell', '-NoProfile', '-Command', ps], capture_output=True)
    for _ in range(60):
        try:
            if os.path.exists(os.path.join(SDMC, 'nsa', 'web-pin.txt')):
                call('GET', '/api/screens', timeout=5)
                return
        except (urllib.error.URLError, OSError):
            pass
        time.sleep(1)
    sys.exit('NSA\'s web server did not answer in Azahar')


def screens():
    from PIL import Image
    data = call('GET', '/api/screens')

    def rgb565(buf, w, h):
        img = Image.new('RGB', (w, h))
        px = img.load()
        for i in range(w * h):
            v = struct.unpack_from('<H', buf, i * 2)[0]
            px[i % w, i // w] = (((v >> 11) & 31) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3)
        return img

    img = Image.new('RGB', (400, 480))
    img.paste(rgb565(data, 400, 240), (0, 0))
    img.paste(rgb565(data[400 * 240 * 2:], 320, 240), (40, 240))
    return img


def act(action):
    kind = action[0]
    if kind == 'wait':
        time.sleep(action[1])
        return
    if kind == 'press':
        call('POST', f'/api/input/press?keys={action[1]}&frames={action[2]}')
        time.sleep(0.6 + action[2] / 60)
    elif kind == 'tap':
        call('POST', f'/api/input/tap?x={action[1]}&y={action[2]}&frames=4')
        time.sleep(0.6)


def run(args):
    os.makedirs(args.out, exist_ok=True)
    fixture = Fixture(args.lang)
    fixture.setup()
    try:
        start_azahar(args.nsa)
        time.sleep(4)
        only = set(args.only.split(',')) if args.only else None
        for n, (name, actions) in enumerate(STEPS):
            for a in actions:
                act(a)
            time.sleep(SETTLE)
            if only is None or name in only:
                screens().save(os.path.join(args.out, f'{n:02d}-{name}.png'))
                log(f'{n:02d} {name}')
    finally:
        kill_azahar()
        fixture.restore()


# ---- comparing two runs ----

def compare(args):
    from PIL import Image, ImageChops
    os.makedirs(args.out, exist_ok=True)
    names = sorted(set(os.listdir(args.a)) | set(os.listdir(args.b)))
    rows = []
    changed = 0
    for name in names:
        if not name.endswith('.png'):
            continue
        pa, pb = os.path.join(args.a, name), os.path.join(args.b, name)
        if not (os.path.exists(pa) and os.path.exists(pb)):
            rows.append((name, 'missing in ' + ('B' if os.path.exists(pa) else 'A'), None))
            changed += 1
            continue
        ia, ib = Image.open(pa).convert('RGB'), Image.open(pb).convert('RGB')
        diff = ImageChops.difference(ia, ib).convert('L').point(lambda v: 255 if v else 0)
        diff.paste(0, (0, 0, 400, TOP_BAR))
        count = diff.histogram()[255]
        if count:
            changed += 1
            overlay = ib.copy()
            overlay.paste((255, 0, 0), mask=diff)
            out = Image.new('RGB', (1200, 480))
            out.paste(ia, (0, 0))
            out.paste(ib, (400, 0))
            out.paste(overlay, (800, 0))
            out.save(os.path.join(args.out, name))
            rows.append((name, f'{count} px differ', name))
        else:
            rows.append((name, 'same', None))

    with open(os.path.join(args.out, 'report.html'), 'w', encoding='utf-8') as f:
        f.write('<!doctype html><meta charset="utf-8"><title>UI snapshots</title><body style="font-family:sans-serif">')
        f.write(f'<h1>{changed} of {len(rows)} steps differ</h1><p>A: {args.a}<br>B: {args.b}</p><table>')
        for name, what, img in rows:
            f.write(f'<tr><td>{name}</td><td>{what}</td></tr>')
            if img:
                f.write(f'<tr><td colspan=2><img src="{img}" width=900><br>A | B | B with the differences in red</td></tr>')
        f.write('</table></body>')

    for name, what, _ in rows:
        log(f'{what:>16}  {name}')
    log(f'{changed} of {len(rows)} steps differ')
    return 1 if changed else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('run')
    r.add_argument('--nsa', required=True, help='the NSA.3dsx to start')
    r.add_argument('--out', required=True)
    r.add_argument('--lang', default='es')
    r.add_argument('--only', help='comma-separated step names to save (all are still walked)')
    c = sub.add_parser('compare')
    c.add_argument('a')
    c.add_argument('b')
    c.add_argument('--out', required=True)
    args = ap.parse_args()
    if args.cmd == 'run':
        run(args)
    else:
        sys.exit(compare(args))


main()
