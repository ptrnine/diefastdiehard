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

### 2. Align the input arrival with the server ticks (~5–10 ms, medium) — done

See `docs/multiplayer.md`, Tick alignment. A tick's input leaves in the
frame it runs in, so with 60 Hz vsync the margin can only be kept within a
frame (`[2 ms, 2 ms + 16.7 ms]`): the gain is mostly fewer dry ticks there,
and the full few ms at higher frame rates. In the tests (5 ms frames) the
first inputs' ~30 ms margin goes down to ~5 ms within ~3 s.

The adaptive input buffer counts whole ticks, and an input can arrive
anywhere within a tick. The client could slightly adjust its own tick rate
(well under 1 %) so its inputs arrive a few ms before the server needs them:
the buffer is then a time margin, not whole ticks. Common in fast-paced
shooters; prediction and lag compensation stay the same (the client runs
ahead of the server by the one way trip + the small margin; the view time
keeps the hit tests exact).

### 3. Smoother hand-over of a predicted knockback (~5–7 ms, medium) — done (~5 ms)

Done differently than blending: the jump came from the predicted knockback
starting at the wrong time (the shown time of its detection), not from the
hand-over itself. It now starts where the server's does (the server's tick
at or before the hit time, see `docs/multiplayer.md`, Hit prediction), so
the confirming snapshots continue it without a jump. The margin is 5 ms:
at 3 ms one test still mispredicts an own hit on a falling remote player.

The 10 ms interpolation margin (`server_clock::interp_margin_s`) is only
needed because a predicted hard knockback jumps when the confirming
snapshot takes over (`net: a hard hit on a remote player flies smoothly`
fails with 7 ms). The hit predictions no longer depend on the margin (they
use the exact view time). Blending the hand-over over a few frames would
let the margin drop to ~3 ms.

### 4. A higher tick rate, 120 Hz (~15–20 ms at ≥ 120 Hz frames, bigger change) — done

`physic_simulation::tick_rate` = 120. Everything quantized to ticks halves:
input sampling, the queue wait, the state interval in the interpolation
delay. **Only with frames as short as the ticks:** the ticks run in the
frame loop, so 60 Hz frames (the default vsync and `framerate_limit`) run
two ticks back to back. Both sample the same polled input, the server
reads and sends both at once, and the states still arrive ~17 ms apart.
At 60 fps the gain is close to none. Running the network part of a tick
at its real time (a sim/net thread, or sleeping within the frame) would
lift that; not done.

- About twice the physics and AI CPU, twice the STATE datagrams, and
  slightly bigger INPUT ones (more unacknowledged ticks in a window, the
  window is now up to 64 ticks): ~51 B a datagram in the
  `net: traffic in a fight` test (it checked < 40 B at 60 Hz).
- The server already sent a state per tick (change 1).
- Tick counts that stand for a time derive from `tick_rate`: the lag
  compensation histories (`history_ticks`, `acked_ticks_kept`: at 120 Hz
  the old counts were shorter than the 1 s `max_lag_compensation`), the
  input queue (default 6 ticks, up to 16), the drain window (0.5 s: the
  same ~17 ms of queue drained a second), the position trace, the gun
  hit cooldown. `protocol_version` 21.
- The weapon bobbing filter (tuned per 60 Hz tick) is scaled to the tick
  length. The explicit Euler steps make a jump slightly higher (estimated
  ~1.7 %, 155 → 158 px).
- The 20 ms interpolation minimum (`net interp min`) caps the gain on the
  interpolation delay (~14 ms computed with 120 Hz frames).

### Expected result

| changes | client to client |
|---|---|
| now | ~60 ms |
| 1 + 2 + 3 | ~40 ms |
| 1 + 2 + 3 + 4 | ~25–30 ms with ≥ 120 Hz frames (and a lower `net interp min`); ~40 ms at 60 fps |

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
