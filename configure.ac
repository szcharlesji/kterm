AC_INIT([kterm], [2.7.0])
AM_INIT_AUTOMAKE([foreign])
AC_PROG_CC
m4_version_prereq([2.70], [], [AC_PROG_CC_C99])
AM_PROG_CC_C_O
AC_PROG_INSTALL
AC_CONFIG_FILES([Makefile])
AC_CONFIG_MACRO_DIR([m4])

AC_ARG_ENABLE(
  [kindle],
  [AS_HELP_STRING([--enable-kindle], [compile for Kindle [default=no]])],
  [enable_kindle=$enableval], [enable_kindle=no])

AM_CONDITIONAL([KINDLE], [test "x$enable_kindle" = "xyes"])

AC_ARG_ENABLE(
  [gtk3],
  [AS_HELP_STRING([--enable-gtk3], [compile with GTK+ version 3 [default=no]])],
  [enable_gtk3=$enableval], [enable_gtk3=no])

AM_CONDITIONAL([GTK3], [test "x$enable_gtk3" = "xyes"])

AM_COND_IF(
  [GTK3],
  [gtk_package=gtk+-3.0],
  [gtk_package=gtk+-2.0])

PKG_CHECK_MODULES([GTK], [$gtk_package])

dnl GDK_X11_LIBS is referenced by Makefile.am but nothing ever set it. It used
dnl to not matter, because the X libraries arrived indirectly through libgdk
dnl or libvte. Binutils stopped resolving symbols through an intermediate DSO
dnl (--no-copy-dt-needed-entries is the default now), so libgdk-x11's calls to
dnl XGetGeometry and friends have to be satisfied explicitly. Not fatal: a
dnl build whose toolkit brings its own X libraries can still succeed.
PKG_CHECK_MODULES([GDK_X11], [x11 xext], [],
  [AC_MSG_WARN([x11/xext not found via pkg-config, relying on indirect linkage])])

dnl A statically linked libvte pulls in round() but pkg-config does not list
dnl libm, so the link fails unless we ask for it explicitly. Checked here,
dnl before the Kindle block, because the static-link probe below needs it.
AC_SEARCH_LIBS([round], [m])

dnl glib merged gthread into libglib in 2.32 and gtk+-2.0.pc stopped listing
dnl it. Kindle firmware still runs glib 2.29, where gthread is a separate
dnl library that g_type_init() calls into during gtk_init(). Building against
dnl a modern .pc therefore produces a binary that segfaults inside
dnl gtk_init -> gdk_pre_parse_libgtk_only -> g_type_init -> g_thread_init_glib,
dnl because the thread vtable was never populated. Link it when it exists;
dnl on glib >= 2.32 this is a harmless no-op stub.
AC_CHECK_LIB([gthread-2.0], [g_thread_init],
  [GTHREAD_LIBS="-lgthread-2.0"],
  [AC_MSG_WARN([libgthread not found, assuming glib >= 2.32])],
  [$GTK_LIBS])
AC_SUBST([GTHREAD_LIBS])

AM_COND_IF(
  [KINDLE],
  [dnl If Kindle
  AC_MSG_CHECKING([whether compiler supports ARM target])
  AC_COMPILE_IFELSE(
    [AC_LANG_PROGRAM([], [return __arm__])],
    [AC_MSG_RESULT([yes])],
    [AC_MSG_RESULT([no])
     AC_MSG_WARN([building for Kindle without ARM toolchain])])

  dnl Libtermcap is needed for static builds with vte < 2.91
  require_termcap=yes
  AC_DEFINE([KINDLE], [1])

  dnl Modern binutils turns on RELRO by default. A non-PIE executable that
  dnl references libglib's exported variables needs copy relocations for them,
  dnl and the linker then parks g_threads_got_initialized in .data.rel.ro,
  dnl which the loader makes read-only before libgthread has run. glib 2.29,
  dnl which is what Kindle firmware ships, still keeps threads in a separate
  dnl library, so gtk_init() -> g_type_init() -> g_thread_init_glib() writes
  dnl that flag and the process dies on a read-only page. Kindle's own
  dnl binaries carry no RELRO segment; match them.
  AC_MSG_CHECKING([whether the linker accepts -z norelro])
  saved_LDFLAGS=$LDFLAGS
  LDFLAGS="$LDFLAGS -Wl,-z,norelro"
  AC_LINK_IFELSE(
    [AC_LANG_PROGRAM([], [])],
    [AC_MSG_RESULT([yes])],
    [AC_MSG_RESULT([no])
     LDFLAGS=$saved_LDFLAGS])

  saved_PKG_CONFIG=$PKG_CONFIG
  PKG_CONFIG="$PKG_CONFIG --static"
  AM_COND_IF(
    [GTK3],
    [PKG_CHECK_MODULES([VTE], [vte-2.91], [require_termcap=no], [PKG_CHECK_MODULES([VTE], [vte-2.90])])],
    [PKG_CHECK_MODULES([VTE], [vte])])
  PKG_CONFIG=$saved_PKG_CONFIG

  if test "x$require_termcap" = "xyes"; then
    saved_LIBS=$LIBS
    LIBS=
    AC_SEARCH_LIBS(
      [tgetent],
      [curses ncurses readline termcap],
      [TERMCAP_LIBS=$LIBS],
      [AC_MSG_WARN([libtermcap is needed for static build])])
    LIBS=$saved_LIBS
  else
    TERMCAP_LIBS=
  fi

  AC_MSG_CHECKING([whether libvte may be linked statically])
  dnl This will only work with GNU linker
  [VTE_STATIC_LIBS=`echo "-Wl,--as-needed $VTE_LIBS -Wl,--no-as-needed" | sed -e "s/\(-lvte[-_.0-9]*\)/-Wl,-Bstatic \1 -Wl,-Bdynamic $TERMCAP_LIBS/g"`]
  saved_CFLAGS=$CFLAGS
  saved_LIBS=$LIBS
  CFLAGS="$GTK_CFLAGS $VTE_CFLAGS"
  dnl Probe with everything the real link line gets, or this reports failure
  dnl for an unrelated reason and silently falls back to a dynamic libvte.
  LIBS="$GTK_LIBS $VTE_STATIC_LIBS $GDK_X11_LIBS $LIBS"
  AC_LINK_IFELSE(
    [AC_LANG_PROGRAM([[#include <vte/vte.h>]], [[vte_terminal_new();]])],
    [AC_MSG_RESULT([yes])
     VTE_LIBS=$VTE_STATIC_LIBS],
    [AC_MSG_RESULT([no])
     AC_MSG_WARN([building for Kindle with non-static libvte])])
  LIBS=$saved_LIBS
  CFLAGS=$saved_CFLAGS

  PKG_CHECK_MODULES([DBUS], [dbus-1], [AC_DEFINE([HAVE_DBUS], [1])], [AC_MSG_WARN([screen rotation on Kindle will be disabled without dbus])])],

  [dnl If not Kindle
  AM_COND_IF(
    [GTK3],
    [PKG_CHECK_MODULES([VTE], [vte-2.91], [], [PKG_CHECK_MODULES([VTE], [vte-2.90])])],
    [PKG_CHECK_MODULES([VTE], [vte])])])

AC_SUBST([GTK_CFLAGS])
AC_SUBST([GTK_LIBS])
AC_SUBST([VTE_CFLAGS])
AC_SUBST([VTE_LIBS])
AC_SUBST([GDK_X11_CFLAGS])
AC_SUBST([GDK_X11_LIBS])
AC_SUBST([DBUS_LIBS])
AC_SUBST([DBUS_CFLAGS])

AC_OUTPUT
