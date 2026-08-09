/* graphics.c
 *
 * This file is part of kterm
 *
 * Copyright(C) 2026 kterm contributors
 *
 * This is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Library General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <vte/vte.h>

#include "graphics.h"
#include "config.h"

/** Levels the panel can actually show; anything finer is wasted effort */
#define GFX_GREYS 16

/** Refuse to keep more than this many images alive at once */
#define GFX_MAX_IMAGES 32

typedef struct {
    glong id;
    GdkPixbuf *pixbuf;   /**< as received, unscaled */
    GdkWindow *win;      /**< the overlay, NULL until placed */
    gdouble abs_row;     /**< row in scrollback coordinates */
    gint col;            /**< zero based column */
    gint cols, rows;     /**< cell box the application asked for */
} GfxImage;

static struct {
    GtkWidget *terminal;
    int listen_fd;
    int client_fd;
    guint listen_watch;
    guint client_watch;
    gchar path[128];
    GList *images;       /**< GfxImage*, newest first */
    GByteArray *in;      /**< bytes from ktsh not yet parsed */
    gsize want;          /**< payload bytes still expected, 0 when at a header */
    /* header fields of the transfer in progress */
    glong h_id;
    gint h_fmt, h_w, h_h;
} gfx;

/* ------------------------------------------------------------------ */
/* image list                                                         */
/* ------------------------------------------------------------------ */

static void image_free(GfxImage *im) {
    if (im->win) { gdk_window_destroy(im->win); }
    if (im->pixbuf) { g_object_unref(im->pixbuf); }
    g_free(im);
}

static GfxImage * image_find(glong id) {
    GList *l;
    for (l = gfx.images; l; l = l->next) {
        GfxImage *im = l->data;
        if (im->id == id) { return im; }
    }
    return NULL;
}

static void image_drop(glong id) {
    GfxImage *im = image_find(id);
    if (!im) { return; }
    gfx.images = g_list_remove(gfx.images, im);
    image_free(im);
}

void graphics_clear(void) {
    g_list_foreach(gfx.images, (GFunc) image_free, NULL);
    g_list_free(gfx.images);
    gfx.images = NULL;
}

/* ------------------------------------------------------------------ */
/* pixels                                                             */
/* ------------------------------------------------------------------ */

/**
 * Floyd-Steinberg to the panel's grey levels.
 *
 * Nearest-level rounding on a 16 level display turns any gradient into
 * bands, which on a photograph looks far worse than the noise dithering
 * introduces. Done in place on a packed RGB pixbuf.
 */
static void dither_grey(GdkPixbuf *pb) {
    gint w = gdk_pixbuf_get_width(pb);
    gint h = gdk_pixbuf_get_height(pb);
    gint stride = gdk_pixbuf_get_rowstride(pb);
    gint nch = gdk_pixbuf_get_n_channels(pb);
    guchar *pix = gdk_pixbuf_get_pixels(pb);
    gfloat *err;
    gint x, y;

    if (w <= 0 || h <= 0) { return; }
    err = g_new0(gfloat, (gsize) (w + 2) * 2);

    for (y = 0; y < h; y++) {
        gfloat *cur = err + (y & 1 ? (w + 2) : 0);
        gfloat *nxt = err + (y & 1 ? 0 : (w + 2));
        memset(nxt, 0, sizeof(gfloat) * (gsize) (w + 2));

        for (x = 0; x < w; x++) {
            guchar *p = pix + y * stride + x * nch;
            /* Rec.601 luma: the panel has no colour to preserve */
            gfloat lum = 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
            gfloat want = lum + cur[x + 1];
            gint step, q;
            gfloat e;

            if (want < 0.0f) { want = 0.0f; }
            if (want > 255.0f) { want = 255.0f; }
            step = (gint) ((want * (GFX_GREYS - 1) / 255.0f) + 0.5f);
            q = step * 255 / (GFX_GREYS - 1);
            e = want - (gfloat) q;

            p[0] = p[1] = p[2] = (guchar) q;

            cur[x + 2] += e * 7.0f / 16.0f;
            nxt[x]     += e * 3.0f / 16.0f;
            nxt[x + 1] += e * 5.0f / 16.0f;
            nxt[x + 2] += e * 1.0f / 16.0f;
        }
    }
    g_free(err);
}

