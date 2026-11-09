#!/usr/bin/env python3
"""Speed tests of the transfers: FTP, the web page and downloads by URL (what a QR install does), with
a trace of every second on both sides, to find out where a slow transfer spends its time.

    tools/speed_tests.py --host 192.168.1.133                      NSA, every test
    tools/speed_tests.py --host IP --sizes 16,256 --only ftp-up,sd  some of them
    tools/speed_tests.py --host IP --server ftpd --ftp-port 5000    the same FTP tests against ftpd

Each run writes test-results/speed-<date>-<label>/: summary.md (one row per test), report.html (a chart
per test), <test>-client.csv (what the PC saw, every 0.5 s), <test>-console.csv (what NSA saw, every
second) and speed-log.txt as fetched from the console.

On the console NSA keeps the traces in sdmc:/nsa/speed-log.txt (source/core/speedlog.h): DEBUG builds
always, the others while sdmc:/nsa/speed.flag exists. Each second of a transfer says where its time
went: waiting for the network (nothing to read, no room to send), inside the socket calls, inside the
card (or the install), how many frames the UI drew, whether the music played and, with --cpu (DEBUG),
how busy each core was. Reading them:
  - wait high, disk low: the network or the PC is the limit (or the SOC service, shared by every thread);
  - disk high: the card (or the install) is the limit; disk_max shows single slow writes;
  - both low but slow: the thread does not get the CPU (UI, music, other threads).

Tests (--only):
  ftp-up, ftp-down     one file of each size to /nsa/speedtest/ and back (checked byte for byte)
  ftp-null, ftp-zero   NSA only: the same through the network alone (STOR /dev/null, RETR /dev/zero/<MB>)
  ftp-small            --small-count files of --small-kb KB up and down: the cost per file
  sd                   NSA only, SITE SDBENCH: the card alone, writing and reading, with and without
                       the stdio buffer and with the size set first
  web-up, web-down     the web page's upload and download (Settings -> Web server on; the PIN with --pin
                       or, in DEBUG builds, from /nsa/web-pin.txt)
  url                  NSA only: this PC serves a file and the web page asks the console to download it
                       (the same code as a QR install); with remote use on (DEBUG or remote.flag) the
                       console's "Save" is pressed by the script, else press X (Save) on the console.
                       It lands in /nsa/received/ and is deleted afterwards. --cia FILE installs it instead (A).

SITE commands (sd, url's key presses, --cpu) need a DEBUG build or sdmc:/nsa/remote.flag.
Windows may ask to let Python accept connections for the url test (port --http-port).
"""
import argparse, ftplib, hashlib, html, http.client, http.server, io, json, os, random, re, socket, sys
import threading, time, urllib.parse

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument('--host', required=True)
ap.add_argument('--server', choices=('nsa', 'ftpd'), default='nsa', help='what serves FTP (ftpd: FTP tests only)')
ap.add_argument('--ftp-port', type=int)
ap.add_argument('--web-port', type=int, default=8080)
ap.add_argument('--http-port', type=int, default=8765, help='port of this PC for the url test')
ap.add_argument('--pin')
ap.add_argument('--sizes', default='1,16,64,256', help='MB, comma separated')
ap.add_argument('--only', help='comma separated tests (see above)')
ap.add_argument('--small-count', type=int, default=100)
ap.add_argument('--small-kb', type=int, default=256)
ap.add_argument('--url-mb', type=int, default=64)
ap.add_argument('--sd-modes', default='buffer,pre,unbuffered', help='sd test: which of buffer, pre (size set first), unbuffered')
ap.add_argument('--cia', help='url test: serve this CIA and install it (A) instead of saving a file')
ap.add_argument('--wait-mode', choices=('poll', 'nap', 'hybrid'), help='NSA DEBUG: how the transfer with the turn waits (SITE POLL)')
ap.add_argument('--cpu', action='store_true', help='SITE CPU while testing: each core\'s load in the traces (DEBUG)')
ap.add_argument('--label', default='', help='a word for the folder name (console, settings...)')
ap.add_argument('--out', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'test-results'))
ap.add_argument('--pause', type=float, default=3, help='seconds between tests')
ap.add_argument('--keep', action='store_true', help='leave /nsa/speedtest on the console')
a = ap.parse_args()

