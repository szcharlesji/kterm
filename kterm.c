/* kterm.c
 *
 * This file is part of kterm
 *
 * Copyright(C) 2013-16 Bartek Fabiszewski (www.fabiszewski.net)
 *
 * This is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Library General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <gtk/gtk.h>
#include <vte/vte.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <getopt.h>
#include "keyboard.h"
#include "statusbar.h"
#ifdef KINDLE
#include "kindle.h"
#endif
#include "config.h"

/** Vte version check for early versions */
#ifndef VTE_CHECK_VERSION
#define VTE_CHECK_VERSION(x,y,z) FALSE
#endif

/** Global config */
KTconf *conf;
/** Global debug */
gboolean debug = FALSE;

/**
 * Signals handler
 * @param signo Signal number
 */
static void exit_on_signal(gint signo) {
    D printf("exiting on signal %i\n", signo);
    if (gtk_main_level()) {
        gtk_main_quit();
    } else {
        exit(0);
    }
}

/**
 * Install signal handlers
 */
static void install_signal_handlers(void) {
    signal(SIGCHLD, SIG_IGN); // kernel should handle zombies
    signal(SIGINT, exit_on_signal);
    signal(SIGQUIT, exit_on_signal);
    signal(SIGTERM, exit_on_signal);
}

/**
 * Free all resources
 * @param keyboard Keyboard structure
 */
static void clean_on_exit(Keyboard *keyboard) {
    D printf("cleanup\n");
#ifdef KINDLE
    keyboard_grab(NULL, FALSE);
    orientation_restore();
#endif
    keyboard_free(&keyboard);
    g_free(conf);
}

/**
 * Grab focus on callback
 * @param widget Calling widget
 * @param data User data
 */
static void grab_focus(GtkWidget *widget, gpointer data) {
    UNUSED(data);
    D printf("grab focus: %s\n", gtk_widget_get_name(widget)) ;
    gtk_widget_grab_focus(widget);
}

/**
 * Terminal exit handler
 */
static void terminal_exit(void) {
    sleep(1); // time for kb to send key up event
    gtk_main_quit();
}

#if VTE_CHECK_VERSION(0,20,0)
/**
 * Set terminal cursor shape
 * @param terminal Terminal
 * @param cursor_shape Letter representing desired shape ('B', 'I' or 'U')
 */
static void set_terminal_cursor(VteTerminal *terminal, gchar cursor_shape) {
#if VTE_CHECK_VERSION(0,38,0)
    VteCursorShape shape = 0;
#else
    VteTerminalCursorShape shape = 0;
#endif
    switch (cursor_shape) {
        case 'B':
            shape = VTE_CURSOR_SHAPE_BLOCK;
            break;
        case 'I':
            shape = VTE_CURSOR_SHAPE_IBEAM;
            break;
        case 'U':
            shape = VTE_CURSOR_SHAPE_UNDERLINE;
            break;
    }
    vte_terminal_set_cursor_shape(terminal, shape);
}
#endif

/**
 * Set terminal font
 * @param terminal Terminal
 * @param font_family Font family
 * @param font_size Font size
 */
static void set_terminal_font(VteTerminal *terminal, const gchar *font_family, const gint font_size) {
    gchar font_name[200];
    snprintf(font_name, sizeof(font_name), "%s %i", font_family, font_size);
    D printf("font_name: %s\n", font_name);
    PangoFontDescription *desc = pango_font_description_from_string(font_name);
    vte_terminal_set_font(VTE_TERMINAL(terminal), desc);
    pango_font_description_free(desc);
}

/**
 * Resize terminal font
 * @param terminal Terminal
 * @param mod FONT_UP or FONT_DOWN
 */
static void resize_font(VteTerminal *terminal, const guint mod) {
    const PangoFontDescription *pango_desc = vte_terminal_get_font(VTE_TERMINAL(terminal));
    gint font_size = 0;
    if (pango_desc) {
        // ask pango for the size rather than splitting the description string:
        // family names ending in a digit ("JetBrainsMono Nerd Font Mono 3270")
        // make the last-space heuristic pick up the wrong token
        font_size = pango_font_description_get_size(pango_desc) / PANGO_SCALE;
    }
    if (font_size <= 0) { font_size = (gint) conf->font_size; }
    if (mod == FONT_UP) {
        font_size++;
    }
    else if (font_size > 1) {
        font_size--;
    }
    D printf("font_family: %s\n", conf->font_family);
    D printf("font_size: %i\n", font_size);
    conf->font_size = (guint) font_size;
    set_terminal_font(terminal, conf->font_family, font_size);
}

/**
 * Increase font size menu callback
 * @param widget Calling widget
 * @param terminal Terminal
 */
static void fontup(GtkWidget *widget, gpointer terminal) {
    UNUSED(widget);
    resize_font(terminal, FONT_UP);
}

/**
 * Decrease font size menu callback
 * @param widget Calling widget
 * @param terminal Terminal
 */
static void fontdown(GtkWidget *widget, gpointer terminal) {
    UNUSED(widget);
    resize_font(terminal, FONT_DOWN);
}
/**
 * Path of a file living next to the kterm binary
 * @param name File name
 * @param buf Buffer receiving the path
 * @param len Buffer size
 * @return True on success
 */
static gboolean sibling_path(const gchar *name, gchar *buf, gsize len) {
    gchar dir[PATH_MAX];
    gsize dlen, nlen;
    if (!kterm_exe_dir(dir, sizeof(dir))) { return FALSE; }
    dlen = strlen(dir);
    nlen = strlen(name);
    if (dlen + nlen + 2 > len) { return FALSE; }
    memcpy(buf, dir, dlen);
    buf[dlen] = '/';
    memcpy(buf + dlen + 1, name, nlen + 1);
    return TRUE;
}

