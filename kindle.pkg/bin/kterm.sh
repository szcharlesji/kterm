#!/bin/sh
EXTENSION=/mnt/us/extensions/kterm
DPI=`cat /var/log/Xorg.0.log | grep DPI | sed -n "s/.*(\([0-9]\+\), [0-9]\+).*/\1/p"`
#use different layouts for high resolution devices
if [ ${DPI} -gt 290 ]; then
  PARAM="-l ${EXTENSION}/layouts/keyboard-300dpi.xml"
elif [ ${DPI} -gt 200 ]; then
  PARAM="-l ${EXTENSION}/layouts/keyboard-200dpi.xml"
fi

# Terminfo shipped with the extension.
#
# TERM stays "xterm" on purpose. The bundled tree does contain an
# xterm-256color entry, but on a grayscale eink panel the 256 colour cube
# collapses into a handful of near identical mid greys, whereas the 16 entry
# palette kterm installs is tuned for contrast on this display. Applications
# told they have 16 colours produce noticeably more readable output here.
# Switch it if you disagree; nothing else depends on the value.
export TERM=xterm TERMINFO=${EXTENSION}/vte/terminfo

# Fonts. The rootfs is read only and its fontconfig only scans
# /usr/share/fonts and ~/.fonts, so point fontconfig at ours instead.
# See fonts/README.md for adding a Nerd Font.
if [ -r "${EXTENSION}/fonts/fonts.conf" ]; then
  export FONTCONFIG_FILE=${EXTENSION}/fonts/fonts.conf
  mkdir -p /var/local/kterm-fontcache 2>/dev/null
fi

# Local customisations (HOME, PATH, ssh config, aliases...) live here so
# they survive reinstalling the extension.
if [ -r "${EXTENSION}/bin/local.sh" ]; then
  . "${EXTENSION}/bin/local.sh"
fi

${EXTENSION}/bin/kterm ${PARAM} "$@"