/**
 * Build the overlay window for an image, sized to its cell box.
 *
 * The picture becomes the window's background rather than something we
 * paint in an expose handler, so X keeps it on screen by itself and VTE's
 * own drawing is clipped against it. The window selects no events, so
 * taps fall through to the terminal underneath.
 */
static void image_realize(GfxImage *im) {
    GdkWindow *parent;
    GdkWindowAttr attr;
    GdkPixbuf *scaled;
    GdkPixmap *pixmap;
    gint cw, ch, w, h, x, y;
    gdouble top;

    if (!gfx.terminal) { return; }
    parent = gtk_widget_get_window(gfx.terminal);
    if (!parent || !im->pixbuf) { return; }

    cw = vte_terminal_get_char_width(VTE_TERMINAL(gfx.terminal));
    ch = vte_terminal_get_char_height(VTE_TERMINAL(gfx.terminal));
    if (cw <= 0 || ch <= 0) { return; }

    w = (im->cols > 0) ? im->cols * cw : gdk_pixbuf_get_width(im->pixbuf);
    h = (im->rows > 0) ? im->rows * ch : gdk_pixbuf_get_height(im->pixbuf);
    if (w <= 0 || h <= 0) { return; }

    top = gtk_adjustment_get_value(vte_terminal_get_adjustment(VTE_TERMINAL(gfx.terminal)));
    x = im->col * cw;
    y = (gint) ((im->abs_row - top) * ch);

    if (im->win) { gdk_window_destroy(im->win); im->win = NULL; }

    scaled = gdk_pixbuf_scale_simple(im->pixbuf, w, h, GDK_INTERP_BILINEAR);
    if (!scaled) { return; }
    dither_grey(scaled);

    memset(&attr, 0, sizeof(attr));
    attr.window_type = GDK_WINDOW_CHILD;
    attr.x = x;
    attr.y = y;
    attr.width = w;
    attr.height = h;
    attr.wclass = GDK_INPUT_OUTPUT;
    attr.visual = gdk_drawable_get_visual(GDK_DRAWABLE(parent));
    attr.colormap = gdk_drawable_get_colormap(GDK_DRAWABLE(parent));
    attr.event_mask = 0;

    im->win = gdk_window_new(parent, &attr,
                             GDK_WA_X | GDK_WA_Y | GDK_WA_VISUAL | GDK_WA_COLORMAP);
    if (!im->win) { g_object_unref(scaled); return; }

    pixmap = gdk_pixmap_new(GDK_DRAWABLE(im->win), w, h, -1);
    if (pixmap) {
        gdk_draw_pixbuf(GDK_DRAWABLE(pixmap), NULL, scaled,
                        0, 0, 0, 0, w, h, GDK_RGB_DITHER_NONE, 0, 0);
        gdk_window_set_back_pixmap(im->win, pixmap, FALSE);
        g_object_unref(pixmap);
    }
    g_object_unref(scaled);

    gdk_window_show(im->win);
}

/** Move every placed image to match the current scroll position */
static void images_reposition(void) {
    GtkAllocation alloc;
    GtkAdjustment *adj;
    GList *l;
    gint cw, ch;
    gdouble top;

    if (!gfx.terminal || !gfx.images) { return; }
    if (!gtk_widget_get_window(gfx.terminal)) { return; }

    adj = vte_terminal_get_adjustment(VTE_TERMINAL(gfx.terminal));
    cw = vte_terminal_get_char_width(VTE_TERMINAL(gfx.terminal));
    ch = vte_terminal_get_char_height(VTE_TERMINAL(gfx.terminal));
    if (!adj || cw <= 0 || ch <= 0) { return; }
    top = gtk_adjustment_get_value(adj);
    gtk_widget_get_allocation(gfx.terminal, &alloc);

    for (l = gfx.images; l; l = l->next) {
        GfxImage *im = l->data;
        gint y;
        if (!im->win) { continue; }
        y = (gint) ((im->abs_row - top) * ch);
        gdk_window_move(im->win, im->col * cw, y);
        /* An image scrolled clear of the viewport is hidden rather than
         * destroyed, so scrolling back brings it into view again. */
        if (y > alloc.height || y + im->rows * ch < 0) { gdk_window_hide(im->win); }
        else { gdk_window_show(im->win); }
    }
}

void graphics_refresh(void) {
    GList *l;
    for (l = gfx.images; l; l = l->next) { image_realize(l->data); }
}