/**
 * Is this an executable regular file?
 * Deliberately stat() rather than access(X_OK): on Kindle the extension lives
 * on a FUSE volume (fuse.fsp) mounted without default_permissions, where the
 * access() permission check does not reflect the mode bits and reports
 * perfectly runnable binaries as not executable.
 * @param path File path
 * @return True if it looks runnable
 */
static gboolean is_executable(const gchar *path) {
    struct stat st;
    if (stat(path, &st) != 0) { return FALSE; }
    if (!S_ISREG(st.st_mode)) { return FALSE; }
    return (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0;
}

/**
 * Record the active color scheme where the shim can find it.
 * VTE 0.28 cannot answer an OSC 11 background color query itself, so ktsh
 * answers on its behalf and needs to know what kterm actually painted.
 * @param scheme VTE_SCHEME_LIGHT or VTE_SCHEME_DARK
 */
static void write_scheme_file(gboolean scheme) {
    gchar path[PATH_MAX];
    FILE *fp;
    if (!sibling_path(SCHEME_FILE, path, sizeof(path))) { return; }
    if ((fp = fopen(path, "w")) == NULL) {
        D printf("cannot write scheme file %s\n", path);
        return;
    }
    fprintf(fp, "%s\n", (scheme == VTE_SCHEME_DARK) ? "dark" : "light");
    fclose(fp);
}

/**
 * Setup terminal color scheme
 * @param terminal Terminal
 * @param scheme VTE_SCHEME_LIGHT or VTE_SCHEME_DARK
 */
static void set_terminal_colors(GtkWidget *terminal, gboolean scheme) {
    /*
     * Sixteen gray levels rather than eight. VTE synthesises whatever it is
     * not given, and from an eight entry base the bright half collapses onto
     * the normal half, so bold colored text became indistinguishable. The
     * ramps below are chosen for contrast against their own background: on
     * the light scheme even "white" (slot 7) is dark enough to read on paper,
     * and "bright white" (slot 15) is the emphasis color, i.e. black.
     */
    static const guint8 ramp_light[KT_PALETTE_SIZE] = {
        0x00, 0x40, 0x60, 0x88, 0x28, 0x58, 0x70, 0x30,
        0x78, 0x58, 0x78, 0xa0, 0x40, 0x70, 0x88, 0x00
    };
    static const guint8 ramp_dark[KT_PALETTE_SIZE] = {
        0x00, 0xa0, 0x88, 0xc0, 0x70, 0x98, 0xb0, 0xd0,
        0x68, 0xc0, 0xa8, 0xe0, 0x90, 0xb8, 0xd0, 0xff
    };
    const guint8 *ramp = (scheme == VTE_SCHEME_DARK) ? ramp_dark : ramp_light;
    guint8 bg = (scheme == VTE_SCHEME_DARK) ? 0x00 : 0xff;
    guint8 fg = (scheme == VTE_SCHEME_DARK) ? 0xff : 0x00;
    gint n;
#if GTK_CHECK_VERSION(3,14,0)
    GdkRGBA palette[KT_PALETTE_SIZE];
    GdkRGBA color_bg, color_fg;
# define KT_SET_COLOR(c, v) do { \
        (c).red = (c).green = (c).blue = (gdouble) (v) / 255.0; (c).alpha = 1; \
    } while (0)
#else
    GdkColor palette[KT_PALETTE_SIZE];
    GdkColor color_bg, color_fg;
    GdkColor color_dim;
# define KT_SET_COLOR(c, v) do { \
        (c).pixel = 0; (c).red = (c).green = (c).blue = (guint16) ((v) * 0x101); \
    } while (0)
#endif
    for (n = 0; n < KT_PALETTE_SIZE; n++) {
        KT_SET_COLOR(palette[n], ramp[n]);
    }
    KT_SET_COLOR(color_bg, bg);
    KT_SET_COLOR(color_fg, fg);
#if !GTK_CHECK_VERSION(3,14,0)
    KT_SET_COLOR(color_dim, (scheme == VTE_SCHEME_DARK) ? 0x88 : 0x78);
#endif
#undef KT_SET_COLOR
    vte_terminal_set_colors(VTE_TERMINAL(terminal), NULL, NULL, palette, KT_PALETTE_SIZE);
    vte_terminal_set_color_background(VTE_TERMINAL(terminal), &color_bg);
    vte_terminal_set_color_foreground(VTE_TERMINAL(terminal), &color_fg);
#if !GTK_CHECK_VERSION(3,14,0)
    vte_terminal_set_color_dim(VTE_TERMINAL(terminal), &color_dim);
#endif
    vte_terminal_set_color_bold(VTE_TERMINAL(terminal), &color_fg);
    vte_terminal_set_color_cursor(VTE_TERMINAL(terminal), NULL);
    vte_terminal_set_color_highlight(VTE_TERMINAL(terminal), NULL);
    conf->color_reversed = scheme;
    write_scheme_file(scheme);
}

/**
 * Reverse color scheme menu callback
 * @param widget Calling widget
 * @param terminal Terminal
 */
static void reverse_colors(GtkWidget *widget, gpointer terminal) {
    UNUSED(widget);
    set_terminal_colors(terminal, !conf->color_reversed);
}

/**
 * Keyboard widget size allocation signal handler.
 * Updates keyboard size
 * @param keyboard_box Keyboard widget
 * @param alloc Size allocation
 * @param keyboard Keyboard structure
 */
