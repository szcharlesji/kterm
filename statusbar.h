/* statusbar.h
 *
 * This file is part of kterm
 *
 * A thin strip above the terminal showing the clock, the date and the
 * battery, plus a button that opens the popup menu so the menu is reachable
 * without a two finger tap.
 *
 * This is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Library General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef statusbar_h
#define statusbar_h

#include <gtk/gtk.h>

/**
 * Build the status bar
 * @param menu Popup menu opened by the bar's button
 * @return The widget, to be packed above the terminal
 */
GtkWidget * statusbar_new(GtkWidget *menu);

/**
 * Refresh the bar now and every STATUSBAR_INTERVAL_S seconds after.
 * Labels are only touched when their text actually changes, because every
 * repaint costs an eink update.
 * @param bar Status bar widget
 */
void statusbar_start(GtkWidget *bar);

/**
 * Stop the refresh timer
 * @param bar Status bar widget
 */
void statusbar_stop(GtkWidget *bar);

#endif /* statusbar_h */
