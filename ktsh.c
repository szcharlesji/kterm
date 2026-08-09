/*
 * ktsh.c - kterm shell shim, pty plumbing
 *
 * Runs a child shell on its own pty and pumps bytes between that pty and
 * the terminal kterm gave us, passing everything through ktfilter.
 *
 * Copyright (C) 2026 kterm contributors
 * Licensed under the GNU General Public License v3 (see COPYING).
 */

#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#else
#define _XOPEN_SOURCE 600
#define _DEFAULT_SOURCE 1
#define _BSD_SOURCE 1
#endif

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "ktsh.h"

#ifndef VERSION
#define VERSION "2.7"
#endif

#define KT_READ_SIZE 8192
#define KT_IDLE_MS   20     /* how long a lone ESC may be held back */

static struct termios saved_tio;
static int tio_saved = 0;
static volatile sig_atomic_t got_winch = 0;
static volatile sig_atomic_t got_chld = 0;

static void restore_tio(void) {
    if (tio_saved) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_tio);
        tio_saved = 0;
    }
}

static void on_winch(int sig) { (void) sig; got_winch = 1; }
static void on_chld(int sig)  { (void) sig; got_chld = 1; }

static void on_fatal(int sig) {
    restore_tio();
    signal(sig, SIG_DFL);
    raise(sig);
}

/** write() that survives partial writes and signals */
static int write_all(int fd, const unsigned char *buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, buf + off, n - off);
        if (w < 0) {
            if (errno == EINTR) { continue; }
            if (errno == EAGAIN) { continue; }
            return -1;
        }
        off += (size_t) w;
    }
    return 0;
}

static int flush_buf(int fd, KtBuf *b) {
    int rc = 0;
    if (b->len) { rc = write_all(fd, b->data, b->len); }
    ktbuf_clear(b);
    return rc;
}

static void copy_winsize(int from, int to) {
    struct winsize ws;
    if (ioctl(from, TIOCGWINSZ, &ws) != 0) { return; }
    /* kterm spawns us before the widget has been sized, so the first read
     * can legitimately be 0x0; give the shell something usable until the
     * real SIGWINCH arrives rather than a zero width terminal */
    if (ws.ws_col == 0) { ws.ws_col = 80; }
    if (ws.ws_row == 0) { ws.ws_row = 24; }
    ioctl(to, TIOCSWINSZ, &ws);
}

static void set_raw(void) {
    struct termios t;
    if (!isatty(STDIN_FILENO)) { return; }
    if (tcgetattr(STDIN_FILENO, &saved_tio) != 0) { return; }
    tio_saved = 1;
    atexit(restore_tio);
    t = saved_tio;
    t.c_iflag &= (tcflag_t) ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    t.c_oflag &= (tcflag_t) ~OPOST;
    t.c_lflag &= (tcflag_t) ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t.c_cflag &= (tcflag_t) ~(CSIZE | PARENB);
    t.c_cflag |= CS8;
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
}

static void usage(void) {
    printf("Usage: ktsh [OPTIONS] [--] [command [args...]]\n");
    printf("        -b <light|dark>  background the terminal actually has\n");
    printf("        -c <mode>        truecolor folding: 256 (default), gray, keep\n");
    printf("        -C <path>        file to receive OSC 52 clipboard writes\n");
    printf("        -d <path>        log both byte streams to path\n");
    printf("        -G <0|1>         kitty graphics support (default 1)\n");
    printf("        -s <path>        file kterm records the live scheme in\n");
    printf("        -h               show this message\n");
    printf("        -v               print version and exit\n");
    printf("\n");
    printf("Set KTSH_DISABLE=1 to bypass the filter entirely.\n");
    exit(0);
}