static void keyboard_update(GtkWidget *keyboard_box, GtkAllocation *alloc, Keyboard *keyboard) {
    UNUSED(keyboard_box);
    GdkScreen *screen = gdk_screen_get_default();
    gint screen_height = gdk_screen_get_height(screen);
    static gint saved_width = -1;
    static gint saved_height = -1;
    if (conf->kb_on && alloc && (alloc->width != saved_width || screen_height != saved_height)) {
        D printf("set keyboard size: %ix%i\n", alloc->width, alloc->height);
        g_idle_add(keyboard_set_size, keyboard);
        saved_width = alloc->width;
        saved_height = screen_height;
    }
}

#ifdef KINDLE
/**
 * Rotate screen menu callback
 * @param widget Calling widget
 * @param box Kterm container
 */
static void screen_rotate(GtkWidget *widget, gpointer box) {
    UNUSED(widget);
    UNUSED(box);
    char request = 0;
    if (conf->orientation == 'U') {
        request = 'R';
    } else {
        request = 'U';
    }
    if (set_orientation(request)) {
        conf->orientation = request;
    }
}
#endif

/**
 * Reset terminal manu callback
 * @param widget Calling widget
 * @param terminal Terminal
 */
static void reset_terminal(GtkWidget *widget, gpointer terminal) {
    UNUSED(widget);
    vte_terminal_reset(terminal, TRUE, TRUE);
}

/**
 * Toggle keyboard menu callback
 * @param widget Calling widget
 * @param box Kterm container
 */
static void toggle_keyboard(GtkWidget *widget, gpointer box) {
    UNUSED(widget);
    GtkWidget *keyboard_box = NULL;
    GList *box_list = gtk_container_get_children(GTK_CONTAINER(box));
    for (GList *cur = box_list; cur != NULL; cur = cur->next) {
        const gchar *box_name = gtk_widget_get_name(GTK_WIDGET(cur->data));
        D printf("box: %s\n", box_name);
        if (!strncmp(box_name, "kbBox", 5)) { keyboard_box = GTK_WIDGET(cur->data); }
    }
    g_list_free(box_list);
    if (keyboard_box) {
        if (conf->kb_on) {
            gtk_widget_hide(keyboard_box);
            conf->kb_on = FALSE;
        } else {
            gtk_widget_show(keyboard_box);
            conf->kb_on = TRUE;
        }
    }
}

#ifdef KINDLE
/**
 * Wrapper for g_signal_handlers_disconnect_by_func()
 * The only reason is to avoid warnings for converting a function pointer to a void pointer.
 * FIXME: is there a better way?
 * @param instance: The instance to remove handlers from.
 * @param func: The C closure callback of the handlers (useless for non-C closures).
 * @param data: The closure data of the handlers' closures.
 * @return The number of handlers that matched.
 */
static guint signal_handlers_disconnect_by_func(gpointer instance, GCallback func, gpointer data) {
    return g_signal_handlers_disconnect_by_func(instance, *(gpointer*)&func, data);
}

/**
 * Callback on menu deactivated signal
 * @param widget Calling widget
 * @param data User data
 */
static void menu_deactivate_cb(GtkWidget *widget, gpointer data) {
    UNUSED(widget);
    D printf("Menu deactivated\n");
    GdkEventButton *event = data;
    signal_handlers_disconnect_by_func(widget, G_CALLBACK(menu_deactivate_cb), data);
    gdk_test_simulate_button(event->window, (gint) event->x, (gint) event->y, 1, event->state, GDK_BUTTON_RELEASE);
    gdk_event_free((GdkEvent *) event);
}
#endif

/**
 * Paste menu callback
 * @param widget Calling widget
 * @param terminal Terminal
 */
static void paste_clipboard(GtkWidget *widget, gpointer terminal) {
    UNUSED(widget);
    vte_terminal_paste_clipboard(VTE_TERMINAL(terminal));
}

/**
 * Force a full redraw menu callback.
 * Partial updates leave ghosting on an eink panel; repainting everything
 * gives the display driver a reason to do a full refresh.
 * @param widget Calling widget
 * @param terminal Terminal
 */
static void refresh_screen(GtkWidget *widget, gpointer terminal) {
    UNUSED(widget);
    gtk_widget_queue_draw(gtk_widget_get_toplevel(GTK_WIDGET(terminal)));
}

/**
 * Mouse reporting toggle menu callback.
 * With reporting off, taps never reach the application, which is the quick
 * way out if an application leaves mouse tracking on and every tap starts
 * spraying escape sequences at the shell.
 * @param widget Calling widget
 * @param data Unused
 */
static void toggle_mouse(GtkWidget *widget, gpointer data) {
    UNUSED(data);
    conf->mouse_on = gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(widget));
    D printf("mouse_report = %i\n", conf->mouse_on);
}

/**
 * Save settings menu callback
 * @param widget Calling widget
 * @param data Unused
 */
static void save_settings(GtkWidget *widget, gpointer data) {
    UNUSED(widget);
    UNUSED(data);
    save_config(conf);
}

/**
 * Build popup menu
 * @param terminal Terminal widget
 * @param box Kterm container
 * @return Menu widget
 */