NSA = a.server == 'nsa'
a.ftp_port = a.ftp_port or (5021 if NSA else 5000)
SIZES = [int(x) for x in a.sizes.split(',') if x.strip()]
ALL = ['ftp-up', 'ftp-down', 'ftp-null', 'ftp-zero', 'ftp-small', 'sd', 'web-up', 'web-down', 'url']
# ftpd has /devZero for the network-only tests (its RETR never ends: the script stops it).
NSA_ONLY = {'sd', 'web-up', 'web-down', 'url'}
ONLY = [t.strip() for t in a.only.split(',')] if a.only else [t for t in ALL if NSA or t not in NSA_ONLY]
for t in ONLY:
    if t not in ALL:
        sys.exit(f'unknown test {t}')
    if not NSA and t in NSA_ONLY:
        sys.exit(f'{t} needs NSA')
if 'ftp-down' in ONLY and 'ftp-up' not in ONLY:
    sys.exit('ftp-down downloads what ftp-up uploaded: run both')

DIR = '/nsa/speedtest'
RUN = time.strftime('%Y%m%d-%H%M%S')
OUT = os.path.abspath(os.path.join(a.out, f'speed-{RUN}-{a.server}' + (f'-{a.label}' if a.label else '')))
os.makedirs(OUT, exist_ok=True)
MB = 1024 * 1024
PIECE = 64 * 1024

def say(*args):
    print(*args, flush=True)

# ---- data: the same pseudo-random bytes for a size every time, so a download can be checked ----

def blocks(total, seed):
    base = random.Random(seed).randbytes(MB)
    i = 0
    while total > 0:
        b = (i.to_bytes(8, 'little') + base[8:])[:min(MB, total)]
        yield b
        total -= len(b)
        i += 1

def digest(total, seed):
    h = hashlib.sha256()
    for b in blocks(total, seed):
        h.update(b)
    return h.hexdigest()

# ---- what the PC sees: bytes over time ----

class Meter:
    def __init__(self):
        self.start = time.perf_counter()
        self.points = [(0.0, 0)]
        self.bytes = 0
        self.last = 0.0

    def add(self, n):
        self.bytes += n
        t = time.perf_counter() - self.start
        if t - self.last >= 0.5:
            self.points.append((t, self.bytes))
            self.last = t

    def finish(self):
        if hasattr(self, 'secs'):
            return self
        self.secs = time.perf_counter() - self.start
        self.points.append((self.secs, self.bytes))
        return self

    def kbs(self):
        return self.bytes / 1024 / self.secs if self.secs > 0 else 0

    def per_second(self):
        """KB/s over each whole second, from the points."""
        return [(self.at(s) - self.at(s - 1)) / 1024 for s in range(1, int(self.secs) + 1)]

    def at(self, t):
        pts = self.points
        for k in range(1, len(pts)):
            if pts[k][0] >= t:
                (t0, b0), (t1, b1) = pts[k - 1], pts[k]
                return b0 + (b1 - b0) * ((t - t0) / (t1 - t0) if t1 > t0 else 1)
        return pts[-1][1]

    def edges(self):
        ps = self.per_second()
        if not ps:
            return self.kbs(), self.kbs(), self.kbs()
        head, tail = ps[:10], ps[-10:]
        return sum(head) / len(head), sum(tail) / len(tail), min(ps)

    def csv(self, path):
        with open(path, 'w') as f:
            f.write('t,bytes,KB/s_last_0.5s\n')
            prev = (0.0, 0)
            for t, b in self.points[1:]:
                dt = t - prev[0]
                f.write(f'{t:.2f},{b},{(b - prev[1]) / 1024 / dt if dt > 0 else 0:.0f}\n')
                prev = (t, b)

# ---- FTP ----

def ftp_open(timeout=120):
    f = ftplib.FTP(timeout=timeout)
    f.connect(a.host, a.ftp_port)
    f.login()
    f.set_pasv(True)
    return f

def ftp_quiet(f, cmd):
    try:
        return f.sendcmd(cmd)
    except ftplib.all_errors as e:
        return str(e)

def ftp_upload(f, path, total, seed, meter):
    # NSA sets the file's size first when it is told (and shows a CIA's progress).
    if NSA:
        ftp_quiet(f, f'ALLO {total}')
    conn = f.transfercmd(f'STOR {path}')
    try:
        for b in blocks(total, seed):
            for k in range(0, len(b), PIECE):
                conn.sendall(b[k:k + PIECE])
                meter.add(min(PIECE, len(b) - k))
    finally:
        conn.close()
    return f.voidresp()

