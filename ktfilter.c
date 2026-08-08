/*
 * ktfilter.c - the ktsh escape sequence filter
 *
 * A VT500-style parser that rewrites the byte stream flowing between a
 * modern application and VTE 0.28. VTE 0.28 inserts any escape sequence
 * it does not recognise into the screen as literal text, so anything the
 * application emits that postdates 2011 has to be removed here or it
 * shows up as garbage.
 *
 * Copyright (C) 2026 kterm contributors
 * Licensed under the GNU General Public License v3 (see COPYING).
 */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "ktsh.h"

#define KT_SEQ_MAX      (64 * 1024)
#define KT_CSI_MAX      256

/* ------------------------------------------------------------------ */
/* byte buffer                                                        */
/* ------------------------------------------------------------------ */

void ktbuf_init(KtBuf *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void ktbuf_free(KtBuf *b) {
    free(b->data);
    ktbuf_init(b);
}

void ktbuf_clear(KtBuf *b) {
    b->len = 0;
}

void ktbuf_add(KtBuf *b, const void *data, size_t n) {
    if (n == 0) { return; }
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n) { cap *= 2; }
        unsigned char *p = realloc(b->data, cap);
        if (!p) { return; } /* drop on OOM rather than abort a live terminal */
        b->data = p;
        b->cap = cap;
    }
    memcpy(b->data + b->len, data, n);
    b->len += n;
}

void ktbuf_addc(KtBuf *b, unsigned char c) {
    ktbuf_add(b, &c, 1);
}

static void ktbuf_adds(KtBuf *b, const char *s) {
    ktbuf_add(b, s, strlen(s));
}

void ktbuf_consume(KtBuf *b, size_t n) {
    if (n >= b->len) { b->len = 0; return; }
    memmove(b->data, b->data + n, b->len - n);
    b->len -= n;
}

/* ------------------------------------------------------------------ */
/* colour conversion                                                  */
/* ------------------------------------------------------------------ */

static const int kt_cube_levels[6] = { 0, 95, 135, 175, 215, 255 };

static int kt_cube_index(int v) {
    int best = 0, bestd = 1 << 30, i;
    for (i = 0; i < 6; i++) {
        int d = v - kt_cube_levels[i];
        if (d < 0) { d = -d; }
        if (d < bestd) { bestd = d; best = i; }
    }
    return best;
}

int ktfilter_rgb_to_256(int r, int g, int b) {
    int ri = kt_cube_index(r), gi = kt_cube_index(g), bi = kt_cube_index(b);
    int cr = kt_cube_levels[ri], cg = kt_cube_levels[gi], cb = kt_cube_levels[bi];
    int cube = 16 + 36 * ri + 6 * gi + bi;
    int cubed = (cr - r) * (cr - r) + (cg - g) * (cg - g) + (cb - b) * (cb - b);
    int lum = (r * 299 + g * 587 + b * 114) / 1000;
    int idx = (lum - 8 + 5) / 10;
    int gv, grayd;
    if (idx < 0) { idx = 0; }
    if (idx > 23) { idx = 23; }
    gv = 8 + 10 * idx;
    grayd = (gv - r) * (gv - r) + (gv - g) * (gv - g) + (gv - b) * (gv - b);
    return (grayd < cubed) ? 232 + idx : cube;
}

/** Luminance folded onto the 24 step gray ramp, with true black and white */
static int kt_rgb_to_gray(int r, int g, int b) {
    int lum = (r * 299 + g * 587 + b * 114) / 1000;
    int idx;
    if (lum <= 4) { return 16; }
    if (lum >= 250) { return 231; }
    idx = (lum - 8 + 5) / 10;
    if (idx < 0) { idx = 0; }
    if (idx > 23) { idx = 23; }
    return 232 + idx;
}

static int kt_fold_color(KtFilter *f, int r, int g, int b) {
    if (f->color_mode == KT_COLOR_GRAY) { return kt_rgb_to_gray(r, g, b); }
    return ktfilter_rgb_to_256(r, g, b);
}

/* ------------------------------------------------------------------ */
/* small helpers                                                      */
/* ------------------------------------------------------------------ */