static GtkWidget * build_popup(GtkWidget *terminal, GtkWidget *box) {
    // popup menu on button release
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *fontup_item = gtk_menu_item_new_with_label("Font increase");
    GtkWidget *fontdown_item = gtk_menu_item_new_with_label("Font decrease");
    GtkWidget *color_item = gtk_menu_item_new_with_label("Reverse colors");
    GtkWidget *kb_item = gtk_menu_item_new_with_label("Toggle keyboard");
    GtkWidget *paste_item = gtk_menu_item_new_with_label("Paste");
    GtkWidget *mouse_item = gtk_check_menu_item_new_with_label("Mouse reporting");
    GtkWidget *refresh_item = gtk_menu_item_new_with_label("Refresh screen");
    GtkWidget *reset_item = gtk_menu_item_new_with_label("Reset terminal");
    GtkWidget *save_item = gtk_menu_item_new_with_label("Save settings");
#ifdef KINDLE
    GtkWidget *rotate_item = gtk_menu_item_new_with_label("Screen rotate");
#endif
    GtkWidget *quit_item = gtk_menu_item_new_with_label("Quit");

    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mouse_item), conf->mouse_on);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), fontup_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), fontdown_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), color_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), kb_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), paste_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mouse_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), refresh_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), reset_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), save_item);
#ifdef KINDLE
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), rotate_item);
#endif
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit_item);


    g_signal_connect(G_OBJECT(fontup_item), "activate", G_CALLBACK(fontup), (gpointer) terminal);
    g_signal_connect(G_OBJECT(fontdown_item), "activate", G_CALLBACK(fontdown), (gpointer) terminal);
    g_signal_connect(G_OBJECT(color_item), "activate", G_CALLBACK(reverse_colors), (gpointer) terminal);
    g_signal_connect(G_OBJECT(kb_item), "activate", G_CALLBACK(toggle_keyboard), box);
    g_signal_connect(G_OBJECT(paste_item), "activate", G_CALLBACK(paste_clipboard), (gpointer) terminal);
    g_signal_connect(G_OBJECT(mouse_item), "toggled", G_CALLBACK(toggle_mouse), NULL);
    g_signal_connect(G_OBJECT(refresh_item), "activate", G_CALLBACK(refresh_screen), (gpointer) terminal);
    g_signal_connect(G_OBJECT(reset_item), "activate", G_CALLBACK(reset_terminal), (gpointer) terminal);
    g_signal_connect(G_OBJECT(save_item), "activate", G_CALLBACK(save_settings), NULL);
#ifdef KINDLE
    g_signal_connect(G_OBJECT(rotate_item), "activate", G_CALLBACK(screen_rotate), box);
#endif
    g_signal_connect(G_OBJECT(quit_item), "activate", G_CALLBACK(gtk_main_quit), NULL);

    gtk_widget_show_all(menu);
    return menu;
}

#ifdef KINDLE
/**
 * State of the touch gesture in progress.
 *
 * Button 1 is fully mediated: the press is never handed straight to vte,
 * because at press time we cannot yet tell a tap from a drag from a hold.
 * The gesture is classified as it develops and the appropriate events are
 * synthesised afterwards.
 */
static struct {
    guint longpress_source;  /** Pending hold timer, 0 if none */
    gdouble origin_x;        /** Where the finger went down */
    gdouble origin_y;
    gdouble last_y;          /** Position at the previous motion event */
    gdouble accum;           /** Sub-row scroll remainder */
    gboolean down;           /** A finger is actually down right now */
    gboolean moved;          /** Travelled past the slop threshold */
    gboolean precise;        /** Hold engaged: raw events go through to the app */
    gboolean synthetic;      /** Dispatching our own event, do not classify it */
    guint32 press_time;      /** Timestamp of the press, for synthetic events */
} touch;

/** Pointer device, needed so gtk3 does not complain about synthetic events */
#if GTK_CHECK_VERSION(3,0,0)
static GdkDevice * getptrdevice(void) {
# if GTK_CHECK_VERSION(3,20,0)
    return gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_display_get_default()));
# else
    return gdk_device_manager_get_client_pointer(
        gdk_display_get_device_manager(gdk_display_get_default()));
# endif
}
#endif

/**
 * Deliver a synthetic button event to the terminal
 * @param terminal Terminal widget
 * @param type GDK_BUTTON_PRESS or GDK_BUTTON_RELEASE
 * @param button Button number
 * @param x Position within the widget
 * @param y Position within the widget
 */
static void send_button_event(GtkWidget *terminal, GdkEventType type,
                              guint button, gdouble x, gdouble y) {
    GdkWindow *window = gtk_widget_get_window(terminal);
    GdkEvent *event;
    if (!window) { return; }
    event = gdk_event_new(type);
    event->button.window = g_object_ref(window);
    event->button.send_event = TRUE;
    // carry the real press time forward. A timeout has no current event, so
    // gtk_get_current_event_time() would hand vte a zero timestamp and confuse
    // its double click accounting.
    event->button.time = touch.press_time;
    event->button.x = x;
    event->button.y = y;
    event->button.x_root = x;
    event->button.y_root = y;
    event->button.state = 0;
    event->button.button = button;
    event->button.axes = NULL;
#if GTK_CHECK_VERSION(3,0,0)
    gdk_event_set_device(event, getptrdevice());
#else
    event->button.device = gdk_device_get_core_pointer();
#endif
    // gtk_main_do_event dispatches straight back into button_event, which
    // would classify our own click as a new gesture and synthesise another
    // one, forever. Mark the round trip so it is passed through instead.
    touch.synthetic = TRUE;
    gtk_main_do_event(event);
    touch.synthetic = FALSE;
    gdk_event_free(event);
}

/**
 * Deliver a synthetic scroll event to the terminal.
 * Going through vte rather than moving the adjustment ourselves is what makes
 * this work everywhere: vte reports a wheel button to the application when it
 * has asked for mouse tracking, and scrolls its own buffer when it has not. So
 * a drag scrolls the pager in a full screen TUI and the scrollback at a shell,
 * with no need to guess which one we are looking at.
 * @param terminal Terminal widget
 * @param direction Scroll direction
 * @param x Position within the widget
 * @param y Position within the widget
 */
