/* parse_config.c
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

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"

/**
 * Does this line assign the named key?
 * A bare strncmp lets a shorter key swallow a longer one that starts with it,
 * so touch_scroll would match touch_scroll_speed and the longer key would
 * never be reachable. Require a delimiter after the name.
 * @param line Config line
 * @param key Key name
 * @return True when the line sets exactly this key
 */
static gboolean key_is(const gchar *line, const gchar *key) {
    gsize len = strlen(key);
    if (strncmp(line, key, len) != 0) { return FALSE; }
    return line[len] == ' ' || line[len] == '=' || line[len] == '\t';
}

/**
 * Directory holding the running kterm binary
 * @param buf Buffer receiving the path
 * @param len Buffer size
 * @return True on success, false if the path could not be determined
 */
gboolean kterm_exe_dir(gchar *buf, gsize len) {
    gchar self[PATH_MAX], *s;
    gssize n;
    if ((n = readlink("/proc/self/exe", self, sizeof(self) - 1)) == -1) { return FALSE; }
    self[n] = '\0';
    if ((s = strrchr(self, '/')) == NULL) { return FALSE; }
    *s = '\0';
    snprintf(buf, len, "%s", self);
    return TRUE;
}

/**
 * Parse kterm config
 * @return KTconf structure or NULL
 */