static void kt_log(KtFilter *f, const char *dir, const unsigned char *p, size_t n) {
    size_t i;
    if (!f->log) { return; }
    fprintf(f->log, "%s ", dir);
    for (i = 0; i < n; i++) {
        if (p[i] == 0x1b) { fputs("<ESC>", f->log); }
        else if (p[i] == 0x07) { fputs("<BEL>", f->log); }
        else if (p[i] >= 0x20 && p[i] < 0x7f) { fputc(p[i], f->log); }
        else { fprintf(f->log, "<%02x>", p[i]); }
    }
    fputc('\n', f->log);
    fflush(f->log);
}

/** Split a NUL terminated parameter string on `sep`, in place, into argv style */
static int kt_split(char *s, char sep, char **out, int max) {
    int n = 0;
    char *p = s;
    if (max <= 0) { return 0; }
    out[n++] = p;
    while (*p) {
        if (*p == sep) {
            *p = '\0';
            if (n < max) { out[n++] = p + 1; }
            else { return n; }
        }
        p++;
    }
    return n;
}

static int kt_atoi(const char *s) {
    int v = 0;
    if (!s || !*s) { return 0; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

static int kt_base64_decode(const char *in, size_t inlen, unsigned char *out, size_t outmax) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int acc = 0, bits = 0;
    size_t i, n = 0;
    for (i = 0; i < inlen; i++) {
        const char *q;
        if (in[i] == '=') { break; }
        q = memchr(tbl, in[i], 64);
        if (!q) { continue; }
        acc = (acc << 6) | (int) (q - tbl);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n < outmax) { out[n++] = (unsigned char) ((acc >> bits) & 0xff); }
        }
    }
    return (int) n;
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                          */
/* ------------------------------------------------------------------ */

void ktfilter_init(KtFilter *f) {
    memset(f, 0, sizeof(*f));
    ktbuf_init(&f->pending);
    ktbuf_init(&f->in_pending);
    f->state = KT_GROUND;
    f->color_mode = KT_COLOR_256;
    f->last_button = 0;
    strcpy(f->fg_spec, "rgb:0000/0000/0000");
    strcpy(f->bg_spec, "rgb:ffff/ffff/ffff");
}

void ktfilter_free(KtFilter *f) {
    ktbuf_free(&f->pending);
    ktbuf_free(&f->in_pending);
    free(f->clipboard_path);
    free(f->scheme_path);
    f->clipboard_path = NULL;
    f->scheme_path = NULL;
}

/* ------------------------------------------------------------------ */
/* CSI                                                                */
/* ------------------------------------------------------------------ */

/*
 * DECSET/DECRST parameters that VTE 0.28 has no entry for. Forwarding
 * them is mostly harmless (its decset handler ignores unknown numbers),
 * but several of them we need to intercept anyway to learn what the
 * application wanted, and dropping the rest keeps the log honest.
 */
static int kt_mode_unsupported(int mode) {
    switch (mode) {
        case 1004:  /* focus in/out reporting            */
        case 1005:  /* utf-8 extended mouse coordinates  */
        case 1006:  /* SGR mouse                         */
        case 1015:  /* urxvt mouse                       */
        case 1016:  /* SGR pixel mouse                   */
        case 2004:  /* bracketed paste                   */
        case 2026:  /* synchronised output               */
        case 2027:  /* grapheme clustering               */
        case 2048:  /* in-band resize notifications      */
        case 7727:  /* mintty application escape         */
        case 8452:  /* sixel scrolling cursor placement  */
        case 9001:  /* win32 input mode                  */
            return 1;
        default:
            return 0;
    }
}

static void kt_handle_decset(KtFilter *f, char *params, int set, KtBuf *out) {
    char *fields[32];
    char kept[KT_CSI_MAX];
    int n, i, nkept = 0;
    size_t klen = 0;

    kept[0] = '\0';
    n = kt_split(params, ';', fields, 32);
    for (i = 0; i < n; i++) {
        int mode = kt_atoi(fields[i]);
        switch (mode) {
            case 1006: f->mouse_sgr = set; break;
            case 1015: f->mouse_urxvt = set; break;
            case 1005: f->mouse_utf8 = set; break;
            case 1000: case 1002: case 1003: f->mouse_any = set; break;
            default: break;
        }
        if (kt_mode_unsupported(mode)) { continue; }
        if (nkept > 0 && klen + 1 < sizeof(kept)) { kept[klen++] = ';'; }
        klen += (size_t) snprintf(kept + klen, sizeof(kept) - klen, "%d", mode);
        if (klen >= sizeof(kept)) { klen = sizeof(kept) - 1; break; }
        nkept++;
    }
    kept[klen] = '\0';
    if (nkept == 0) { return; }
    ktbuf_adds(out, "\033[?");
    ktbuf_adds(out, kept);
    ktbuf_addc(out, set ? 'h' : 'l');
}