static void send_scroll_event(GtkWidget *terminal, GdkScrollDirection direction,
                              gdouble x, gdouble y) {
    GdkWindow *window = gtk_widget_get_window(terminal);
    GdkEvent *event;
    if (!window) { return; }
    event = gdk_event_new(GDK_SCROLL);
    event->scroll.window = g_object_ref(window);
    event->scroll.send_event = TRUE;
    event->scroll.time = gtk_get_current_event_time();
    event->scroll.x = x;
    event->scroll.y = y;
    event->scroll.x_root = x;
    event->scroll.y_root = y;
    event->scroll.state = 0;
    event->scroll.direction = direction;
#if GTK_CHECK_VERSION(3,0,0)
    gdk_event_set_device(event, getptrdevice());
#else
    event->scroll.device = gdk_device_get_core_pointer();
#endif
    touch.synthetic = TRUE;
    gtk_main_do_event(event);
    touch.synthetic = FALSE;
    gdk_event_free(event);
}

/**
 * Cancel a pending hold
 */
static void longpress_cancel(void) {
    if (touch.longpress_source) {
        g_source_remove(touch.longpress_source);
        touch.longpress_source = 0;
    }
}

/**
 * Hold timer. The finger has been still for long enough, so switch this
 * gesture into a precise drag: from here on the real events reach the
 * application, which is what makes selecting text and dragging a pane
 * divider possible.
 * @param data Terminal widget
 * @return Always false, the timer fires once
 */
static gboolean longpress_cb(gpointer data) {
    GtkWidget *terminal = data;
    touch.longpress_source = 0;
    if (touch.moved) { return FALSE; }
    touch.precise = TRUE;
    // the finger is already down; tell the application the drag starts here
    send_button_event(terminal, GDK_BUTTON_PRESS, 1, touch.origin_x, touch.origin_y);
    return FALSE;
}

/**
 * Turn vertical finger travel into whole-row scroll steps
 * @param terminal Terminal widget
 * @param y Current pointer position
 */
static void touch_scroll_drag(GtkWidget *terminal, gdouble y) {
    glong char_height = vte_terminal_get_char_height(VTE_TERMINAL(terminal));
    GdkScrollDirection direction;
    gint rows, steps, i;

    if (char_height <= 0) { return; }
    touch.accum += y - touch.last_y;
    touch.last_y = y;
    rows = (gint) (touch.accum / (gdouble) char_height);
    if (rows == 0) { return; }
    touch.accum -= rows * (gdouble) char_height;

    // content follows the finger: dragging down reveals earlier lines
    direction = (rows > 0) ? GDK_SCROLL_UP : GDK_SCROLL_DOWN;
    steps = ABS(rows);
    if (steps > TOUCH_SCROLL_MAX_STEP) { steps = TOUCH_SCROLL_MAX_STEP; }
    for (i = 0; i < steps; i++) {
        send_scroll_event(terminal, direction, touch.origin_x, y);
    }
}
#endif /* KINDLE */

/**
 * Mouse button event callback
 * @param terminal Terminal widget
 * @param event Button event
 * @param box Kterm container
 * @return True to stop processing event, false otherwise
 */
static gboolean button_event(GtkWidget *terminal, GdkEventButton *event, gpointer menu) {
    UNUSED(terminal);
    D printf("event-type: %i\n", event->type);
    D printf("event-button: %i\n", event->button);
#ifdef KINDLE
    // an event we generated ourselves: hand it to vte untouched
    if (touch.synthetic) { return FALSE; }
    if (event->type == GDK_MOTION_NOTIFY) {
        GdkEventMotion *motion = (GdkEventMotion *) event;
        // A touch lands as a pointer warp: X delivers motion to the new
        // position before the press. Classifying that as a drag turned every
        // tap into a scroll, and scrolled the content out from under a hold
        // before the selection had started. Only motion while a finger is
        // genuinely down is part of a gesture.
        if (!touch.down) { return TRUE; }
        // precise drag: hand the motion over so the application can select
        // text or drag a pane divider
        if (touch.precise) { return FALSE; }
        if (!touch.moved &&
            (ABS(motion->x - touch.origin_x) > TOUCH_SCROLL_SLOP ||
             ABS(motion->y - touch.origin_y) > TOUCH_SCROLL_SLOP)) {
            touch.moved = TRUE;
            longpress_cancel();  // a drag is not a hold
        }
        if (touch.moved && conf->touch_scroll) { touch_scroll_drag(terminal, motion->y); }
        return TRUE;
    }
    if (event->button == 1) {
        if (event->type == GDK_BUTTON_PRESS) {
            longpress_cancel();
            touch.origin_x = event->x;
            touch.origin_y = touch.last_y = event->y;
            touch.accum = 0;
            touch.down = TRUE;
            touch.moved = FALSE;
            touch.precise = FALSE;
            touch.press_time = event->time;
            touch.longpress_source = g_timeout_add(TOUCH_LONGPRESS_MS, longpress_cb, terminal);
            // swallow for now: a press alone does not tell us what this is yet
            return TRUE;
        }
        if (event->type == GDK_BUTTON_RELEASE) {
            gboolean was_precise = touch.precise;
            gboolean was_tap = !touch.moved;
            longpress_cancel();
            touch.precise = FALSE;
            touch.moved = FALSE;
            touch.down = FALSE;
            // a precise drag opened with a real press, so it needs the real release
            if (was_precise) { return FALSE; }
            if (was_tap && conf->mouse_on) {
                // now that it is settled as a tap, deliver it as a click
                send_button_event(terminal, GDK_BUTTON_PRESS, 1, event->x, event->y);
                send_button_event(terminal, GDK_BUTTON_RELEASE, 1, event->x, event->y);
            }
            return TRUE;
        }
    }
#endif
    if (event->button == BUTTON_MENU) {
#ifdef KINDLE
        // ignore button click to disable paste (quite messy on kindle)
        if (event->type == GDK_BUTTON_PRESS) { return TRUE; }
        guint button = 0;
        // emit button 1 release on deactivate, otherwise vte enters selection mode
        GdkEventButton *event_copy = (GdkEventButton *) gdk_event_copy((GdkEvent *) event);
        g_signal_connect(menu, "deactivate", G_CALLBACK(menu_deactivate_cb), event_copy);
#else
        if (event->type == GDK_BUTTON_RELEASE) { return FALSE; }
        guint button = event->button;
#endif

        gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, button, event->time);
        return TRUE;
    }
    return FALSE;
}

