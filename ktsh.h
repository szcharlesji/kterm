/*
 * ktsh.h - kterm shell shim
 *
 * ktsh sits between kterm's VTE 0.28 widget and the shell, filtering the
 * byte stream in both directions so that modern terminal applications
 * (and modern shells, local or over ssh) behave sanely on a terminal
 * emulator that predates them by fifteen years.
 *
 * The filter is deliberately free of GTK/GLib so it can be built and
 * unit tested on any POSIX host, independent of the Kindle cross build.
 *
 * Copyright (C) 2026 kterm contributors
 * Licensed under the GNU General Public License v3 (see COPYING).
 */

#ifndef KTSH_H
#define KTSH_H

#include <stddef.h>
#include <stdio.h>

/** Growable byte buffer */
typedef struct {
    unsigned char *data;
    size_t len;
    size_t cap;
} KtBuf;

void ktbuf_init(KtBuf *b);
void ktbuf_free(KtBuf *b);
void ktbuf_clear(KtBuf *b);
void ktbuf_add(KtBuf *b, const void *data, size_t n);
void ktbuf_addc(KtBuf *b, unsigned char c);
void ktbuf_consume(KtBuf *b, size_t n);

/** How 24-bit colour is folded down to something VTE 0.28 understands */
typedef enum {
    KT_COLOR_256,   /**< nearest xterm-256 palette index (default) */
    KT_COLOR_GRAY,  /**< luminance mapped onto the 24-step gray ramp */
    KT_COLOR_KEEP   /**< pass through untouched (for debugging) */
} KtColorMode;

/** Parser states, shared by the downstream state machine */
typedef enum {
    KT_GROUND,
    KT_ESC,
    KT_CSI,
    KT_OSC,
    KT_STRING    /**< DCS / APC / PM / SOS - consumed and discarded */
} KtState;

typedef struct {
    /* --- downstream (application -> terminal) parser --- */
    KtState state;
    KtBuf pending;          /**< bytes of the sequence being accumulated */
    unsigned char str_kind; /**< introducer of the active KT_STRING block */
    int esc_seen;           /**< inside OSC/STRING: a bare ESC was buffered */

    /* --- upstream (terminal -> application) parser --- */
    KtBuf in_pending;       /**< partial ESC [ M report held back */

    /* --- negotiated modes, learned from what the application asked for --- */
    int mouse_sgr;          /**< app requested DECSET 1006 */
    int mouse_urxvt;        /**< app requested DECSET 1015 */
    int mouse_utf8;         /**< app requested DECSET 1005 */
    int mouse_any;          /**< app requested 1000/1002/1003 */
    int last_button;        /**< legacy release reports omit the button */

    /* --- configuration --- */
    KtColorMode color_mode;
    char fg_spec[32];       /**< e.g. "rgb:0000/0000/0000" */
    char bg_spec[32];       /**< e.g. "rgb:ffff/ffff/ffff" */
    char *clipboard_path;   /**< where OSC 52 payloads are written, or NULL */
    char *scheme_path;      /**< file kterm records the live scheme in, or NULL */
    FILE *log;              /**< debug log, or NULL */

    /* --- statistics, surfaced by the test runner and -d --- */
    unsigned long n_osc_dropped;
    unsigned long n_color_queries;
    unsigned long n_mouse_rewritten;
    unsigned long n_truecolor;
} KtFilter;

void ktfilter_init(KtFilter *f);
void ktfilter_free(KtFilter *f);

/**
 * Filter application output on its way to the terminal.
 * @param f      filter state
 * @param in     bytes read from the pty master
 * @param n      number of bytes
 * @param out    receives what the terminal should see
 * @param reply  receives bytes owed back to the application
 *               (colour query answers, DCS refusals)
 */
void ktfilter_downstream(KtFilter *f, const unsigned char *in, size_t n,
                         KtBuf *out, KtBuf *reply);

/**
 * Filter terminal input on its way to the application.
 * Rewrites VTE's legacy X10 mouse reports into SGR form when the
 * application asked for SGR and VTE silently declined.
 * @param f    filter state
 * @param in   bytes read from our stdin
 * @param n    number of bytes
 * @param out  receives what the application should see
 */
void ktfilter_upstream(KtFilter *f, const unsigned char *in, size_t n, KtBuf *out);

/**
 * Release any input bytes held back while waiting to see whether they
 * were the start of a mouse report. Call when input goes idle.
 */
void ktfilter_upstream_flush(KtFilter *f, KtBuf *out);

/** True when ktfilter_upstream is holding bytes and needs a flush timeout */
int ktfilter_upstream_pending(const KtFilter *f);

/** Nearest xterm-256 palette index for an 8-bit-per-channel colour */
int ktfilter_rgb_to_256(int r, int g, int b);

#endif /* KTSH_H */