/*
 * SGR. Folds 24-bit colour down to an indexed colour VTE understands,
 * drops underline colour, and flattens colon subparameter forms that
 * VTE's parser would otherwise spill onto the screen.
 */
static void kt_handle_sgr(KtFilter *f, char *params, KtBuf *out) {
    char *fields[64];
    char rebuilt[KT_CSI_MAX * 2];
    int n, i, changed = 0, nout = 0;
    size_t rlen = 0;

    n = kt_split(params, ';', fields, 64);

    for (i = 0; i < n; i++) {
        char *fld = fields[i];
        char buf[32];
        const char *emit = fld;
        int base = kt_atoi(fld);

        if (strchr(fld, ':') != NULL) {
            /* colon subparameter form, self contained */
            char tmp[128];
            char *sub[8];
            int ns;
            snprintf(tmp, sizeof(tmp), "%s", fld);
            ns = kt_split(tmp, ':', sub, 8);
            if ((base == 38 || base == 48) && ns >= 2 && kt_atoi(sub[1]) == 2) {
                /* 38:2:r:g:b or the colourspace form 38:2::r:g:b */
                int off = (ns >= 6) ? 3 : 2;
                int r = kt_atoi(sub[off]);
                int g = (off + 1 < ns) ? kt_atoi(sub[off + 1]) : 0;
                int b = (off + 2 < ns) ? kt_atoi(sub[off + 2]) : 0;
                snprintf(buf, sizeof(buf), "%d;5;%d", base, kt_fold_color(f, r, g, b));
                emit = buf;
                changed = 1;
                f->n_truecolor++;
            } else if ((base == 38 || base == 48) && ns >= 3 && kt_atoi(sub[1]) == 5) {
                snprintf(buf, sizeof(buf), "%d;5;%d", base, kt_atoi(sub[2]));
                emit = buf;
                changed = 1;
            } else if (base == 58) {
                changed = 1;
                continue;   /* underline colour: no such thing in VTE 0.28 */
            } else if (base == 4) {
                /* 4:0 is "no underline", 4:1..4:5 are styles VTE cannot draw */
                emit = (kt_atoi(sub[1]) == 0) ? "24" : "4";
                changed = 1;
            } else {
                snprintf(buf, sizeof(buf), "%d", base);
                emit = buf;
                changed = 1;
            }
        } else if (base == 38 || base == 48 || base == 58) {
            int sel = (i + 1 < n) ? kt_atoi(fields[i + 1]) : -1;
            if (sel == 2 && i + 4 < n) {
                int r = kt_atoi(fields[i + 2]);
                int g = kt_atoi(fields[i + 3]);
                int b = kt_atoi(fields[i + 4]);
                i += 4;
                if (base == 58) { changed = 1; continue; }
                snprintf(buf, sizeof(buf), "%d;5;%d", base, kt_fold_color(f, r, g, b));
                emit = buf;
                changed = 1;
                f->n_truecolor++;
            } else if (sel == 5 && i + 2 < n) {
                int idx = kt_atoi(fields[i + 2]);
                i += 2;
                if (base == 58) { changed = 1; continue; }
                snprintf(buf, sizeof(buf), "%d;5;%d", base, idx);
                emit = buf;
            } else if (base == 58) {
                changed = 1;
                continue;
            }
        }

        if (nout > 0 && rlen + 1 < sizeof(rebuilt)) { rebuilt[rlen++] = ';'; }
        rlen += (size_t) snprintf(rebuilt + rlen, sizeof(rebuilt) - rlen, "%s", emit);
        if (rlen >= sizeof(rebuilt)) { rlen = sizeof(rebuilt) - 1; break; }
        nout++;
    }
    rebuilt[rlen] = '\0';

    if (!changed) {
        /* fast path: re-emit exactly what arrived */
        ktbuf_adds(out, "\033[");
        ktbuf_add(out, f->pending.data, f->pending.len);
        return;
    }
    ktbuf_adds(out, "\033[");
    ktbuf_adds(out, rebuilt);
    ktbuf_addc(out, 'm');
}

