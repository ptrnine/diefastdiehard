---
name: ui-input-testing
description: Test diefastdiehard's graphical UI (Nuklear widgets, lobby, player configurator) and synthetic mouse/keyboard input under Xvfb. Use when verifying what the UI renders, clicking buttons/checkboxes, driving the lobby or dev console, or checking multiplayer UI without a physical display.
---

# UI / input testing under Xvfb

The game UI is Nuklear-on-SFML. You cannot use a real mouse; you synthesize X
events with `XSendEvent` to the exact window. Everything below is what actually
worked — follow the recipe, don't improvise the input delivery.

## Workflow

- [ ] 1. Start Xvfb and the game(s) (below)
- [ ] 2. Identify window IDs with `xinput geom`
- [ ] 3. Screenshot + crop to read the screen (and measure target coordinates)
- [ ] 4. Send input with `xinput click` / `xinput key`
- [ ] 5. Verify: process liveness, logs, screenshot after
- [ ] 6. If an action didn't register: re-measure coordinates from a crop,
      retry once; then suspect window ID or focus, not the click recipe

## Setup

```sh
Xvfb :99 -screen 0 1600x900x24 &        # x24, NOT the hyphen form
gcc -o /tmp/xinput scripts/xinput.c -lX11   # from this skill directory, once

# server (run from the repo root — the game resolves data/ + fs.cfg from cwd)
DISPLAY=:99 LD_LIBRARY_PATH=easybuild-debug/3rd/lib:easybuild-debug/3rd/lib64:\
easybuild-debug/submodules/luacpp/3rd/lib:easybuild-debug/submodules/luacpp/3rd/lib64 \
./easybuild-debug/dfdh --server > /tmp/dfdh_srv.log 2>&1 &

# client, ~4 s later, in its own bash line (each launch in its own shell line —
# `cmd1 > log1 & cmd2 > log2 &` on one line misattributes output)
DISPLAY=:99 LD_LIBRARY_PATH=... ./easybuild-debug/dfdh --client 127.0.0.1:10443 --name tester > /tmp/dfdh_cli.log 2>&1 &
```

The server opens the lobby on start; the client joins it automatically
(`net client: joined the game as <name>` in the client log).

Windows: `DISPLAY=:99 /tmp/xinput geom :99` lists root children — the main
windows are the only 1280x900 ones (server ~`0x200006`, client ~`0x400006`,
both at root (640,204)). **Window IDs are reused after a restart — re-run
geom after every relaunch.**

## Reading the screen

No OCR/tesseract/PIL available — you read screenshots directly.

```sh
DISPLAY=:99 import -window root /tmp/s.png        # root is 1600x900
convert /tmp/s.png -crop 1280x900+640+204 +repage /tmp/w.png   # exact window
```

Read `/tmp/w.png`. To measure an element's position, crop a thin horizontal
or vertical strip through it and dump the pixels:

```sh
convert /tmp/w.png -crop 300x12+850+655 +repage -depth 8 txt:- | head -40
```

Find the border color transitions (NK buttons are gray ~rgb(210,210,210) with
a darker border on the dark background). **Never** scan a full screenshot
pixel-by-pixel in a script — it takes minutes.

Coordinate bookkeeping: `txt:-`/crop coordinates are window-relative already
when you crop the window first. If you resized a screenshot instead, the scale
factor is **0.64** (1600→1024), not 0.75 — but prefer cropping so you never
convert.

## Sending input

All commands below send events to the given window (`send_event=True`); SFML
polls its own queue and delivers them.

```sh
/tmp/xinput move  :99 400006 <x> <y>      # MouseMotion only
/tmp/xinput click :99 400006 <x> <y>      # motion, 80 ms, press, 80 ms, release
/tmp/xinput key   :99 400006 grave q u i t return
/tmp/xinput keysym :99 q                  # print the server keycode for a char
```

Coordinates are **window-relative** (window is 1280x900).

Why the click recipe is exactly this:

- SFML forwards the motion to `nk_input_motion` — the only NK call that
  updates the mouse position. `nk_input_button` (from ButtonPress/Release)
  does **not** move the mouse. So a `MotionNotify` at the target must
  precede the press or the hover check fails.
- `NK_BUTTON_DEFAULT` fires on *release* with `down && clicked`: press and
  release must land in **different frames**. The 80 ms gaps (~several frames)
  are required.
- Checkboxes use the `clicked` counter and tolerate same-frame press+release,
  but use the same full recipe for everything.

Keyboard: keys *do* reach the app (dev console opens on `grave`), but a
Nuklear edit field ignores typed text until it has mouse focus — **click the
edit box first**, then send characters. The server keymap is not US layout;
`xinput key` resolves keycodes via `XKeysymToKeycode` on the live display,
so always go through it (never hardcode keycodes).

**Do not use XTEST** (`XTestFakeButtonEvent`): fake button events get routed
to a wrong child window on this setup. `XSendEvent` to the target window is
the proven delivery path.

## Verifying an action

1. Process liveness: `ps aux | grep "[d]fdh"` (expected exits: client quits,
   server stays).
2. Logs: `tail /tmp/dfdh_cli.log /tmp/dfdh_srv.log`. The game logs the events
   you care about: `joined the game`, `lobby closed`, `left the game`,
   `client ... left`, `player ... deleted`.
3. Screenshot before/after and read both (e.g. lobby closed on **both**
   sides — the server broadcasts lobby state).

An action that "didn't work" is almost always a wrong coordinate: NK buttons
size to their content and do **not** fill their layout cell — re-measure
from a strip dump; never trust coordinates derived from layout code.

## Gotchas

- `pkill -f <binary-path>` kills your own shell (the pattern appears in the
  shell's own command line). Use `pkill -x dfdh`, `pkill -x Xvfb`.
- Xvfb screen arg must be `1600x900x24`; the hyphen form
  (`1600x900-24`) fails with "Invalid screen configuration".
- Run the game from the repo root or `easybuild/dist`, never from
  `easybuild-debug/` (asset resolution uses `fs::current_path()`).
- `nk_buffer_init_fixed ... Assertion 'm'` = the window was closed mid-frame
  and the frame rendered to the dead GL context. `engine.hpp` guards the
  render phases with `_wnd.isOpen()`; if you see it again, a new close path
  bypasses the guard.
- build-plz does not track header dependencies: `rm easybuild-debug/dfdh`
  before `./build-plz --debug` after touching any header.
- `data/scripts/dfdh.lua` must be **empty** for `lua_test` to pass (it is
  user WIP — truncate it if a test run fails there, never commit it).
- A synthetic click that closes the window (e.g. lobby "leave") exits the
  client cleanly: `net client: left the game` in the client log and
  `client ... left` in the server log. Any assertion/abort instead means a
  regression in the shutdown path, not a bad click.
- The dev console opens with `grave` and is not always visibly drawn at the
  expected spot; judge it by its effects (commands run, `unknown command`
  log line), not only by the screenshot.
- When writing your own X11 helper code: `Success` is **0** on this system
  and `XQueryTree` returns 1 on a successful query with the result filled —
  gate on the filled result (`kids`/`n`), not on the status value.
- After a click that quits the app, expect a harmless
  `X Error ... BadWindow (invalid Window parameter)` on the follow-up
  synthetic events — the window is already gone; the exit itself is clean.