/* ------------------------------------------------------------------ */
/* protocol from ktsh                                                 */
/* ------------------------------------------------------------------ */

static void handle_img(glong id, gint fmt, gint w, gint h,
                       const guchar *data, gsize len) {
    GdkPixbuf *pb = NULL;
    GfxImage *im;

    if (fmt == 100 || fmt == 0) {
        /* PNG and friends; let gdk-pixbuf sniff the actual format */
        GdkPixbufLoader *ld = gdk_pixbuf_loader_new();
        GError *err = NULL;
        if (gdk_pixbuf_loader_write(ld, data, len, &err)
            && gdk_pixbuf_loader_close(ld, &err)) {
            pb = gdk_pixbuf_loader_get_pixbuf(ld);
            if (pb) { g_object_ref(pb); }
        } else {
            D printf("gfx: decode failed: %s\n", err ? err->message : "?");
            if (err) { g_error_free(err); }
            gdk_pixbuf_loader_close(ld, NULL);
        }
        g_object_unref(ld);
    } else if ((fmt == 24 || fmt == 32) && w > 0 && h > 0) {
        gint nch = (fmt == 32) ? 4 : 3;
        gsize need = (gsize) w * h * nch;
        if (len >= need) {
            guchar *copy = g_memdup(data, (guint) need);
            pb = gdk_pixbuf_new_from_data(copy, GDK_COLORSPACE_RGB, fmt == 32,
                                          8, w, h, w * nch,
                                          (GdkPixbufDestroyNotify) g_free, NULL);
        }
    }

    if (!pb) { D printf("gfx: image %ld not decodable (fmt %d)\n", id, fmt); return; }

    image_drop(id);
    im = g_new0(GfxImage, 1);
    im->id = id;
    im->pixbuf = pb;
    gfx.images = g_list_prepend(gfx.images, im);

    while (g_list_length(gfx.images) > GFX_MAX_IMAGES) {
        GList *last = g_list_last(gfx.images);
        image_free(last->data);
        gfx.images = g_list_delete_link(gfx.images, last);
    }
}

static void handle_place(glong id, gint cols, gint rows, gint row, gint col) {
    GfxImage *im = image_find(id);
    GtkAdjustment *adj;

    if (!im || !gfx.terminal) { return; }
    adj = vte_terminal_get_adjustment(VTE_TERMINAL(gfx.terminal));
    im->cols = cols;
    im->rows = rows;
    im->col = (col > 0) ? col - 1 : 0;               /* the report is 1 based */
    im->abs_row = (adj ? gtk_adjustment_get_value(adj) : 0) + ((row > 0) ? row - 1 : 0);
    image_realize(im);
}

/** Consume as many complete records as the buffer holds */
static void parse_records(void) {
    for (;;) {
        if (gfx.want > 0) {
            if (gfx.in->len < gfx.want) { return; }
            handle_img(gfx.h_id, gfx.h_fmt, gfx.h_w, gfx.h_h, gfx.in->data, gfx.want);
            g_byte_array_remove_range(gfx.in, 0, (guint) gfx.want);
            gfx.want = 0;
            continue;
        }
        {
            guint8 *nl = memchr(gfx.in->data, '\n', gfx.in->len);
            gchar line[256];
            gsize linelen;
            glong id = 0;
            gint a = 0, b = 0, c = 0, d = 0;
            unsigned long len = 0;

            if (!nl) {
                /* a header this long is not a header; drop the garbage */
                if (gfx.in->len > sizeof(line)) { g_byte_array_set_size(gfx.in, 0); }
                return;
            }
            linelen = (gsize) (nl - gfx.in->data);
            if (linelen >= sizeof(line)) { linelen = sizeof(line) - 1; }
            memcpy(line, gfx.in->data, linelen);
            line[linelen] = '\0';
            g_byte_array_remove_range(gfx.in, 0, (guint) (linelen + 1));

            if (sscanf(line, "IMG %ld %d %d %d %lu", &id, &a, &b, &c, &len) == 5) {
                gfx.h_id = id; gfx.h_fmt = a; gfx.h_w = b; gfx.h_h = c;
                gfx.want = len;
                continue;
            }
            if (sscanf(line, "PLACE %ld %d %d %d %d", &id, &a, &b, &c, &d) == 5) {
                handle_place(id, a, b, c, d);
                continue;
            }
            if (sscanf(line, "DEL %ld", &id) == 1) { image_drop(id); continue; }
            if (strcmp(line, "DELALL") == 0) { graphics_clear(); continue; }
            D printf("gfx: unknown record '%s'\n", line);
        }
    }
}