static void kt_handle_csi(KtFilter *f, KtBuf *out) {
    unsigned char *seq = f->pending.data;
    size_t len = f->pending.len;
    unsigned char final, priv = 0, inter = 0;
    char params[KT_CSI_MAX];
    size_t i = 0, plen = 0;

    if (len == 0) { return; }
    final = seq[len - 1];

    if (seq[0] == '<' || seq[0] == '=' || seq[0] == '>' || seq[0] == '?') {
        priv = seq[0];
        i = 1;
    }
    for (; i + 1 < len; i++) {
        if (seq[i] >= 0x20 && seq[i] <= 0x2f) { inter = seq[i]; continue; }
        if (plen + 1 < sizeof(params)) { params[plen++] = (char) seq[i]; }
    }
    params[plen] = '\0';

    /* sequences VTE 0.28 would print rather than execute */
    if (final == 'u' && priv != 0) { return; }                                    /* kitty keyboard */
    if (final == 'q' && inter == ' ') { return; }                                 /* DECSCUSR       */
    if (final == 'q' && priv == '>') { return; }                                  /* XTVERSION      */
    if (final == 'p' && inter == '$') { return; }                                 /* DECRQM/DECRQSS */
    if (final == 'c' && priv == '=') { return; }                                  /* DA3            */
    if (final == 'S' && priv == '?') { return; }                                  /* XTSMGRAPHICS   */
    if (final == 'm' && priv != 0) { return; }   /* XTMODKEYS: fish emits CSI > 4 ; 1 m at its prompt */
    if (final == 'n' && priv == '>') { return; }                                  /* XTDISABLEMODKEYS */
    if ((final == '}' || final == '~') && inter == '\'') { return; }              /* DECIC/DECDC    */
    if (final == 't') {
        int op = kt_atoi(params);
        if (op == 22 || op == 23) { return; }                                     /* title stack    */
    }

    if ((final == 'h' || final == 'l') && priv == '?') {
        kt_handle_decset(f, params, final == 'h', out);
        return;
    }
    if (final == 'm' && priv == 0) {
        kt_handle_sgr(f, params, out);
        return;
    }

    ktbuf_adds(out, "\033[");
    ktbuf_add(out, f->pending.data, f->pending.len);
}

/* ------------------------------------------------------------------ */
/* OSC                                                                */
/* ------------------------------------------------------------------ */

static void kt_write_clipboard(KtFilter *f, const char *b64, size_t n) {
    unsigned char *buf;
    int len;
    FILE *fp;
    if (!f->clipboard_path) { return; }
    buf = malloc(n + 1);
    if (!buf) { return; }
    len = kt_base64_decode(b64, n, buf, n);
    fp = fopen(f->clipboard_path, "wb");
    if (fp) {
        if (len > 0) { fwrite(buf, 1, (size_t) len, fp); }
        fclose(fp);
    }
    free(buf);
}

/**
 * Re-read the scheme kterm last painted. Reversing colours from the popup
 * menu has to change the answer we give to colour queries, and the child
 * process is long gone by then, so the state is passed through a file.
 */
static void kt_refresh_scheme(KtFilter *f) {
    char buf[32];
    FILE *fp;
    if (!f->scheme_path) { return; }
    if ((fp = fopen(f->scheme_path, "r")) == NULL) { return; }
    if (fgets(buf, sizeof(buf), fp)) {
        if (strncmp(buf, "dark", 4) == 0) {
            strcpy(f->bg_spec, "rgb:0000/0000/0000");
            strcpy(f->fg_spec, "rgb:ffff/ffff/ffff");
        } else if (strncmp(buf, "light", 5) == 0) {
            strcpy(f->bg_spec, "rgb:ffff/ffff/ffff");
            strcpy(f->fg_spec, "rgb:0000/0000/0000");
        }
    }
    fclose(fp);
}

