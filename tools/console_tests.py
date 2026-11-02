#!/usr/bin/env python3
"""Tests on the real consoles: exporting a CIA and sending between two 3DS, with failures
made on purpose (see docs/PLAN.md, "Tests on the consoles").

Both consoles run a DEBUG build of NSA (its FTP server is on), sit on the main menu and are
on the same Wi-Fi as this PC. Then:

  console_tests.py --a 192.168.1.133 --b 192.168.1.139     everything
  console_tests.py --a IP                                   only what needs one console
  console_tests.py --a IP --b IP --only send_file_cut       some tests (prefix match)
  console_tests.py --list                                   the tests

A sends and exports, B receives. Nothing is installed and nothing is deleted outside
/nsa/tests/, /nsa/received/ and the exported CIA of the title used. Each test drives the screens
over the FTP server (SITE TEST UI says what is on screen, SITE PRESS answers), arms faults with
SITE FAULT and checks the files with SITE SHA256. A failed test keeps both consoles' transfer
logs and screenshots in test-results/.
"""
import argparse, ftplib, hashlib, io, json, os, random, socket, struct, sys, threading, time, traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

PORT = 5021
# Big enough that a cut after RESUME_SKIP chunks of 256 KiB can go on from a saved
# place (it is saved every 4 MiB), small enough to be quick.
PAYLOAD_SIZE = 16 * 1024 * 1024
RESUME_SKIP = 28
MIN_TITLE_SIZE = 12 * 1024 * 1024
TESTS_DIR = '/nsa/tests'
RECEIVED_DIR = '/nsa/received'
CIAS_DIR = '/nsa/cias'
TMP_DIR = '/nsa/tmp'
HINT_ROW = 'Search nearby (no Wi-Fi)'


class TestFailed(Exception):
    pass


def check(condition, message):
    if not condition:
        raise TestFailed(message)


class Console:
    def __init__(self, label, host):
        self.label, self.host = label, host
        self.ftp = None
        self.connect()
        self.last = None

    def connect(self):
        self.ftp = ftplib.FTP()
        # Hashing a big file on the console takes a while (SITE SHA256).
        self.ftp.connect(self.host, PORT, timeout=180)
        self.ftp.login()

    def cmd(self, line, ok_errors=False):
        for attempt in range(3):
            try:
                return self.ftp.sendcmd(line)
            except (ftplib.error_perm, ftplib.error_temp) as e:
                if ok_errors:
                    return str(e)
                raise TestFailed(f'{self.label}: {line} -> {e}')
            except (OSError, EOFError, ftplib.error_reply):
                # The FTP server may cut a connection after the radio changes: again.
                time.sleep(2)
                self.connect()
        raise TestFailed(f'{self.label}: no answer to {line}')

    def ui(self):
        reply = self.cmd('SITE TEST UI')
        return json.loads(reply[4:])

    def press(self, keys, frames=4):
        self.cmd(f'SITE PRESS {keys} {frames}')

    def fault(self, point, skip=0, count=1):
        self.cmd(f'SITE FAULT {point} {skip} {count}')

    def clear_faults(self):
        self.cmd('SITE FAULT CLEAR')

    def faults(self):
        return self.cmd('SITE FAULT LIST')[4:]

    def sha256(self, path):
        reply = self.cmd(f'SITE SHA256 {path}', ok_errors=True)
        return reply[4:].strip() if reply.startswith('200') else None

    def listdir(self, path):
        try:
            return [os.path.basename(n) for n in self.ftp.nlst(path)]
        except ftplib.error_perm:
            return []

    def list_long(self, path):
        """[(is a folder, name)] of a folder, or None when it is not there."""
        rows = []
        try:
            self.ftp.retrlines(f'LIST {path}', rows.append)
        except ftplib.error_perm:
            return None
        out = []
        for row in rows:
            parts = row.split(None, 8)
            if len(parts) == 9:
                out.append((parts[0][0] == 'd', parts[8]))
        return out

    def size(self, path):
        try:
            self.ftp.voidcmd('TYPE I')
            return self.ftp.size(path)
        except ftplib.error_perm:
            return None

    def delete(self, path):
        try:
            self.ftp.delete(path)
        except ftplib.error_perm:
            pass

    def mkdir(self, path):
        try:
            self.ftp.mkd(path)
        except ftplib.error_perm:
            pass

    def put(self, path, data):
        self.ftp.storbinary(f'STOR {path}', io.BytesIO(data))

    def get(self, path):
        out = io.BytesIO()
        self.ftp.retrbinary(f'RETR {path}', out.write)
        return out.getvalue()

    def log_size(self):
        try:
            return len(self.get('/nsa/transfer-log.txt'))
        except ftplib.error_perm:
            return 0

    def log_since(self, mark):
        try:
            data = self.get('/nsa/transfer-log.txt')
        except ftplib.error_perm:
            return ''
        # The log starts over at 96 KB: then all of it is new.
        return (data[mark:] if len(data) >= mark else data).decode('utf-8', 'replace')

    def home(self):
        """Back to the main menu: B on everything (No, Reject, back)."""
        for _ in range(25):
            ui = self.ui()
            if ui['depth'] <= 1:
                return
            if ui['kind'] == 'prompt' and 'B' not in ui['buttons']:
                self.press('A')
            else:
                self.press('B')
            time.sleep(0.6)
        raise TestFailed(f'{self.label}: could not get back to the main menu ({self.ui()})')

    def screenshot(self, folder, prefix):
        try:
            self.cmd('SITE SCREENSHOT', ok_errors=True)
            time.sleep(3)
            for screen in ('top', 'bottom'):
                with open(os.path.join(folder, f'{prefix}-{screen}.bmp'), 'wb') as f:
                    f.write(self.get(f'/nsa/screenshots/{screen}.bmp'))
        except Exception:
            pass


