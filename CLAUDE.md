# Project notes for Claude

**NSA** is the owner's fork (`tinaut1986/NSA`) of Steveice10's FBI 2.6.1, a 3DS title
manager. It installs as its own app (product code `CTR-P-CNSA`, unique ID `0xFB1A0`)
next to FBI, keeps its data in `sdmc:/nsa/` and numbers its own releases from `v0.1.0`.

## Language

- Chat with the owner in **Spanish (castellano)**.
- Code, comments, identifiers, commit messages and docs: **English**.

## How we work

1. **Every change goes on a new branch**, unless the owner says otherwise. Topic
   branches (`feat/...`, `fix/...`, `chore/...`, `docs/...`) are cut from `release`.
2. **No pull requests.** The owner tests by hand on the console.
3. When the change is ready, **build the CIA and send it to the owner** (or install
   it, see below). Do not commit yet.
4. The owner tests it. Only when they say it is good: **commit, `git merge --no-ff`
   into `release`, and push**.

If a branch the owner is still testing is needed by the next change, cut the new one
from it and say so.

## Git and GitHub

- `origin` = `tinaut1986/NSA` (a fork of `Steveice10/FBI`), `upstream` = Steveice10/FBI.
  The push URL of `upstream` is `DISABLED`; `gh repo set-default tinaut1986/NSA`.
- `master` = stable, only moves by merging `release`. `release` = beta, the working line.
- A `vX.Y.Z` tag on `release` publishes a Beta; once `master` contains it, re-running
  the release workflow on the tag promotes it to stable (`.github/workflows/build-release.yml`).
  Write `docs/release-notes/<tag>.md` first (`docs/release-notes/README.md`).

## Build

```sh
export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
git submodule update --init          # buildtools
make -j"$(nproc)" DEBUG=1            # output/3ds-arm/NSA.cia and 3ds/NSA/NSA.3dsx
make -C tests                        # host tests (needs libjansson-dev)
```

`DEBUG=1` starts the FTP server with NSA and shows DEBUG next to the version; the
version itself comes from `tools/version.sh`. `bannertool` and `makerom` are in `tools/bin/`.
`make` does not rebuild on a version or `DEBUG` change: `make clean` first.

## Installing on the console

Ask before installing while the owner is using the console.

- **NSA's FTP server** (Settings → FTP Server, always on in DEBUG builds), port 5000:
  `curl -T output/3ds-arm/NSA.cia ftp://IP:5000/nsa/update.cia -Q "-SITE INSTALL /nsa/update.cia"`.
  NSA restarts into the new build 10 s later.
- **FBI's Remote Install → Receive URLs** (port 5000): serve the CIA over HTTP from
  this PC and send the URL with FBI's protocol (4-byte big-endian length, then the
  URL). The owner confirms on the console. Do not probe the port with an empty
  connection: FBI shows "Failed to read payload length".
- The owner's console was at `192.168.1.133` (2026-10-27).
