/* statusbar.c
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
#include <time.h>

#include "statusbar.h"
#include "config.h"

/** Per-bar state, hung off the widget */
typedef struct {
    GtkWidget *clock_label;
    GtkWidget *battery_label;
    GtkWidget *menu;
    guint timer;
    gchar clock_text[64];
    gchar battery_text[32];
} Statusbar;

/**
 * Read battery charge from sysfs.
 * lipc-get-prop would also work but costs a process spawn every tick.
 * @param charging Set true when the battery is charging, may be NULL
 * @return Percentage, or -1 if no battery was found
 */
static gint battery_read(gboolean *charging) {
    static gchar cap_path[PATH_MAX];
    static gchar status_path[PATH_MAX];
    gchar buf[32];
    FILE *fp;
    gint pct = -1;

    if (cap_path[0] == '\0') {
        const gchar *root = "/sys/class/power_supply";
        GDir *dir = g_dir_open(root, 0, NULL);
        const gchar *name;
        if (!dir) { return -1; }
        while ((name = g_dir_read_name(dir)) != NULL) {
            gchar candidate[PATH_MAX];
            snprintf(candidate, sizeof(candidate), "%s/%s/capacity", root, name);
            if (g_file_test(candidate, G_FILE_TEST_EXISTS)) {
                snprintf(cap_path, sizeof(cap_path), "%s", candidate);
                snprintf(status_path, sizeof(status_path), "%s/%s/status", root, name);
                break;
            }
        }
        g_dir_close(dir);
        if (cap_path[0] == '\0') { return -1; }
    }

    if ((fp = fopen(cap_path, "r")) == NULL) { return -1; }
    if (fgets(buf, sizeof(buf), fp)) { pct = atoi(buf); }
    fclose(fp);

    if (charging) {
        *charging = FALSE;
        if ((fp = fopen(status_path, "r")) != NULL) {
            if (fgets(buf, sizeof(buf), fp)) {
                *charging = (strncmp(buf, "Charging", 8) == 0);
            }
            fclose(fp);
        }
    }
    return pct;
}

/**
 * Refresh callback. Only writes a label when its text differs, so an idle
 * terminal does not repaint the strip once a minute for nothing.
 * @param data Statusbar state
 * @return Always true, keep the timer running
 */
static gboolean statusbar_tick(gpointer data) {
    Statusbar *sb = data;
    gchar buf[64];
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    gboolean charging = FALSE;
    gint pct;

    if (tm && strftime(buf, sizeof(buf), "%a %e %b   %H:%M", tm) > 0) {
        if (strcmp(buf, sb->clock_text) != 0) {
            snprintf(sb->clock_text, sizeof(sb->clock_text), "%s", buf);
            gtk_label_set_text(GTK_LABEL(sb->clock_label), buf);
        }
    }

    pct = battery_read(&charging);
    if (pct >= 0) {
        snprintf(buf, sizeof(buf), "%d%%%s", pct, charging ? " +" : "");
    } else {
        buf[0] = '\0';
    }
    if (strcmp(buf, sb->battery_text) != 0) {
        snprintf(sb->battery_text, sizeof(sb->battery_text), "%s", buf);
        gtk_label_set_text(GTK_LABEL(sb->battery_label), buf);
    }
    return TRUE;
}

/**
 * Menu button callback
 * @param widget Calling widget
 * @param data Statusbar state
 */
static void statusbar_menu_clicked(GtkWidget *widget, gpointer data) {
    Statusbar *sb = data;
    UNUSED(widget);
    if (sb->menu) {
        gtk_menu_popup(GTK_MENU(sb->menu), NULL, NULL, NULL, NULL, 0,
                       gtk_get_current_event_time());
    }
}

/**
 * Free state when the widget goes away
 * @param data Statusbar state
 */
static void statusbar_destroy(gpointer data) {
    Statusbar *sb = data;
    if (sb->timer) { g_source_remove(sb->timer); }
    g_free(sb);
}

