#!/usr/bin/env python3
"""Tests of "one transfer at a time" (source/core/xferturn.h) and of the cases that once froze NSA, against a console.

    tools/turn_tests.py --host 192.168.1.132 [--pin 123456] [--only busy,vanish]

NSA needs Settings -> Web server on; without --pin the PIN is read from /nsa/web-pin.txt (DEBUG builds write it).
Each test checks that the console keeps answering (the web page's /api/info within --slow seconds, every second
while something happens) and that the turn is free again afterwards.

  busy-ftp     an FTP upload holds the turn: the page's upload, live screens and install-url get 503 with what holds it;
               browsing (/api/list, /api/info) still answers; afterwards the turn is free
  busy-web     a web page upload holds the turn: FTP STOR gets 450; LIST and a small RETR still work
  vanish-up    an FTP upload whose client resets the connection halfway
  vanish-down  an FTP download whose client stops reading (a PC that went away): NSA gives up after its 30 s
  reset-down   an FTP download whose client resets the connection halfway
  web-vanish   a web page upload whose client resets halfway
It writes in /nsa/turntest/ and deletes it afterwards.
"""
import argparse, ftplib, http.client, io, json, os, socket, struct, sys, threading, time, urllib.parse

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument('--host', required=True)
ap.add_argument('--ftp-port', type=int, default=5021)
ap.add_argument('--web-port', type=int, default=8080)
ap.add_argument('--pin')
ap.add_argument('--only', help='comma separated tests')
ap.add_argument('--slow', type=float, default=2.0, help='seconds a control request may take before it counts as a freeze')
a = ap.parse_args()

TESTS = ['busy-ftp', 'busy-web', 'vanish-up', 'vanish-down', 'reset-down', 'web-vanish']
ONLY = [t.strip() for t in a.only.split(',')] if a.only else TESTS
DIR = '/nsa/turntest'
MB = 1024 * 1024
BLOCK = os.urandom(64 * 1024)
fails = 0


def say(*x):
    print(*x, flush=True)


def check(name, ok, detail=''):
    global fails
    say(('ok   ' if ok else 'FAIL ') + name + (f'  ({detail})' if detail else ''))
    fails += 0 if ok else 1


def ftp_open(timeout=60):
    f = ftplib.FTP(timeout=timeout)
    f.connect(a.host, a.ftp_port)
    f.login()
    return f


def quiet(f, cmd):
    try:
        return f.sendcmd(cmd)
    except ftplib.all_errors as e:
        return str(e)


def web(method, path, body=None, timeout=10):
    """(status, json or None, seconds)."""
    t = time.perf_counter()
    c = http.client.HTTPConnection(a.host, a.web_port, timeout=timeout)
    try:
        c.request(method, path, body=body, headers={'X-Pin': PIN})
        r = c.getresponse()
        data = r.read()
        try:
            j = json.loads(data)
        except ValueError:
            j = None
        return r.status, j, time.perf_counter() - t
    finally:
        c.close()


def info():
    # Longer than the web page's 10 s stall (WEB_STALL_MS), which a request may wait out.
    return web('GET', '/api/info', timeout=15)


def reset(sock):
    """Closes with an RST, as a client that crashed or lost the network."""
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
    except OSError:
        pass
    sock.close()


class Watch:
    """Asks /api/info every second in the background and keeps the slowest answer."""

    def __init__(self):
        self.slowest, self.failures, self.stop = 0.0, 0, False
        self.t = threading.Thread(target=self.run, daemon=True)
        self.t.start()

    def run(self):
        while not self.stop:
            try:
                _, _, secs = info()
                self.slowest = max(self.slowest, secs)
            except OSError:
                self.failures += 1
            time.sleep(1)

    def end(self):
        self.stop = True
        self.t.join()
        return self.slowest, self.failures


def free_after(name, within=40):
    """The turn is free again (info.transfer null) within `within` seconds."""
    t = time.time()
    while time.time() - t < within:
        try:
            s, j, _ = info()
            if s == 200 and j and j.get('transfer') is None:
                check(name + ': the turn is free again', True, f'{time.time() - t:.1f} s')
                return True
        except OSError:
            pass
        time.sleep(1)
    check(name + ': the turn is free again', False, f'still taken after {within} s')
    return False


