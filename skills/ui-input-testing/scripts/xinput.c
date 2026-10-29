/*
 * xinput: synthetic X input helpers for diefastdiehard UI testing.
 *
 * Builds:  gcc -o /tmp/xinput xinput.c -lX11
 *
 * Usage:
 *   xinput geom <display>                    list root children + geometry
 *   xinput move <display> <win> <x> <y>      MouseMotion (window-relative)
 *   xinput click <display> <win> <x> <y>     motion, 80ms, press, 80ms, release
 *   xinput key <display> <win> <key>...      keypress(es); key = one char,
 *                                            "grave" or "return"
 *   xinput keysym <display> <key>            print the server keycode for a key
 *
 * Why the click recipe is exactly motion -> 80ms -> press -> 80ms -> release:
 * SFML only feeds nk_input_motion from mouse moves (nk_input_button does not
 * update the mouse position), and NK_BUTTON_DEFAULT fires on release with
 * down && clicked, so press and release must span different frames.
 * Events go through XSendEvent with send_event=True to the exact window —
 * XTEST fake button events misroute on this setup.
 */
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static KeySym key_to_sym(const char *s) {
    if (strcmp(s, "grave") == 0)
        return 0x60; /* backtick; do NOT write the C literal here, it is an empty string */
    if (strcmp(s, "return") == 0)
        return XK_Return;
    if (strlen(s) == 1)
        return (KeySym) s[0];
    return XStringToKeysym(s);
}

static int send_event(Display *d, Window win, int type, int x, int y, int code, int mask) {
    XEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.xany.type       = type;
    ev.xany.send_event = True;
    ev.xany.display    = d;
    ev.xany.window     = win;
    if (type == MotionNotify) {
        ev.xmotion.x         = x;
        ev.xmotion.y         = y;
        ev.xmotion.time      = CurrentTime;
        ev.xmotion.same_screen = True;
    }
    else if (type == ButtonPress || type == ButtonRelease) {
        ev.xbutton.x          = x;
        ev.xbutton.y          = y;
        ev.xbutton.button     = code;
        ev.xbutton.time       = CurrentTime;
        ev.xbutton.same_screen = True;
    }
    else if (type == KeyPress || type == KeyRelease) {
        ev.xkey.keycode = code;
        ev.xkey.state   = 0;
        ev.xkey.time    = CurrentTime;
    }
    XSendEvent(d, win, True, mask, &ev);
    XFlush(d);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2)
        return 1;
    const char *cmd    = argv[1];
    Display    *d      = XOpenDisplay(argv[2]);
    if (!d) {
        fprintf(stderr, "cannot open display %s\n", argv[2]);
        return 1;
    }

    if (strcmp(cmd, "geom") == 0) {
        Window root = DefaultRootWindow(d);
        Window root_ret, parent_ret;
        unsigned int n = 0;
        Window *kids   = nullptr;
        /* this Xlib returns 1 (not Success=0) on a successful query with the
         * result filled — gate on the result, not the status */
        XQueryTree(d, root, &root_ret, &parent_ret, &kids, &n);
        if (kids == nullptr)
            return 1;
        for (int i = 0; i < n; i++) {
            XWindowAttributes a;
            if (XGetWindowAttributes(d, kids[i], &a))
                printf("%x: %dx%d at (%d,%d) map_state=%d\n", kids[i], a.width, a.height, a.x, a.y, a.map_state);
        }
        XFree(kids);
    }
    else if (strcmp(cmd, "keysym") == 0) {
        if (argc < 4)
            return 1;
        printf("%d\n", XKeysymToKeycode(d, key_to_sym(argv[3])));
    }
    else if (strcmp(cmd, "move") == 0 || strcmp(cmd, "click") == 0) {
        if (argc < 6)
            return 1;
        Window win = (Window) strtoul(argv[3], nullptr, 16);
        int x      = atoi(argv[4]);
        int y      = atoi(argv[5]);
        send_event(d, win, MotionNotify, x, y, 0, PointerMotionMask);
        if (strcmp(cmd, "click") == 0) {
            printf("click win %x at (%d,%d)\n", win, x, y);
            usleep(80000);
            send_event(d, win, ButtonPress, x, y, 1, ButtonPressMask);
            usleep(80000);
            send_event(d, win, ButtonRelease, x, y, 1, ButtonReleaseMask);
        }
        else
            printf("motion win %x (%d,%d)\n", win, x, y);
    }
    else if (strcmp(cmd, "key") == 0) {
        if (argc < 5)
            return 1;
        Window win = (Window) strtoul(argv[3], nullptr, 16);
        for (int i = 4; i < argc; i++) {
            KeyCode code = XKeysymToKeycode(d, key_to_sym(argv[i]));
            printf("key %s (%d)\n", argv[i], code);
            send_event(d, win, KeyPress, 0, 0, code, KeyPressMask);
            usleep(60000);
            send_event(d, win, KeyRelease, 0, 0, code, KeyReleaseMask);
        }
    }
    else {
        fprintf(stderr, "unknown command %s\n", cmd);
        return 1;
    }

    XCloseDisplay(d);
    return 0;
}