# ---- driving the screens ----

class Rule:
    """When `console` shows a dialog called `name` (and `text` is in its message), choose
    `choice`: an option's label, a button ('A', 'B'...), PEER (the first console in the send
    list) or SAVE_HERE (the folder picker's first row)."""

    def __init__(self, console, name, choice, text=None, kind=None, once=False):
        self.console, self.name, self.choice, self.text, self.kind, self.once = console, name, choice, text, kind, once
        self.used = 0

    def matches(self, console, ui):
        if console is not self.console or (self.once and self.used > 0):
            return False
        if self.name is not None and ui['name'] != self.name:
            return False
        if self.kind is not None and ui['kind'] != self.kind:
            return False
        return self.text is None or (ui['text'] or '').find(self.text) >= 0


def answer(console, ui, choice):
    if choice == 'PEER':
        if ui['kind'] != 'list' or ui['count'] < 1 or ui['selectedName'] in (None, HINT_ROW):
            return False
        console.press('A')
        return True
    if choice == 'ENTER':
        # The folder picker: row 0 is "Save here", row 1 the first folder: go into it.
        if ui['kind'] != 'list' or ui['count'] < 2:
            return False
        console.press('DOWN' if ui['selected'] < 1 else 'A')
        return True
    if choice == 'SAVE_TOP':
        # "Save here" is row 0, but the selection may be on a folder: go up first.
        if ui['kind'] != 'list':
            return False
        console.press('UP' if ui['selected'] > 0 else 'A')
        return True
    if choice == 'SAVE_HERE':
        if ui['kind'] != 'list' or ui['selected'] != 0:
            return False
        console.press('A')
        return True
    if choice in ui['options']:
        button = ui['buttons'][ui['options'].index(choice)]
        console.press('A' if button == 'ANY' else button)
        return True
    console.press(choice)
    return True


def describe(ui):
    text = (ui['text'] or '').replace('\n', ' ')
    return f"{ui['kind']} '{ui['name']}'" + (f": {text}" if text else '')


class Driver:
    """Watches the consoles, answers their dialogs by the rules and remembers every dialog seen
    (`events`: (console label, name, text)) until `done(driver)` or the timeout."""

    def __init__(self, consoles, rules, done, timeout=240, stop_on_unknown=True):
        self.consoles, self.rules, self.done, self.timeout = consoles, rules, done, timeout
        self.stop_on_unknown = stop_on_unknown
        self.events = []
        self.seen = {}

    def saw(self, label, name, text=None):
        return any(e[0] == label and e[1] == name and (text is None or text in (e[2] or '')) for e in self.events)

    def run(self):
        begin = time.time()
        pressed = {}
        while time.time() - begin < self.timeout:
            for c in self.consoles:
                ui = c.ui()
                # A progress line changes all the time: an info counts once.
                key = (ui['depth'], ui['name'], ui['kind'], ui['text'] if ui['kind'] != 'info' else None)
                if self.seen.get(c.label) != key:
                    self.seen[c.label] = key
                    if ui['kind'] in ('prompt', 'list') or ui['name'] in ('Sending', 'Receiving', 'Exporting CIA'):
                        self.events.append((c.label, ui['name'], ui['text']))
                        print(f'      {c.label}: {describe(ui)}')
                    pressed.pop(c.label, None)

                if self.done(self):
                    return self

                if ui['kind'] not in ('prompt', 'list'):
                    continue
                # Pressed already: give the screen time to change before pressing again.
                if c.label in pressed and time.time() - pressed[c.label] < 2.5:
                    continue
                rule = next((r for r in self.rules if r.matches(c, ui)), None)
                if rule is None:
                    if ui['kind'] == 'prompt' and self.stop_on_unknown:
                        raise TestFailed(f'{c.label} asks something the test did not expect: {describe(ui)}')
                    continue
                if answer(c, ui, rule.choice):
                    rule.used += 1
                    pressed[c.label] = time.time()
            time.sleep(0.4)
        raise TestFailed(f'timed out; last dialogs: {self.events[-6:]}')


# ---- the CIA ----

def align(n, a=64):
    return (n + a - 1) // a * a


SIGNATURE_SIZES = {0x10003: 0x200 + 0x3C, 0x10004: 0x100 + 0x3C, 0x10005: 0x3C + 0x40}


