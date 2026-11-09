#!/usr/bin/env python3
"""Tests of the web server (source/fbi/webserver.c) against NSA in Azahar or on a console.

    tools/web_tests.py --host 127.0.0.1 [--pin 123456]

NSA needs "web_server": true in sdmc:/nsa/settings.json (or Settings -> Web server). Without --pin it
is fetched from /nsa/web-pin.txt over the FTP server (port 5021), which DEBUG builds write.
"""
import argparse, json, os, sys, urllib.request, urllib.error, ftplib, io, time

ap = argparse.ArgumentParser()
ap.add_argument('--host', required=True)
ap.add_argument('--port', type=int, default=8080)
ap.add_argument('--pin')
a = ap.parse_args()
base = f'http://{a.host}:{a.port}'

if not a.pin:
    ftp = ftplib.FTP(); ftp.connect(a.host, 5021); ftp.login()
    buf = io.BytesIO(); ftp.retrbinary('RETR /nsa/web-pin.txt', buf.write); a.pin = buf.getvalue().decode().strip()

def req(method, path, pin=None, data=None, headers=None):
    h = dict(headers or {})
    if pin: h['X-Pin'] = pin
    r = urllib.request.Request(base + path, data=data, method=method, headers=h)
    try:
        with urllib.request.urlopen(r, timeout=30) as resp: return resp.status, resp.read()
    except urllib.error.HTTPError as e: return e.code, e.read()

fails = 0
def check(name, ok):
    global fails
    print(('ok   ' if ok else 'FAIL ') + name); fails += 0 if ok else 1

s, b = req('GET', '/'); check('page', s == 200 and b.startswith(b'<!doctype html>'))
s, b = req('GET', '/api/info'); check('no PIN refused', s == 401)
s, b = req('GET', '/api/info', '000000'); check('wrong PIN refused', s == 401)
s, b = req('GET', '/api/info', a.pin); check('info', s == 200 and json.loads(b)['ok'])
s, b = req('POST', '/api/mkdir?path=/webtest', a.pin); check('mkdir', s == 200)
s, b = req('POST', '/api/mkdir?path=/webtest/x', None, headers={'Cookie': 'pin=' + a.pin}); check('change with the cookie only refused', s == 401)
blob = os.urandom(300000)
s, b = req('PUT', '/api/file?path=/webtest/big.bin', a.pin, blob); check('upload', s == 200)
s, b = req('GET', '/api/file?path=/webtest/big.bin', a.pin); check('download', s == 200 and b == blob)
s, b = req('GET', '/api/file?path=/webtest/big.bin', a.pin, headers={'Range': 'bytes=299990-'}); check('range', s == 206 and b == blob[299990:])
s, b = req('GET', '/api/list?path=/webtest', a.pin); check('list', s == 200 and any(f['name'] == 'big.bin' and f['size'] == 300000 for f in json.loads(b)['files']))
s, b = req('GET', '/api/list?path=/../nsa', a.pin); check('.. refused', s == 400)
s, b = req('POST', '/api/rename?path=/webtest/big.bin&to=/webtest/b2.bin', a.pin); check('rename', s == 200)
s, b = req('DELETE', '/api/file?path=/', a.pin); check('root not deletable', s == 403)
s, b = req('POST', '/api/mkdir?path=/webtest/d', a.pin); s2, b2 = req('PUT', '/api/file?path=/webtest/d/f.txt', a.pin, b'hello')
s3, b3 = req('POST', '/api/copy?path=/webtest/d&to=/webtest/d2', a.pin); check('copy a folder', s3 == 200)
s, b = req('GET', '/api/file?path=/webtest/d2/f.txt', a.pin); check('copied content', s == 200 and b == b'hello')
s, b = req('POST', '/api/copy?path=/webtest/d&to=/webtest/d2', a.pin); check('copy onto an existing name is 409', s == 409)
s, b = req('POST', '/api/copy?path=/webtest/d&to=/webtest/d/in', a.pin); check('copy into itself is 400', s == 400)
s, b = req('POST', '/api/copy?path=/webtest/nope&to=/webtest/n2', a.pin); check('copy of a missing file is 404', s == 404)
s, b = req('DELETE', '/api/file?path=/webtest', a.pin); check('delete folder', s == 200)
s, b = req('GET', '/theme.json'); t = json.loads(b) if s == 200 else {}
check('theme is public and has colours', s == 200 and t.get('theme') in ('cards', 'pixel', 'home') and t['colors']['accent'].startswith('#'))
s, b = req('GET', '/icons.png'); check('icon atlas is public', s == 200 and b[:4] == b'\x89PNG')
s, b = req('GET', '/icons.js'); check('icon names are public', s == 200 and b'ICON_NAMES' in b)
s, b = req('GET', '/api/settings', a.pin); st = json.loads(b) if s == 200 else {}
check('settings', s == 200 and st.get('theme') == t.get('theme') and len(st.get('accents', [])) > 1 and st.get('night') in ('off', 'on', 'auto'))
s, b = req('POST', '/api/settings?theme=nope', a.pin); check('unknown theme refused', s == 400)
s, b = req('POST', '/api/settings?musicVolume=101', a.pin); check('volume out of range refused', s == 400)
s, b = req('POST', '/api/settings?night=' + st.get('night', 'off')); check('settings need the PIN', s == 401)
s, b = req('POST', '/api/settings?night=' + st.get('night', 'off'), a.pin); check('settings set (unchanged)', s == 200 and json.loads(b)['night'] == st.get('night'))
s, b = req('GET', '/font.ttf'); check('pixel font', s == 200 and len(b) > 10000)
s, b = req('GET', '/api/titles', a.pin); titles = json.loads(b)['titles'] if s == 200 else []
check('titles list', s == 200 and isinstance(titles, list))
if titles:
    t0 = titles[0]; q = f"id={t0['id']}&media={t0['media']}"
    s, b = req('GET', '/api/title?' + q, a.pin); check('title details', s == 200 and 'name' in json.loads(b))
    s, b = req('POST', '/api/save/pack?' + q + '&kind=nope', a.pin); check('bad kind is 400', s == 400)
    s, b = req('GET', '/api/icon?' + q, a.pin); check('icon is a 48x48 BMP', s == 200 and b[:2] == b'BM' and len(b) == 54 + 48 * 48 * 3 or s == 404)
