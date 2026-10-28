# Multiplayer

Server-authoritative multiplayer over UDP. The server runs the full game.
Clients predict their own player (with rollback), see remote players in the
past (interpolated snapshots), and predict their own bullets and bullet hits.
The server compensates for each client's lag.

## Running

```sh
./dfdh --server [--port=10443] [--input-buffer=auto|N]                       # host: plays too (local keyboard player, bots)
./dfdh --client 127.0.0.1:10443 [--name=client0] [--interp-margin=MS] [--interp-min=MS] [--input-margin=MS]  # client
```

The latency flags are optional (see Latency settings).

A client sends its skin, tracer color and gun from its local player config
(`player_configurator` of `--name`) in HELLO. In client mode,
`game_update` deletes keyboard controller 0, which the level script
recreates on every level load. The keyboard instead feeds a
`net_player_controller` (id 1000) that is sampled once per tick
(`net_client::input_source`). The client does not spawn its player
locally: the first server state places it.

## Lobby

Both sides open the lobby on launch (`game_state::lobby_open`, state in
`src/lobby.hpp`, window in `src/ui/lobby_ui.hpp`). Nothing is simulated
while `lobby_state::active`.

- **Server owner:** chooses the map and the game mode (default: the
  current ones), every player's group, and presses Start
  (`game_state::lobby_start`: loads the level, applies the game mode,
  resets the scores, spawns everyone). The console `lobby` command sends
  everyone back to the lobby, and `lobby start` starts the game.
- **Client:** sees the choices and the ready flags. It chooses its own group
  and ready flag (LOBBY request), and "customize" opens the player
  configurator. Closing the configurator resends HELLO, which reconfigures
  the joined player.
- **Requests:** the client resends its latest LOBBY request every 100 ms
  while the lobby is active. The server applies each `seq` once, so a resend
  never overrides a group the owner changed in between. It ignores requests
  outside of the lobby.
- **Phases:** the client follows the lobby phase from STATE (`lobby_info`):
  - it starts the game when it leaves its lobby, loading the level first;
  - in game, it loads the server's level whenever that differs;
  - back in the lobby, it resets its ready flag.

## Code map

| file | contents |
|------|----------|
| `src/net/net_protocol.hpp` | messages, (de)serialization, `protocol_version`, input bits |
| `src/net/net_udp.hpp` | `udp_channel`: non-blocking `sf::UdpSocket` wrapper, simulated latency |
| `src/net/net_server.hpp` | join/leave, per-tick input queue, per-client state encoding |
| `src/net/net_client.hpp` | join, input sending, prediction/reconciliation, remote players, server bullets |
| `src/net/net_sync.hpp` | `server_clock`, `snapshot_buffer`, `input_history`, `knockback_offset` |
| `src/net/net_player_controller.hpp` | key events → `player_input_state` |
| `src/game_state.hpp` | `spawn_bullet` (lag compensation), `hitscan`, `apply_hit_at` + `history` (retroactive hits), `players_tick_update` |
| `src/player.hpp` | `apply_input`, `physic_state` save/restore, `teleport`, render offset, hit handling, position trace |
| `src/weapon.hpp` | tick-based weapon logic, `sync_state`, seeded shots |
| `src/base/game_clock.hpp` | the game time (`game_clock`, manual in tests) |
| `diefastdiehard.cpp` | CLI, wiring the server/client into the main loop |
| `tests/net_test.cpp` | protocol, units and loopback game tests |

## Player roles

| flag | where | meaning |
|------|-------|---------|
| `player::net_controlled()` | server | a client's player: driven by network input, fully simulated (bullets, hits, death) |
| `player::server_driven()` | client | every player the server knows about: no local bullets, death or authoritative knockback; its state comes from the server |
| `player::predicted()` | client | the local player (also server-driven): its physics, recoil, own bullets and hits are predicted |

`game_state::locally_controlled(pl)` (camera, local effects) is true for a
local controller's player, and for the client's predicted player.

## Protocol

Every datagram has a header `u16 type, u16 payload_size`, then an
`ss::serializer` payload: little endian, with **u16** string and vector
length prefixes (`net::wire_size_type`; the serializer default is u64).
**Bump `net::protocol_version`** on any wire change: a HELLO with another
version is answered with ERROR. The current version is 20.

| id | dir | payload |
|----|-----|---------|
| 1 HELLO | C → S | `u16 version`, name, pistol section, face_id, body_id, body color, tracer color |
| 2 WELCOME | S → C | player name |
| 3 INPUT | C → S | `u32 last_seq`, `u32 client_time_ms`, `u16 rtt_ms`, `vector<u8> bits`: the input ticks not acknowledged yet (up to 64), where `bits[i]` belongs to tick `last_seq - size + 1 + i`; `u32 view_base`, then (if not 0) `size − 1` raw `u8` view steps: the views (see Lag) |
| 4 STATE | S → C | see below; encoded per client, sent once per server tick (every 100 ms while paused) |
| 5 LEAVE | C → S | player name |
| 6 ERROR | S → C | message (duplicate name, version mismatch, ...) |
| 7 LOBBY | C → S | `u32 session`, `u32 seq`, `i16 group`, `u8 ready`: the client's lobby choice (see Lobby; seqs restart with a new random session) |