def verify_cia(data, title_id=None):
    """Every content's SHA-256 against its TMD, and the TMD's own hashes. Raises TestFailed."""
    check(len(data) >= 0x2020, 'the CIA is shorter than its header')
    header_size, kind, version, cert_size, ticket_size, tmd_size, meta_size, content_size = struct.unpack_from('<IHHIIIIQ', data, 0)
    check(header_size == 0x2020, f'CIA header size {header_size:#x}')
    tmd_at = align(align(align(header_size) + cert_size) + ticket_size)
    content_at = align(tmd_at + tmd_size)
    check(content_at + content_size <= len(data), 'the CIA is shorter than its header says')

    sig = struct.unpack_from('>I', data, tmd_at)[0]
    check(sig in SIGNATURE_SIZES, f'TMD signature type {sig:#x}')
    tmd = data[tmd_at + 4 + SIGNATURE_SIZES[sig]:tmd_at + tmd_size]
    tmd_title = struct.unpack_from('>Q', tmd, 0x4C)[0]
    if title_id is not None:
        check(tmd_title == title_id, f'TMD title {tmd_title:016X}, expected {title_id:016X}')
    count = struct.unpack_from('>H', tmd, 0x9E)[0]
    infos = tmd[0xC4:0xC4 + 64 * 0x24]
    chunks = tmd[0xC4 + 64 * 0x24:0xC4 + 64 * 0x24 + count * 0x30]
    check(len(chunks) == count * 0x30, 'the TMD is cut short')
    check(hashlib.sha256(infos).digest() == tmd[0xA4:0xC4], 'TMD: the info records hash is wrong')
    check(hashlib.sha256(chunks).digest() == infos[4:0x24], 'TMD: the content records hash is wrong')

    offset = content_at
    for i in range(count):
        cid, index, ctype, size = struct.unpack_from('>IHHQ', chunks, i * 0x30)
        digest = chunks[i * 0x30 + 0x10:i * 0x30 + 0x30]
        check(not ctype & 1, f'content {index} is marked encrypted')
        check(offset + size <= len(data), f'content {index} is cut short')
        check(hashlib.sha256(data[offset:offset + size]).digest() == digest, f'content {index} (id {cid:08x}) does not match its hash')
        offset += size   # Contents follow each other with no alignment.
    return count


# ---- the tests ----

TESTS = []


def test(consoles):
    def register(fn):
        TESTS.append((fn.__name__, consoles, fn))
        return fn
    return register


class Context:
    def __init__(self, a, b, title, out):
        self.a, self.b, self.title, self.out = a, b, title, out
        self.reference = None   # SHA-256 of a clean export of the title, once made.
        self.title_id = None
        self.title_name = None
        self.title_size = 0

    def consoles(self):
        return [c for c in (self.a, self.b) if c is not None]

    def pick_title(self):
        if self.title_id is not None:
            return self.title_id
        if self.title:
            self.title_id = int(self.title, 16)
            return self.title_id
        reply = self.a.cmd('SITE TEST TITLES')
        rows = self.a.get(reply.split()[1]).decode('utf-8', 'replace').splitlines()
        # A game, an update or a DLC (not system titles, not DSiWare), smallest first; one of
        # some MiB, so that a fault after a few writes or chunks happens and a cut can go on
        # from a saved place.
        titles = [(row.split(' ', 3) + [''])[:4] for row in rows]
        titles = [t for t in titles if t[0][:8] in ('00040000', '0004000E', '0004008C') and int(t[1]) > 0]
        big = [t for t in titles if int(t[1]) >= MIN_TITLE_SIZE]
        for tid, size, version, name in (big or titles)[:1]:
            self.title_id, self.title_name, self.title_size = int(tid, 16), name, int(size)
            print(f'    title: {tid} "{name}", {int(size) / 1048576:.1f} MiB (the smallest of {MIN_TITLE_SIZE >> 20} MiB or more; --title to choose)')
            return self.title_id
        raise TestFailed('no title on the SD card to export')

    def tag(self):
        return f'[{self.pick_title():016X}]'

    # The exported CIA of the title on A, and how to find a received one on B.
    def exported(self, console, folder):
        names = [n for n in console.listdir(folder) if self.tag() in n and n.endswith('.cia')]
        return f'{folder}/{names[0]}' if names else None

    def forget_title_files(self, console, folder):
        for n in console.listdir(folder):
            if self.tag() in n:
                console.delete(f'{folder}/{n}')

    def payload(self, seed, size=PAYLOAD_SIZE, name='payload.bin', folder=TESTS_DIR):
        data = random.Random(seed).randbytes(size)
        self.a.mkdir(TESTS_DIR)
        self.a.mkdir(folder)
        path = f'{folder}/{name}'
        self.a.put(path, data)
        for suffix in ('', '.part', '.part.pos'):
            self.b.delete(f'{RECEIVED_DIR}/{name}{suffix}')
        return path, hashlib.sha256(data).hexdigest()


def export_rules(ctx, retry='Yes'):
    a = ctx.a
    return [Rule(a, 'Confirmation', 'Yes', text='Export the selected title'),
            Rule(a, 'Error', 'OK'),
            Rule(a, 'Confirmation', retry, text='Retry?'),
            Rule(a, 'Success', 'OK'),
            Rule(a, 'Failure', 'OK')]


