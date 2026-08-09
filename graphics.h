/* graphics.h
 *
 * This file is part of kterm
 *
 * Inline images for a terminal widget that has no idea they exist.
 *
 * VTE 0.28 cannot draw images and cannot be made to. Instead each image
 * gets its own child GdkWindow stacked over the terminal, with the picture
 * installed as that window's background pixmap. X then repaints it for us
 * and clips VTE's own drawing against it, so the two never fight over the
 * same pixels. This matters because vte_terminal_expose() often does not
 * paint during the expose at all - when an update timeout is pending it
 * only queues regions and paints later - so anything drawn from an
 * expose-event handler would be painted over moments afterwards.
 *
 * ktsh does the protocol work and sends us decoded images over a unix
 * socket; see ktfilter.c. We only decode, scale, dither and place.
 *
 * This is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Library General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef graphics_h
#define graphics_h

#include <gtk/gtk.h>

/**
 * Open the socket ktsh will send images on and start listening.
 * Must be called before the child is spawned, because the path has to go
 * into its environment.
 * @param path_out Receives the socket path
 * @param path_len Size of path_out
 * @return TRUE if the socket is listening
 */
gboolean graphics_init(gchar *path_out, gsize path_len);

/**
 * Bind the image layer to a realized terminal widget. Until this is
 * called images are stored but not drawn.
 * @param terminal Realized terminal widget
 */
void graphics_attach(GtkWidget *terminal);

/** Drop every image, e.g. on reset. */
void graphics_clear(void);

/** Re-scale and re-place everything, after a font size or rotation change. */
void graphics_refresh(void);

/** Close the socket and remove it from the filesystem. */
void graphics_shutdown(void);

#endif /* graphics_h */
