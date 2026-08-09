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
 * This is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Library General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef graphics_h
#define graphics_h

#include <gtk/gtk.h>

/**
 * Draw a test pattern over the terminal to prove the overlay survives
 * VTE's own painting. Enabled with KTERM_GFX_SPIKE=1.
 * @param terminal Realized terminal widget
 */
void graphics_spike(GtkWidget *terminal);

#endif /* graphics_h */