def run_export(ctx, retry='Yes'):
    ctx.forget_title_files(ctx.a, CIAS_DIR)
    ctx.a.cmd(f'SITE TEST EXPORT {ctx.pick_title():016X}')
    finished = lambda d: (d.saw('A', 'Success') or d.saw('A', 'Failure') or (retry == 'No' and d.saw('A', 'Confirmation', 'Retry?'))) and ctx.a.ui()['depth'] <= 1
    return Driver([ctx.a], export_rules(ctx, retry), finished, timeout=900).run()


def export_and_check(ctx):
    d = run_export(ctx)
    check(d.saw('A', 'Success'), 'the export did not say it was done')
    path = ctx.exported(ctx.a, CIAS_DIR)
    check(path is not None, f'no CIA with {ctx.tag()} in {CIAS_DIR}')
    digest = ctx.a.sha256(path)
    count = verify_cia(ctx.a.get(path), ctx.pick_title())
    return d, digest, count


@test(1)
def export_clean(ctx):
    """Exports the title and checks the CIA: header, TMD hashes and every content's hash."""
    d, digest, count = export_and_check(ctx)
    ctx.reference = digest
    print(f'    {count} content(s), sha256 {digest[:16]}...')


def export_reference(ctx):
    if ctx.reference is None:
        export_clean(ctx)
    return ctx.reference


@test(1)
def export_retry_write_first(ctx):
    """An SD write fails inside the first content; Retry: the CIA is the same as a clean one."""
    reference = export_reference(ctx)
    ctx.a.fault('export.write', 1, 1)
    d, digest, _ = export_and_check(ctx)
    check(d.saw('A', 'Error'), 'the export did not report the failure')
    check(digest == reference, 'the CIA after a retry differs from a clean export')


@test(1)
def export_retry_read(ctx):
    """A read of the title fails partway; Retry: the CIA is the same as a clean one."""
    reference = export_reference(ctx)
    ctx.a.fault('export.read', 2, 1)
    d, digest, _ = export_and_check(ctx)
    check(d.saw('A', 'Error'), 'the export did not report the failure')
    check(digest == reference, 'the CIA after a retry differs from a clean export')


@test(1)
def export_give_up(ctx):
    """A write fails and the user does not retry: no "exported", no CIA left behind."""
    ctx.a.fault('export.write', 1, 1)
    d = run_export(ctx, retry='No')
    check(not d.saw('A', 'Success'), 'it said it exported')
    time.sleep(1)
    check(ctx.exported(ctx.a, CIAS_DIR) is None, 'a broken CIA was left in ' + CIAS_DIR)


def send_rules(ctx, retry='Yes', receiver_choice='Save'):
    a, b = ctx.a, ctx.b
    return [Rule(a, 'Send to...', 'PEER', kind='list', once=True),
            Rule(a, 'Error', 'OK'),
            Rule(a, 'Confirmation', retry, text='Retry?'),
            Rule(a, 'Success', 'OK'),
            Rule(a, 'Failure', 'OK'),
            Rule(b, 'Receive', 'Yes'),
            Rule(b, 'Incoming file', receiver_choice),
            Rule(b, None, 'SAVE_HERE', kind='list'),
            Rule(b, 'Error', 'OK'),
            Rule(b, 'Success', 'OK'),
            Rule(b, 'Failure', 'OK')]


def both_done(d):
    return d.saw('A', 'Success') and d.saw('B', 'Success')


def run_send(ctx, start, rules=None, done=both_done, timeout=300):
    ctx.b.cmd('SITE TEST RECEIVE')
    time.sleep(1.5)
    ctx.a.cmd(start)
    return Driver([ctx.a, ctx.b], rules or send_rules(ctx), done, timeout).run()


def check_received(ctx, name, digest):
    got = ctx.b.sha256(f'{RECEIVED_DIR}/{name}')
    check(got is not None, f'B has no {RECEIVED_DIR}/{name}')
    check(got == digest, f'{name} on B differs from what A sent')
    check(ctx.b.sha256(f'{RECEIVED_DIR}/{name}.part.pos') is None, 'the .part.pos is still there')


def check_resumed(ctx, log):
    starts = [line for line in log.splitlines() if 'from byte' in line]
    check(len(starts) >= 2, f'expected a second try in B\'s log, found {starts}')
    check(not starts[-1].rstrip().endswith('from byte 0'), f'the retry started over instead of going on: {starts[-1]}')


@test(2)
def send_file_clean(ctx):
    """A file from A to B over the Wi-Fi; B keeps an identical copy."""
    path, digest = ctx.payload(1)
    run_send(ctx, f'SITE TEST SEND {path}')
    check_received(ctx, 'payload.bin', digest)


@test(2)
def send_file_cut_sender(ctx):
    """A's connection drops partway; Retry on A, B goes on by itself from where it was."""
    path, digest = ctx.payload(2)
    mark = ctx.b.log_size()
    ctx.a.fault('send.chunk', RESUME_SKIP, 1)
    d = run_send(ctx, f'SITE TEST SEND {path}')
    check(d.saw('A', 'Confirmation', 'Retry?'), 'A did not offer to retry')
    check(sum(1 for e in d.events if e[0] == 'B' and e[1] == 'Receive') <= 1, 'B asked again for the same file')
    check_received(ctx, 'payload.bin', digest)
    check_resumed(ctx, ctx.b.log_since(mark))