static gboolean client_readable(GIOChannel *chan, GIOCondition cond, gpointer data) {
    guchar buf[8192];
    gsize got = 0;
    GIOStatus st;

    UNUSED(data);
    if (cond & (G_IO_HUP | G_IO_ERR)) {
        gfx.client_watch = 0;
        gfx.client_fd = -1;
        g_io_channel_unref(chan);
        return FALSE;
    }

    st = g_io_channel_read_chars(chan, (gchar *) buf, sizeof(buf), &got, NULL);
    if (st == G_IO_STATUS_ERROR || (st == G_IO_STATUS_EOF && got == 0)) {
        gfx.client_watch = 0;
        gfx.client_fd = -1;
        g_io_channel_unref(chan);
        return FALSE;
    }
    if (got) {
        g_byte_array_append(gfx.in, buf, (guint) got);
        parse_records();
    }
    return TRUE;
}

static gboolean listen_readable(GIOChannel *chan, GIOCondition cond, gpointer data) {
    int fd;
    GIOChannel *cc;

    UNUSED(chan); UNUSED(cond); UNUSED(data);
    fd = accept(gfx.listen_fd, NULL, NULL);
    if (fd < 0) { return TRUE; }
    if (gfx.client_fd >= 0) { close(fd); return TRUE; }   /* one shim is enough */

    gfx.client_fd = fd;
    cc = g_io_channel_unix_new(fd);
    g_io_channel_set_encoding(cc, NULL, NULL);            /* binary */
    g_io_channel_set_buffered(cc, FALSE);
    g_io_channel_set_close_on_unref(cc, TRUE);
    gfx.client_watch = g_io_add_watch(cc, G_IO_IN | G_IO_HUP | G_IO_ERR,
                                      client_readable, NULL);
    D printf("gfx: shim connected\n");
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                          */
/* ------------------------------------------------------------------ */

gboolean graphics_init(gchar *path_out, gsize path_len) {
    struct sockaddr_un sa;
    GIOChannel *chan;

    gfx.listen_fd = -1;
    gfx.client_fd = -1;
    gfx.in = g_byte_array_new();

    snprintf(gfx.path, sizeof(gfx.path), "/tmp/kterm-gfx-%d", (int) getpid());
    unlink(gfx.path);

    gfx.listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (gfx.listen_fd < 0) { return FALSE; }

    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", gfx.path);
    if (bind(gfx.listen_fd, (struct sockaddr *) &sa, sizeof(sa)) != 0
        || listen(gfx.listen_fd, 1) != 0) {
        D printf("gfx: cannot listen on %s: %s\n", gfx.path, strerror(errno));
        close(gfx.listen_fd);
        gfx.listen_fd = -1;
        return FALSE;
    }

    chan = g_io_channel_unix_new(gfx.listen_fd);
    gfx.listen_watch = g_io_add_watch(chan, G_IO_IN, listen_readable, NULL);
    g_io_channel_unref(chan);

    snprintf(path_out, path_len, "%s", gfx.path);
    return TRUE;
}

/** Images are anchored to scrollback rows, so scrolling only moves them */
static void on_scroll(GtkAdjustment *adj, gpointer data) {
    UNUSED(adj); UNUSED(data);
    images_reposition();
}

void graphics_attach(GtkWidget *terminal) {
    GtkAdjustment *adj;

    gfx.terminal = terminal;
    adj = vte_terminal_get_adjustment(VTE_TERMINAL(terminal));
    if (adj) { g_signal_connect(adj, "value-changed", G_CALLBACK(on_scroll), NULL); }
}

void graphics_shutdown(void) {
    graphics_clear();
    if (gfx.client_watch) { g_source_remove(gfx.client_watch); gfx.client_watch = 0; }
    if (gfx.listen_watch) { g_source_remove(gfx.listen_watch); gfx.listen_watch = 0; }
    if (gfx.listen_fd >= 0) { close(gfx.listen_fd); gfx.listen_fd = -1; }
    if (gfx.path[0]) { unlink(gfx.path); }
    if (gfx.in) { g_byte_array_free(gfx.in, TRUE); gfx.in = NULL; }
}