STATE contains:
- **Header:** `tick` (broadcast sequence number), `server_time` (time of the simulated tick, 1/10000 s: ticks are 1/120 s apart; at 60 Hz whole ms were up to 0.67 ms early), `ack_seq` (the last input tick applied to this client's player), `echo_client_time_ms` + `echo_hold_ms` (RTT echo of the newest input), and `input_margin` (`i16`, 1/10 ms: how long before the server needed it the tick `ack_seq` arrived, negative if the queue ran dry waiting for it; see Tick alignment).
- **`own_weapon`:** the client's own player's weapon logic state (`weapon_sync`: ammo, logic animation id and time, time since the last shot, reloading/trigger flags, shot index and seed), if it holds a gun. Only the recipient's player is reconciled, so other players' weapons are not sent.
- **`mode`, the game mode, only when the client may not know it:** section name, bullet gravity, crate drop period (see `src/game_mode.hpp`). It is sent with the same policy as the player configs (1 s after the join or a change, every 30th broadcast); the client applies the newest one with `game_state::game_mode_set` and sets `game_mode_networked`, so its console cannot change the game mode.
- **`lobby`, only when the client may not know it (the same policy as `mode`):** `u8 active`, level and game mode sections. While active, these are the owner's choices; in game, what is played.
- **`player_config`, only the ones the client may not know:**
  - fields: player id, name, pistol, face/body ids, body color, tracer, group;
  - players are referenced by their `u16` id everywhere else; ids are assigned by the server and never reused;
  - a config is sent to a client during 1 s after it joined, during 1 s after the config changed, and every 30th broadcast, so lost datagrams cost nothing;
  - the client keeps them by id: a known config is changed only by the newest STATE, and a player whose config is unknown yet is skipped.
- **`state_player`, per player:**
  - id;
  - physic state: pos and **raw** velocity as full floats (the client reconciles with them), flags (lock_y, on_left, shooting, throwing, ready in the lobby, long_shot: the adjust mode of `key_adjust_up`, which aims the gun up under bullet gravity; clients apply it to remote players), jumps_left, accel factors (u16 fixed point);
  - `throwing`: the player throws its gun away (`player::drop_gun` plays a 0.25-0.45 s wind-up and throw, the server drops the gun at its end). Clients show the throw of remote players from this flag and the local one from the predicted drop input; the server's drop (the secondary gun in the player config, or an empty pistol: no gun) replaces the gun at the release of the shown throw (`player::setup_pistol_on_release`). The local player's thrown gun is predicted: it spawns at the local release with local physics and takes over the id of the first new server gun owned by the local player (`net_client::confirm_predicted_gun`). From then on every STATE corrects it to the server's gun forwarded by the difference of their flight times (`age`) along the ballistic path, smoothed by a decaying render offset; a prediction the server never confirms disappears after max(1 s, 2 RTT + 0.5 s);
  - gun hits: the predicted gun collides with the other players where they are shown and its hits are predicted like own bullet hits (knockback offset, confirmed by `hits`). A gun hit id is the gun id tagged with `dropped_gun::hit_ref_tag` (never a bullet id; an unconfirmed prediction has its own tag and is re-keyed when the server's gun takes it over). The server tests the gun of a network thrower against the players rewound by lag + rewind (its gun is released lag after the thrower's, which saw the others rewind in the past) and applies the hit with `apply_hit_at` at the tick end, so both agree. A server state from before the prediction's last bounce does not correct it (it can't be forwarded past the bounce);
  - teleport counter;
  - score (`i16`, `player::score`): the server scores kills and suicides (`game_state::player_on_dead`), clients only show it;
  - `hits`: ids of the bullets that hit the player in the last second.
- **`shot_info`, per shot spawned since the last broadcast:** owner id, first bullet id, shot key (`pull_seq`, `shot_index`, first pellet), muzzle start, forward time and rewind (u16, 1/32768 s; rewind 0: the bullet collides physically), hit_mass, gravity flag, and the muzzle velocity of each pellet. Pellet `i` has the id `first_id + i` and the pellet number `first_pellet + i`; pellets of one shot share everything but the velocity. The server keeps spawned bullets in a pending list until the next broadcast.
- **Other world state:** crates (id, pos, vel, landed); dropped guns (id, section, pos, vel, angle, on_left, long_shot, owner: the id of the player who threw it, age: flight time in ms); `alive_set`: the ids of all bullets alive on the server, as a bitmap from the smallest one.

Typical sizes: the example in `tests/net_test.cpp` (4 players, a 7 pellet shot, 30 live bullets, 2 crates, 1 dropped gun) is 373 bytes (1131 in version 6). In a loopback fight with two players and a shotgun, a state averages ~150 bytes and an input ~28 bytes (`udp_channel::bytes_sent`). Keep a STATE well below the ~1470 byte MTU.

Input bits are held states, except edges (`in_edge_bits`), which are set only on the tick of the press:

| bit | meaning |
|-----|---------|
| 0 / 1 | LEFT / RIGHT (held) |
| 2 / 3 | JUMP / JUMP-DOWN (edge) |
| 4 | SHOT (held) |
| 6 | LONG-SHOT (held) |
| 7 | DROP gun (edge) |

Join rules: one client record per address. A name already used by another
client gets ERROR. A HELLO with a new name from the same address renames
the client: the old player is deleted.

## Time and ticks