@test(2)
def send_file_cut_receiver(ctx):
    """B drops a connection partway; Retry on A, B goes on from where it was."""
    path, digest = ctx.payload(3)
    mark = ctx.b.log_size()
    ctx.b.fault('recv.chunk', RESUME_SKIP, 1)
    run_send(ctx, f'SITE TEST SEND {path}')
    check_received(ctx, 'payload.bin', digest)
    check_resumed(ctx, ctx.b.log_since(mark))


@test(2)
def send_file_write_fails(ctx):
    """B's SD write fails; both say so, Retry goes on and the copy is identical."""
    path, digest = ctx.payload(4)
    ctx.b.fault('recv.write', RESUME_SKIP, 1)
    d = run_send(ctx, f'SITE TEST SEND {path}')
    check(d.saw('B', 'Error'), 'B did not report the failure')
    check_received(ctx, 'payload.bin', digest)


@test(2)
def send_file_read_fails(ctx):
    """A can not read its own file partway; it says so and Retry finishes it."""
    path, digest = ctx.payload(5)
    ctx.a.fault('send.read', RESUME_SKIP, 1)
    d = run_send(ctx, f'SITE TEST SEND {path}')
    check(d.saw('A', 'Error'), 'A did not report the failure')
    check_received(ctx, 'payload.bin', digest)


@test(2)
def stray_connection(ctx):
    """While B receives, something else connects to its port 5002 (another console's hello,
    then a silent connection): the transfer still finishes."""
    path, digest = ctx.payload(6, size=24 * 1024 * 1024)
    strays = []

    def poke(d):
        receiving = any(e[0] == 'B' and e[1] == 'Receiving' for e in d.events)
        if receiving and not strays:
            hello = b'NSAT' + struct.pack('>I', 6) + b'stray'.ljust(32, b'\0') + b'x.bin'.ljust(256, b'\0') + bytes(20)
            for payload in (hello, b''):
                s = socket.create_connection((ctx.b.host, 5002), timeout=5)
                if payload:
                    s.sendall(payload)
                strays.append(s)
            print('      PC: two stray connections to B:5002')
        return both_done(d)

    try:
        run_send(ctx, f'SITE TEST SEND {path}', done=poke)
    finally:
        for s in strays:
            s.close()
    check(strays, 'the transfer ended before the stray connections were made (make the payload bigger)')
    check_received(ctx, 'payload.bin', digest)


@test(2)
def reaccept_needs_same_file(ctx):
    """After a failure, a different file with the same name and size is asked about again
    (it must not go ahead by itself)."""
    path, _ = ctx.payload(7)
    ctx.a.fault('send.chunk', 4, 1)
    rules = send_rules(ctx, retry='No')

    # Until A has given up and B waits again on its Receive screen (which remembers the failure).
    def gave_up(d):
        if not d.saw('A', 'Confirmation', 'Retry?') or ctx.a.ui()['kind'] == 'prompt':
            return False
        ui = ctx.b.ui()
        return ui['name'] == 'Receive' and ui['kind'] == 'info'

    run_send(ctx, f'SITE TEST SEND {path}', rules=rules, done=gave_up)
    ctx.a.home()
    ctx.a.clear_faults()

    other, _ = ctx.payload(8, folder=TESTS_DIR + '/other')
    a, b = ctx.a, ctx.b
    rules = [Rule(a, 'Send to...', 'PEER', kind='list', once=True), Rule(b, 'Receive', 'No'),
             Rule(a, 'Failure', 'OK'), Rule(a, 'Error', 'OK'), Rule(b, 'Error', 'OK')]
    d = Driver([a, b], rules, lambda d: d.saw('A', 'Failure') or d.saw('A', 'Success'), timeout=120)
    a.cmd(f'SITE TEST SEND {other}')
    d.run()
    check(d.saw('B', 'Receive'), 'B took a different file without asking')


@test(2)
def receiver_cancels(ctx):
    """B cancels while receiving: A says the other console cancelled and does not offer a retry."""
    path, _ = ctx.payload(9, size=24 * 1024 * 1024)
    cancelled = []

    def cancel(d):
        if not cancelled and any(e[0] == 'B' and e[1] == 'Receiving' for e in d.events):
            time.sleep(1)
            ctx.b.press('B')
            cancelled.append(True)
        return d.saw('A', 'Failure')

    d = run_send(ctx, f'SITE TEST SEND {path}', done=cancel, timeout=180)
    check(not d.saw('A', 'Confirmation', 'Retry?'), 'A offered to retry a cancelled transfer')


def title_done(d):
    return d.saw('A', 'Success') and d.saw('B', 'Success')


def run_send_title(ctx, rules=None):
    ctx.forget_title_files(ctx.b, RECEIVED_DIR)
    return run_send(ctx, f'SITE TEST SENDTITLE {ctx.pick_title():016X}', rules=rules, done=title_done, timeout=900)


