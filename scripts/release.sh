#!/usr/bin/env bash
# Cut a GitHub release.
#
#   scripts/release.sh v2.7.0
#
# Cross-builds on the Linux build host, pulls the package back, and publishes
# it with the installer attached. Run from a clean checkout of the tag you are
# releasing.
#
# Why this is not a GitHub Actions workflow: the build needs a sysroot that
# kindle-sdk extracts from Amazon firmware. Those libraries cannot be
# redistributed or cached in a public repository, so the sysroot has to stay on
# a machine you control. Everything else is reproducible from BUILD-KINDLE.md.
set -euo pipefail

TAG=${1:-}
BUILD_HOST=${KTERM_BUILD_HOST:-charlesji@trashcan}
BUILD_DIR=${KTERM_BUILD_DIR:-kterm-src}
REPO=${KTERM_REPO:-szcharlesji/kterm}

[ -n "$TAG" ] || { echo "usage: $0 <tag>   e.g. $0 v2.7.0" >&2; exit 2; }
command -v gh >/dev/null || { echo "gh CLI not found" >&2; exit 1; }

if [ -n "$(git status --porcelain)" ]; then
    echo "working tree is dirty; commit before releasing" >&2
    exit 1
fi

echo "==> syncing source to $BUILD_HOST"
rsync -az --delete --exclude '.git' ./ "$BUILD_HOST:$BUILD_DIR/"

echo "==> cross-building"
ssh "$BUILD_HOST" "bash ~/build-cross-kterm.sh" > /tmp/kterm-release-build.log 2>&1 || {
    tail -30 /tmp/kterm-release-build.log; exit 1;
}
grep -q 'DONE' /tmp/kterm-release-build.log || { echo "build did not finish"; exit 1; }

VER=$(sed -n 's/^AC_INIT(\[kterm\], \[\([^]]*\)\].*/\1/p' configure.ac)
REMOTE_ZIP="$BUILD_DIR/kterm-kindle-$VER.zip"
OUT=dist
mkdir -p "$OUT"
scp -q "$BUILD_HOST:$REMOTE_ZIP" "$OUT/kterm-kindle.zip"
cp kindle.pkg/install.sh "$OUT/install.sh"

echo "==> verifying the binaries are for a Kindle"
ssh "$BUILD_HOST" "bash -lc '
  H=arm-kindlehf-linux-gnueabihf
  \$HOME/x-tools/\$H/bin/\$H-readelf -h $BUILD_DIR/kterm | grep -E \"Flags:\"
  \$HOME/x-tools/\$H/bin/\$H-readelf -d $BUILD_DIR/kterm | grep -c libvte || true
'"

( cd "$OUT" && shasum -a 256 kterm-kindle.zip install.sh > SHA256SUMS )
cat "$OUT/SHA256SUMS"

echo "==> publishing $TAG"
gh release create "$TAG" \
    --repo "$REPO" \
    --title "kterm $TAG" \
    --notes-file <(cat <<EOF
Kindle build for firmware 5.16.3 and newer (hard-float).

## Install

On the Kindle, over ssh:

\`\`\`sh
curl -sSL https://github.com/$REPO/releases/latest/download/install.sh | sh
\`\`\`

Upgrading keeps your \`kterm.conf\`, \`bin/local.sh\` and \`menu.json\`, and
backs up the old install to \`/mnt/us/kterm-backup-*.tar.gz\` first.

Or download \`kterm-kindle.zip\` and unzip it into \`/mnt/us/extensions/\`.

## Verify

\`\`\`
$(cat "$OUT/SHA256SUMS")
\`\`\`
EOF
) \
    "$OUT/kterm-kindle.zip" "$OUT/install.sh" "$OUT/SHA256SUMS"

echo "==> done: https://github.com/$REPO/releases/tag/$TAG"