def ftp_download(f, path, meter, check=None, limit=None):
    """`limit`: stop after that many bytes (ftpd's endless /devZero); any reply is taken then."""
    h = hashlib.sha256() if check else None
    conn = f.transfercmd(f'RETR {path}')
    try:
        while limit is None or meter.bytes < limit:
            d = conn.recv(PIECE if limit is None else min(PIECE, limit - meter.bytes))
            if not d:
                break
            meter.add(len(d))
            if h:
                h.update(d)
    finally:
        conn.close()
    if limit is not None:
        meter.finish()
        try:
            f.getresp()
        except ftplib.all_errors:
            pass
        return '226 stopped', None
    reply = f.voidresp()
    return reply, (h.hexdigest() == check) if check else None

def fetch_text(path):
    try:
        f = ftp_open()
        buf = io.BytesIO()
        f.retrbinary(f'RETR {path}', buf.write)
        f.quit()
        return buf.getvalue().decode('utf-8', 'replace')
    except ftplib.all_errors:
        return None

def site(cmd, timeout=120):
    f = ftp_open(timeout)
    try:
        return f.sendcmd('SITE ' + cmd)
    except ftplib.all_errors as e:
        return str(e)
    finally:
        try:
            f.quit()
        except ftplib.all_errors:
            pass

# ---- the tests: each one gives rows for the summary ----

rows = []          # dicts: test, size, client, trace (kind, name) to match, ok, note

def row(test, size_mb, meter=None, ok=True, note='', trace=None, console_kbs=None):
    r = {'test': test, 'size': size_mb, 'ok': ok, 'note': note, 'trace': trace, 'console_kbs': console_kbs}
    if meter:
        r['client'] = meter.kbs()
        r['first'], r['last'], r['slowest'] = meter.edges()
        r['secs'] = meter.secs
        name = f'{test}-{size_mb}MB'
        meter.csv(os.path.join(OUT, name + '-client.csv'))
        r['series'] = meter.per_second()
    rows.append(r)
    kbs = f"{r.get('client', 0):.0f} KB/s" if meter else (f'{console_kbs} KB/s' if console_kbs else '')
    say(f"{'ok  ' if ok else 'FAIL'} {test} {size_mb} MB  {kbs}  {note}")

def test_ftp_up_down():
    f = ftp_open(300)
    ftp_quiet(f, f'MKD {DIR}')
    for mb in SIZES:
        path = f'{DIR}/up-{mb}.bin'
        if 'ftp-up' in ONLY:
            m = Meter()
            try:
                reply = ftp_upload(f, path, mb * MB, mb, m)
                ok = reply.startswith('226')
            except ftplib.all_errors as e:
                ok, reply = False, str(e)
            row('ftp-up', mb, m.finish(), ok, '' if ok else reply, ('ftp-up', path))
            time.sleep(a.pause)
        if 'ftp-down' in ONLY:
            m = Meter()
            try:
                reply, same = ftp_download(f, path, m, digest(mb * MB, mb))
                ok = reply.startswith('226') and same
                note = '' if ok else ('different bytes' if not same else reply)
            except ftplib.all_errors as e:
                ok, note = False, str(e)
            row('ftp-down', mb, m.finish(), ok, note, ('ftp-down', path))
            time.sleep(a.pause)
        if not a.keep:
            ftp_quiet(f, f'DELE {path}')
    f.quit()

def test_ftp_network():
    f = ftp_open(300)
    for mb in SIZES:
        if 'ftp-null' in ONLY:
            m = Meter()
            try:
                ok = ftp_upload(f, '/dev/null' if NSA else '/devZero', mb * MB, mb, m).startswith('226')
                note = ''
            except ftplib.all_errors as e:
                ok, note = False, str(e)
            row('ftp-null', mb, m.finish(), ok, note, ('ftp-null', '/dev/null'))
            time.sleep(a.pause)
        if 'ftp-zero' in ONLY:
            m = Meter()
            try:
                reply, _ = ftp_download(f, f'/dev/zero/{mb}', m) if NSA else ftp_download(f, '/devZero', m, limit=mb * MB)
                ok = reply.startswith('226') and m.bytes == mb * MB
                note = '' if ok else f'{m.bytes} bytes, {reply}'
            except ftplib.all_errors as e:
                ok, note = False, str(e)
            row('ftp-zero', mb, m.finish(), ok, note, ('ftp-zero', f'/dev/zero/{mb}'))
            time.sleep(a.pause)
            if not NSA:
                # ftpd drops the session after the stopped RETR: a new one for the next test.
                try:
                    f.quit()
                except ftplib.all_errors:
                    pass
                f = ftp_open(300)
    try:
        f.quit()
    except ftplib.all_errors:
        pass