def check_received_title(ctx):
    path = ctx.exported(ctx.b, RECEIVED_DIR)
    check(path is not None, f'B has no CIA with {ctx.tag()} in {RECEIVED_DIR}')
    verify_cia(ctx.b.get(path), ctx.pick_title())
    # The padding between sections is zeroed, so the CIA is the same file on both sides.
    if ctx.reference is not None:
        check(ctx.b.sha256(path) == ctx.reference, 'the received CIA is not byte for byte the one exported on A')


@test(2)
def send_title_clean(ctx):
    """The title from A to B as a package; B builds a valid CIA (saved, not installed)."""
    run_send_title(ctx)
    check_received_title(ctx)


@test(2)
def send_title_cut(ctx):
    """The title's transfer drops partway; Retry goes on and the CIA is valid."""
    mark = ctx.b.log_size()
    ctx.pick_title()
    big = ctx.title_size > 12 * 1024 * 1024
    ctx.a.fault('send.chunk', RESUME_SKIP if big else 6, 1)
    run_send_title(ctx)
    check_received_title(ctx)
    if big:
        check_resumed(ctx, ctx.b.log_since(mark))
    else:
        print('    (the title is too small to go on from a saved place: only the retry is checked)')


@test(2)
def send_title_read_fails(ctx):
    """A can not read the title partway; Retry finishes it and the CIA is valid."""
    ctx.a.fault('title.read', 4, 1)
    run_send_title(ctx)
    check_received_title(ctx)


# ---- folders ----

def tree(seed, sizes, dirs=()):
    """{relative path: bytes} for `sizes` ({path: size}), and the folders to make (the empty
    ones included)."""
    rnd = random.Random(seed)
    files = {path: rnd.randbytes(size) for path, size in sizes.items()}
    folders = set(dirs)
    for path in files:
        parts = path.split('/')[:-1]
        folders.update('/'.join(parts[:i + 1]) for i in range(len(parts)))
    return files, sorted(folders, key=lambda d: (d.count('/'), d))


def put_tree(console, base, files, folders):
    console.mkdir(TESTS_DIR)
    rmtree(console, base)
    console.mkdir(base)
    for d in folders:
        console.mkdir(f'{base}/{d}')
    for path, data in files.items():
        console.put(f'{base}/{path}', data)


def list_tree(console, path):
    """(files, folders) under `path`, as paths relative to it; None when it is not there."""
    found_files, found_dirs = set(), set()

    def walk(rel):
        rows = console.list_long(f'{path}/{rel}' if rel else path)
        if rows is None:
            return False
        for is_dir, name in rows:
            if name in ('.', '..'):
                continue
            item = f'{rel}/{name}' if rel else name
            if is_dir:
                found_dirs.add(item)
                walk(item)
            else:
                found_files.add(item)
        return True

    return (found_files, found_dirs) if walk('') else None


def rmtree(console, path):
    """Deletes a folder of the tests (only under /nsa/tests or /nsa/received)."""
    check(path.startswith((TESTS_DIR + '/', RECEIVED_DIR + '/', TMP_DIR + '/')) and '..' not in path, f'refusing to delete {path}')
    rows = console.list_long(path)
    if rows is None:
        return
    for is_dir, name in rows:
        if name in ('.', '..'):
            continue
        if is_dir:
            rmtree(console, f'{path}/{name}')
        else:
            console.delete(f'{path}/{name}')
    try:
        console.ftp.rmd(path)
    except ftplib.error_perm:
        pass


def forget_received(ctx, name):
    for suffix in ('', ' (2)', ' (3)'):
        rmtree(ctx.b, f'{RECEIVED_DIR}/{name}{suffix}')
    rmtree(ctx.b, f'{TMP_DIR}/{name}.part')
    ctx.b.delete(f'{TMP_DIR}/{name}.part.pos')


def check_folder(ctx, name, files, folders, where=None):
    where = where or f'{RECEIVED_DIR}/{name}'
    got = list_tree(ctx.b, where)
    check(got is not None, f'B has no {where}')
    check(got[0] == set(files), f'files differ: missing {sorted(set(files) - got[0])[:5]}, extra {sorted(got[0] - set(files))[:5]}')
    check(got[1] == set(folders), f'folders differ: missing {sorted(set(folders) - got[1])[:5]}, extra {sorted(got[1] - set(folders))[:5]}')
    for path, data in files.items():
        if data:
            check(ctx.b.sha256(f'{where}/{path}') == hashlib.sha256(data).hexdigest(), f'{path} on B differs from what A sent')
        else:
            check(ctx.b.size(f'{where}/{path}') == 0, f'{path} should be empty')
    left = [n for d in (RECEIVED_DIR, TMP_DIR) for n in ctx.b.listdir(d) if n.startswith(name + '.part')]
    check(not left, f'left behind on B: {left}')


def run_send_folder(ctx, name, files, folders, rules=None, done=both_done, timeout=300):
    base = f'{TESTS_DIR}/{name}'
    put_tree(ctx.a, base, files, folders)
    forget_received(ctx, name)
    return run_send(ctx, f'SITE TEST SENDDIR {base}', rules=rules, done=done, timeout=timeout)