int main(int argc, char **argv) {
    KtFilter filter;
    KtBuf to_term, to_app;
    char *shell_argv[3];
    char **cmd;
    const char *logpath = NULL, *clip = NULL, *schemefile = NULL;
    const char *scheme = getenv("KTERM_SCHEME");
    int master, slave, i, status = 0;
    pid_t pid;
    const char *ptspath;

    ktfilter_init(&filter);

    /* ---- arguments ---- */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) { i++; break; }
        if (argv[i][0] != '-' || argv[i][1] == '\0') { break; }
        if (strcmp(argv[i], "-h") == 0) { usage(); }
        if (strcmp(argv[i], "-v") == 0) { printf("ktsh %s\n", VERSION); return 0; }
        if (i + 1 >= argc) { fprintf(stderr, "ktsh: %s needs an argument\n", argv[i]); return 2; }
        if (strcmp(argv[i], "-b") == 0) { scheme = argv[++i]; continue; }
        if (strcmp(argv[i], "-d") == 0) { logpath = argv[++i]; continue; }
        if (strcmp(argv[i], "-C") == 0) { clip = argv[++i]; continue; }
        if (strcmp(argv[i], "-s") == 0) { schemefile = argv[++i]; continue; }
        if (strcmp(argv[i], "-G") == 0) { filter.graphics = atoi(argv[++i]) != 0; continue; }
        if (strcmp(argv[i], "-c") == 0) {
            const char *m = argv[++i];
            if (strcmp(m, "gray") == 0) { filter.color_mode = KT_COLOR_GRAY; }
            else if (strcmp(m, "keep") == 0) { filter.color_mode = KT_COLOR_KEEP; }
            else { filter.color_mode = KT_COLOR_256; }
            continue;
        }
        fprintf(stderr, "ktsh: unknown option %s\n", argv[i]);
        return 2;
    }

    if (i < argc) {
        cmd = argv + i;
    } else {
        const char *sh = getenv("SHELL");
        if (!sh || !*sh) { sh = "/bin/sh"; }
        shell_argv[0] = (char *) sh;
        shell_argv[1] = NULL;
        cmd = shell_argv;
    }

    if (getenv("KTSH_DISABLE")) {
        execvp(cmd[0], cmd);
        perror("ktsh: exec");
        return 127;
    }

    /* The colours kterm actually painted, reported to anyone who asks. */
    if (scheme && strcmp(scheme, "dark") == 0) {
        strcpy(filter.bg_spec, "rgb:0000/0000/0000");
        strcpy(filter.fg_spec, "rgb:ffff/ffff/ffff");
    }
    if (clip) { filter.clipboard_path = strdup(clip); }
    if (schemefile) { filter.scheme_path = strdup(schemefile); }
    if (logpath) {
        filter.log = fopen(logpath, "a");
        if (filter.log) { fprintf(filter.log, "--- ktsh %s start ---\n", VERSION); }
    }

    /* ---- pty ---- */
    master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) {
        perror("ktsh: openpt");
        return 1;
    }
    ptspath = ptsname(master);
    if (!ptspath) { perror("ktsh: ptsname"); return 1; }

    signal(SIGWINCH, on_winch);
    signal(SIGCHLD, on_chld);
    signal(SIGHUP, on_fatal);
    signal(SIGTERM, on_fatal);
    signal(SIGPIPE, SIG_IGN);

    pid = fork();
    if (pid < 0) { perror("ktsh: fork"); return 1; }

    if (pid == 0) {
        /* ---- child: become session leader on the new pty ---- */
        setsid();
        slave = open(ptspath, O_RDWR);
        if (slave < 0) { perror("ktsh: open pts"); _exit(127); }
#ifdef TIOCSCTTY
        ioctl(slave, TIOCSCTTY, 0);
#endif
        copy_winsize(STDIN_FILENO, slave);
        dup2(slave, STDIN_FILENO);
        dup2(slave, STDOUT_FILENO);
        dup2(slave, STDERR_FILENO);
        if (slave > STDERR_FILENO) { close(slave); }
        close(master);
        signal(SIGWINCH, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        execvp(cmd[0], cmd);
        perror("ktsh: exec");
        _exit(127);
    }

    /* ---- parent: pump ---- */
    set_raw();
    copy_winsize(STDIN_FILENO, master);
    ktbuf_init(&to_term);
    ktbuf_init(&to_app);

    for (;;) {
        struct pollfd pfd[2];
        unsigned char buf[KT_READ_SIZE];
        int nfds = 0, timeout, rc;
        int stdin_slot = -1, master_slot;

        if (got_winch) {
            got_winch = 0;
            copy_winsize(STDIN_FILENO, master);
        }

        pfd[nfds].fd = master;
        pfd[nfds].events = POLLIN;
        master_slot = nfds++;

        pfd[nfds].fd = STDIN_FILENO;
        pfd[nfds].events = POLLIN;
        stdin_slot = nfds++;

        timeout = ktfilter_upstream_pending(&filter) ? KT_IDLE_MS : -1;
        rc = poll(pfd, (nfds_t) nfds, timeout);

        if (rc < 0) {
            /* SIGCHLD and SIGWINCH both land here; the child dying is handled
             * by the drain loop below, which reads whatever it wrote last */
            if (errno == EINTR) {
                if (got_chld) { break; }
                continue;
            }
            break;
        }
        if (rc == 0) {
            /* input went quiet: whatever we held back was not a mouse report */
            ktfilter_upstream_flush(&filter, &to_app);
            if (flush_buf(master, &to_app) < 0) { break; }
            continue;
        }

        if (pfd[master_slot].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t n = read(master, buf, sizeof(buf));
            if (n > 0) {
                ktfilter_downstream(&filter, buf, (size_t) n, &to_term, &to_app);
                if (flush_buf(STDOUT_FILENO, &to_term) < 0) { break; }
                if (flush_buf(master, &to_app) < 0) { break; }
            } else if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN)) {
                break;  /* child closed the pty */
            }
        }

        if (stdin_slot >= 0 && (pfd[stdin_slot].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n > 0) {
                ktfilter_upstream(&filter, buf, (size_t) n, &to_app);
                if (flush_buf(master, &to_app) < 0) { break; }
            } else if (n == 0) {
                /* our terminal went away; let the child notice via SIGHUP */
                close(master);
                break;
            }
        }
    }

    /* Drain anything the child wrote just before exiting. */
    for (;;) {
        unsigned char buf[KT_READ_SIZE];
        ssize_t n;
        struct pollfd pfd;
        pfd.fd = master;
        pfd.events = POLLIN;
        if (poll(&pfd, 1, 50) <= 0) { break; }
        n = read(master, buf, sizeof(buf));
        if (n <= 0) { break; }
        ktfilter_downstream(&filter, buf, (size_t) n, &to_term, &to_app);
        flush_buf(STDOUT_FILENO, &to_term);
    }

    restore_tio();
    kill(pid, SIGHUP);
    if (waitpid(pid, &status, 0) < 0) { status = 0; }

    if (filter.log) {
        fprintf(filter.log,
                "--- ktsh exit: osc_dropped=%lu color_queries=%lu mouse_rewritten=%lu truecolor=%lu ---\n",
                filter.n_osc_dropped, filter.n_color_queries,
                filter.n_mouse_rewritten, filter.n_truecolor);
        fclose(filter.log);
    }
    ktbuf_free(&to_term);
    ktbuf_free(&to_app);
    ktfilter_free(&filter);

    if (WIFEXITED(status)) { return WEXITSTATUS(status); }
    return 1;
}
