/*
 * ktsh_test.c - byte level tests for the ktsh escape sequence filter
 *
 * Runs on any POSIX host; no Kindle, no GTK, no VTE required.
 *
 * Copyright (C) 2026 kterm contributors
 * Licensed under the GNU General Public License v3 (see COPYING).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ktsh.h"

static int failures = 0;
static int checks = 0;

static const char *vis(const unsigned char *p, size_t n) {
    static char out[2048];
    size_t i, o = 0;
    for (i = 0; i < n && o + 8 < sizeof(out); i++) {
        if (p[i] == 0x1b) { o += (size_t) sprintf(out + o, "<ESC>"); }
        else if (p[i] == 0x07) { o += (size_t) sprintf(out + o, "<BEL>"); }
        else if (p[i] >= 0x20 && p[i] < 0x7f) { out[o++] = (char) p[i]; }
        else { o += (size_t) sprintf(out + o, "<%02x>", p[i]); }
    }
    out[o] = '\0';
    return out;
}

static void expect(const char *name, const KtBuf *got, const char *want) {
    size_t wlen = strlen(want);
    checks++;
    if (got->len == wlen && (wlen == 0 || memcmp(got->data, want, wlen) == 0)) {
        printf("  ok   %s\n", name);
        return;
    }
    failures++;
    printf("  FAIL %s\n", name);
    printf("       want: %s\n", vis((const unsigned char *) want, wlen));
    printf("       got : %s\n", vis(got->data, got->len));
}

static void expect_int(const char *name, long got, long want) {
    checks++;
    if (got == want) { printf("  ok   %s\n", name); return; }
    failures++;
    printf("  FAIL %s: want %ld, got %ld\n", name, want, got);
}

/* ------------------------------------------------------------------ */

/** Feed a downstream string; returns terminal-bound output in `out`. */
static void down(KtFilter *f, const char *s, KtBuf *out, KtBuf *reply) {
    ktfilter_downstream(f, (const unsigned char *) s, strlen(s), out, reply);
}

static void up(KtFilter *f, const char *s, size_t n, KtBuf *out) {
    ktfilter_upstream(f, (const unsigned char *) s, n, out);
}

/* ------------------------------------------------------------------ */

static void test_shell_integration_osc(void) {
    KtFilter f;
    KtBuf out, reply;
    printf("shell integration OSC (the fish/zsh tofu)\n");
    ktfilter_init(&f);
    ktbuf_init(&out); ktbuf_init(&reply);

    /* A real fish 4.x prompt: OSC 7 cwd, OSC 133 prompt marks, then text. */
    down(&f, "\033]7;file://trashcan/home/charlesji\033\\"
             "\033]133;A;click_events=1\007"
             "~@trashcan > "
             "\033]133;B\007", &out, &reply);
    expect("osc 7 and 133 removed, prompt text kept", &out, "~@trashcan > ");
    expect("nothing owed back to the app", &reply, "");
    expect_int("three OSC dropped", (long) f.n_osc_dropped, 3);

    ktbuf_free(&out); ktbuf_free(&reply); ktfilter_free(&f);
}

static void test_color_query(void) {
    KtFilter f;
    KtBuf out, reply;
    printf("background colour query (the light/dark mode fix)\n");

    ktfilter_init(&f);
    ktbuf_init(&out); ktbuf_init(&reply);
    down(&f, "\033]11;?\033\\", &out, &reply);
    expect("query produces no screen output", &out, "");
    expect("ST query answered with ST", &reply, "\033]11;rgb:ffff/ffff/ffff\033\\");
    ktbuf_free(&out); ktbuf_free(&reply); ktfilter_free(&f);

    ktfilter_init(&f);
    ktbuf_init(&out); ktbuf_init(&reply);
    down(&f, "\033]11;?\007", &out, &reply);
    expect("BEL query answered with BEL", &reply, "\033]11;rgb:ffff/ffff/ffff\007");
    ktbuf_free(&out); ktbuf_free(&reply); ktfilter_free(&f);

    ktfilter_init(&f);
    strcpy(f.bg_spec, "rgb:0000/0000/0000");
    strcpy(f.fg_spec, "rgb:ffff/ffff/ffff");
    ktbuf_init(&out); ktbuf_init(&reply);
    down(&f, "\033]10;?\007", &out, &reply);
    expect("dark scheme reports white foreground", &reply, "\033]10;rgb:ffff/ffff/ffff\007");
    ktbuf_free(&out); ktbuf_free(&reply); ktfilter_free(&f);
}

