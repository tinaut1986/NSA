#!/bin/sh
# Prints the version this checkout builds as.
#   VERSION=X.Y.Z CHANNEL=release -> X.Y.Z       (CI, stable release)
#   VERSION=X.Y.Z CHANNEL=beta    -> X.Y.Z-beta  (CI, pre-release)
#   otherwise, from git:
#     exactly on a vX.Y.Z tag     -> X.Y.Z
#     N commits after vX.Y.Z      -> X.Y.(Z+1)-dev.N+hash
#     before our first tag        -> 0.1.0-dev.N+hash (N: commits since FBI 2.6.1)
# NSA numbers its own releases from v0.1.0; upstream FBI's tags (2.6.1...)
# are never a version of ours.
# The updater orders one number's builds dev < beta < plain.
set -eu

if [ -n "${VERSION:-}" ]; then
    case "${CHANNEL:-release}" in
        beta) echo "${VERSION}-beta" ;;
        *) echo "${VERSION}" ;;
    esac
    exit 0
fi

cd "$(dirname "$0")/.."
if ! desc=$(git describe --tags --long --match 'v[0-9]*.[0-9]*.[0-9]*' 2>/dev/null); then
    commits=$(git rev-list --count 2.6.1..HEAD 2>/dev/null || echo 0)
    echo "0.1.0-dev.${commits}+$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
    exit 0
fi

tag=${desc%-*-g*}
rest=${desc#"$tag"-}
commits=${rest%%-*}
hash=${rest#*-g}
base=${tag#v}

if [ "$commits" = 0 ]; then
    echo "$base"
    exit 0
fi

major=${base%%.*}
minor=${base#*.}; minor=${minor%%.*}
micro=${base##*.}
echo "${major}.${minor}.$((micro + 1))-dev.${commits}+${hash}"
