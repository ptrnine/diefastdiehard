---
name: ui-input-testing
description: Test diefastdiehard's graphical UI (Nuklear widgets, lobby, player configurator, dev console) and synthetic mouse/keyboard/gameplay input under Xvfb. Use when verifying what the UI renders, clicking buttons/checkboxes/combos, driving the lobby or dev console, moving a player, or checking multiplayer UI without a physical display.
---

# UI / input testing under Xvfb

The game UI is Nuklear-on-SFML. There is no real mouse: `scripts/xinput.c`
synthesizes X events with `XSendEvent` to the exact window, and
`scripts/ui.sh` wraps everything (Xvfb, launch, window placement,
screenshots, OCR, input). Use the wrapper; don't improvise input delivery.

Needs only local packages: `Xvfb`, ImageMagick (`import`, `convert`),
`tesseract` with `eng` data, `gcc` + libX11 headers.

## Quick start

```sh
export UI_OUT=<scratchpad>/ui          # state, logs, screenshots
U=skills/ui-input-testing/scripts/ui.sh

$U up                                  # Xvfb :99 (3840x1200) + build helper
$U launch srv --server                 # window placed at (0,0)
$U wait srv "lobby opened"
$U launch cli --client 127.0.0.1:10443 --name tester   # placed right of srv
$U wait cli "joined the game"
$U ocr cli                             # what's on screen + click points
$U click cli ready                     # click by visible text
$U shot srv                            # -> $UI_OUT/srv.png, read it
$U down                                # kill instances + Xvfb
```

Each `launch` diffs the window list to find its own window, so names map to
the right window even though X window IDs change between runs. Logs go to
`$UI_OUT/<name>.log`. `DFDH_BIN=easybuild/dist/dfdh` runs a dist build
(from its own dir, with its own `data/settings`). Run `$U` alone for help.

## Reading the screen

- `$U shot NAME` captures exactly the window: image pixel = window
  coordinate = click coordinate. Read it at full size; if you downscale
  for a quick look, multiply coordinates back.
- `$U ocr NAME` prints `cx cy w h text` per word; `cx cy` is a click point.
  Reliable on Nuklear panels (lobby, combo lists, console); noisy over level
  backgrounds — filter by region (`| awk '$2 > 250 && $2 < 640'`).
- `$U click NAME "cancel lobby" [NTH]` matches consecutive words on one line,
  case- and punctuation-insensitive (OCR reads a checkbox as `“ready`; NK
  checkboxes toggle on their label too). On a miss it prints the OCR dump.
- Targets without text (combo arrows, face/hat icons, `x` close buttons):
  take coordinates from the screenshot. Combo box: click its body, then
  `ocr` the opened list and click the entry by text.
- For pixel-exact measurement crop a strip and dump it:
  `convert $UI_OUT/cli.png -crop 300x12+850+655 +repage -depth 8 txt:- | head`.
  Never scan a full screenshot pixel by pixel in a script (minutes).

## Input

```sh
$U click NAME X Y [BTN]          # motion, 80 ms, press, 80 ms, release
$U move NAME X Y                 # hover only
$U key NAME grave Escape Up a    # X keysym names, press+release each
$U hold NAME 0.6 Right           # gameplay: keys down for 0.6 s, then up
$U type NAME 'any "Text" (here)' # Shift set automatically for shifted chars
$U console NAME "fps"            # grave, type, Return, grave
```

- Why the click shape: SFML feeds `nk_input_motion` only from mouse moves
  (`nk_input_button` does not move the NK mouse), so motion must precede
  the press; `NK_BUTTON_DEFAULT` fires on release with `down && clicked`, so
  press and release must land in different frames.
- **Do not use XTEST** (`XTestFakeButtonEvent`): fake button events get
  routed to a wrong child window here. `XSendEvent` to the window works.
- Player controls are event-driven (`player::update_input` on
  KeyPressed/KeyReleased), so synthetic keys move players, including a
  network client's. Default keys: arrows, `Comma` shot, `Period` grenade,
  `G` drop, `M` adjust (per-player in `data/settings/players_settings.cfg`).
  A single `key` tap barely moves; use `hold`.
