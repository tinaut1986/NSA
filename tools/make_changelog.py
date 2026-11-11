#!/usr/bin/env python3
"""Builds the changelog pages NSA's Settings -> Changelog reads (source/fbi/update.c).

    tools/make_changelog.py --out <site>/docs/changelog [--releases releases.json] [--repo owner/name]

One folder per interface language (the codes in source/core/i18n.c) with pages of
UPDATE_HISTORY_PAGE releases, newest first:

    changelog/<code>/<n>.json = {"format": 1, "page": n, "pages": total,
                                 "releases": [{"tag": "vX.Y.Z", "prerelease": bool, "notes": "- ..."}]}

The releases (which tags are published, and which are betas) come from GitHub's API, or from a file with that list
(--releases). The notes come from docs/release-notes/<tag>.<code>.md, else <tag>.md (English), else the English
block of the release body. The release bodies themselves are never changed: NSA v0.9.9 and older read those.

GitHub Pages serves --out from the branch feat/website (folder /docs); the release workflow runs this there.
"""

import argparse
import json
import os
import re
import shutil
import sys
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAGE = 10        # UPDATE_HISTORY_PAGE in source/fbi/update_manifest.h
FORMAT = 1       # UPDATE_HISTORY_FORMAT
NOTES_OPEN = "<!-- nsa-notes -->"
NOTES_CLOSE = "<!-- /nsa-notes -->"
TAG = re.compile(r"^v(\d+)\.(\d+)\.(\d+)$")


def languages():
    text = open(os.path.join(ROOT, "source", "core", "i18n.c"), encoding="utf-8").read()
    codes = re.findall(r'\[LANGUAGE_[A-Z_]+\] = \{"([a-z]{2})"', text)
    if "en" not in codes:
        sys.exit("make_changelog: no language codes found in source/core/i18n.c")
    return codes


def fetch_releases(repo):
    headers = {"Accept": "application/vnd.github+json", "User-Agent": "nsa-make-changelog"}
    token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    if token:
        headers["Authorization"] = "Bearer " + token
    out = []
    page = 1
    while True:
        url = f"https://api.github.com/repos/{repo}/releases?per_page=100&page={page}"
        with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=60) as r:
            batch = json.load(r)
        out += batch
        if len(batch) < 100:
            return out
        page += 1


def body_notes(body):
    """The English block of a release body, without its "## What's new" heading."""
    a = body.find(NOTES_OPEN)
    if a < 0:
        return ""
    block = body[a + len(NOTES_OPEN):]
    b = block.find(NOTES_CLOSE)
    block = block[:b] if b >= 0 else block
    lines = block.replace("\r", "").strip("\n").split("\n")
    if lines and lines[0].startswith("## "):
        lines = lines[1:]
    return "\n".join(lines).strip("\n") + "\n"


def read_notes(notes_dir, tag, code):
    for name in ([f"{tag}.{code}.md"] if code != "en" else []) + [f"{tag}.md"]:
        path = os.path.join(notes_dir, name)
        if os.path.isfile(path):
            text = open(path, encoding="utf-8").read().replace("\r", "").strip("\n")
            return text + "\n"
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True, help="the changelog folder (replaced)")
    ap.add_argument("--releases", help="GitHub's release list as JSON, instead of asking the API")
    ap.add_argument("--repo", default="tinaut1986/NSA")
    ap.add_argument("--notes", default=os.path.join(ROOT, "docs", "release-notes"))
    args = ap.parse_args()

    if args.releases:
        releases = json.load(open(args.releases, encoding="utf-8"))
    else:
        releases = fetch_releases(args.repo)

    published = []
    for r in releases:
        m = TAG.match(r.get("tag_name") or "")
        if r.get("draft") or not m:
            continue
        published.append((tuple(int(x) for x in m.groups()), r))
    published.sort(key=lambda x: x[0], reverse=True)

    codes = languages()
    missing = {}
    out = os.path.abspath(args.out)
    if os.path.isdir(out):
        shutil.rmtree(out)

    for code in codes:
        entries = []
        for _, r in published:
            tag = r["tag_name"]
            notes = read_notes(args.notes, tag, code)
            if notes is None or (code != "en" and not os.path.isfile(os.path.join(args.notes, f"{tag}.{code}.md"))):
                missing.setdefault(code, []).append(tag)
            if notes is None:
                notes = body_notes(r.get("body") or "")
            entries.append({"tag": tag, "prerelease": bool(r.get("prerelease")), "notes": notes})

        pages = max(1, (len(entries) + PAGE - 1) // PAGE)
        os.makedirs(os.path.join(out, code))
        for n in range(1, pages + 1):
            page = {"format": FORMAT, "page": n, "pages": pages, "releases": entries[(n - 1) * PAGE:n * PAGE]}
            with open(os.path.join(out, code, f"{n}.json"), "w", encoding="utf-8", newline="\n") as f:
                json.dump(page, f, ensure_ascii=False, separators=(",", ":"))
                f.write("\n")

    print(f"make_changelog: {len(published)} releases, {len(codes)} languages, "
          f"{max(1, (len(published) + PAGE - 1) // PAGE)} pages each, in {out}")
    for code, tags in sorted(missing.items()):
        print(f"make_changelog: no {code} notes for {', '.join(tags)} (English shown)")


if __name__ == "__main__":
    main()