- `physic_simulation` runs fixed 120 Hz ticks (`physic_simulation::tick_rate`),
  exactly 1/120 s apart (`period_of`, nanoseconds). Each tick is stamped
  with its scheduled time (`current_update_time`), not the catch-up time.
  Tick counts that stand for a time (histories, buffers, windows) derive
  from `tick_rate`.
  - It was 60 Hz: 120 Hz halves what is quantized to ticks (input
    sampling, the input queue, the state interval), but only when the
    frames are as short: the ticks run in the frame loop, so 60 Hz frames
    (the default vsync / `framerate_limit`) run two ticks back to back,
    which sample the same input and are sent at once.
  - It used to round the period down to whole milliseconds: 16 ms, 62.5 Hz.
    Against 60 Hz frames (vsync), every 25th frame ran two ticks. The
    server then sent one state for both (a 32 ms gap) and the client sent
    two inputs at once, so the interpolation delay (~57 ms) and the input
    buffer (2 ticks, ~39 ms wait) grew even on a 1 ms LAN. Exact ticks: one
    per 60 Hz frame, ~28 ms and 1 tick.
- Network code hooks into ticks with ordered callbacks
  (`add_tick_begin_callback` / `add_tick_end_callback(name, cb, order)`):

  | tick begin | tick end |
  |------------|----------|
  | `net_server`: read the inputs, apply them / `net_client`: sample input | `rewound_bullets` (-1) |
  | `history` (10) | `players` (0): `players_tick_update` — weapons, shots, recoil, death |
  | | `history` (100) |
  | | `net_client` (100): record the prediction, send the input |
  | | `net_server` (1000): send the state |

- **Network I/O at the tick boundaries:** the server reads its socket
  (`recv` without waiting) at every tick begin, right before applying the
  inputs, and sends the state right after the tick; the client sends its
  input right after its tick. The server used to read once per frame, after
  the frame's ticks: an input that arrived during a frame waited for the
  next frame's tick (~8 ms on average). Only INPUT is handled in the tick;
  the rest (HELLO, LEAVE, LOBBY: they create and delete players) waits for
  the frame's `update`, which also reads the socket and sends the state
  while nothing ticks (lobby, paused game: every 100 ms).

- Players are updated on ticks, not frames. So shots, recoil and reloads land
  on the same input ticks on both sides.
- **Game time comes from `game_clock`**, never `std::chrono::steady_clock`
  directly. This covers sim ticks, net timing, simulated latency, recent hit
  ids and crate drops. Tests drive it by hand, so new gameplay or network
  timers must use `game_clock` / `game_timer` too.
  Drawing-only timers (`sf::Clock`, `timer`) are fine on real time.

## Input

- **Client:** samples the input once per tick, applies it to the local
  player and stores it in `input_history`. Right after each tick, it sends the
  ticks the server has not acknowledged yet (after the newest `ack_seq`, up
  to 32). A lost or reordered datagram costs nothing, because each newer one
  repeats every unacknowledged tick.
- **Server** (`net_server::apply_inputs`, tick begin): queues ticks by seq
  and drops ticks it already applied, so a late or duplicated datagram never
  brings back an old input. Each server tick:
  - applies exactly one queued input tick, after `last_input_seq(seq)`, so
    the weapon counts input ticks and seeds its shots by the pull tick;
  - runs a jitter buffer: after running dry it waits for the client's
    target number of queued ticks. Dry ticks repeat the last held bits
    without edges and consume no seq;
  - the target adapts to the client's input arrival jitter
    (`client_t::input_jitter`, a `server_clock` of `arrival − client send
    time` over 2 s, restarted after 0.5 s without input; it was
    `arrival − seq·tick`, which takes the client's tick alignment for
    jitter):
    `ceil(jitter / tick − 0.1) + 1` ticks (`adaptive_target`, 1..16). Each
    queued tick is a tick of input latency and of the client's shot
    forwarding (`lag`). Until 1 s is measured, the target is 6 (50 ms);
  - drains a queue that stayed deeper than the target for a whole window
    of 60 ticks (`drain_window_ticks`, 0.5 s): merges its oldest tick, once it
    is repeated (see below) by the previous and the next tick. Without it,
    the backlog of a client hitch (a stall, then the ticks at once) stayed
    queued for good;
  - above `max(16, target + 8)` queued ticks, merges the oldest repeated
    tick away: the same held bits as the previous tick, no edges
    (preferably the next tick is the same input too). Only a tick of time
    is lost: every trigger press and release stays on its tick. Merging
    the oldest tick dropped short presses and
    releases: the server fired fewer (or more) shots than the client
    predicted, and the client reloaded while the server's gun still had
    ammo. No repeated tick: merges the oldest, its edges carry over to the
    next tick. A merged seq is consumed (`client_t::merged_seqs`, -> its
    trigger): newer input windows still carry it and re-queued it, undoing
    the merge (and applying its edges twice). Merged ticks are not
    simulated, so the client's prediction of their physics diverges and
    gets corrected. The weapon runs them, though: it counts input ticks,
    and a step over merged ticks runs them one by one with their own
    triggers (`player::merged_triggers`; a pull on one seeds the shots by
    its seq), firing every shot due in the step. One longer step instead
    fired once at most, a tick late after a reload ended in it, and with
    the trigger of the next tick: the client saw its ammo corrected;
  - after 0.5 s without input, applies a neutral input.
- **View:** with every input tick the client reports its view: the server
  time it was showing then (`_shown_time`, the render time of the last
  frame; INPUT `view_base` and per tick `view_steps` in 0.25 ms, ~1 byte a
  tick). A bullet at flight `f` was on its screen together with the
  players at `view + f`: that is what the server tests.
