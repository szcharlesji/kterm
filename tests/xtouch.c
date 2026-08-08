/* xtouch - inject synthetic touch gestures through XTest, so kterm's touch
 * handling can be exercised without a finger.
 *
 *   xtouch tap  X Y
 *   xtouch hold X Y MS          press, wait, release (triggers precise mode)
 *   xtouch drag X1 Y1 X2 Y2     press, glide, release
 *   xtouch hdrag X1 Y1 X2 Y2 MS hold first, then drag (precise drag)
 */
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* declared here so the XTest headers are not needed in the sysroot */
extern int XTestFakeButtonEvent(Display *, unsigned int, int, unsigned long);
extern int XTestFakeMotionEvent(Display *, int, int, int, unsigned long);

static Display *dpy;

static void move(int x, int y) {
    XTestFakeMotionEvent(dpy, -1, x, y, 0);
    XFlush(dpy);
    usleep(15000);
}

static void button(int press) {
    XTestFakeButtonEvent(dpy, 1, press, 0);
    XFlush(dpy);
    usleep(30000);
}

static void glide(int x1, int y1, int x2, int y2) {
    int i, steps = 14;
    for (i = 1; i <= steps; i++) {
        move(x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps);
    }
}

int main(int argc, char **argv) {
    const char *op;
    if (argc < 4) { fprintf(stderr, "usage: xtouch tap|hold|drag|hdrag ...\n"); return 2; }
    if ((dpy = XOpenDisplay(NULL)) == NULL) { fprintf(stderr, "no display\n"); return 1; }
    op = argv[1];

    if (!strcmp(op, "tap") && argc >= 4) {
        move(atoi(argv[2]), atoi(argv[3]));
        button(1); usleep(60000); button(0);
    } else if (!strcmp(op, "hold") && argc >= 5) {
        move(atoi(argv[2]), atoi(argv[3]));
        button(1); usleep((useconds_t) atoi(argv[4]) * 1000); button(0);
    } else if (!strcmp(op, "drag") && argc >= 6) {
        move(atoi(argv[2]), atoi(argv[3]));
        button(1);
        glide(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), atoi(argv[5]));
        button(0);
    } else if (!strcmp(op, "hdrag") && argc >= 7) {
        move(atoi(argv[2]), atoi(argv[3]));
        button(1);
        usleep((useconds_t) atoi(argv[6]) * 1000);   /* let the hold engage */
        glide(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), atoi(argv[5]));
        button(0);
    } else {
        fprintf(stderr, "bad arguments\n");
        return 2;
    }
    XFlush(dpy);
    XCloseDisplay(dpy);
    return 0;
}
