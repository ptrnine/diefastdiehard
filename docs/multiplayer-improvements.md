# Multiplayer: reducing the latency between clients

Ideas to cut the delay between two clients (client1 acts → client2 sees it)
without losing what the current implementation gives: predicted local
player, lag compensation from the reported view time, hit prediction on the
local and the remote players, and smooth remote players. See
`docs/multiplayer.md` for how these work.

The numbers are estimates from the code and the loopback measurements
(`tests/net_test.cpp` fixture), not from a real network. `hud net` shows the
interpolation delay and the input queue wait in game.

## Where the time goes now

1 ms ping, 60 Hz frames (vsync), ticks exactly 1/60 s apart:

| step | ms |
|---|---|
| the key press waits for client1's next tick (input sampling) | ~8 (0–16) |
| the server reads its socket once per frame, after the frame's ticks | ~8 (0–17) |
| the input waits in the server's queue (adaptive buffer, 1 tick target) | ~0–16 |
| client2's interpolation delay: jitter (~1) + state interval (~17) + margin (10) | ~28 |
| client2 renders and displays the frame | ~17 |
| **total** | **~60** |

## Options that keep the current advantages

### 1. Network I/O at tick boundaries (~8 ms, low risk) — done

The server reads its socket in its per-frame `update`, after
`gs.game_update()` ran the frame's ticks: an input that arrives during a
frame waits for the next frame's tick. Read the socket right before applying
the inputs (the tick begin callback of `net_server`), send the states right
after each tick, and send the client's inputs right after each client tick.
No game logic changes.

### 2. Align the input arrival with the server ticks (~5–10 ms, medium)

The adaptive input buffer counts whole ticks, and an input can arrive
anywhere within a tick. The client could slightly adjust its own tick rate
(well under 1 %) so its inputs arrive a few ms before the server needs them:
the buffer is then a time margin, not whole ticks. Common in fast-paced
shooters; prediction and lag compensation stay the same (the client runs
ahead of the server by the one way trip + the small margin; the view time
keeps the hit tests exact).

### 3. Smoother hand-over of a predicted knockback (~5–7 ms, medium)

The 10 ms interpolation margin (`server_clock::interp_margin_s`) is only
needed because a predicted hard knockback jumps when the confirming
snapshot takes over (`net: a hard hit on a remote player flies smoothly`
fails with 7 ms). The hit predictions no longer depend on the margin (they
use the exact view time). Blending the hand-over over a few frames would
let the margin drop to ~3 ms.

### 4. A higher tick rate, e.g. 120 Hz (~15–20 ms, bigger change)

Everything quantized to ticks halves: input sampling, the queue wait, the
state interval in the interpolation delay. Costs:

- about twice the physics CPU and twice the STATE traffic (~150 B per
  state, still small);
- the server must send a state per tick, not once per frame (a 60 Hz frame
  would run two ticks);
- the physics must be checked at the new timestep (jumps, knockback, gun
  timings tuned at 60 Hz).

### Expected result

| changes | client to client |
|---|---|
| now | ~60 ms |
| 1 + 2 + 3 | ~40 ms |
| 1 + 2 + 3 + 4 | ~25–30 ms |

Suggested order: 1, then 2 (each contained and measurable with `hud net`),
then 3; 4 last.

## What not to do

- **Extrapolate the remote players to the present.** They would appear
  sooner, but shown where they are not: the lag compensation could no
  longer hit what the shooter sees.
- **Interpolate with less than jitter + state interval.** The snapshot to
  draw would not have arrived yet: the remote players stutter.

## Outside the networking

The render/display step (~17 ms) shrinks only with rendering later in the
frame (frame pacing) or without vsync.