static void test_title_and_hyperlink(void) {
    KtFilter f;
    KtBuf out, reply;
    printf("titles and hyperlinks\n");
    ktfilter_init(&f);
    ktbuf_init(&out); ktbuf_init(&reply);

    down(&f, "\033]0;my title\033\\", &out, &reply);
    expect("ST terminated title rewritten to BEL form", &out, "\033]0;my title\007");

    ktbuf_clear(&out);
    down(&f, "\033]8;;https://example.com\033\\click me\033]8;;\033\\", &out, &reply);
    expect("hyperlink wrapper stripped, label kept", &out, "click me");

    ktbuf_free(&out); ktbuf_free(&reply); ktfilter_free(&f);
}

static void test_truecolor(void) {
    KtFilter f;
    KtBuf out, reply;
    printf("24-bit colour folding\n");
    ktfilter_init(&f);
    ktbuf_init(&out); ktbuf_init(&reply);

    down(&f, "\033[38;2;255;0;0mred", &out, &reply);
    expect("semicolon truecolor folded to indexed", &out, "\033[38;5;196mred");

    ktbuf_clear(&out);
    down(&f, "\033[1;38;2;0;255;0;48;2;0;0;0mx", &out, &reply);
    expect("mixed attributes preserved around folding",
           &out, "\033[1;38;5;46;48;5;16mx");

    ktbuf_clear(&out);
    down(&f, "\033[38:2::255:0:0mz", &out, &reply);
    expect("colon colourspace form folded", &out, "\033[38;5;196mz");

    ktbuf_clear(&out);
    down(&f, "\033[38:2:0:0:255mz", &out, &reply);
    expect("colon 4-arg form folded", &out, "\033[38;5;21mz");

    ktbuf_clear(&out);
    down(&f, "\033[0m\033[31mplain", &out, &reply);
    expect("ordinary SGR passes through untouched", &out, "\033[0m\033[31mplain");

    ktbuf_clear(&out);
    down(&f, "\033[4:3;58;2;255;0;0munder", &out, &reply);
    expect("curly underline flattened, underline colour dropped",
           &out, "\033[4munder");

    expect_int("truecolor conversions counted", (long) f.n_truecolor, 5);
    ktbuf_free(&out); ktbuf_free(&reply); ktfilter_free(&f);
}

static void test_decset(void) {
    KtFilter f;
    KtBuf out, reply;
    printf("mode negotiation\n");
    ktfilter_init(&f);
    ktbuf_init(&out); ktbuf_init(&reply);

    /* What nvim and Claude Code actually send on startup. */
    down(&f, "\033[?1000;1002;1006h", &out, &reply);
    expect("SGR request swallowed, legacy modes forwarded to VTE",
           &out, "\033[?1000;1002h");
    expect_int("SGR mouse recorded", f.mouse_sgr, 1);
    expect_int("legacy mouse recorded", f.mouse_any, 1);

    ktbuf_clear(&out);
    down(&f, "\033[?2004h\033[?2026h", &out, &reply);
    expect("bracketed paste and sync output dropped entirely", &out, "");

    ktbuf_clear(&out);
    down(&f, "\033[?1049h", &out, &reply);
    expect("alternate screen still forwarded", &out, "\033[?1049h");

    ktbuf_clear(&out);
    down(&f, "\033[?1006l", &out, &reply);
    expect_int("SGR mouse cleared", f.mouse_sgr, 0);

    ktbuf_free(&out); ktbuf_free(&reply); ktfilter_free(&f);
}

