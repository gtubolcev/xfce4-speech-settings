#!/usr/bin/env bash
# Builds a signed source package of the committed HEAD and uploads it to the
# Launchpad PPA, which builds the .debs.
#
#   scripts/upload-ppa.sh [codename]
#
# The upstream version is the one in meson.build, plus +git<date>.<commit>
# when HEAD is not the tag v<version>; the package version adds -1~ppa<N>
# (N counted from the PPA, a version can be uploaded only once) and
# ~<codename>. PPA=... overrides the target (ppa:gtubolcev/linux-tts);
# SIGN_KEY=... the key; NO_UPLOAD=1 only builds the source package
# (unsigned) into _build/ppa/.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. /etc/os-release
DIST=${1:-$VERSION_CODENAME}
PPA=${PPA:-ppa:gtubolcev/linux-tts}
PPA_ARCHIVE=$(sed -E 's|^ppa:([^/]+)/(.+)$|~\1/+archive/ubuntu/\2|' <<< "$PPA")
SRC=$(sed -n 's/^Source: //p' "$ROOT/debian/control")
export DEBFULLNAME=${DEBFULLNAME:-"Grigory Tuboltsev"}
export DEBEMAIL=${DEBEMAIL:-"gtubolcev@gmail.com"}

git -C "$ROOT" diff --quiet HEAD || { echo "error: uncommitted changes" >&2; exit 1; }
UPSTREAM=$(sed -n "s/^  version: '\(.*\)',$/\1/p" "$ROOT/meson.build")
if [ "$(git -C "$ROOT" tag --points-at HEAD)" != "v$UPSTREAM" ]; then
  UPSTREAM=$UPSTREAM+git$(git -C "$ROOT" log -1 --date=format:%Y%m%d --format=%cd).$(git -C "$ROOT" rev-parse --short=7 HEAD)
fi

# the next upload number for versions starting with $1
next_upload() {
  curl -fsS --retry 5 --retry-all-errors "https://api.launchpad.net/devel/${PPA_ARCHIVE}?ws.op=getPublishedSources&source_name=$SRC&exact_match=true" \
    | python3 -c 'import json, sys
prefix = sys.argv[1]
nums = [e["source_package_version"][len(prefix):].split("~")[0] for e in json.load(sys.stdin)["entries"]
        if e["source_package_version"].startswith(prefix)]
print(max((int(n) for n in nums if n.isdigit()), default=0) + 1)' "$1"
}

WORK=$ROOT/_build/ppa/$DIST
rm -rf "$WORK"
mkdir -p "$WORK"
git -C "$ROOT" archive --format=tar --prefix="$SRC-$UPSTREAM/" HEAD -- . ':(exclude)debian' \
  | xz -9 > "$WORK/${SRC}_$UPSTREAM.orig.tar.xz"
git -C "$ROOT" archive --format=tar --prefix="$SRC-$UPSTREAM/" HEAD | tar -x -C "$WORK"
cd "$WORK/$SRC-$UPSTREAM"
VERSION=$UPSTREAM-1~ppa
VERSION=$VERSION$(next_upload "$VERSION")~$DIST
dch --newversion "$VERSION" --distribution "$DIST" --force-distribution \
  "Build of $(git -C "$ROOT" rev-parse --short=7 HEAD) for the PPA."
if [ -n "${NO_UPLOAD:-}" ]; then
  dpkg-buildpackage -S -d -us -uc -sa
  exit 0
fi
dpkg-buildpackage -S -d -sa ${SIGN_KEY:+--sign-key="$SIGN_KEY"}
cd ..
dput "$PPA" ./*_source.changes
