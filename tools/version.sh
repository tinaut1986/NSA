#!/bin/sh
# Prints the version this checkout builds as.
#   VERSION=X.Y.Z CHANNEL=release -> X.Y.Z       (CI, stable release)
#   VERSION=X.Y.Z CHANNEL=beta    -> X.Y.Z-beta  (CI, pre-release)
#   otherwise, from git:
#     exactly on a vX.Y.Z tag     -> X.Y.Z
#     N commits after vX.Y.Z      -> X.Y.(Z+1)-dev.N+hash
# Upstream FBI's tags (2.6.1...) count as a base until we have one of ours.
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
desc=$(git describe --tags --long --match 'v[0-9]*.[0-9]*.[0-9]*' 2>/dev/null ||
       git describe --tags --long --match '[0-9]*.[0-9]*.[0-9]*' 2>/dev/null ||
       echo "0.0.0-0-g$(git rev-parse --short HEAD 2>/dev/null || echo unknown)")

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