s, b = req('GET', '/api/title?id=0004000000000000&media=sd', a.pin); check('unknown title is 404', s == 404)
s, b = req('POST', '/api/save/pack?id=0004000000000000&media=sd&kind=save', a.pin); check('save of an unknown title is 404', s == 404)
s, b = req('POST', '/api/save/receive?path=/webtest/x.nsasave', a.pin); check('receive only from /nsa/received', s == 400)
s, b = req('POST', '/api/install-url', a.pin, b'ftp://x/y.cia\n'); check('only http(s) URLs', s == 400)
s, b = req('POST', '/api/install-url', a.pin, b''); check('empty URL list refused', s == 400)
s, b = req('POST', '/api/title/launch?id=0004000000000000&media=sd', a.pin); check('launch of an unknown title is 404', s == 404)
s, b = req('POST', '/api/title/delete?id=0004000000000000&media=sd', a.pin); check('delete of an unknown title is 404', s == 404)
if titles:
    s, b = req('GET', '/api/save/backups?' + q + '&kind=save', a.pin); check('backups list', s == 200 and isinstance(json.loads(b)['backups'], list))
    s, b = req('POST', '/api/save/import?' + q + '&kind=save&path=/etc/passwd', a.pin); check('import needs a listed backup (404 or queued then refused)', s in (200, 404))
# The import above may still be pending on the console (409 "still asking about the last request") for a moment.
for _ in range(15):
    s, b = req('POST', '/api/install-url', a.pin, b'http://127.0.0.1:9/none.cia\n')
    if s != 409:
        break
    time.sleep(1)
check('install from a URL is queued for the console to ask', s == 200)
s, b = req('GET', '/api/tickets', a.pin); check('tickets list', s == 200 and isinstance(json.loads(b)['tickets'], list))
s, b = req('GET', '/api/extdata', a.pin); check('extra data list', s == 200 and isinstance(json.loads(b)['extdata'], list))
s, b = req('GET', '/api/extdata/info?id=zz&media=sd', a.pin); check('extra data info needs a real id', s == 400)
s, b = req('GET', '/api/extdata/info?id=0000000000000000&media=card', a.pin); check('extra data media is sd or nand', s == 400)
s, b = req('GET', '/api/extdata/icon?id=0000000000000000&media=sd', a.pin); check('icon of a missing extra data is 404', s == 404)
s, b = req('GET', '/api/screens', a.pin); check('screens: 400x240 + 320x240 RGB565', (s == 200 and len(b) == (400 + 320) * 240 * 2) or s == 503)
s, b = req('POST', '/api/input/press?keys=NOPE', a.pin); check('unknown button refused', s == 400)
s, b = req('POST', '/api/input/tap?x=400&y=1', a.pin); check('tap outside the screen refused', s == 400)
s, b = req('POST', '/api/input/press?keys=L', a.pin); check('press', s == 200)
s, b = req('POST', '/api/input/tap?x=10&y=10', a.pin); check('tap', s == 200)
s, b = req('POST', '/api/input/drag?x=10&y1=100&y2=40', a.pin); check('drag', s == 200)
print('failures:', fails); sys.exit(1 if fails else 0)