def test_ftp_small():
    f = ftp_open(120)
    ftp_quiet(f, f'MKD {DIR}')
    ftp_quiet(f, f'MKD {DIR}/small')
    n, size = a.small_count, a.small_kb * 1024
    per = []
    m = Meter()
    ok = True
    for i in range(n):
        t = time.perf_counter()
        try:
            ok = ftp_upload(f, f'{DIR}/small/{i:04d}.bin', size, 1000 + i, m).startswith('226') and ok
        except ftplib.all_errors:
            ok = False
        per.append(time.perf_counter() - t)
    m.finish()
    with open(os.path.join(OUT, 'ftp-small-up-files.csv'), 'w') as out:
        out.write('file,secs\n' + ''.join(f'{i},{s:.3f}\n' for i, s in enumerate(per)))
    row('ftp-small-up', round(n * size / MB, 1), m, ok, f'{n} files, {sum(per) / n * 1000:.0f} ms a file')
    time.sleep(a.pause)

    per = []
    m = Meter()
    ok = True
    for i in range(n):
        t = time.perf_counter()
        try:
            reply, same = ftp_download(f, f'{DIR}/small/{i:04d}.bin', m, digest(size, 1000 + i))
            ok = reply.startswith('226') and same and ok
        except ftplib.all_errors:
            ok = False
        per.append(time.perf_counter() - t)
    m.finish()
    with open(os.path.join(OUT, 'ftp-small-down-files.csv'), 'w') as out:
        out.write('file,secs\n' + ''.join(f'{i},{s:.3f}\n' for i, s in enumerate(per)))
    row('ftp-small-down', round(n * size / MB, 1), m, ok, f'{n} files, {sum(per) / n * 1000:.0f} ms a file')
    if not a.keep:
        for i in range(n):
            ftp_quiet(f, f'DELE {DIR}/small/{i:04d}.bin')
        ftp_quiet(f, f'RMD {DIR}/small')
    f.quit()
    time.sleep(a.pause)

def test_sd():
    mb = max(SIZES)
    modes = {'buffer': (256, False), 'pre': (256, True), 'unbuffered': (0, False)}
    for buf, pre in (modes[m.strip()] for m in a.sd_modes.split(',')):
        reply = site(f'SDBENCH {mb} {buf}' + (' PRE' if pre else ''), timeout=1200)
        ok = reply.startswith('200')
        w = re.search(r'write (\d+) KB/s', reply)
        r = re.search(r'read (\d+) KB/s', reply)
        note = f"buffer {buf} KB{', preallocated' if pre else ''}: read {r.group(1) if r else '?'} KB/s; {reply[4:]}"
        row('sd-write', mb, None, ok, note, ('sd-write', 'preallocated' if pre else 'growing'), int(w.group(1)) if w else None)
        time.sleep(a.pause)

# ---- the web page ----

def web_pin():
    if a.pin:
        return a.pin
    text = fetch_text('/nsa/web-pin.txt')
    if not text:
        sys.exit('web tests: no --pin and no /nsa/web-pin.txt (DEBUG builds write it)')
    return text.strip()