/**
 * Physical size in pixels, so the menu button stays tappable regardless of
 * panel density. At 300 dpi a default-sized button is about 3 mm across,
 * which is well under a fingertip.
 * @param mm Desired size in millimetres
 * @return Size in pixels
 */
static gint mm_to_px(gdouble mm) {
    gdouble dpi = gdk_screen_get_resolution(gdk_screen_get_default());
    if (dpi <= 0) { dpi = 96; }
    return (gint) (mm * MM_TO_IN * dpi);
}

/** Match the terminal's face so the strip does not look bolted on */
static void statusbar_set_font(GtkWidget *widget) {
    gchar font_name[128];
    PangoFontDescription *desc;
    snprintf(font_name, sizeof(font_name), "%s %u",
             conf->font_family, conf->font_size + STATUSBAR_FONT_BUMP);
    desc = pango_font_description_from_string(font_name);
#if GTK_CHECK_VERSION(3,0,0)
    gtk_widget_override_font(widget, desc);
#else
    gtk_widget_modify_font(widget, desc);
#endif
    pango_font_description_free(desc);
}

GtkWidget * statusbar_new(GtkWidget *menu) {
    Statusbar *sb = g_malloc0(sizeof(Statusbar));
    GtkWidget *bar;
    GtkWidget *button;

#if GTK_CHECK_VERSION(3,2,0)
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    bar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
#else
    GtkWidget *box = gtk_hbox_new(FALSE, 0);
    bar = gtk_vbox_new(FALSE, 0);
#endif
    gtk_widget_set_name(bar, "statusBox");

    sb->menu = menu;
    sb->clock_label = gtk_label_new("");
    sb->battery_label = gtk_label_new("");
    button = gtk_button_new_with_label("\xe2\x98\xb0");  /* U+2630 trigram, reads as a menu glyph */
    gtk_widget_set_name(button, "ktermKbButton");        /* reuse the keyboard button style */
    gtk_button_set_focus_on_click(GTK_BUTTON(button), FALSE);
    gtk_widget_set_can_focus(button, FALSE);
    gtk_widget_set_size_request(button, mm_to_px(STATUSBAR_BUTTON_MM),
                                        mm_to_px(STATUSBAR_HEIGHT_MM));

    statusbar_set_font(sb->clock_label);
    statusbar_set_font(sb->battery_label);
    statusbar_set_font(button);

    gtk_misc_set_alignment(GTK_MISC(sb->clock_label), 0.0, 0.5);
    gtk_misc_set_alignment(GTK_MISC(sb->battery_label), 1.0, 0.5);
    gtk_misc_set_padding(GTK_MISC(sb->clock_label), STATUSBAR_PAD, 0);
    gtk_misc_set_padding(GTK_MISC(sb->battery_label), STATUSBAR_PAD, 0);

    gtk_box_pack_start(GTK_BOX(box), sb->clock_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), sb->battery_label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(bar), box, FALSE, FALSE, 0);
#if GTK_CHECK_VERSION(3,2,0)
    gtk_box_pack_start(GTK_BOX(bar),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);
#else
    gtk_box_pack_start(GTK_BOX(bar), gtk_hseparator_new(), FALSE, FALSE, 0);
#endif

    g_signal_connect(button, "clicked", G_CALLBACK(statusbar_menu_clicked), sb);
    g_object_set_data_full(G_OBJECT(bar), "statusbar", sb, statusbar_destroy);

    return bar;
}

void statusbar_start(GtkWidget *bar) {
    Statusbar *sb = g_object_get_data(G_OBJECT(bar), "statusbar");
    if (!sb || sb->timer) { return; }
    statusbar_tick(sb);
    sb->timer = g_timeout_add_seconds(STATUSBAR_INTERVAL_S, statusbar_tick, sb);
}

void statusbar_stop(GtkWidget *bar) {
    Statusbar *sb = g_object_get_data(G_OBJECT(bar), "statusbar");
    if (!sb || !sb->timer) { return; }
    g_source_remove(sb->timer);
    sb->timer = 0;
}