- **Lag** (`net_server::lag_compensation_of`, per applied tick, capped at
  `game_state::max_lag_compensation`, 1 s), with `behind = S_k − view`
  (`S_k`: the server time of the tick):
  - `lag = rtt/2 + wait`, rounded down to whole ticks and at most `behind`:
    how far a shot is forwarded (an estimate of the time since the client
    fired it; `wait` is the measured time the tick spent in the queue).
    Whole ticks: the server sweeps the path at the flights the client
    sweeps its prediction at (`view + n` ticks);
  - `rewind = behind − lag`: how far the targets are rewound. Only the sum
    matters for the hits, and it is measured, not estimated: the old
    `rtt/2 + interp` was off by the asymmetry of the trips, the RTT jitter,
    the render clock's slewing and an interp reported a trip ago.
  - Without a view (nothing shown yet), `rewind = rtt/2 + 26 ms`.
- **Arrival times** (`received_datagram::arrival`): the kernel's receive
  timestamp (`SIOCGSTAMP`, Linux), the delivery time of a simulated
  latency, otherwise the read time. The socket is read at tick boundaries
  and frames, a datagram may have waited for the read.

### Tick alignment

The jitter buffer counts whole ticks, and an input can arrive anywhere
within a tick: on average it waited half a tick more than needed. The
client slews its real tick period instead (`physic_simulation::
tick_period_scale`, at most ±0.5 %; the simulated timestep stays 1/120 s),
so its inputs arrive `input_margin` (2 ms) before the server applies them
(`net_client::align_ticks`, every frame):
- **Measured by the server:** every STATE carries the margin of the tick
  `ack_seq`: the read at its tick begin minus its arrival, or minus how
  late it arrived after the queue ran dry for it.
- **Independent of the slewing:** the client records per tick how much
  later than without the slewing it ran (`_tick_shifts`). A margin sample
  plus its tick's shift does not change with the slewing, so the expected
  margin of the inputs sent now is the window's smallest one (2 s) minus
  the current shift (`input_margin_estimate`). Without that it overshot by
  the window's delay.
- **Frames:** a tick's input leaves in the frame the tick runs in, so it
  can only move by whole frames. The margin is kept in `[input_margin,
  input_margin + frame interval]` (at 60 Hz vsync: up to a frame early; at
  higher frame rates tighter).
- **Jitter:** below the range, the client speeds up at once. Above it, it
  slows down only by the smallest margin of 6 s (`margin_reduce_window_s`):
  the 2 s minimum jumps up whenever the latest arrival leaves the window,
  and each input late again is a dry tick, a misprediction.
- The server's adaptive buffer still refills a dry queue to its target at
  once (the coarse margin); the slewing then trims it. With jitter, the
  server's target stays in ticks; the alignment matters on steady links.
- **Shown delay at the tick's time:** the ticks now drift against the
  frames. `shown_delay` used the last frame's shown time, up to a frame
  off the tick's time: the server bullets' drawn gap wobbled by up to a
  frame of their speed (`shown_time_at_tick`: the shown time at the
  scheduled time of the tick `_input_seq`).

## Latency settings

| setting | side | default | CLI | console |
|---------|------|---------|-----|---------|
| input jitter buffer | server | adaptive | `--input-buffer=auto\|N` (ticks) | `net buffer auto\|N` |
| interpolation margin | client | 5 ms | `--interp-margin=MS` | `net interp margin MS` |
| interpolation minimum | client | 20 ms | `--interp-min=MS` | `net interp min MS` |
| input margin | client | 2 ms | `--input-margin=MS` | `net input margin MS` |

`net` shows each client's buffer target, input jitter, queue wait and input
margin (server) and the RTT, the interpolation delay with its parts and the
input margin with the tick rate adjustment (client). `hud net
[on/off]` shows the same on the screen, at the top left corner (under the
fps counter), every frame.

The interpolation delay is the state jitter + the longest state interval
in the window (a tick, ~8.3 ms, with server frames as short; 60 Hz server
frames send two ticks at once, ~17 ms) + the margin: ~23 ms on a steady
link with 60 Hz server frames, ~14 ms with 120 Hz ones, which the 20 ms
minimum raises (it was max(jitter + 26 ms, 33 ms), then ~28 ms with a
10 ms margin). The hit predictions do not
need it (they use the exact view), nor does the hand-over of a predicted
knockback since it starts where the server's does (see Hit prediction;
it jumped by up to ~100 px of a hard hit before, whatever the margin, which
the render offset smoothed into a visible speed change). At 3 ms an own bullet's hit
on a falling remote player is mispredicted in the tests (`net: own bullet
hits on a falling remote player ...`, not understood yet); 4 ms passes, the
default keeps a millisecond more.

## Local player: prediction and reconciliation

`input_history` entry per predicted tick:
- the input;
- `tick_extra`:
  - the recoil impulse (applied after the move);
  - predicted hits (`bullet_ref`, impulse; applied before the move);
  - the weapon `sync_state` after the tick;
- the predicted `physic_state` after the tick.

Entries are recorded at tick end, at order 100, after the game's tick end callbacks.

`net_client::reconcile` runs on a STATE with a newer `ack_seq`:
1. **Strip confirmed hits:** remove from the unacknowledged ticks every
   predicted hit whose bullet id is in the server's `hits`. The server state
   already contains those hits, so they must never be applied twice.
2. **Decide whether to replay.** Re-simulate when any of these holds:
   - the player teleported (the counter is newer);
   - the acknowledged tick is not in the history;
   - the physic state differs from the prediction (> 1 px, > 10 px/s,
     or a lock_y or jumps change);
   - the weapon `sync_state` differs (time epsilon 1 ms). This check is
     skipped when the client's gun differs from the server's, or while
     `ack_seq == 0`.