SMALL_TREE = ({'one.bin': 1, 'empty.txt': 0, 'sub/exact.bin': 256 * 1024, 'sub/plus1.bin': 256 * 1024 + 1,
               'sub/deep/more/d.bin': 600000, 'with space/f g.txt': 33}, ('emptydir', 'sub/emptier'))
BIG_TREE = {'big/a.bin': 4 * 1024 * 1024, 'big/b.bin': 5 * 1024 * 1024, 'big/sub/c.bin': 6 * 1024 * 1024 + 7}


@test(2)
def send_folder_clean(ctx):
    """A folder with subfolders, an empty file, empty folders and files around the chunk size
    goes from A to B and arrives identical."""
    files, folders = tree(21, SMALL_TREE[0], SMALL_TREE[1])
    run_send_folder(ctx, 'tree1', files, folders)
    check_folder(ctx, 'tree1', files, folders)


@test(2)
def send_folder_name_taken(ctx):
    """The same folder twice: the second arrives as "<name> (2)" and the first is untouched."""
    files, folders = tree(22, SMALL_TREE[0], SMALL_TREE[1])
    run_send_folder(ctx, 'tree2', files, folders)
    run_send(ctx, f'SITE TEST SENDDIR {TESTS_DIR}/tree2')
    check_folder(ctx, 'tree2', files, folders)
    check_folder(ctx, 'tree2', files, folders, where=f'{RECEIVED_DIR}/tree2 (2)')


@test(2)
def send_folder_cut_sender(ctx):
    """A's connection drops partway through a folder; Retry on A and B goes on by itself."""
    files, folders = tree(23, BIG_TREE)
    mark = ctx.b.log_size()
    ctx.a.fault('send.chunk', RESUME_SKIP, 1)
    d = run_send_folder(ctx, 'tree3', files, folders)
    check(d.saw('A', 'Confirmation', 'Retry?'), 'A did not offer to retry')
    check(sum(1 for e in d.events if e[0] == 'B' and e[1] == 'Receive') <= 1, 'B asked again for the same folder')
    check_folder(ctx, 'tree3', files, folders)
    check_resumed(ctx, ctx.b.log_since(mark))


@test(2)
def send_folder_cut_receiver(ctx):
    """B drops a connection partway; Retry on A and B goes on from where it was."""
    files, folders = tree(24, BIG_TREE)
    mark = ctx.b.log_size()
    ctx.b.fault('recv.chunk', RESUME_SKIP, 1)
    run_send_folder(ctx, 'tree4', files, folders)
    check_folder(ctx, 'tree4', files, folders)
    check_resumed(ctx, ctx.b.log_since(mark))


@test(2)
def send_folder_write_fails(ctx):
    """B's SD write fails inside a file of the folder; Retry and the copy is identical."""
    files, folders = tree(25, BIG_TREE)
    ctx.b.fault('recv.write', RESUME_SKIP, 1)
    d = run_send_folder(ctx, 'tree5', files, folders)
    check(d.saw('B', 'Error'), 'B did not report the failure')
    check_folder(ctx, 'tree5', files, folders)


@test(2)
def send_folder_read_fails(ctx):
    """A can not read one of its files partway; it says so and Retry finishes it."""
    files, folders = tree(26, BIG_TREE)
    ctx.a.fault('send.read', RESUME_SKIP, 1)
    d = run_send_folder(ctx, 'tree6', files, folders)
    check(d.saw('A', 'Error'), 'A did not report the failure')
    check_folder(ctx, 'tree6', files, folders)


@test(2)
def send_folder_many_small(ctx):
    """250 small files in 20 folders (one chunk each): they all arrive."""
    rnd = random.Random(27)
    sizes = {f'd{i % 20}/f{i}.dat': rnd.randint(1, 3000) for i in range(250)}
    files, folders = tree(27, sizes)
    run_send_folder(ctx, 'tree7', files, folders, timeout=600)
    check_folder(ctx, 'tree7', files, folders)



@test(2)
def send_folder_to_other_folder(ctx):
    """B saves the folder in a folder other than nsa/received: it arrives there, nothing is left in
    nsa/tmp or nsa/received, and a "<name>.part" of the user's own in that folder is not touched."""
    dest = f'{RECEIVED_DIR}/dest9'
    rmtree(ctx.b, dest)
    ctx.b.mkdir(dest)
    ctx.b.mkdir(f'{dest}/tree9.part')
    ctx.b.put(f'{dest}/tree9.part/keep.txt', b'mine')
    files, folders = tree(29, SMALL_TREE[0], SMALL_TREE[1])
    b = ctx.b
    rules = [r for r in send_rules(ctx) if not (r.console is b and r.choice == 'SAVE_HERE')]
    rules += [Rule(b, RECEIVED_DIR + '/', 'ENTER', kind='list'), Rule(b, dest + '/', 'SAVE_TOP', kind='list')]
    run_send_folder(ctx, 'tree9', files, folders, rules=rules)
    check_folder(ctx, 'tree9', files, folders, where=f'{dest}/tree9')
    check(ctx.b.list_long(f'{RECEIVED_DIR}/tree9') is None, 'the folder is also in nsa/received')
    check(ctx.b.get(f'{dest}/tree9.part/keep.txt') == b'mine', 'the user\'s own tree9.part was touched')
    rmtree(ctx.b, dest)


