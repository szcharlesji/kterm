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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "graphics.h"
#include "config.h"

/**
 * Build a child window showing a pixbuf.
 *
 * The picture is installed as the window's background rather than painted
 * in an expose handler. X repaints a window background by itself, so there
 * is nothing to keep in sync and nothing that can be lost to VTE repainting
 * underneath.
 *
 * The window selects no events, so X propagates taps up to the terminal
 * and touch keeps working over an image.
 *
 * @param parent Window to stack the image over
 * @param pixbuf Image to show
 * @param x Position within the parent
 * @param y Position within the parent
 * @return The new window, already mapped
 */
static GdkWindow * overlay_new(GdkWindow *parent, GdkPixbuf *pixbuf, gint x, gint y) {
    GdkWindowAttr attr;
    GdkWindow *win;
    GdkPixmap *pixmap;
    gint w = gdk_pixbuf_get_width(pixbuf);
    gint h = gdk_pixbuf_get_height(pixbuf);

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

    win = gdk_window_new(parent, &attr,
                         GDK_WA_X | GDK_WA_Y | GDK_WA_VISUAL | GDK_WA_COLORMAP);
    if (!win) { return NULL; }

    pixmap = gdk_pixmap_new(GDK_DRAWABLE(win), w, h, -1);
    if (!pixmap) { gdk_window_destroy(win); return NULL; }

    gdk_draw_pixbuf(GDK_DRAWABLE(pixmap), NULL, pixbuf,
                    0, 0, 0, 0, w, h, GDK_RGB_DITHER_NONE, 0, 0);
    gdk_window_set_back_pixmap(win, pixmap, FALSE);
    g_object_unref(pixmap);          /* the window holds its own reference */

    gdk_window_show(win);
    return win;
}

/** A pattern that is obvious on a grayscale panel: ramp, frame and cross */
static GdkPixbuf * test_pattern(gint w, gint h) {
    GdkPixbuf *pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, w, h);
    guchar *pix, *row;
    gint rowstride, nch, x, y;

    if (!pb) { return NULL; }
    pix = gdk_pixbuf_get_pixels(pb);
    rowstride = gdk_pixbuf_get_rowstride(pb);
    nch = gdk_pixbuf_get_n_channels(pb);

    for (y = 0; y < h; y++) {
        row = pix + y * rowstride;
        for (x = 0; x < w; x++) {
            guchar v = (guchar) (x * 255 / (w > 1 ? w - 1 : 1));
            if (x < 2 || y < 2 || x >= w - 2 || y >= h - 2) { v = 0; }
            if (x == y || x == h - y) { v = 255; }
            row[x * nch + 0] = v;
            row[x * nch + 1] = v;
            row[x * nch + 2] = v;
        }
    }
    return pb;
}

void graphics_spike(GtkWidget *terminal) {
    GdkWindow *parent = gtk_widget_get_window(terminal);
    GdkPixbuf *pb;
    GdkWindow *win;

    if (!parent) {
        printf("gfx spike: terminal has no window yet\n");
        return;
    }
    pb = test_pattern(240, 240);
    if (!pb) { return; }

    win = overlay_new(parent, pb, 80, 200);
    g_object_unref(pb);

    printf("gfx spike: overlay %s\n", win ? "created" : "FAILED");
    fflush(stdout);
}