static void kt_handle_osc(KtFilter *f, unsigned char term, KtBuf *out, KtBuf *reply) {
    char body[KT_CSI_MAX];
    size_t n = f->pending.len;
    const char *rest;
    int ps;
    size_t i = 0;

    /* leading numeric selector */
    ps = 0;
    while (i < n && f->pending.data[i] >= '0' && f->pending.data[i] <= '9') {
        ps = ps * 10 + (f->pending.data[i] - '0');
        i++;
    }
    if (i == 0) { ps = -1; }
    if (i < n && f->pending.data[i] == ';') { i++; }
    rest = (const char *) f->pending.data + i;

    switch (ps) {
        case 0: case 1: case 2:
            /* window title. VTE 0.28 only recognises the BEL terminated form. */
            snprintf(body, sizeof(body), "\033]%d;", ps);
            ktbuf_adds(out, body);
            ktbuf_add(out, rest, n - i);
            ktbuf_addc(out, 0x07);
            return;

        case 10: case 11: case 12:
            if (n > i && rest[0] == '?') {
                const char *spec;
                kt_refresh_scheme(f);
                spec = (ps == 11) ? f->bg_spec : f->fg_spec;
                snprintf(body, sizeof(body), "\033]%d;%s", ps, spec);
                ktbuf_adds(reply, body);
                if (term == 0x07) { ktbuf_addc(reply, 0x07); }
                else { ktbuf_adds(reply, "\033\\"); }
                f->n_color_queries++;
            }
            f->n_osc_dropped++;
            return;

        case 52: {
            /* OSC 52 ; <selection> ; <base64> - remote copy */
            const char *semi = memchr(rest, ';', n - i);
            if (semi) {
                size_t off = (size_t) (semi + 1 - rest);
                kt_write_clipboard(f, semi + 1, n - i - off);
            }
            f->n_osc_dropped++;
            return;
        }

        default:
            /* 7 (cwd), 8 (hyperlink), 9, 133 (prompt marks), 633, 777, 1337,
             * palette manipulation, and everything else we have never heard
             * of. All of it would land on the screen as text. */
            f->n_osc_dropped++;
            return;
    }
}

/* ------------------------------------------------------------------ */
/* DCS / APC / PM / SOS                                               */
/* ------------------------------------------------------------------ */

static void kt_handle_string(KtFilter *f, KtBuf *reply) {
    if (f->str_kind != 'P' || f->pending.len < 2) { return; }
    /* Politely refuse the two DCS queries applications actually block on. */
    if (f->pending.data[0] == '+' && f->pending.data[1] == 'q') {
        ktbuf_adds(reply, "\033P0+r\033\\");
    } else if (f->pending.data[0] == '$' && f->pending.data[1] == 'q') {
        ktbuf_adds(reply, "\033P0$r\033\\");
    }
}

/* ------------------------------------------------------------------ */
/* downstream: application -> terminal                                */
/* ------------------------------------------------------------------ */