@test(1)
def send_folder_without_files(ctx):
    """A folder with only empty folders is not sent: A says it has no files."""
    files, folders = tree(28, {}, ('onlydirs/inner',))
    put_tree(ctx.a, f'{TESTS_DIR}/tree8', files, folders)
    a = ctx.a
    d = Driver([a], [Rule(a, 'Failure', 'OK')], lambda d: d.saw('A', 'Failure') and a.ui()['depth'] <= 1, timeout=60)
    a.cmd(f'SITE TEST SENDDIR {TESTS_DIR}/tree8')
    d.run()
    check(d.saw('A', 'Failure', 'no files'), 'A did not say the folder has no files')


@test(2)
def send_folder_unsafe_paths(ctx):
    """A sender that lists "../escape.txt" (or a path with a colon or a leading slash): B refuses
    and writes nothing outside the folder."""
    import fake_peer
    for index, bad in enumerate(('../escape.txt', '/abs.txt', 'a/../../up.txt', 'c:evil.txt', 'ok/./dot.txt')):
        name = f'evil{index}'
        forget_received(ctx, name)
        ctx.b.delete(f'{RECEIVED_DIR}/escape.txt')
        payload, segments, head, crc = fake_peer.folder_package([(bad, b'x' * 1000)])
        errors = []

        def sender():
            try:
                fake_peer.send(ctx.b.host, payload, segments, name.encode(), crc & 0xFFFFFFFF, 2, head, 0)
            except Exception as e:
                errors.append(e)   # B closes the connection when it refuses: expected.

        b = ctx.b
        rules = [Rule(b, 'Receive', 'Yes'), Rule(b, 'Incoming file', 'Save'), Rule(b, None, 'SAVE_HERE', kind='list'),
                 Rule(b, 'Error', 'OK'), Rule(b, 'Success', 'OK'), Rule(b, 'Failure', 'OK')]
        b.cmd('SITE TEST RECEIVE')
        time.sleep(1.5)
        thread = threading.Thread(target=sender, daemon=True)
        thread.start()
        d = Driver([b], rules, lambda d: d.saw('B', 'Error') or d.saw('B', 'Success'), timeout=90).run()
        thread.join(timeout=5)
        check(d.saw('B', 'Error'), f'B accepted the list with "{bad}"')
        check(b.sha256(f'{RECEIVED_DIR}/escape.txt') is None and b.sha256('/nsa/escape.txt') is None, f'"{bad}" wrote outside its folder')
        b.home()
        forget_received(ctx, name)


# ---- running ----

def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--a', help='the console that exports and sends')
    parser.add_argument('--b', help='the console that receives')
    parser.add_argument('--only', nargs='*', default=[], help='tests whose names start with these')
    parser.add_argument('--title', help='the title ID to export and send (default: the smallest on A)')
    parser.add_argument('--list', action='store_true', help='list the tests')
    args = parser.parse_args()

    if args.list:
        for name, needs, fn in TESTS:
            print(f'{name:28} {needs} console(s)  {fn.__doc__.split(chr(10))[0]}')
        return 0
    if not args.a:
        parser.error('--a is needed')

    out = os.path.join('test-results', time.strftime('%Y%m%d-%H%M%S'))
    a = Console('A', args.a)
    b = Console('B', args.b) if args.b else None
    ctx = Context(a, b, args.title, out)

    results = []
    for name, needs, fn in TESTS:
        if args.only and not any(name.startswith(o) for o in args.only):
            continue
        if needs == 2 and b is None:
            results.append((name, 'skip', 'needs --b'))
            continue
        print(f'\n== {name}: {fn.__doc__.split(chr(10))[0]}')
        marks = {c.label: c.log_size() for c in ctx.consoles()}
        try:
            for c in ctx.consoles():
                c.clear_faults()
                c.home()
            fn(ctx)
            results.append((name, 'pass', ''))
            print('   PASS')
        except Exception as e:
            reason = str(e) if isinstance(e, TestFailed) else traceback.format_exc()
            results.append((name, 'FAIL', reason))
            print(f'   FAIL: {reason}')
            os.makedirs(out, exist_ok=True)
            print(f'      saving logs and screenshots in {out}/')
            for c in ctx.consoles():
                try:
                    with open(os.path.join(out, f'{name}-{c.label}-log.txt'), 'w') as f:
                        f.write(c.log_since(marks[c.label]))
                    c.screenshot(out, f'{name}-{c.label}')
                except Exception:
                    pass
        finally:
            for c in ctx.consoles():
                try:
                    left = c.faults()
                    if left != 'none':
                        print(f'      faults on {c.label} (point hits/fired): {left}')
                    c.clear_faults()
                    c.home()
                except Exception:
                    pass

    print('\n' + '\n'.join(f'{state:4}  {name}' + (f'  ({why.splitlines()[0]})' if why else '') for name, state, why in results))
    failed = sum(1 for r in results if r[1] == 'FAIL')
    if failed:
        print(f'\n{failed} failed; logs and screenshots in {out}/')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