def test_web():
    pin = web_pin()
    path = f'{DIR}/web.bin'
    ftp = ftp_open()
    ftp_quiet(ftp, f'MKD {DIR}')
    ftp.quit()
    for mb in SIZES:
        q = urllib.parse.quote(path)
        if 'web-up' in ONLY:
            m = Meter()
            def body():
                for b in blocks(mb * MB, mb):
                    for k in range(0, len(b), PIECE):
                        piece = b[k:k + PIECE]
                        m.add(len(piece))
                        yield piece
            try:
                c = http.client.HTTPConnection(a.host, a.web_port, timeout=120)
                c.request('PUT', f'/api/file?path={q}', body=body(), headers={'X-Pin': pin, 'Content-Length': str(mb * MB)})
                resp = c.getresponse()
                resp.read()
                ok, note = resp.status == 200, '' if resp.status == 200 else f'HTTP {resp.status}'
                c.close()
            except OSError as e:
                ok, note = False, str(e)
            row('web-up', mb, m.finish(), ok, note, ('web-up', path))
            time.sleep(a.pause)
        if 'web-down' in ONLY:
            m = Meter()
            h = hashlib.sha256()
            try:
                c = http.client.HTTPConnection(a.host, a.web_port, timeout=120)
                c.request('GET', f'/api/file?path={q}', headers={'X-Pin': pin})
                resp = c.getresponse()
                while True:
                    d = resp.read(PIECE)
                    if not d:
                        break
                    m.add(len(d))
                    h.update(d)
                c.close()
                same = h.hexdigest() == digest(mb * MB, mb)
                ok = resp.status == 200 and same
                note = '' if ok else (f'HTTP {resp.status}' if resp.status != 200 else 'different bytes (was web-up run?)')
            except OSError as e:
                ok, note = False, str(e)
            row('web-down', mb, m.finish(), ok, note, ('web-down', path))
            time.sleep(a.pause)
    if not a.keep:
        ftp = ftp_open()
        ftp_quiet(ftp, f'DELE {path}')
        ftp.quit()

# ---- a download by URL: this PC serves, the console downloads ----

class Served:
    meter = None
    lock = threading.Lock()
    data = None       # bytes of the CIA, or None for the generated file
    size = 0
    connections = 0

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def do_GET(self):
        start, end = 0, Served.size - 1
        m = re.match(r'bytes=(\d+)-(\d*)', self.headers.get('Range', ''))
        if m:
            start = int(m.group(1))
            end = min(int(m.group(2)) if m.group(2) else end, Served.size - 1)
            self.send_response(206)
            self.send_header('Content-Range', f'bytes {start}-{end}/{Served.size}')
        else:
            self.send_response(200)
        self.send_header('Accept-Ranges', 'bytes')
        self.send_header('Content-Length', str(end - start + 1))
        self.send_header('Content-Type', 'application/octet-stream')
        self.end_headers()
        with Served.lock:
            Served.connections += 1
        pos = start
        try:
            while pos <= end:
                n = min(PIECE, end - pos + 1)
                if Served.data is not None:
                    piece = Served.data[pos:pos + n]
                else:
                    block = pos // MB
                    b = Served.blocks[block]
                    piece = b[pos - block * MB:pos - block * MB + n]
                    n = len(piece)
                self.wfile.write(piece)
                with Served.lock:
                    Served.meter.add(n)
                pos += n
        except OSError:
            pass

def my_address():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.connect((a.host, 9))
    ip = s.getsockname()[0]
    s.close()
    return ip