/**
 * Print version number.
 * If possible check for consistent libraries usage.
 */
static void version(void) {
    printf("kterm %s (vte %i.%i.%i, gtk+ %i.%i.%i)\n",
           VERSION, VTE_MAJOR_VERSION, VTE_MINOR_VERSION, VTE_MICRO_VERSION,
           GTK_MAJOR_VERSION, GTK_MINOR_VERSION, GTK_MICRO_VERSION);
#if VTE_CHECK_VERSION(0,40,0)
    if (vte_get_major_version() != VTE_MAJOR_VERSION ||
        vte_get_minor_version() != VTE_MINOR_VERSION ||
        vte_get_micro_version() != VTE_MICRO_VERSION) {
        printf("Warning, using different vte version than compiled with (%i.%i.%i)!\n",
               vte_get_major_version(),
               vte_get_minor_version(),
               vte_get_micro_version());
    }
#endif
#if GTK_CHECK_VERSION(3,0,0)
    if (gtk_get_major_version() != GTK_MAJOR_VERSION ||
        gtk_get_minor_version() != GTK_MINOR_VERSION ||
        gtk_get_micro_version() != GTK_MICRO_VERSION) {
        printf("Warning, using different gtk+ version than compiled with (%i.%i.%i)!\n",
               gtk_get_major_version(),
               gtk_get_minor_version(),
               gtk_get_micro_version());
    }
#endif
    exit(0);
}

/**
 * Print usage info and exit
 */
static void usage(void) {
    printf("Usage: kterm [OPTIONS]\n");
    printf("        -c <0|1>      color scheme (0 light, 1 dark)\n");
    printf("        -d            debug mode\n");
    printf("        -e <command>  execute command in kterm\n");
    printf("        -E <var>      set environment variable\n");
    printf("        -f <family>   font family\n");
    printf("        -h            show this message\n");
    printf("        -k <0|1>      keyboard off/on\n");
    printf("        -l <path>     keyboard layout config path\n");
    printf("        -m <0|1>      mouse reporting off/on\n");
#ifdef KINDLE
    printf("        -o <U|R|L>    screen orientation (up, right, left)\n");
#endif
    printf("        -s <size>     font size\n");
    printf("        -S <0|1>      escape sequence shim (ktsh) off/on\n");
    printf("        -t <encoding> terminal encoding\n");
#if VTE_CHECK_VERSION(0,20,0)
    printf("        -u <B|I|U>    cursor shape (block, I-beam, underline)\n");
#endif
    printf("        -v            print version and exit\n");
    exit(0);
}

/**
 * Setup terminal
 * @param terminal Terminal
 * @param command Command passed to terminal, null if none
 * @param envv Null terminated array of env variable=value pairs passed to terminal
 * @param error Set on error, null otherwise
 */
