#!/usr/bin/env python3
"""Tests of the web server (source/fbi/webserver.c) against NSA in Azahar or on a console.

    tools/web_tests.py --host 127.0.0.1 [--pin 123456]

NSA needs "web_server": true in sdmc:/nsa/settings.json (or Settings -> Web server). Without --pin it
is fetched from /nsa/web-pin.txt over the FTP server (port 5021), which DEBUG builds write.
"""
import argparse, json, os, sys, urllib.request, urllib.error, ftplib, io

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
s, b = req('DELETE', '/api/file?path=/webtest', a.pin); check('delete folder', s == 200)
print('failures:', fails); sys.exit(1 if fails else 0)