- Keycodes are resolved on the live display (`XKeysymToKeycode`); never
  hardcode them.

## Dev console

- `grave` toggles it; its edit field (the only NK text field in the game)
  takes focus automatically, so type right away. `$U console` assumes the
  console is closed — if a previous step left it open, `$U key NAME grave`
  first or the toggle inverts.
- Commands: `help`, `list`, `fps`, `lobby [start]`, `game mode list`,
  `level <name>`, `shutdown` (quits; there is no `quit`). `unknown command`
  in the log = typo or leftover text in the field.
- Arguments split on whitespace **and** quotes. A Lua line is one argument:
  wrap it in single quotes, use `"` inside:
  `$U console srv "lua 'log_impl(\"info\", \"x=\" .. 2*3)'"`.
- Lua `print` goes to the C stdout, block-buffered into the log file — you
  won't see it. Use `log_impl("info", ...)` to get a log line; failures log
  `lua error: ...`. For non-interactive Lua at startup there is
  `--lua-exec-stdin` (`ui.sh launch` gives stdin `/dev/null`; run the binary
  directly with a heredoc for that).

## Verifying an action

Check, in order: the log, the screenshot, process liveness
(`kill -0 $(cat $UI_OUT/NAME.pid)`). Useful log lines (`$U wait NAME REGEX`):

| Event | Log line |
|---|---|
| server up / lobby | `lobby opened` (server and client) |
| client in | `net client: joined the game as <n>` / server: `client ... joined as player <n>` |
| match start | server: `lobby: game started on <lvl> (<mode>)`; both: `game running` |
| lobby cancelled | `lobby closed` (not logged on start) |
| client out | `net client: left the game` / server: `client <ip> left`, `player <n> deleted` |
| console | `fps: on`, `unknown command`, `command '<c>' accepts N arguments` |

Grep for substrings, not `^[`: audio errors are glued onto log lines.
Lobby state is broadcast, so check **both** windows (e.g. a client's
`ready` shows as `ready: 1/1` on the server). An action that "didn't work"
is almost always a wrong coordinate — re-`ocr` and retry once, then suspect
the window state (console open, configurator on top), not the recipe.

## Gotchas

- Window size/position come from `data/settings/engine.cfg` (written by the
  game, gitignored; a dist has its own). With a screen smaller than
  `window_pos + window_size` part of the window is simply not rendered into
  screenshots — that is why `up` uses a large screen and `launch` moves the
  window (moving does not rewrite the cfg; resizing would).
- Expected log noise: `ALSA ...`, `miniaudio ERROR`, `Failed to initialize
  sound` (no audio device), and `net client: prediction correction` lines.
- `window 0x... is gone (the app closed it)` after a click/key = the app
  quit on that input (e.g. lobby `leave`, `shutdown`); expected. An
  assertion/abort instead is a shutdown-path regression. `nk_buffer_init_fixed
  ... Assertion 'm'` = a frame rendered to a closed window; `engine.hpp`
  guards render with `_wnd.isOpen()`, so a new close path bypasses it.
- Closing the player configurator re-sends the config: the client logs
  `joined the game` again (the server stays quiet). Not a reconnect.
- `pkill -f <pattern>` matches your own shell's command line and kills it.
  `ui.sh down` kills by recorded pid; otherwise `pkill -x dfdh`.
- Xvfb screen spec is `WxHx24`; the hyphen form fails with "Invalid screen
  configuration". Override with `UI_SCREEN`, display with `UI_DISPLAY`.
- The game resolves assets from cwd; `ui.sh` runs it from the repo root (or
  the dist dir). A dist built before new assets were added logs `Cannot load
  texture` — rebuild it with `./build-plz prod`.
- build-plz does not track header dependencies: `rm easybuild-debug/dfdh`
  before `./build-plz --debug` after touching any header.
- `data/scripts/dfdh.lua` must be empty for `lua_test` to pass (user WIP —
  never commit it).