def ftp_works(name):
    try:
        f = ftp_open(20)
        bio = io.BytesIO(b'x' * 1000)
        r = f.storbinary(f'STOR {DIR}/after.bin', bio)
        f.quit()
        check(name + ': FTP takes a new upload', r.startswith('226'), r)
    except ftplib.all_errors as e:
        check(name + ': FTP takes a new upload', False, str(e))


def upload_ftp(path, mb, started=None, stop_after=None):
    """An FTP upload of `mb` MB; `stop_after` MB: reset the data connection there instead."""
    f = ftp_open(120)
    conn = f.transfercmd(f'STOR {path}')
    if started:
        started.set()
    sent = 0
    try:
        while sent < mb * MB:
            conn.sendall(BLOCK)
            sent += len(BLOCK)
            if stop_after is not None and sent >= stop_after * MB:
                reset(conn)
                reset(f.sock)
                return None
        conn.close()
        return f.voidresp()
    except OSError as e:
        return str(e)


# ---- tests ----

def t_busy_ftp():
    started = threading.Event()
    result = {}
    th = threading.Thread(target=lambda: result.setdefault('r', upload_ftp(f'{DIR}/big.bin', 48, started)), daemon=True)
    th.start()
    started.wait(20)
    time.sleep(2)
    s, j, secs = info()
    check('busy-ftp: /api/info shows the transfer', s == 200 and j.get('transfer') and j['transfer'].get('kind') == 'FTP', json.dumps(j.get('transfer') if j else None))
    s, j, secs = web('GET', '/api/list?path=' + urllib.parse.quote('/nsa'))
    check('busy-ftp: browsing still answers', s == 200 and secs < a.slow, f'{s}, {secs:.2f} s')
    s, j, secs = web('GET', '/api/screens')
    check('busy-ftp: live screens refused with 503', s == 503 and j and j.get('busy'), f'{s}')
    s, j, secs = web('PUT', '/api/file?path=' + urllib.parse.quote(f'{DIR}/web.bin'), b'hello')
    check('busy-ftp: a web upload refused with 503', s == 503 and j and j.get('busy'), f'{s} {j.get("error") if j else ""}')
    s, j, secs = web('POST', '/api/install-url', b'http://127.0.0.1/x.cia\n')
    check('busy-ftp: install-url refused with 503', s == 503, f'{s}')
    th.join(300)
    check('busy-ftp: the upload ends well', str(result.get('r', '')).startswith('226'), str(result.get('r')))
    free_after('busy-ftp', 10)


