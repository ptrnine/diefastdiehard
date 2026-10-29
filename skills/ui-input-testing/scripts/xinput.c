/*
 * xinput: synthetic X input helpers for diefastdiehard UI testing.
 * Usually driven through ui.sh; uses $DISPLAY.
 *
 * Build:  gcc -o xinput xinput.c -lX11
 *
 * Usage:
 *   xinput wins                        mapped root children > 1x1: "0xID WxH X Y"
 *   xinput place <win> <x> <y>         move a window (root coords; no WM on Xvfb)
 *   xinput move  <win> <x> <y>         MotionNotify (window-relative)
 *   xinput click <win> <x> <y> [btn]   motion, 80ms, press, 80ms, release
 *   xinput key   <win> <keysym>...     press+release each key: X keysym names
 *                                      ("grave", "Return", "Escape", "Up", "a")
 *   xinput keydown|keyup <win> <keysym>...  press or release only (hold a key)
 *   xinput type  <win> <text>          type text; Shift is set for shifted chars
 *   xinput keysym <key>                print the server keycode for a key
 *
 * Why the click recipe is exactly motion -> 80ms -> press -> 80ms -> release:
 * SFML only feeds nk_input_motion from mouse moves (nk_input_button does not
 * update the mouse position), and NK_BUTTON_DEFAULT fires on release with
 * down && clicked, so press and release must span different frames.
 * Events go through XSendEvent with send_event=True to the exact window —
 * XTEST fake button events misroute on this setup.
 */
#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static KeySym key_to_sym(const char* s) {
    /* lowercase aliases kept for old command lines */
    if (strcmp(s, "return") == 0)
        return XK_Return;
    if (strcmp(s, "escape") == 0)
        return XK_Escape;
    if (strlen(s) == 1)
        return (KeySym)(unsigned char)s[0];
    return XStringToKeysym(s);
}

static void send_event(Display* d, Window win, int type, int x, int y, unsigned code, unsigned state, long mask) {
    XEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.xany.type       = type;
    ev.xany.send_event = True;
    ev.xany.display    = d;
    ev.xany.window     = win;
    if (type == MotionNotify) {
        ev.xmotion.x           = x;
        ev.xmotion.y           = y;
        ev.xmotion.time        = CurrentTime;
        ev.xmotion.same_screen = True;
    }
    else if (type == ButtonPress || type == ButtonRelease) {
        ev.xbutton.x           = x;
        ev.xbutton.y           = y;
        ev.xbutton.button      = code;
        ev.xbutton.time        = CurrentTime;
        ev.xbutton.same_screen = True;
    }
    else if (type == KeyPress || type == KeyRelease) {
        ev.xkey.keycode     = code;
        ev.xkey.state       = state;
        ev.xkey.time        = CurrentTime;
        ev.xkey.same_screen = True;
    }
    XSendEvent(d, win, True, mask, &ev);
    XFlush(d);
}

/* press: 1 = press, 0 = release, -1 = both */
static int send_key(Display* d, Window win, KeySym sym, const char* name, int press) {
    KeyCode code = XKeysymToKeycode(d, sym);
    if (code == 0) {
        fprintf(stderr, "no keycode for key %s\n", name);
        return 1;
    }
    /* the keysym is on level 1 of its keycode -> it needs Shift */
    unsigned state = XkbKeycodeToKeysym(d, code, 0, 0) == sym ? 0 : ShiftMask;
    if (press != 0) {
        send_event(d, win, KeyPress, 0, 0, code, state, KeyPressMask);
        usleep(30000);
    }
    if (press != 1) {
        send_event(d, win, KeyRelease, 0, 0, code, state, KeyReleaseMask);
        usleep(30000);
    }
    return 0;
}

/* A click that quits the app destroys the window before the release is sent:
 * report it instead of Xlib's default abort-with-dump */
static int on_x_error(Display* d, XErrorEvent* e) {
    (void)d;
    if (e->error_code == BadWindow)
        fprintf(stderr, "window 0x%lx is gone (the app closed it)\n", e->resourceid);
    else
        fprintf(stderr, "X error %d (request %d)\n", e->error_code, e->request_code);
    return 0;
}

static Window parse_win(const char* s) {
    return (Window)strtoul(s, NULL, 16);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: xinput wins|place|move|click|key|keydown|keyup|type|keysym ...\n");
        return 1;
    }
    const char* cmd = argv[1];
    Display*    d   = XOpenDisplay(NULL);
    if (!d) {
        fprintf(stderr, "cannot open display %s\n", getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");
        return 1;
    }

    XSetErrorHandler(on_x_error);

    int rc = 0;
    if (strcmp(cmd, "wins") == 0) {
        Window       root_ret, parent_ret;
        unsigned int n    = 0;
        Window*      kids = NULL;
        /* XQueryTree returns nonzero on success (not Success=0): gate on the result */
        XQueryTree(d, DefaultRootWindow(d), &root_ret, &parent_ret, &kids, &n);
        for (unsigned i = 0; i < n; i++) {
            XWindowAttributes a;
            if (XGetWindowAttributes(d, kids[i], &a) && a.map_state == IsViewable && a.width > 1 && a.height > 1)
                printf("0x%lx %dx%d %d %d\n", kids[i], a.width, a.height, a.x, a.y);
        }
        if (kids)
            XFree(kids);
    }
    else if (strcmp(cmd, "keysym") == 0 && argc >= 3) {
        printf("%d\n", XKeysymToKeycode(d, key_to_sym(argv[2])));
    }
    else if (strcmp(cmd, "place") == 0 && argc >= 5) {
        XMoveWindow(d, parse_win(argv[2]), atoi(argv[3]), atoi(argv[4]));
        XSync(d, False);
    }
    else if ((strcmp(cmd, "move") == 0 || strcmp(cmd, "click") == 0) && argc >= 5) {
        Window win = parse_win(argv[2]);
        int    x   = atoi(argv[3]);
        int    y   = atoi(argv[4]);
        send_event(d, win, MotionNotify, x, y, 0, 0, PointerMotionMask);
        if (strcmp(cmd, "click") == 0) {
            unsigned btn = argc >= 6 ? (unsigned)atoi(argv[5]) : 1;
            usleep(80000);
            send_event(d, win, ButtonPress, x, y, btn, 0, ButtonPressMask);
            usleep(80000);
            send_event(d, win, ButtonRelease, x, y, btn, 0, ButtonReleaseMask);
        }
    }
    else if ((strcmp(cmd, "key") == 0 || strcmp(cmd, "keydown") == 0 || strcmp(cmd, "keyup") == 0) && argc >= 4) {
        Window win   = parse_win(argv[2]);
        int    press = strcmp(cmd, "key") == 0 ? -1 : strcmp(cmd, "keydown") == 0;
        for (int i = 3; i < argc; i++)
            rc |= send_key(d, win, key_to_sym(argv[i]), argv[i], press);
    }
    else if (strcmp(cmd, "type") == 0 && argc >= 4) {
        Window win = parse_win(argv[2]);
        for (const char* c = argv[3]; *c; c++) {
            char name[2] = {*c, 0};
            rc |= send_key(d, win, *c == '\n' ? XK_Return : (KeySym)(unsigned char)*c, name, -1);
        }
    }
    else {
        fprintf(stderr, "unknown command or missing arguments: %s\n", cmd);
        rc = 1;
    }

    XCloseDisplay(d);
    return rc;
}