static void setup_terminal(GtkWidget *terminal, gchar *command, gchar **envv, GError **error) {
    gchar *argv[TERM_ARGS_MAX] = { NULL };
    gint argc = 0;
    gchar *shell = NULL;
    /* these outlive the call: vte only reads argv when it forks */
    static gchar shim_path[PATH_MAX];
    static gchar scheme_path[PATH_MAX];

    /*
     * Run the child under ktsh, which strips the escape sequences VTE 0.28
     * would print as literal text, answers the color queries it cannot
     * answer, and converts its legacy mouse reports to the SGR form modern
     * applications expect. If the shim is missing we simply go without it.
     */
    if (conf->shim_on && sibling_path(SHIM_FILE, shim_path, sizeof(shim_path))
        && is_executable(shim_path)) {
        argv[argc++] = shim_path;
        argv[argc++] = (gchar *) "-c";
        argv[argc++] = conf->shim_color;
        argv[argc++] = (gchar *) "-b";
        argv[argc++] = (gchar *) (conf->color_reversed ? "dark" : "light");
        if (sibling_path(SCHEME_FILE, scheme_path, sizeof(scheme_path))) {
            argv[argc++] = (gchar *) "-s";
            argv[argc++] = scheme_path;
        }
        argv[argc++] = (gchar *) "--";
        D printf("shim: %s\n", shim_path);
    } else if (conf->shim_on) {
        D printf("shim not usable at '%s', running without it\n", shim_path);
    }

#if VTE_CHECK_VERSION(0,25,1)
    if (!command || *command == '\0') {
        // prepend args with shell
        if ((shell = vte_get_user_shell()) == NULL) {
            shell = g_strdup("/bin/sh");
        }
        argv[argc++] = shell;
    }
#endif
    if (command) {
        gchar *argbuf = strtok(command, " ");
        while (argbuf != NULL && argc < (TERM_ARGS_MAX - 1)) {
            argv[argc++] = argbuf;
            argbuf = strtok(NULL, " ");
        }
    }

    set_terminal_colors(terminal, conf->color_reversed);
    vte_terminal_set_scrollback_lines(VTE_TERMINAL(terminal), VTE_SCROLLBACK_LINES);
    set_terminal_font(VTE_TERMINAL(terminal), conf->font_family, (gint) conf->font_size);
#if VTE_CHECK_VERSION(0,20,0)
    set_terminal_cursor(VTE_TERMINAL(terminal), conf->cursor_shape);
#endif
#if VTE_CHECK_VERSION(0,38,0)
    vte_terminal_set_encoding(VTE_TERMINAL(terminal), conf->encoding, NULL);
#else
    vte_terminal_set_encoding(VTE_TERMINAL(terminal), conf->encoding);
#endif
    vte_terminal_set_allow_bold(VTE_TERMINAL(terminal), TRUE);
    
#if VTE_CHECK_VERSION(0,38,0)
    vte_terminal_spawn_sync(VTE_TERMINAL(terminal), 0, NULL, argv, envv, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, error);
#elif VTE_CHECK_VERSION(0,25,1)
    vte_terminal_fork_command_full(VTE_TERMINAL(terminal), 0, NULL, argv, envv, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, error);
#else
    gboolean ret = TRUE;
    ret = vte_terminal_fork_command(VTE_TERMINAL(terminal), argv[0], (argv[0] ? argv : NULL), envv, NULL, FALSE, FALSE, FALSE);
    if (!ret) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "vte_terminal_fork_command returned error");
    }
#endif
    if (shell) { g_free(shell); }
    if (*error) {
        g_prefix_error(error, "VTE terminal fork failed.\n");
        D printf("%s\n", (*error)->message);
    }
}

/**
 * Print key event debug info on callback
 * @param widget Calling widget
 * @param event Event
 * @return Always false to propagate event
 */
static gboolean debug_key_event(GtkWidget *widget, GdkEventKey *event) {
    printf("key event; widget: %s\n", gtk_widget_get_name(widget));
    printf("key event; type: %s (%i)\n", (event->type == 8) ? "press" : "release", event->type);
    printf("key event; window: %p\n", (void *) event->window);
    printf("key event; send event: %i\n", event->send_event);
    printf("key event; time: %i\n", event->time);
    printf("key event; state: %i\n", event->state);
    printf("key event; keyval: %i (%s)\n", event->keyval, gdk_keyval_name(event->keyval));
    printf("key event; length: %i\n", event->length);
    printf("key event; string: %s\n", event->string);
    printf("key event; hardware_keycode: %i\n", event->hardware_keycode);
    printf("key event; group: %i\n", event->group);
    printf("key event; modifier: %i\n", event->is_modifier);
    return FALSE;
}

/**
 * Display dialog with error message and clear error
 * @param window Parent window
 * @param error Error structure
 */
static void error_handle(GtkWidget *window, GError **error) {
    GtkDialogFlags flags = GTK_DIALOG_DESTROY_WITH_PARENT;
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window), flags,
                                               GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
                                               "%s", (*error)->message);
#ifdef KINDLE
    gtk_window_set_title(GTK_WINDOW(dialog), TITLE_DIALOG);
#endif
    gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER_ALWAYS);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    g_clear_error(error);
}

