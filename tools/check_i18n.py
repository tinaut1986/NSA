#!/usr/bin/env python3
"""Checks the translation tables (source/core/i18n_<code>.c) against Spanish's.

Spanish (i18n_es.c) is the reference for which English strings exist. For each
other language the keys must be the same, and each translation must keep what
the code depends on: the printf conversions in the same order, the \\uE000..\\uE005
button glyphs, the line breaks, and a leading or trailing space where the key has one
(hints are glued to a glyph). Run: python3 tools/check_i18n.py [code ...]
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent / "source" / "core"
CODES = ["es", "ca", "fr", "de", "it", "pt", "ru", "ja", "zh", "tw"]

MACROS = {"APP_NAME": "NSA", "APP_DATA_DIR": "/nsa/", "APP_LEGACY_DATA_DIR": "/fbi/", "APP_REPOSITORY": "tinaut1986/NSA",
          "APP_UPSTREAM_VERSION": "FBI 2.6.1"}

STRING = r'"(?:[^"\\]|\\.)*"'
CHAIN = rf'{STRING}(?:\s*{STRING})*'
ENTRY = re.compile(rf'\{{\s*({CHAIN})\s*,\s*({CHAIN})\s*\}}')
CONVERSION = re.compile(r'(?<!\d)%[-+ #0]*(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|h|ll|l|z|j|t)?[diuoxXcsfFeEgGp%]')
GLYPH = re.compile('[-]')


def decode(chain):
    out = []
    for part in re.findall(STRING, chain):
        body = part[1:-1]
        out.append(re.sub(r'\\(u[0-9a-fA-F]{4}|x[0-9a-fA-F]{2}|.)', unescape, body))
    return "".join(out)


def unescape(m):
    e = m.group(1)
    if e[0] == 'u' and len(e) == 5:
        return chr(int(e[1:], 16))
    if e[0] == 'x' and len(e) == 3:
        return chr(int(e[1:], 16))
    return {'n': '\n', 't': '\t', '"': '"', '\\': '\\', "'": "'", 'r': '\r'}.get(e, e)


def load(code):
    text = (ROOT / f"i18n_{code}.c").read_text(encoding="utf-8")
    text = re.sub(r'(?m)^\s*//[^\n]*', '', text)
    # appinfo.h's macros are string literals joined to their neighbours.
    for name, value in MACROS.items():
        text = re.sub(rf'\b{name}\b', '"' + value + '"', text)
    entries = [(decode(k), decode(v)) for k, v in ENTRY.findall(text)]
    return entries


def check(code, reference):
    problems = []
    try:
        entries = load(code)
    except FileNotFoundError:
        return [f"i18n_{code}.c does not exist"]

    seen = {}
    for key, value in entries:
        if key in seen:
            problems.append(f"duplicate key {key!r}")
        seen[key] = value

    for key in reference:
        if key not in seen:
            problems.append(f"missing {key!r}")
    for key in seen:
        if key not in reference:
            problems.append(f"extra key {key!r}")

    for key, value in seen.items():
        if key not in reference:
            continue
        if CONVERSION.findall(key) != CONVERSION.findall(value):
            problems.append(f"conversions differ in {key!r} -> {value!r}")
        if GLYPH.findall(key) != GLYPH.findall(value):
            problems.append(f"button glyphs differ in {key!r} -> {value!r}")
        if key.count("\n") != value.count("\n"):
            problems.append(f"line breaks differ in {key!r} -> {value!r}")
        if key.startswith(" ") != value.startswith(" ") or key.endswith(" ") != value.endswith(" "):
            problems.append(f"edge spaces differ in {key!r} -> {value!r}")
        if not value.strip():
            problems.append(f"empty translation of {key!r}")
    return problems


def main():
    codes = sys.argv[1:] or CODES
    reference = dict(load("es"))
    failed = False
    for code in codes:
        problems = check(code, reference) if code != "es" else []
        print(f"{code}: {len(load(code)) if (ROOT / f'i18n_{code}.c').exists() else 0} entries, {len(problems)} problems")
        for p in problems[:40]:
            print("   ", p)
        failed = failed or bool(problems)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