def t_busy_web():
    result = {}

    def up():
        try:
            def body():
                for _ in range(48 * MB // len(BLOCK)):
                    yield BLOCK
            c = http.client.HTTPConnection(a.host, a.web_port, timeout=120)
            c.request('PUT', '/api/file?path=' + urllib.parse.quote(f'{DIR}/webbig.bin'), body=body(), headers={'X-Pin': PIN, 'Content-Length': str(48 * MB)})
            r = c.getresponse()
            r.read()
            result['r'] = r.status
        except OSError as e:
            result['r'] = str(e)

    th = threading.Thread(target=up, daemon=True)
    th.start()
    time.sleep(4)
    f = ftp_open(30)
    try:
        r = f.storbinary(f'{"STOR"} {DIR}/ftp.bin', io.BytesIO(b'x' * 1000))
    except ftplib.error_temp as e:
        r = str(e)
    except ftplib.all_errors as e:
        r = 'error ' + str(e)
    check('busy-web: FTP STOR refused with 450', r.startswith('450'), r)
    try:
        names = f.nlst('/nsa')
        check('busy-web: FTP LIST still works', len(names) > 0, f'{len(names)} names')
        bio = io.BytesIO()
        f.retrbinary('RETR /nsa/settings.json', bio.write)
        check('busy-web: a small FTP RETR still works', len(bio.getvalue()) > 0, f'{len(bio.getvalue())} bytes')
    except ftplib.all_errors as e:
        check('busy-web: FTP LIST and a small RETR still work', False, str(e))
    f.quit()
    th.join(300)
    check('busy-web: the web upload ends well', result.get('r') == 200, str(result.get('r')))
    free_after('busy-web', 10)


def t_vanish_up():
    w = Watch()
    r = upload_ftp(f'{DIR}/cut.bin', 64, stop_after=8)
    check('vanish-up: client reset halfway', r is None, str(r))
    free_after('vanish-up', 40)
    slow, failures = w.end()
    check('vanish-up: the console kept answering', failures == 0 and slow < a.slow, f'slowest {slow:.2f} s, {failures} failed')
    ftp_works('vanish-up')


def t_down(stop_reading):
    name = 'vanish-down' if stop_reading else 'reset-down'
    f = ftp_open(30)
    # Something to download: 32 MB.
    f.storbinary(f'STOR {DIR}/down.bin', io.BytesIO(BLOCK * (32 * MB // len(BLOCK))))
    w = Watch()
    conn = f.transfercmd(f'RETR {DIR}/down.bin')
    got = 0
    while got < 4 * MB:
        d = conn.recv(65536)
        if not d:
            break
        got += len(d)
    if stop_reading:
        # The PC went away without a word: NSA's send finds no room and gives up after 30 s.
        conn.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        time.sleep(40)
        reset(conn)
    else:
        reset(conn)
    reset(f.sock)
    free_after(name, 45)
    slow, failures = w.end()
    check(name + ': the console kept answering', failures == 0 and slow < a.slow, f'slowest {slow:.2f} s, {failures} failed')
    ftp_works(name)


def t_web_vanish():
    w = Watch()
    s = socket.create_connection((a.host, a.web_port), timeout=20)
    head = (f'PUT /api/file?path={urllib.parse.quote(DIR + "/webcut.bin")} HTTP/1.1\r\nHost: x\r\nX-Pin: {PIN}\r\n'
            f'Content-Length: {64 * MB}\r\n\r\n').encode()
    s.sendall(head)
    for _ in range(8 * MB // len(BLOCK)):
        s.sendall(BLOCK)
    reset(s)
    free_after('web-vanish', 40)
    slow, failures = w.end()
    # The page is served one request at a time: it waits for the stalled upload up to WEB_STALL_MS (10 s), a second
    # in recv() and up to 2 s closing (net_close_gently) before the next request: under 15 s (it was over 30).
    check('web-vanish: the page answered again within the stall (15 s)', failures == 0 and slow < 15, f'slowest {slow:.2f} s, {failures} failed')
    s2, j, _ = web('GET', '/api/list?path=' + urllib.parse.quote(DIR))
    leftovers = [x['name'] for x in (j or {}).get('files', []) if x['name'].startswith('webcut')]
    check('web-vanish: no half file left', not leftovers, ', '.join(leftovers))


def cleanup():
    try:
        f = ftp_open(30)
        for n in f.nlst(DIR):
            quiet(f, f'DELE {n if n.startswith("/") else DIR + "/" + n}')
        quiet(f, f'RMD {DIR}')
        f.quit()
    except ftplib.all_errors:
        pass


if a.pin:
    PIN = a.pin
else:
    f = ftp_open()
    bio = io.BytesIO()
    f.retrbinary('RETR /nsa/web-pin.txt', bio.write)
    f.quit()
    PIN = bio.getvalue().decode().strip()

s, j, _ = info()
if s != 200:
    sys.exit(f'/api/info answered {s}: is the web server on, and the PIN right?')
say(f'{j.get("console")} {j.get("version")}, tests {",".join(ONLY)}')
f = ftp_open()
quiet(f, f'MKD {DIR}')
f.quit()
try:
    for t in ONLY:
        {'busy-ftp': t_busy_ftp, 'busy-web': t_busy_web, 'vanish-up': t_vanish_up, 'vanish-down': lambda: t_down(True),
         'reset-down': lambda: t_down(False), 'web-vanish': t_web_vanish}[t]()
        time.sleep(2)
finally:
    cleanup()
say(f'{fails} failed' if fails else 'all passed')
sys.exit(1 if fails else 0)
