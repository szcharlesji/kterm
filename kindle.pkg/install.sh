#!/bin/sh
# kterm installer. Run on the Kindle:
#
#   curl -sSL https://github.com/szcharlesji/kterm/releases/latest/download/install.sh | sh
#
# or, with a zip already on the device:
#
#   sh install.sh /mnt/us/kterm-kindle.zip
#
# Upgrading keeps your kterm.conf, bin/local.sh and menu.json, and takes a
# backup first. Nothing here touches the read-only rootfs.
set -e

REPO=szcharlesji/kterm
EXT=/mnt/us/extensions/kterm
TMP=/tmp/kterm-install.$$
ZIP=$1

log()  { echo "  $*"; }
fail() { echo "error: $*" >&2; exit 1; }

[ -d /mnt/us ] || fail "/mnt/us not found, is this a Kindle?"
mkdir -p /mnt/us/extensions

if [ -z "$ZIP" ]; then
    command -v curl >/dev/null || fail "curl not found, download the zip yourself and pass it as an argument"
    ZIP=$TMP.zip
    URL="https://github.com/$REPO/releases/latest/download/kterm-kindle.zip"
    log "downloading $URL"
    # busybox wget has no working TLS here; curl does
    curl -sSL --fail -o "$ZIP" "$URL" || fail "download failed"
fi
[ -s "$ZIP" ] || fail "$ZIP is missing or empty"

rm -rf "$TMP"
mkdir -p "$TMP"
unzip -q -o "$ZIP" -d "$TMP" || fail "unzip failed"
[ -x "$TMP/kterm/bin/kterm" ] || [ -f "$TMP/kterm/bin/kterm" ] || fail "zip does not look like a kterm package"

if [ -d "$EXT" ]; then
    BACKUP=/mnt/us/kterm-backup-$(date +%Y%m%d-%H%M).tar.gz
    log "backing up to $BACKUP"
    ( cd /mnt/us/extensions && tar czf "$BACKUP" kterm ) || fail "backup failed"

    # keep anything you customised
    for keep in bin/kterm.conf bin/local.sh menu.json; do
        if [ -f "$EXT/$keep" ]; then
            log "keeping $keep"
            cp "$EXT/$keep" "$TMP/kterm/$keep"
        fi
    done

    # keep fonts you added yourself, or an upgrade silently deletes the font
    # your kterm.conf names and everything falls back to a system face
    if [ -d "$EXT/fonts" ]; then
        mkdir -p "$TMP/kterm/fonts"
        for f in "$EXT"/fonts/*.ttf "$EXT"/fonts/*.otf "$EXT"/fonts/*.ttc; do
            [ -f "$f" ] || continue
            base=$(basename "$f")
            [ -f "$TMP/kterm/fonts/$base" ] && continue
            log "keeping font $base"
            cp "$f" "$TMP/kterm/fonts/$base"
        done
    fi
fi

log "installing to $EXT"
# /mnt/us is a fuse volume that will not overwrite in place, so replace
rm -rf "$EXT"
mkdir -p "$EXT"
( cd "$TMP/kterm" && tar cf - . ) | ( cd "$EXT" && tar xf - ) || fail "copy failed"

chmod 755 "$EXT/bin/kterm" "$EXT/bin/ktsh" "$EXT/bin/kterm.sh" 2>/dev/null || true
[ -f "$EXT/install.sh" ] && chmod 755 "$EXT/install.sh"
mkdir -p /var/local/kterm-fontcache 2>/dev/null || true

rm -rf "$TMP" "$TMP.zip"

echo
echo "kterm installed. Open KUAL and pick kterm."
"$EXT/bin/kterm" -v 2>/dev/null || true
echo "Fonts: drop .ttf files in $EXT/fonts and set font_family in bin/kterm.conf"