def test_url():
    pin = web_pin()
    if a.cia:
        Served.data = open(a.cia, 'rb').read()
        Served.size = len(Served.data)
        name = os.path.basename(a.cia)
    else:
        Served.size = a.url_mb * MB
        Served.blocks = list(blocks(Served.size, 7))
        name = f'speedtest-{RUN}-{a.url_mb}MB.bin'
    Served.meter = Meter()
    server = http.server.ThreadingHTTPServer(('0.0.0.0', a.http_port), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = f'http://{my_address()}:{a.http_port}/{urllib.parse.quote(name)}'
    say(f'url: the console downloads {url}')

    c = http.client.HTTPConnection(a.host, a.web_port, timeout=30)
    c.request('POST', '/api/install-url', body=(url + '\n').encode(), headers={'X-Pin': pin})
    resp = c.getresponse()
    resp.read()
    if resp.status != 200:
        server.shutdown()
        row('url', round(Served.size / MB), None, False, f'install-url: HTTP {resp.status}')
        return

    key = 'A' if a.cia else 'X'
    time.sleep(3)
    pressed = site(f'PRESS {key}')
    manual = not pressed.startswith('200')
    if manual:
        say(f'url: press {key} ({"Install" if a.cia else "Save"}) on the console now')
    Served.meter = Meter()   # The download starts after the answer.

    # Done when the console's trace of it ends (or nothing has come for 60 s; 3 minutes to press the key by hand).
    trace = None
    last, last_change = 0, time.time() + (120 if manual else 0)
    while trace is None and time.time() - last_change < 60:
        time.sleep(5)
        log = fetch_text('/nsa/speed-log.txt') or ''
        for t in parse_traces(log):
            if t['kind'] == 'url' and t['name'] == url[:127] and t.get('end'):
                trace = t
        if Served.meter.bytes != last:
            last, last_change = Served.meter.bytes, time.time()
    Served.meter.finish()
    server.shutdown()
    site('PRESS A')   # The "Success" message.
    ok = trace is not None and trace['end']['ok'] == 'ok'
    row('url', round(Served.size / MB), Served.meter, ok, f'{Served.connections} requests' + ('' if ok else ', no finished trace'),
        ('url', url[:127]))
    if not a.cia and not a.keep:
        f = ftp_open()
        ftp_quiet(f, f'DELE /nsa/received/{name}')
        f.quit()

# ---- the console's traces ----

S_COLS = ['t', 'ms', 'kbs', 'wait_ms', 'waits', 'net_ms', 'net_calls', 'bytes_per_call', 'disk_ms', 'disk_max_ms', 'disk_calls', 'frames', 'gauge']
E_COLS = ['kind', 'name', 'ok', 'bytes', 'secs', 'kbs', 'first10', 'last10', 'slowest', 'fastest', 'wait_ms', 'net_ms', 'disk_ms', 'disk_max_ms', 'disk_bytes', 'cut']

def parse_traces(text):
    traces, cur = [], None
    for line in text.splitlines():
        if line.startswith('# ') and not line.startswith('# s,') and not line.startswith('# e,'):
            p = line[2:].split(' ')
            cur = {'head': line[2:], 'version': p[2] if len(p) > 2 else '', 'model': p[3] if len(p) > 3 else '',
                   'kind': p[4] if len(p) > 4 else '', 'name': ' '.join(p[5:-1]) if len(p) > 6 else '', 'samples': []}
            traces.append(cur)
        elif line.startswith('s,') and cur is not None:
            p = line.split(',', len(S_COLS) + 1)
            s = {k: float(v) for k, v in zip(S_COLS, p[1:1 + len(S_COLS)])}
            s['context'] = p[-1] if len(p) > len(S_COLS) + 1 else ''
            cur['samples'].append(s)
        elif line.startswith('e,') and cur is not None:
            p = line[2:].split(',')
            # The name may hold commas: the fixed columns are counted from both ends.
            tail = len(E_COLS) - 2
            e = dict(zip(E_COLS[:1], p[:1]))
            e['name'] = ','.join(p[1:len(p) - tail])
            e.update(zip(E_COLS[2:], p[len(p) - tail:]))
            cur['end'] = e
    return traces

def console_summary(t):
    e = t['end']
    secs = float(e['secs']) or 1
    ss = t['samples']
    frames = sum(s['frames'] for s in ss) / max(sum(s['ms'] for s in ss) / 1000, 0.001) if ss else 0
    music = sum(1 for s in ss if 'music 1' in s['context'])
    cpu = ''
    loads = [re.findall(r'c(\d) (\d+)', s['context']) for s in ss]
    loads = [l for l in loads if l]
    if loads:
        cores = sorted({c for l in loads for c, _ in l})
        cpu = ' '.join(f'c{c} {sum(int(v) for l in loads for cc, v in l if cc == c) / len(loads):.0f}%' for c in cores)
    return {
        'kbs': int(e['kbs']), 'first': int(e['first10']), 'last': int(e['last10']), 'slowest': int(e['slowest']),
        'wait': float(e['wait_ms']) / 10 / secs, 'net': float(e['net_ms']) / 10 / secs, 'disk': float(e['disk_ms']) / 10 / secs,
        'disk_max': int(e['disk_max_ms']), 'frames': frames, 'music': f'{music}/{len(ss)} s' if ss else '', 'cpu': cpu,
        'bpc': sum(s['bytes_per_call'] * s['net_calls'] for s in ss) / max(sum(s['net_calls'] for s in ss), 1),
    }

def write_console_csv(t, path):
    with open(path, 'w') as f:
        f.write(','.join(S_COLS) + ',context\n')
        for s in t['samples']:
            f.write(','.join(f'{s[k]:g}' for k in S_COLS) + f",{s['context']}\n")

# ---- report ----

def svg_chart(r):
    """KB/s every second: the PC's (line) and the console's, with where the console's time went (bars)."""
    W, H, P = 760, 220, 34
    client = r.get('series') or []
    t = r.get('console_trace')
    cs = t['samples'] if t else []
    n = max(len(client), len(cs), 1)
    top = max([*client, *[s['kbs'] for s in cs], 100])
    x = lambda i: P + (W - P - 8) * i / max(n, 1)
    y = lambda v: H - 20 - (H - 40) * min(v / top, 1)
    out = [f'<svg viewBox="0 0 {W} {H}" class="chart">']
    for frac in (0, .25, .5, .75, 1):
        v = top * frac
        out.append(f'<line x1="{P}" x2="{W - 8}" y1="{y(v):.1f}" y2="{y(v):.1f}" class="grid"/><text x="{P - 4}" y="{y(v) + 4:.1f}" class="ax" text-anchor="end">{v:.0f}</text>')
    bw = (W - P - 8) / max(n, 1)
    for i, s in enumerate(cs):
        ms = s['ms'] or 1000
        hh = H - 40
        acc = 0
        for key, cls in (('disk_ms', 'disk'), ('wait_ms', 'wait'), ('net_ms', 'net')):
            frac = min(s[key] / ms, 1 - acc)
            if frac > 0:
                out.append(f'<rect x="{x(i):.1f}" width="{max(bw - 1, 1):.1f}" y="{H - 20 - hh * (acc + frac):.1f}" height="{hh * frac:.1f}" class="{cls}"/>')
            acc += frac
    for series, cls in ((client, 'client'), ([s['kbs'] for s in cs], 'console')):
        if series:
            pts = ' '.join(f'{x(i + .5):.1f},{y(v):.1f}' for i, v in enumerate(series))
            out.append(f'<polyline points="{pts}" class="{cls}"/>')
    out.append(f'<text x="{P}" y="{H - 4}" class="ax">0 s</text><text x="{W - 8}" y="{H - 4}" class="ax" text-anchor="end">{n} s</text></svg>')
    return ''.join(out)

def write_report(traces):
    used = set()
    for r in rows:
        if not r.get('trace'):
            continue
        kind, name = r['trace']
        for i, t in enumerate(traces):
            if i not in used and t['kind'] == kind and t['name'] == name and t.get('end'):
                used.add(i)
                r['console_trace'] = t
                r['console'] = console_summary(t)
                write_console_csv(t, os.path.join(OUT, f"{r['test']}-{r['size']}MB-console.csv"))
                break

    model = next((t['model'] for t in traces if t.get('model')), '')
    version = next((t['version'] for t in traces if t.get('version')), '')
    head = f"Speed tests {RUN}: {a.server} at {a.host}" + (f', NSA {version} on a {model}' if version else '') + (f' ({a.label})' if a.label else '')
    lines = [f'# {head}', '',
             'KB/s. *PC*: what this computer measured (whole transfer, first 10 s, last 10 s, slowest second). *Console*: NSA\'s '
             'trace (`speed-log.txt`): its KB/s, and the share of the time spent waiting for the network, inside the socket calls and '
             'inside the card (or the install); the slowest single card call; frames the UI drew a second; seconds with music.', '',
             '| test | MB | ok | PC | first 10 s | last 10 s | slowest s | console | wait | net | card | card max ms | B/call | frames/s | music | CPU | note |',
             '|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|']
    for r in rows:
        c = r.get('console')
        pc = lambda k: f"{r[k]:.0f}" if k in r else ''
        lines.append('| ' + ' | '.join([
            r['test'], str(r['size']), 'ok' if r['ok'] else '**FAIL**', pc('client'), pc('first'), pc('last'), pc('slowest'),
            str(c['kbs']) if c else (str(r['console_kbs']) if r.get('console_kbs') else ''),
            f"{c['wait']:.0f}%" if c else '', f"{c['net']:.0f}%" if c else '', f"{c['disk']:.0f}%" if c else '',
            str(c['disk_max']) if c else '', f"{c['bpc'] / 1024:.1f} KB" if c and c['bpc'] else '', f"{c['frames']:.1f}" if c else '',
            c['music'] if c else '', c['cpu'] if c else '', r['note'].replace('|', '/')]) + ' |')
    with open(os.path.join(OUT, 'summary.md'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')

    css = ('body{font:14px system-ui,sans-serif;margin:16px;max-width:820px;background:var(--bg);color:var(--fg)}'
           ':root{--bg:#fff;--fg:#222;--grid:#ddd;--client:#2266cc;--console:#d0602a;--disk:#e8b0a0;--wait:#b8cce8;--net:#c8e0b8}'
           '@media (prefers-color-scheme:dark){:root{--bg:#1b1b1b;--fg:#ddd;--grid:#333;--client:#6aa0ff;--console:#ff9a60;--disk:#6b3a30;--wait:#2c3d58;--net:#33482a}}'
           '.chart{width:100%;height:auto}.grid{stroke:var(--grid)}.ax{fill:var(--fg);font-size:11px}'
           'polyline{fill:none;stroke-width:2}.client{stroke:var(--client)}.console{stroke:var(--console)}'
           '.disk{fill:var(--disk)}.wait{fill:var(--wait)}.net{fill:var(--net)}'
           '.key span{display:inline-block;width:12px;height:12px;margin:0 4px 0 12px;vertical-align:-2px}'
           'table{border-collapse:collapse;font-size:12px}td,th{border:1px solid var(--grid);padding:2px 5px}')
    body = [f'<!doctype html><meta charset="utf-8"><title>Speed tests</title><style>{css}</style><h1>{html.escape(head)}</h1>',
            '<p class="key">KB/s each second:<span style="background:var(--client)"></span>PC<span style="background:var(--console)"></span>console. '
            'Share of each second on the console:<span style="background:var(--disk)"></span>card<span style="background:var(--wait)"></span>'
            'waiting for the network<span style="background:var(--net)"></span>socket calls</p>']
    for r in rows:
        if r.get('series') or r.get('console_trace'):
            c = r.get('console')
            sub = f"PC {r.get('client', 0):.0f} KB/s (first 10 s {r.get('first', 0):.0f}, last 10 s {r.get('last', 0):.0f})"
            if c:
                sub += f"; console {c['kbs']} KB/s, card {c['disk']:.0f}%, waiting {c['wait']:.0f}%, slowest card call {c['disk_max']} ms"
            body.append(f"<h2>{html.escape(r['test'])} {r['size']} MB {'' if r['ok'] else '(FAILED)'}</h2><p>{html.escape(sub)}</p>{svg_chart(r)}")
    with open(os.path.join(OUT, 'report.html'), 'w', encoding='utf-8') as f:
        f.write(''.join(body))

# ---- run ----

def main():
    say(f'{a.server} at {a.host}:{a.ftp_port}, sizes {SIZES} MB, tests {",".join(ONLY)}; results in {OUT}')
    if NSA:
        # Only this run's traces in the log.
        f = ftp_open()
        ftp_quiet(f, 'DELE /nsa/speed-log.txt')
        f.quit()
        if a.cpu:
            say('cpu: ' + site('CPU 500'))
        if a.wait_mode:
            say('wait: ' + site('POLL ' + a.wait_mode.upper()))
    try:
        if 'ftp-up' in ONLY or 'ftp-down' in ONLY:
            test_ftp_up_down()
        if 'ftp-null' in ONLY or 'ftp-zero' in ONLY:
            test_ftp_network()
        if 'ftp-small' in ONLY:
            test_ftp_small()
        if 'sd' in ONLY:
            test_sd()
        if 'web-up' in ONLY or 'web-down' in ONLY:
            test_web()
        if 'url' in ONLY:
            test_url()
    except KeyboardInterrupt:
        say('stopped')
    finally:
        traces = []
        if NSA:
            if a.cpu:
                site('CPU OFF')
            text = fetch_text('/nsa/speed-log.txt')
            if text is None:
                say('no /nsa/speed-log.txt on the console: a release build needs sdmc:/nsa/speed.flag')
            else:
                with open(os.path.join(OUT, 'speed-log.txt'), 'w', encoding='utf-8') as f:
                    f.write(text)
                traces = parse_traces(text)
        if not a.keep:
            try:
                f = ftp_open()
                ftp_quiet(f, f'RMD {DIR}')
                f.quit()
            except ftplib.all_errors:
                pass
        write_report(traces)
        say(f'summary: {os.path.join(OUT, "summary.md")}')
        say(f'charts:  {os.path.join(OUT, "report.html")}')
    return 0 if all(r['ok'] for r in rows) else 1

sys.exit(main())