void ktfilter_downstream(KtFilter *f, const unsigned char *in, size_t n,
                         KtBuf *out, KtBuf *reply) {
    size_t i;

    kt_log(f, "<<", in, n);

    for (i = 0; i < n; i++) {
        unsigned char c = in[i];

        switch (f->state) {
            case KT_GROUND:
                if (c == 0x1b) {
                    f->state = KT_ESC;
                    ktbuf_clear(&f->pending);
                } else {
                    ktbuf_addc(out, c);
                }
                break;

            case KT_ESC:
                switch (c) {
                    case '[': f->state = KT_CSI; ktbuf_clear(&f->pending); break;
                    case ']': f->state = KT_OSC; ktbuf_clear(&f->pending); f->esc_seen = 0; break;
                    case 'P': case '_': case '^': case 'X':
                        f->state = KT_STRING;
                        f->str_kind = c;
                        ktbuf_clear(&f->pending);
                        f->esc_seen = 0;
                        break;
                    case 0x1b:
                        ktbuf_addc(out, 0x1b);
                        break;          /* stay in KT_ESC */
                    default:
                        ktbuf_addc(out, 0x1b);
                        ktbuf_addc(out, c);
                        f->state = KT_GROUND;
                        break;
                }
                break;

            case KT_CSI:
                if (c < 0x20) {
                    /* C0 controls are executed in place, even mid sequence */
                    ktbuf_addc(out, c);
                    break;
                }
                ktbuf_addc(&f->pending, c);
                if (c >= 0x40 && c <= 0x7e) {
                    kt_handle_csi(f, out);
                    f->state = KT_GROUND;
                } else if (f->pending.len > KT_CSI_MAX) {
                    /* not a real sequence; give it back verbatim */
                    ktbuf_adds(out, "\033[");
                    ktbuf_add(out, f->pending.data, f->pending.len);
                    f->state = KT_GROUND;
                }
                break;

            case KT_OSC:
            case KT_STRING:
                if (f->esc_seen) {
                    f->esc_seen = 0;
                    if (c == '\\') {
                        if (f->state == KT_OSC) { kt_handle_osc(f, 0x1b, out, reply); }
                        else { kt_handle_string(f, reply); }
                        f->state = KT_GROUND;
                        break;
                    }
                    ktbuf_addc(&f->pending, 0x1b);
                    /* fall through and handle c normally */
                }
                if (c == 0x1b) {
                    f->esc_seen = 1;
                } else if (c == 0x07 && f->state == KT_OSC) {
                    kt_handle_osc(f, 0x07, out, reply);
                    f->state = KT_GROUND;
                } else if (c == 0x07 && f->state == KT_STRING) {
                    kt_handle_string(f, reply);
                    f->state = KT_GROUND;
                } else {
                    if (f->pending.len < KT_SEQ_MAX) { ktbuf_addc(&f->pending, c); }
                }
                break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* upstream: terminal -> application                                  */
/* ------------------------------------------------------------------ */

/*
 * VTE 0.28 predates SGR mouse reporting, so when an application asks for
 * DECSET 1006 it keeps sending the legacy X10 form:
 *
 *     ESC [ M  (32+button)  (32+col)  (32+row)
 *
 * Applications whose parser only knows SGR echo those bytes onto the
 * screen, which is exactly the garbage seen when tapping the Kindle.
 * Rewrite them into the SGR form the application asked for.
 */
static void kt_rewrite_mouse(KtFilter *f, const unsigned char *p, KtBuf *out) {
    int cb = p[3] - 32;
    int cx = p[4] - 32;
    int cy = p[5] - 32;
    int base = cb & 0x03;
    char buf[48];

    if (cb < 0) { cb = 0; }
    if (cx < 0) { cx = 0; }
    if (cy < 0) { cy = 0; }

    if (base == 3) {
        /* X10 does not say which button was released; remember the press */
        int b = (f->last_button & 0x03) | (cb & ~0x03);
        snprintf(buf, sizeof(buf), "\033[<%d;%d;%dm", b, cx, cy);
    } else {
        if (!(cb & 32) && !(cb & 64)) { f->last_button = cb; }
        snprintf(buf, sizeof(buf), "\033[<%d;%d;%dM", cb, cx, cy);
    }
    ktbuf_adds(out, buf);
    f->n_mouse_rewritten++;
}

void ktfilter_upstream(KtFilter *f, const unsigned char *in, size_t n, KtBuf *out) {
    const unsigned char *p;
    size_t i = 0, len;

    kt_log(f, ">>", in, n);

    /* When the application is happy with what VTE sends, stay out of the
     * way entirely - no buffering, no added latency on keystrokes. */
    if (!f->mouse_sgr && !f->mouse_urxvt && f->in_pending.len == 0) {
        ktbuf_add(out, in, n);
        return;
    }

    ktbuf_add(&f->in_pending, in, n);
    p = f->in_pending.data;
    len = f->in_pending.len;

    while (i < len) {
        size_t avail = len - i;
        if (p[i] != 0x1b) {
            ktbuf_addc(out, p[i]);
            i++;
            continue;
        }
        if (!f->mouse_sgr && !f->mouse_urxvt) {
            ktbuf_addc(out, p[i]);
            i++;
            continue;
        }
        if (avail >= 3 && (p[i + 1] != '[' || p[i + 2] != 'M')) {
            ktbuf_addc(out, p[i]);
            i++;
            continue;
        }
        if (avail == 2 && p[i + 1] != '[') {
            ktbuf_addc(out, p[i]);
            i++;
            continue;
        }
        if (avail < 6) { break; }   /* viable prefix, wait for the rest */
        kt_rewrite_mouse(f, p + i, out);
        i += 6;
    }
    ktbuf_consume(&f->in_pending, i);
}

void ktfilter_upstream_flush(KtFilter *f, KtBuf *out) {
    if (f->in_pending.len == 0) { return; }
    ktbuf_add(out, f->in_pending.data, f->in_pending.len);
    ktbuf_clear(&f->in_pending);
}

int ktfilter_upstream_pending(const KtFilter *f) {
    return f->in_pending.len > 0;
}
