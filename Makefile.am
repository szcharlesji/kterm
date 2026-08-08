bin_PROGRAMS = kterm ktsh
kterm_SOURCES = keyboard.c kterm.c parse_config.c parse_layout.c
if KINDLE
kterm_SOURCES += kindle.c
endif

# ktsh is a plain POSIX program: no gtk, no vte, no glib. It is the escape
# sequence shim kterm runs the child shell under, and it is deliberately
# buildable and testable on any host.
ktsh_SOURCES = ktsh.c ktfilter.c

check_PROGRAMS = ktsh_test
ktsh_test_SOURCES = tests/ktsh_test.c ktfilter.c
TESTS = ktsh_test

ktermconfdir = $(sysconfdir)/kterm
ktermconf_DATA = kterm.conf
layoutsdir = $(ktermconfdir)/layouts
layouts_DATA = layouts/keyboard.xml
layoutsimgdir = $(layoutsdir)/img
layoutsimg_DATA = layouts/img/*.png
fontsdir = $(ktermconfdir)/fonts
fonts_DATA = fonts/fonts.conf fonts/README.md

kterm_CFLAGS = @GTK_CFLAGS@ @VTE_CFLAGS@ @GDK_X11_CFLAGS@ @DBUS_CFLAGS@ -DSYSCONFDIR=\"$(sysconfdir)\"
kterm_LDADD = @GTK_LIBS@ @VTE_LIBS@ @GDK_X11_LIBS@ @GTHREAD_LIBS@ @DBUS_LIBS@

EXTRA_DIST = ktsh.h fonts/fonts.conf fonts/README.md tests/ktsh_test.c

AM_CPPFLAGS = -pedantic -Wall -Wextra

kterm_pkg = $(abs_builddir)/kterm-kindle-$(VERSION).zip

dist-kindle: kterm ktsh
	rm -f $(kterm_pkg)
	KTERM_TMPDIR=`mktemp -d` \
	&& mkdir $${KTERM_TMPDIR}/kterm \
	&& cp -RL kindle.pkg/* $${KTERM_TMPDIR}/kterm \
	&& cp kterm ktsh $${KTERM_TMPDIR}/kterm/bin \
	&& cd $${KTERM_TMPDIR} \
	&& sed -i'.bak' 's/@VER@/$(VERSION)/' kterm/config.xml \
	&& rm kterm/config.xml.bak \
	&& zip -qr $(kterm_pkg) kterm \
	&& cd - \
	&& rm -rf $${KTERM_TMPDIR}

distclean-local:
	-rm -f $(kterm_pkg)
