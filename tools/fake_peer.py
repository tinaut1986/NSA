#!/usr/bin/env python3
"""A stand-in for the other console, to test NSA's console to console transfer (protocol 5,
see docs/PLAN.md, "Console to console") against Azahar or a console, with no second console.

  fake_peer.py send HOST FILE        send a file to NSA's Receive (HOST: 127.0.0.1 for Azahar)
  fake_peer.py title HOST MB         send a made-up title package of about MB megabytes
  fake_peer.py receive               be the receiver: answers probes and takes one transfer
                                     (list 127.0.0.1 in /nsa/peers.txt of the sender)

NSA asks its user first (Allow, then Install/Save): press the buttons yourself, or with the FTP
server's `SITE PRESS A` (curl -Q "SITE PRESS A" ftp://HOST:5021/). `--drop 0.6` cuts the
transfer at 60 % to try the resume; run it again to see it go on. Prints what it learns.
"""
import hashlib, os, socket, struct, sys, threading, time, zlib

CHUNK = 256 * 1024
PORT = 5002


def hello(name, size, check, kind, head):
    return (b'NSAT' + struct.pack('>I', 5) + b'fake peer'.ljust(32, b'\0') + name.ljust(256, b'\0')
            + struct.pack('>IIIII', size >> 32, size & 0xFFFFFFFF, check, kind, head))


def package(mb):
    contents = [os.urandom(int(mb * 700 * 1024) + 13), os.urandom(int(mb * 300 * 1024))]
    head = b'NSAP' + struct.pack('>IQIIII', 1, 0x0004000000123456, 1, 0, 0, len(contents))
    for i, c in enumerate(contents):
        head += struct.pack('>IHHQ', i, i, 1, len(c))
    return head, contents


def chunks(segments, start, keep_first):
    out, base = [], 0
    for i, n in enumerate(segments):
        frm = 0 if base > start or (keep_first and i == 0) else max(0, start - base)
        out += [(base + o, min(CHUNK, n - o)) for o in range(frm, n, CHUNK)]
        base += n
    return out


def send(host, payload, segments, name, check, kind, head, drop):
    control = socket.create_connection((host, PORT), timeout=10)
    control.sendall(hello(name, len(payload), check, kind, head))
    control.settimeout(120)  # until the user of the receiver has answered
    answer = b''
    while len(answer) < 13:
        answer += control.recv(13 - len(answer))
    status, start, session = struct.unpack('>BQI', answer)
    print('answer', status, 'from byte', start)
    if status != 1:
        return
    todo = chunks(segments, start, kind == 1 and start > 0)
    limit = int(len(todo) * drop) if drop else len(todo)
    lock, nxt = threading.Lock(), [0]

    def worker(i):
        s = control
        if i:
            s = socket.create_connection((host, PORT), timeout=10)
            s.sendall(b'NSAD' + struct.pack('>I', session))
        while True:
            with lock:
                k = nxt[0]
                nxt[0] += 1
            if k >= limit:
                break
            off, n = todo[k]
            s.sendall(struct.pack('>QI', off, n) + payload[off:off + n])
        if drop:
            s.close()
            return
        s.sendall(struct.pack('>QI', 0xFFFFFFFFFFFFFFFF, 0))
        if i:  # an extra connection is closed only after the receiver says it has everything
            s.settimeout(30)
            s.recv(1)
            s.close()

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(4)]
    began = time.time()
    [t.start() for t in threads]
    [t.join() for t in threads]
    if not drop:
        control.settimeout(300)
        final = control.recv(1)
        print('final answer', final, '(1 whole, 0 failed, 2 cancelled)', 'in %.1f s' % (time.time() - began))


def receive():
    probes = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    probes.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    probes.bind(('0.0.0.0', 5001))
    server = socket.socket()
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(('0.0.0.0', PORT))
    server.listen(8)

    def answer_probes():
        while True:
            data, addr = probes.recvfrom(100)
            if data == b'NSA?':
                probes.sendto(b'NSA!' + b'fake receiver'.ljust(32, b'\0'), addr)
    threading.Thread(target=answer_probes, daemon=True).start()

    control, _ = server.accept()
    raw = b''
    while len(raw) < 316:
        raw += control.recv(316 - len(raw))
    size = (struct.unpack('>I', raw[296:300])[0] << 32) | struct.unpack('>I', raw[300:304])[0]
    print('hello:', raw[40:296].rstrip(b'\0'), size, 'bytes, kind', struct.unpack('>I', raw[308:312])[0])
    session = 0x1234ABCD
    control.sendall(struct.pack('>BQI', 1, 0, session))
    buf, got, lock = bytearray(size), [0], threading.Lock()

    def reader(sock, first):
        if not first:
            sock.recv(8)
        while True:
            rec = b''
            while len(rec) < 12:
                part = sock.recv(12 - len(rec))
                if not part:
                    return
                rec += part
            off, n = struct.unpack('>QI', rec)
            if off == 0xFFFFFFFFFFFFFFFF:
                if not first:
                    sock.sendall(b'\x01')
                    time.sleep(0.3)
                return
            data = b''
            while len(data) < n:
                data += sock.recv(n - len(data))
            buf[off:off + n] = data
            with lock:
                got[0] += n

    threading.Thread(target=reader, args=(control, True), daemon=True).start()

    def accept_extras():
        server.settimeout(10)
        try:
            while True:
                extra, _ = server.accept()
                threading.Thread(target=reader, args=(extra, False), daemon=True).start()
        except OSError:
            pass
    threading.Thread(target=accept_extras, daemon=True).start()
    began = time.time()
    while got[0] < size and time.time() - began < 600:
        time.sleep(0.1)
    time.sleep(1)
    print('got', got[0], 'of', size, 'sha256', hashlib.sha256(bytes(buf)).hexdigest())
    control.sendall(b'\x01')
    time.sleep(2)


def main():
    args = sys.argv[1:]
    drop = 0.0
    if '--drop' in args:
        i = args.index('--drop')
        drop = float(args[i + 1])
        del args[i:i + 2]
    if not args:
        print(__doc__)
        return
    if args[0] == 'receive':
        receive()
    elif args[0] == 'send':
        data = open(args[2], 'rb').read()
        send(args[1], data, [len(data)], os.path.basename(args[2]).encode(), zlib.crc32(data[:4096]) & 0xFFFFFFFF, 0, 0, drop)
    elif args[0] == 'title':
        head, contents = package(float(args[2]))
        payload = head + b''.join(contents)
        send(args[1], payload, [len(head)] + [len(c) for c in contents], b'Fake [0004000000123456] v1.cia',
             zlib.crc32(head[:4096]) & 0xFFFFFFFF, 1, len(head), drop)
    else:
        print(__doc__)


if __name__ == '__main__':
    main()