static void test_modern_csi(void) {
    KtFilter f;
    KtBuf out, reply;
    printf("post-2011 CSI sequences\n");
    ktfilter_init(&f);
    ktbuf_init(&out); ktbuf_init(&reply);

    down(&f, "\033[>1u" "hello" "\033[<u", &out, &reply);
    expect("kitty keyboard protocol removed", &out, "hello");

    ktbuf_clear(&out);
    down(&f, "\033[2 q" "x", &out, &reply);
    expect("DECSCUSR cursor style removed", &out, "x");

    ktbuf_clear(&out);
    down(&f, "\033[22;0t" "y" "\033[23;0t", &out, &reply);
    expect("title stack push/pop removed", &out, "y");

    /* fish 4.x emits this at every prompt; vte 0.28 typed it onto the screen */
    ktbuf_clear(&out);
    down(&f, "\033[>4;1m~@trashcan > ", &out, &reply);
    expect("XTMODKEYS removed, prompt intact", &out, "~@trashcan > ");

    ktbuf_clear(&out);
    down(&f, "\033[>4;0m\033[>n", &out, &reply);
    expect("XTMODKEYS reset and query removed", &out, "");

    ktbuf_clear(&out);
    down(&f, "\033[2J\033[H\033[1;5H", &out, &reply);
    expect("ordinary CSI untouched", &out, "\033[2J\033[H\033[1;5H");

    ktbuf_clear(&out);
    down(&f, "\033[0;1;31mstill colours\033[m", &out, &reply);
    expect("plain SGR still passes", &out, "\033[0;1;31mstill colours\033[m");

    ktbuf_free(&out); ktbuf_free(&reply); ktfilter_free(&f);
}

static void test_dcs(void) {
    KtFilter f;
    KtBuf out, reply;
    printf("DCS and friends\n");
    ktfilter_init(&f);
    ktbuf_init(&out); ktbuf_init(&reply);

    down(&f, "\033P+q544e\033\\visible", &out, &reply);
    expect("XTGETTCAP body removed", &out, "visible");
    expect("XTGETTCAP politely refused", &reply, "\033P0+r\033\\");

    ktbuf_clear(&out); ktbuf_clear(&reply);
    down(&f, "\033_Gf=100,a=T;AAAA\033\\ok", &out, &reply);
    expect("kitty graphics APC removed", &out, "ok");
    expect("no reply owed for APC", &reply, "");

    ktbuf_free(&out); ktbuf_free(&reply); ktfilter_free(&f);
}

static void test_mouse_translation(void) {
    KtFilter f;
    KtBuf out;
    printf("touch: X10 mouse reports rewritten to SGR\n");
    ktfilter_init(&f);
    ktbuf_init(&out);

    /* Typing is untouched while no application has asked for SGR. */
    up(&f, "ls -la\r", 7, &out);
    expect("keystrokes pass through when SGR is off", &out, "ls -la\r");

    f.mouse_sgr = 1;

    /* This is the exact byte pattern behind the garbage the user saw:
     * press at column 63 row 34, then release. */
    ktbuf_clear(&out);
    up(&f, "\033[M \x5f\x42", 6, &out);
    expect("button 1 press becomes SGR press", &out, "\033[<0;63;34M");

    ktbuf_clear(&out);
    up(&f, "\033[M#\x5f\x42", 6, &out);
    expect("release keeps the button that was pressed", &out, "\033[<0;63;34m");

    ktbuf_clear(&out);
    up(&f, "\033[M\x61\x67\x32", 6, &out);
    expect("wheel-up report translated", &out, "\033[<65;71;18M");

    expect_int("three reports rewritten", (long) f.n_mouse_rewritten, 3);

    ktbuf_free(&out); ktfilter_free(&f);
}