3. **Replay:** restore the server state, and the weapon too if it diverged.
   With `player::replaying(true)`, replay each unacknowledged tick: movement
   input, predicted hits, `sim.step_isolated` + `physic_update`, recoil.
   When the weapon diverged, the trigger input runs through
   `weapon_instance::replay_tick`, which fires no bullets or sounds, and its
   recoil replaces the recorded one.
4. **Smooth:** the position change goes into the render offset
   (`player::add_render_offset`). The offset decays in `physic_update` with a
   50 ms half-life; an accumulated offset over 400 px snaps, and a teleport
   resets it.

`physic_state` and STATE carry the raw velocity
(`physic_point::get_raw_velocity`). While lock_y is set, `get_velocity()`
hides the vertical part, but it still exists. Gravity is applied after the
player's update, so a standing player ends every tick with one gravity step
of it (`g·dt`, 19.2 px/s), on the server and in the prediction alike.
- It matters at a platform edge (the first tick in the air), and it drives
  the weapon bobbing (`physic_point::g_force`, the hands).
- Anything that rebuilds the velocity from `get_velocity()` drops it:
  `apply_impulse`, `velocity(get_velocity() + ...)`. Re-simulations apply
  the recorded impulses only when nonzero, exactly like the live tick.
  Otherwise every replayed tick diverges from the server by 19.2 px/s,
  causing endless 0 px corrections and jiggling hands.
- Remote players are restored from snapshots every frame with the same raw
  velocity. Restoring them with the shown one made the bobbing flip
  whenever a frame boundary fell between ticks.

The stats are `corrections()`, `max_correction()` and `weapon_corrections()`.
Each correction logs at detail level (`net client: prediction correction at
tick N: X px`, `weapon correction ...`).

## Remote players

- **`server_clock`:** estimates the server time from the least delayed state
  in a 2 s window. Its offset spread is the latency jitter:
  `interp_delay = clamp(spread + max state interval + margin, min, 300 ms)`,
  by default a 5 ms margin and a 20 ms minimum (see Latency settings). Remote players are
  rendered at `server_now - interp_delay`, using a monotonic render clock
  that slews by at most 10 % (and snaps beyond 0.5 s).
- **`snapshot_buffer`:** stores each remote player's snapshots (state,
  on_left, shooting, hits, teleports), out of order, for 1 s. The sample is
  interpolated, but never across a teleport counter change: the player jumps
  to the first snapshot after the teleport.
  It never interpolates across a newly reported hit either. A retroactive hit
  rewrote the server's path, so the older snapshot is stale and the victim
  would cross the whole knockback within one snapshot interval. Instead, the
  newer snapshot is moved back along its velocity, with its hits, which
  confirm the prediction at that frame.
- **Drawing** (`player::drawn_position`): a remote player is already placed
  at the frame's render time, so it is not extrapolated again between ticks.
  The tick extrapolation of locally simulated objects used to draw it up to
  a tick ahead and snap back every tick: a sawtooth of `vel·16 ms`. The
  correction offset decays continuously between ticks, not in tick-sized
  steps.
- **Applied every frame** (`apply_remote_snapshots`): sampled snapshot +
  predicted knockback offset → `restore_physic_state`, `place_legs`,
  `on_left`, and `shot` / `relax` for the visual weapon. Only the newest
  STATE changes known player configs, crates, dropped guns and the alive
  bullet set.
- **Jump smoothing:** a jump of the shown position goes into the render
  offset. That covers a confirmed prediction handing over, or the server's
  rewritten history after a retroactive hit. The jump is measured against
  the previous frame's applied state moved on by the average of the old and
  new velocities. The tolerance is 8 px + ½·|Δv|·dt, because a velocity change
  inside the frame is motion, not a jump.
  Do not measure it against the locally simulated state: with frames shorter
  than a tick, the local tick's movement looks like a jump. That was a bug,
  and `tests/net_test.cpp` covers it at 4, 10 and 16 ms frames.
- **Legs:** they live in absolute physics coordinates, so after a
  snapshot moves the player they are re-anchored (`place_legs`). They are
  drawn with the render offset like the body.
- **Weapon:** a remote player's weapon runs locally from the snapshot's
  `shooting` flag, for visuals only. Its ammo and reload are not synced
  (cosmetic).

## Bullets and lag compensation

**Spawning:**
- Every weapon bullet goes through `game_state::spawn_bullet`. A bullet
  never hits its owner, nor players of the owner's group (`bullet_mgr::shot`
  with `player::owner_checker` and the group getter). Grouped bullets hit
  players only.
- **Network shooter** (server): the bullet spawns forwarded to
  `start + vel·lag` along its ballistic path (`ballistic_forward`). The
  skipped path is swept tick by tick (`game_state::hitscan`): the part
  ending at flight `f` against the other players rewound by
  `rewind + lag − f` (`player::position_trace_lookup`), i.e. at `view + f`
  like the rest of the flight. A hit consumes the bullet. (It used to be
  tested against the players at one time, `S_k − rewind`: newer than the
  shooter saw, by up to `lag`.)