KTconf *parse_config(void) {

    D printf("Parsing config file\n");

    gchar conf_path[PATH_MAX];
    conf_path[0] = '\0';

    // if kterm config is not found
    if (access(CONFIG_FULL_PATH, R_OK) == 0) {
        snprintf(conf_path, sizeof(conf_path), "%s", CONFIG_FULL_PATH);
    } else {
        // set path to kterm binary's path
        gchar self[PATH_MAX];
        if (kterm_exe_dir(self, sizeof(self))) {
            snprintf(conf_path, sizeof(conf_path), "%s/%s", self, CONFIG_FILE);
        }
    }
    D printf("config: %s\n", conf_path);

    KTconf *conf = NULL;
    if ((conf = g_malloc0(sizeof(KTconf))) == NULL) {
        D printf("Memory allocation failed\n");
        exit(1);
    }
    
    // defaults
    conf->kb_on = 1;
    conf->color_reversed = FALSE;
    conf->font_size = VTE_FONT_SIZE;
    snprintf(conf->font_family, sizeof(conf->font_family), "%s", VTE_FONT_FAMILY);
    snprintf(conf->encoding, sizeof(conf->encoding), "%s", VTE_ENCODING);
    snprintf(conf->kb_conf_path, sizeof(conf->kb_conf_path), "%s", KB_FULL_PATH);
    conf->orientation = 0;
    conf->shim_on = TRUE;
    conf->mouse_on = TRUE;
    conf->touch_scroll = TRUE;
    conf->statusbar_on = TRUE;
    conf->touch_hold_ms = TOUCH_LONGPRESS_MS;
    conf->touch_scroll_speed = TOUCH_SCROLL_SPEED;
    snprintf(conf->shim_color, sizeof(conf->shim_color), "256");
    snprintf(conf->conf_path, sizeof(conf->conf_path), "%s", conf_path);

    FILE *fp;
    if ((fp = fopen(conf_path, "r")) == NULL) {
        D printf("No config file\n");
        return conf;
    }
    
    gchar buf[PATH_MAX];
    while (fgets(buf, sizeof(buf), fp)) {
        if (buf[0] == '#' || buf[0] == '\n') { continue; }
        if (key_is(buf, "keyboard")) {
            gint kb_on = -1;
            sscanf(buf, "keyboard = %i", &kb_on);
            if (kb_on == 0 || kb_on == 1) {
                conf->kb_on = kb_on;
                D printf("kb_on = %i\n", conf->kb_on);
            }
        }
        else if (key_is(buf, "color_scheme")) {
            gint color_reversed = -1;
            sscanf(buf, "color_scheme = %i", &color_reversed);
            if (color_reversed == 0 || color_reversed == 1) {
                conf->color_reversed = color_reversed;
                D printf("color_scheme = %i\n", conf->color_reversed);
            }
        }
        else if (key_is(buf, "font_family")) {
            gchar str[256];
            sscanf(buf, "font_family = \"%[^\"\n\r]\"", str);
            snprintf(conf->font_family, sizeof(conf->font_family), "%s", str);
            D printf("font_family = %s\n", conf->font_family);
        }
        else if (key_is(buf, "font_size")) {
            guint font_size = 0;
            sscanf(buf, "font_size = %u", &font_size);
            if (font_size > 0) {
                conf->font_size = font_size;
                D printf("font_size = %u\n", conf->font_size);
            }
        }
        else if (key_is(buf, "encoding")) {
            gchar str[256];
            sscanf(buf, "encoding = \"%[^\"\n\r]\"", str);
            snprintf(conf->encoding, sizeof(conf->encoding), "%s", str);
            D printf("encoding = %s\n", conf->encoding);
        }
        else if (key_is(buf, "kb_conf_path")) {
            gchar str2[PATH_MAX];
            sscanf(buf, "kb_conf_path = \"%[^\"\n\r]\"", str2); // need double quotes around path
            snprintf(conf->kb_conf_path, sizeof(conf->kb_conf_path), "%s", str2);
            D printf("kb_conf_path = %s\n", conf->kb_conf_path);
        }
        else if (key_is(buf, "orientation")) {
            gchar orientation = 0;
            sscanf(buf, "orientation = %c", &orientation);
            if (orientation == 'U' || orientation == 'R' || orientation == 'L') {
                conf->orientation = orientation;
                D printf("orientation = %c\n", conf->orientation);
            }
        }
        else if (key_is(buf, "cursor_shape")) {
            gchar cursor_shape = 0;
            sscanf(buf, "cursor_shape = %c", &cursor_shape);
            if (cursor_shape == 'B' || cursor_shape == 'I' || cursor_shape == 'U') {
                conf->cursor_shape = cursor_shape;
                D printf("cursor_shape = %c\n", conf->cursor_shape);
            }
        }
        else if (key_is(buf, "shim")) {
            gint shim_on = -1;
            sscanf(buf, "shim = %i", &shim_on);
            if (shim_on == 0 || shim_on == 1) {
                conf->shim_on = shim_on;
                D printf("shim = %i\n", conf->shim_on);
            }
        }
        else if (key_is(buf, "mouse_report")) {
            gint mouse_on = -1;
            sscanf(buf, "mouse_report = %i", &mouse_on);
            if (mouse_on == 0 || mouse_on == 1) {
                conf->mouse_on = mouse_on;
                D printf("mouse_report = %i\n", conf->mouse_on);
            }
        }
        else if (key_is(buf, "touch_scroll")) {
            gint touch_scroll = -1;
            sscanf(buf, "touch_scroll = %i", &touch_scroll);
            if (touch_scroll == 0 || touch_scroll == 1) {
                conf->touch_scroll = touch_scroll;
                D printf("touch_scroll = %i\n", conf->touch_scroll);
            }
        }
        else if (key_is(buf, "touch_hold_ms")) {
            guint hold = 0;
            sscanf(buf, "touch_hold_ms = %u", &hold);
            if (hold >= 100 && hold <= 5000) {
                conf->touch_hold_ms = hold;
                D printf("touch_hold_ms = %u\n", conf->touch_hold_ms);
            }
        }
        else if (key_is(buf, "touch_scroll_speed")) {
            guint speed = 0;
            sscanf(buf, "touch_scroll_speed = %u", &speed);
            if (speed >= 10 && speed <= 1000) {
                conf->touch_scroll_speed = speed;
                D printf("touch_scroll_speed = %u\n", conf->touch_scroll_speed);
            }
        }
        else if (key_is(buf, "statusbar")) {
            gint statusbar_on = -1;
            sscanf(buf, "statusbar = %i", &statusbar_on);
            if (statusbar_on == 0 || statusbar_on == 1) {
                conf->statusbar_on = statusbar_on;
                D printf("statusbar = %i\n", conf->statusbar_on);
            }
        }
        else if (key_is(buf, "color_folding")) {
            gchar str[256] = { 0 };
            sscanf(buf, "color_folding = \"%[^\"\n\r]\"", str);
            if (!strcmp(str, "256") || !strcmp(str, "gray") || !strcmp(str, "keep")) {
                snprintf(conf->shim_color, sizeof(conf->shim_color), "%s", str);
                D printf("color_folding = %s\n", conf->shim_color);
            }
        }
    }
    
    fclose(fp);

    return conf;
}