static void test_escape_key_not_swallowed(void) {
    KtFilter f;
    KtBuf out;
    printf("a lone ESC still reaches the application\n");
    ktfilter_init(&f);
    ktbuf_init(&out);
    f.mouse_sgr = 1;

    up(&f, "\033", 1, &out);
    expect("lone ESC is held while it might be a report", &out, "");
    expect_int("filter reports pending input", ktfilter_upstream_pending(&f), 1);

    ktfilter_upstream_flush(&f, &out);
    expect("idle flush releases it verbatim", &out, "\033");
    expect_int("nothing left pending", ktfilter_upstream_pending(&f), 0);

    /* An arrow key must not be mistaken for the start of a report. */
    ktbuf_clear(&out);
    up(&f, "\033[A", 3, &out);
    expect("arrow key passes straight through", &out, "\033[A");

    ktbuf_free(&out); ktfilter_free(&f);
}

static void test_split_writes(void) {
    KtFilter f;
    KtBuf out, reply;
    printf("sequences split across reads\n");
    ktfilter_init(&f);
    ktbuf_init(&out); ktbuf_init(&reply);

    down(&f, "\033]13", &out, &reply);
    down(&f, "3;A\007te", &out, &reply);
    down(&f, "xt", &out, &reply);
    expect("OSC split over three reads still removed", &out, "text");

    ktbuf_clear(&out);
    down(&f, "\033[38;2;", &out, &reply);
    down(&f, "255;0;0m!", &out, &reply);
    expect("SGR split mid-parameters still folded", &out, "\033[38;5;196m!");

    ktbuf_free(&out); ktbuf_free(&reply); ktfilter_free(&f);

    /* Same for the upstream mouse report. */
    ktfilter_init(&f);
    ktbuf_init(&out);
    f.mouse_sgr = 1;
    up(&f, "\033[M", 3, &out);
    expect("partial report held", &out, "");
    up(&f, " \x5f\x42", 3, &out);
    expect("completed report translated", &out, "\033[<0;63;34M");
    ktbuf_free(&out); ktfilter_free(&f);
}

static void test_clipboard(void) {
    KtFilter f;
    KtBuf out, reply;
    char buf[64];
    FILE *fp;
    printf("OSC 52 clipboard export\n");
    ktfilter_init(&f);
    f.clipboard_path = strdup("/tmp/ktsh-test-clip");
    remove(f.clipboard_path);
    ktbuf_init(&out); ktbuf_init(&reply);

    /* base64 of "hello kterm" */
    down(&f, "\033]52;c;aGVsbG8ga3Rlcm0=\007", &out, &reply);
    expect("clipboard sequence leaves no screen output", &out, "");
    buf[0] = '\0';
    if ((fp = fopen(f.clipboard_path, "r")) != NULL) {
        size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
        buf[n] = '\0';
        fclose(fp);
    }
    expect_int("payload decoded and stored", strcmp(buf, "hello kterm") == 0, 1);
    expect_int("clipboard write counted", (long) f.n_clipboard, 1);

    /* a query must not clobber what is already there */
    down(&f, "\033]52;c;?\007", &out, &reply);
    expect_int("query did not overwrite", (long) f.n_clipboard, 1);

    remove(f.clipboard_path);
    ktbuf_free(&out); ktbuf_free(&reply); ktfilter_free(&f);
}

static void test_gray_mode(void) {
    KtFilter f;
    KtBuf out, reply;
    printf("gray folding mode\n");
    ktfilter_init(&f);
    f.color_mode = KT_COLOR_GRAY;
    ktbuf_init(&out); ktbuf_init(&reply);

    down(&f, "\033[38;2;255;255;255mW", &out, &reply);
    expect("white folds to the white slot", &out, "\033[38;5;231mW");

    ktbuf_clear(&out);
    down(&f, "\033[38;2;0;0;0mB", &out, &reply);
    expect("black folds to the black slot", &out, "\033[38;5;16mB");

    ktbuf_free(&out); ktbuf_free(&reply); ktfilter_free(&f);
}

int main(void) {
    printf("ktsh filter tests\n\n");
    test_shell_integration_osc();
    test_color_query();
    test_title_and_hyperlink();
    test_truecolor();
    test_decset();
    test_modern_csi();
    test_dcs();
    test_mouse_translation();
    test_escape_key_not_swallowed();
    test_split_writes();
    test_clipboard();
    test_gray_mode();
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