- **Drawn from the muzzle** (`bullet::catch_up_t`): the server's screen
  shows the network shooter firing now, its bullet `lag·speed` ahead. It
  is drawn `lag` earlier in its flight and catches up at 2× its speed
  (`game_state::bullets_catch_up_update` every tick), visual only, gone
  30 px before a player it may hit ahead on its path
  (`game_state::bullet_catch_up_limit`, shared with the clients' catch-up).
- **Compensated:** a network shooter's bullet with a lag or a rewind. A lag
  under a tick rounds down to 0 (a short input queue on a fast link), the
  rewind still compensates it: it used to collide with the players of the
  present, a rewind later than the shooter saw them.
- **Whole flight:** a lag-compensated bullet (`bullet::lag_compensate`)
  does not collide with players physically. Every tick, the segment it flew
  is swept against the players rewound by the same amount
  (`rewound_bullets_tick_update`). Without this, fast-falling players were
  already elsewhere when the bullet arrived.

**Retroactive hits** (`game_state::apply_hit_at`):
- A lag-compensated hit lands on the victim's state at the time the shooter
  saw. `game_state::history` keeps 1.5 s of ticks per player (`history_ticks`): time, teleport
  counter, controls, impulses before and after the move (hits and recoil:
  `player::begin_tick_record` / `take_tick_impulses`) and the end state.
- `apply_hit_at` steps:
  1. finds the last finished record at or before the hit time, with the same
     teleport counter;
  2. adds the hit to the next tick's before-move impulses;
  3. restores the record's end state;
  4. replays the following ticks with their recorded controls and impulses;
  5. rewrites the history's end states and the position trace;
  6. restores the live controls.
- If the hit time is the present, or no history covers it, the hit applies
  now.
- **Effect:** the shooter's predicted knockback becomes the authoritative
  one. The victim and third-party clients see the rewrite as a smoothed
  correction. On the server's screen, the victim jumps to the rewritten
  present by the knockback of the rewound time: the jump goes into its
  render offset, so the host sees it pushed away smoothly instead of
  teleporting.

**Server bullets on clients** (`spawn_server_bullet`): simulated in the
frame of the predicted local player (`bullet::frame_on_client`), so the
client sees, and its prediction hits, the bullet the server will collide
with its player.
- **Why:** the local player runs ahead of the server (the input's trip
  plus its wait in the queue), and the server tests a lag-compensated
  bullet against the players `rewind` in the past. Shown in server present
  time, a bullet was `lead + rewind` behind the one the server tested
  against the local player, so every hit on a moving (falling) player was
  mispredicted: a hit the server did not have, or a missed one.
- **Tick time:** the server applies one input tick per tick, so the server
  time of input tick `k` is `offset + k·dt`, with `offset` from the newest
  states' `server_time − ack_seq·dt` (`update_tick_time`): exact (the time
  of the tick that applied `ack_seq`), so the newest one is taken as is. It
  moves by whole ticks when the server merges queued ticks or runs dry;
  smoothed, it lagged a merge by ~0.3 s and framed the server bullets a tick
  ahead (early predicted hits, e.g. on a player knocked away from a stream).
  `local_frame_time()` is the server time of the local player's current
  tick.
- **Spawn:** at the flight time `forward + (local_frame_time − state time) +
  rewind_ticks·dt`, through the same `game_state::ballistic_forward` the
  server used. The rewind is split on the server's tick grid
  (`hit_test_t`) into whole ticks (`rewind_ticks`, rounded **down**) and a
  fraction (`rewind_lead`): the server tests the path flown in tick
  `k + rewind_ticks` against the player `rewind_lead` of a tick before the
  end of tick `k` and applies the hit in tick `k`. Rounded up, the hit was
  a tick early and tested a tick off (a few px correction on every hit
  whose rewind is not a whole number of ticks). Its tracer color is the
  owner's (from the config).