/** Settings that survive a restart, in the order they are appended */
#define SAVED_KEYS 6

/**
 * Write the settings that can be changed at runtime back to the config file.
 * Existing lines are rewritten in place so user comments and unknown keys
 * are preserved; anything missing is appended.
 * @param conf Kterm config
 */
void save_config(const KTconf *conf) {
    static const gchar *keys[SAVED_KEYS] = {
        "keyboard", "color_scheme", "font_family", "font_size",
        "mouse_report", "touch_scroll"
    };
    gchar values[SAVED_KEYS][PATH_MAX];
    gboolean seen[SAVED_KEYS] = { FALSE };
    gchar tmp_path[PATH_MAX];
    gchar buf[PATH_MAX];
    FILE *in, *out;
    gint i;

    if (conf->conf_path[0] == '\0') {
        D printf("No config file path, not saving\n");
        return;
    }

    snprintf(values[0], sizeof(values[0]), "keyboard = %i", conf->kb_on ? 1 : 0);
    snprintf(values[1], sizeof(values[1]), "color_scheme = %i", conf->color_reversed ? 1 : 0);
    snprintf(values[2], sizeof(values[2]), "font_family = \"%s\"", conf->font_family);
    snprintf(values[3], sizeof(values[3]), "font_size = %u", conf->font_size);
    snprintf(values[4], sizeof(values[4]), "mouse_report = %i", conf->mouse_on ? 1 : 0);
    snprintf(values[5], sizeof(values[5]), "touch_scroll = %i", conf->touch_scroll ? 1 : 0);

    if (strlen(conf->conf_path) + sizeof(".tmp") > sizeof(tmp_path)) { return; }
    strcpy(tmp_path, conf->conf_path);
    strcat(tmp_path, ".tmp");
    if ((out = fopen(tmp_path, "w")) == NULL) {
        D printf("Cannot write %s\n", tmp_path);
        return;
    }

    if ((in = fopen(conf->conf_path, "r")) != NULL) {
        while (fgets(buf, sizeof(buf), in)) {
            gboolean replaced = FALSE;
            if (buf[0] != '#' && buf[0] != '\n') {
                for (i = 0; i < SAVED_KEYS; i++) {
                    gsize len = strlen(keys[i]);
                    if (!strncmp(buf, keys[i], len) && (buf[len] == ' ' || buf[len] == '=')) {
                        fprintf(out, "%s\n", values[i]);
                        seen[i] = TRUE;
                        replaced = TRUE;
                        break;
                    }
                }
            }
            if (!replaced) { fputs(buf, out); }
        }
        fclose(in);
    }

    for (i = 0; i < SAVED_KEYS; i++) {
        if (!seen[i]) { fprintf(out, "%s\n", values[i]); }
    }
    fclose(out);

    if (rename(tmp_path, conf->conf_path) != 0) {
        D printf("Cannot replace %s\n", conf->conf_path);
        unlink(tmp_path);
        return;
    }
    D printf("Saved config to %s\n", conf->conf_path);
}