/** main */
gint main(gint argc, gchar **argv) {
    conf = parse_config(); // call first so args overide defaults/config
    
    gint c = -1;
    gint i = 0;
    gchar *command = NULL;
    gchar *envv[TERM_ARGS_MAX] = { NULL };
    gint envc = 0;
#ifdef KINDLE
    // modify buttons style (gtk+ 2)
    inject_gtkrc();
    // set short prompt
    envv[envc++] = "PS1=[\\W]\\$ ";
    // set terminfo path
    envv[envc++] = "TERMINFO=" TERMINFO_PATH;
#endif
    while((c = getopt(argc, argv, "c:de:E:f:hk:l:m:o:s:S:t:u:v")) != -1) {
        switch(c) {
            case 'c':
                i = atoi(optarg);
                if ((i == TRUE) | (i == FALSE)) { conf->color_reversed = i; }
                break;
            case 'd':
                debug = TRUE;
                // unbuffered, or a crash takes the trace with it when
                // stdout is a pipe or a file rather than a terminal
                setvbuf(stdout, NULL, _IONBF, 0);
                break;
            case 'e':
                command = optarg;
                break;
            case 'E':
                if (envc < TERM_ARGS_MAX - 1) {
                    envv[envc++] = optarg;
                }
                break;
            case 'f':
                snprintf(conf->font_family, sizeof(conf->font_family), "%s", optarg);
                break;
            case 'h':
                usage();
                break;
            case 'k':
                i = atoi(optarg);
                if ((i == TRUE) | (i == FALSE)) { conf->kb_on = i; }
                break;
            case 'l':
                snprintf(conf->kb_conf_path, sizeof(conf->kb_conf_path), "%s", optarg);
                break;
            case 'm':
                i = atoi(optarg);
                if ((i == TRUE) | (i == FALSE)) { conf->mouse_on = i; }
                break;
            case 'S':
                i = atoi(optarg);
                if ((i == TRUE) | (i == FALSE)) { conf->shim_on = i; }
                break;
            case 'o':
                if (optarg[0] == 'U' || optarg[0] == 'R' || optarg[0] == 'L') { conf->orientation = optarg[0]; }
                break;
            case 's':
                i = atoi(optarg);
                if (i > 0) conf->font_size = (guint) i;
                break;
            case 't':
                snprintf(conf->encoding, sizeof(conf->encoding), "%s", optarg);
                break;
            case 'u':
                if (optarg[0] == 'B' || optarg[0] == 'I' || optarg[0] == 'U') { conf->cursor_shape = optarg[0]; }
                break;
            case 'v':
                version();
                break;
        }
    }
    
    /*
     * Tell the child what the terminal actually looks like. VTE 0.28 cannot
     * answer an OSC 11 background color query, so without COLORFGBG every
     * application from vim to Claude Code assumes a dark background and
     * picks a palette that is unreadable on the light scheme.
     * KTERM_SCHEME is the same information for ktsh, which answers the
     * OSC query on VTE's behalf.
     */
    if (envc < TERM_ARGS_MAX - 2) {
        envv[envc++] = conf->color_reversed ? (gchar *) "COLORFGBG=15;0"
                                            : (gchar *) "COLORFGBG=0;15";
        envv[envc++] = conf->color_reversed ? (gchar *) "KTERM_SCHEME=dark"
                                            : (gchar *) "KTERM_SCHEME=light";
    }
    D printf("env: color_reversed=%i shim=%i mouse=%i\n",
             conf->color_reversed, conf->shim_on, conf->mouse_on);
    write_scheme_file(conf->color_reversed);
    D printf("startup: scheme file written\n");

#ifdef KINDLE
    orientation_init();
    D printf("startup: orientation initialised\n");
#endif

    GError *error = NULL;
    gtk_init(&argc, &argv);
    D printf("startup: gtk initialised\n");

    install_signal_handlers();
    
    // window
    //  \- vbox
    //      \- terminal  \- keyboard_box
    //
    // main window
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), TITLE);
#ifdef KINDLE
    // modify buttons style (gtk+ 3)
    inject_styles();
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
#endif
    // box
#if GTK_CHECK_VERSION(3,2,0)
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
#else
    GtkWidget *vbox = gtk_vbox_new(FALSE, 0);
#endif
    gtk_widget_set_name(vbox, "ktermBox");
    gtk_container_add(GTK_CONTAINER(window), vbox);

#if GTK_CHECK_VERSION(3,0,0)
    GtkWidget *keyboard_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_set_homogeneous(GTK_BOX(keyboard_box), TRUE);
#else
    GtkWidget *keyboard_box = gtk_vbox_new(TRUE, 0);
#endif
    gtk_widget_set_name(keyboard_box, "kbBox");
    
    Keyboard *keyboard = build_layout(keyboard_box, &error);
    if G_UNLIKELY(error) {
        error_handle(window, &error);
        clean_on_exit(keyboard);
        exit(1);
    }
    gtk_box_pack_end(GTK_BOX(vbox), keyboard_box, FALSE, FALSE, 0);
    keyboard_set_size(keyboard);
    
    D printf("startup: keyboard built\n");
    GtkWidget *terminal = vte_terminal_new();
    D printf("startup: terminal created\n");
    setup_terminal(terminal, command, envv, &error);
    D printf("startup: terminal set up\n");
    if G_UNLIKELY(error) {
        error_handle(window, &error);
        clean_on_exit(keyboard);
        exit(1);
    }
    gtk_widget_set_name(terminal, "termBox");
    gtk_box_pack_start(GTK_BOX(vbox), terminal, TRUE, TRUE, 0);
    
    GtkWidget *menu = build_popup(terminal, vbox);
    D printf("startup: menu built\n");

    // packed before the terminal so it sits at the top of the window
    GtkWidget *statusbar = NULL;
    if (conf->statusbar_on) {
        statusbar = statusbar_new(menu);
        gtk_box_pack_start(GTK_BOX(vbox), statusbar, FALSE, FALSE, 0);
        gtk_box_reorder_child(GTK_BOX(vbox), statusbar, 0);
        statusbar_start(statusbar);
        D printf("startup: status bar built\n");
    }
    // signals
    g_signal_connect(window, "delete_event", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(terminal, "child-exited", G_CALLBACK(terminal_exit), NULL);
    g_signal_connect(terminal, "button-press-event", G_CALLBACK(button_event), menu);
    g_signal_connect(terminal, "button-release-event", G_CALLBACK(button_event), menu);
    g_signal_connect(terminal, "motion-notify-event", G_CALLBACK(button_event), menu);
    g_signal_connect(terminal, "realize", G_CALLBACK(grab_focus), NULL);
#ifdef KINDLE
    g_object_set(window, "events", GDK_VISIBILITY_NOTIFY_MASK, NULL);
    g_signal_connect(window, "visibility-notify-event", G_CALLBACK(grab_keyboard_cb), NULL);
#endif
    D g_signal_connect(terminal, "key-release-event", G_CALLBACK(debug_key_event), NULL);
    D g_signal_connect(terminal, "key-press-event", G_CALLBACK(debug_key_event), NULL);
    
    gtk_widget_show_all(window);
    if (!conf->kb_on) {
        gtk_widget_hide(keyboard_box);
    }
    UNUSED(statusbar);
    g_signal_connect(keyboard_box, "size-allocate", G_CALLBACK(keyboard_update), keyboard);
    gtk_window_maximize(GTK_WINDOW(window));
    gtk_main();
    
    clean_on_exit(keyboard);
    gtk_widget_destroy(menu);
    D printf("the end\n");
    return 0;
}