- **Hits on the local player** (`tick_hit`), tested like the server does:
  - lag-compensated (`rewind > 0`): the path flown in a tick against the
    player's state `rewind_lead` of a tick before the tick's end (the
    server's interpolated position trace). The hit goes before the move of
    the tick after the last one finished by then (`apply_hit_at`);
  - physical (`rewind == 0`, the host's player and bots): the simulation's
    continuous test (`physical_hit`: the path crossing a side of the box
    moving with the tick's start velocity). The hit goes before the move of
    the tick itself.
  A hit on an already simulated tick goes into its `input_history` entry,
  and the following ticks are re-simulated (`apply_tick_hit`).
- **Past ticks:** a bullet arrives `lead + rewind + state trip` into its
  flight. Its path through the ticks it already crossed is tested against
  their states at once (`predict_past_hit`), the next ticks are swept at
  every tick begin (`sweep_framed_bullets`).
  - The history keeps ~1.07 s of acknowledged ticks (`acked_ticks_kept`, more than
    the longest rewind); the acknowledged tick holds the server's state.
    A teleport drops them.
  - An acknowledged tick `k` can still be hit by a lag-compensated bullet:
    the server sweeps it at `k + rewind_ticks`, which is in the future
    while `k + rewind_ticks > ack`. A shooter with ping (a client) puts most
    close-range hits there. Before, they were not predicted: the bullet was
    shown already past the player, and the hit came later as a correction.
  - Never predicted: hits the server already swept (`k + rewind_ticks <=
    ack`), physical hits on acknowledged ticks, and bullets dead on the
    server. A bullet consumed by the server's forward hit-scan arrives
    dead, with its hit already in the state.
- **Rewound hits are late on the server:** it registers a lag-compensated
  hit `rewind_ticks` after the hit tick. Until then, reconciliation skips
  the states acknowledging a predicted rewound hit (`_rewound_hits`).
- **Remote players** are shown in the past: the bullet path delayed by
  `local_frame_time − render time` (`shown_delay`) is swept against them.
  The path from the muzzle up to there is hit-scanned on spawn.
- **Drawing:** the bullet is drawn at its physical (local frame) position,
  turned like its simulated path, like on the server. Remote players are
  shown in the past, so the bullet may fly through a shown remote player
  ~`shown_delay` before the hit (and the knockback) shows up; it
  disappears at a wall exactly when the local simulation kills it.
- **Drawn from the muzzle** (`bullet::catch_up_t`): the shooter is shown
  firing now, its bullet `flight` (~`forward + frame_ahead + rewind`) into
  its flight. It is drawn that much earlier, from the muzzle, and catches
  up at 2× its speed (`game_state::bullets_catch_up_update` every tick,
  shared with the server's catch-up), visual only, gone 30 px before the
  local player if it may hit it ahead on its path
  (`game_state::bullet_catch_up_limit`). A remote player (server-driven, not
  predicted) is hit when the shown frame (`framed_t::shown_flight`) reaches
  it: while one is ahead of the shown frame, the bullet is not drawn past
  that frame (the catch-up stops there, never grows) and is limited in it,
  so it reaches the shown player when the hit shows up instead of
  vanishing short of it (`game_state::bullet_target_along`).
- Before the first acknowledged input there is no tick time: the bullet is
  forwarded by its state's age (present time) and collides physically.
- Bullets the server already consumed are not spawned, and bullets missing
  from the alive set are removed.
- **Cost:** a remote bullet is simulated further along its flight, about
  `(shooter lag + own RTT and queue wait + shooter rewind)·speed`; the
  catch-up hides it near the muzzle, but a bullet that hits a wall during
  the catch-up disappears short of it.

**Own bullet prediction** (client's predicted player):
- The bullets spawn at once, without an id. Shots are deterministic:
  - `weapon_instance::seed_shots(pull tick)` seeds the shots on a trigger
    pull;
  - dispersion is generated from (pull tick, shot index);
  - each bullet carries the key `shot_key_t {pull_seq, shot_index, pellet}`,
    so client and server fire identical bullets.
- A server bullet with the same key confirms the prediction: it gets the
  server id and the authoritative start/velocity, forwarded by its own
  simulated flight time. If that bullet is already dead on the server, the
  prediction is removed.
- Unconfirmed predictions expire after `max(0.5 s, 2·rtt + 0.25 s)`.

## Hit prediction

`bullet_ref {id, optional shot key}` sits in the bullet's physic point
`user_any`. `player::on_bullet_hit(impulse, pos, ref)` does the following:
- **Authoritative player** (server, or offline): applies the knockback
  (`apply_hit_physics`) and records the bullet id in `recent_hits` for 1 s.
  That record is the source of `state_player::hits`.
- **Predicted local player:** applies the knockback now and queues it into
  the tick's history entry (see reconciliation). A hit the server never
  registers is undone by the next correction.
- **Remote mirror:** no physics. The hit is queued for
  `knockback_offset` (`net_sync.hpp`), an offset on top of the snapshots.
  Two ghost states start from the shown state, one with the hits applied
  and one without. They are stepped with the real physics on a hidden ghost
  player (`ghost_of`, outside of the simulation). The offset is their
  difference, so it follows platforms and edges.
  - **Start:** where the server starts the motion. The server applies a
    retroactive hit before the move of the tick after its last record at or
    before the hit time (`apply_hit_at`), so the confirming snapshots show
    the motion from that tick on. An own bullet's hit carries its hit time
    (`view + flight`, `player::predicted_hit_t::at`): its episode starts
    from the snapshot at or before it (`snapshot_buffer::at_or_before`, the
    server's tick grid), stepped up to the shown time (or waiting for it).
    Each episode keeps its own step phase (`since_step`). Started at the
    shown time of the detection instead, a hard hit's hand-over jumped by
    up to ~100 px (the detection is a frame or two off, the render clock
    slews), and the render offset took ~0.2 s to absorb it: the remote
    player first sped up or slowed down by a third. Server bullets' hits
    are still predicted at the shown time.
  - **Episodes:** hits in a row accumulate in one episode, because
    knockback physics is not additive.
  - **Confirmation:** a hit reported by the snapshots is dropped from the
    prediction, since the server's retroactive hit already shows the same
    motion. The remaining hits of its episode restart from the snapshot.
  - **Unconfirmed hits:** fade out (τ 0.15 s) after
    `clamp(1.5·view_delay + 0.2, 0.3, 1) s`.
  - **No limit:** the offset follows the physics until the hit is confirmed
    or fades out. A 150 px cap used to stop a hard hit in mid-air, after
    which the confirmation crossed the rest of the knockback at once.

Own-bullet hits on remote players carry a shot key, which is resolved to
the server id when the bullet is confirmed: in the knockback episodes and in
the hits still queued on the players (predicted in a tick sweep, taken by
the next frame). A queued hit left with the key only was never confirmed by
the snapshots, so its predicted knockback stayed on top of the server's
(the remote player flew twice as far for a second).

**Own bullets vs. remote players** (`own_bullet_hit`): own predicted
bullets don't collide with players physically. Every tick their path is
swept against the remote players where the server tests it: the path
ending at flight `f` against the players at `view + f`, `view` being the
view the client reported for the shot's tick (`bullet::framed_t::view`, set
at the end of the tick that fired it). That is about the shown time, so it
is in the snapshots, and it needs no clock estimate (an asymmetric link is
covered by a test). Plus the predicted knockback offset, except of the hits
the sampled snapshot already reports (it can be newer than the shown one,
which confirms them: `knockback_offset::offset_unless`). Testing them where they are drawn instead missed
the server's hits on fast-falling players by up to a tick of fall: a hit in
the head the server did not have, or a bullet under the feet that the
server registered. A teleport clears the server's position trace: a newer
teleport in the snapshots makes the test use the first snapshot after it
(a teleport the client doesn't know yet is not predictable).

## Weapons

- **Tick-based logic:** all weapon logic runs in `weapon_instance::tick`,
  once per player tick:
  - animations, the reload included (`shot`, `reload`, `load_start` →
    `shell`* → `load_end`);
  - the fire rate.
  `draw()` only shows the current animation and interpolates it between
  ticks.
- **Counted time:** a network player's weapon counts its applied input ticks
  (`input_tick(seq)`), not server ticks. The server can tick without a new
  input, so a reload ends, and a held trigger fires, on the same input tick
  on the server and in the prediction. A tick without a new input tick
  (the queue ran dry) skips the weapon logic: the held trigger fired, or a
  reload started, on a tick the prediction does not have.
- **Synced state:** `sync_state` holds ammo, animation id and time,
  `since_shot`, reloading/trigger flags, shot index and seed base. The
  server sends it, and the client corrects its predicted player's weapon
  with it (see reconciliation). Expect weapon corrections only after merged
  input ticks or gun changes.

## Teleports

`player::teleport(pos)` moves the player instantly and stops it. It is used
by `game_state::player_spawn`, i.e. spawn and respawn. It also:
- clears the position trace (no lag compensation path across it);
- resets the render offset;
- places the legs;
- bumps the teleport counter that STATE carries.

Clients jump remote players at the first snapshot after a counter change,
dropping their predicted knockback. The local player snaps to the first
state with a newer counter (no smoothing). Retroactive hits never reach
across a teleport.

## Other synced state

- **Appearance, gun and group:** sent in HELLO, then as `player_config`
  (see Protocol). A changed config is applied part by part (a new pistol
  resets the weapon). An empty pistol means no gun. A dropped primary gun
  is replaced by the secondary one on the server.
  Guns are dropped on the server only: a client sends the DROP edge, and
  the dropped gun arrives in STATE.
- **Crates and dropped guns:** server-authoritative physics. Clients keep
  ghosts driven by STATE (`crates_networked`); these are created on new ids
  and removed when gone.
- **Groups:** synced, so client-side collisions respect teams.
- **Death:** only authoritative players die locally. Server-driven players
  die and respawn on the server, and arrive as a teleport.

## Testing and debugging

- **Simulated latency:** `udp_channel::set_simulated_latency(min_ms, max_ms)`
  delays every datagram sent and received by a random amount, so datagrams
  also reorder. `--define=DFDH_DEBUG_PING` sets its default range. Both
  sides of a link delay a datagram twice, on send and on receive.
  - Mean one-way trip ≈ 2× the mean delay; RTT ≈ 4×.
  - For example, ~100 ms ping needs a delay of ~25 ms (24..26 ms).
  - If only one side of the link is built with it, halve these numbers.
  - `set_simulated_send_delay(extra_ms)` delays the sent datagrams only: an
    asymmetric link.
- **`tests/net_test.cpp`:**
  - protocol round-trips, input queue, prediction units,
    `knockback_offset`, retroactive history;
  - `net_game_fixture` / `hit_fixture`: a real server and client
    `game_state` on `lvl_aes` over loopback with simulated latency.
- **Manual clock in tests:** fixtures run on a manual `game_clock`
  (`game_clock_manual_guard`), and `frame()` advances it by `frame_period`
  (5 ms by default) instead of sleeping. With a manual clock,
  `udp_channel::recv` does not block, since loopback datagrams are readable
  immediately. The whole suite takes seconds.
  - A loop that waits for time must advance `game_clock`, or it never ends.
  - Measure time with `game_clock::now()`.
- **Flaky checks:** jitter (80..170 ms) can overflow the input queue and
  merge ticks. Checks that expect zero corrections use a steady latency.
- **Debug log:** run with debug logging to see `prediction correction` /
  `weapon correction` lines. Frequent large corrections with steady latency
  point at a prediction that is not deterministic. Typical causes:
  - something updated per frame instead of per tick;
  - wall-clock time used instead of `game_clock`;
  - state not included in `physic_state` / `sync_state`.

## Known limitations

- **Remote weapons:** their ammo and reload state run locally (cosmetic) and
  can differ from the server's.
- **Unpredictable hits on the local player:** a bullet that hits within
  about `shooter lag` of its flight is consumed by the server's forward
  hit-scan. A physical one (`rewind == 0`) that hits within about
  `own RTT + queue wait` hits an acknowledged tick the server already
  tested. Both are corrected by reconciliation.
- **Remote players vs. framed bullets:** they are swept by the bullet path
  delayed along the muzzle trajectory; that stops when the shown bullet
  dies, so a hit in the last `shown_delay` of its flight is not predicted
  (the snapshots correct it). The bullet is drawn at its physical position
  once caught up: if the catch-up was over before a remote player came
  ahead of its shown frame (moving into its path), it
  passes visibly through the shown player ~`shown_delay` before the hit
  shows up. Held at the shown frame for a remote player, it disappears up
  to `shown_delay·speed` short of a wall behind it.
- **Unpredicted hits on remote players:** if the client simulation missed a
  hit that the server's lag compensation registered, it shows up as a
  smoothed correction about one view delay later. With fast-falling or
  moving targets, the drawn path can briefly leave the server's (≈20 px).
- **History limits:** retroactive hits reach back at most 1.5 s of ticks, and never
  across a teleport. Older hits apply in the present.
- **Bots:** exist on the server only and are synced like other players.
  Levels must match by name.
- **One player per client address**, and no reconnection, encryption or
  matchmaking.
