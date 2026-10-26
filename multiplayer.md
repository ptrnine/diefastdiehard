# Multiplayer implementation plan

## Goal

Add simple server-authoritative multiplayer to diefastdiehard over **UDP**:

* **Server** — run the game binary with `--server --port=PORT`
  (e.g. `./dfdh --server --port=10443`).
* **Client** — run the game binary with `--client IP:PORT`
  (e.g. `./dfdh --client 127.0.0.1:10443`), optionally `--name=NAME`.

A client is attached to a local player through a **network player
controller**: the local keyboard drives the local player (client-side
prediction) and the same key state is streamed to the server, which is
authoritative. The server broadcasts player state snapshots to all
clients, which apply them to remote players.

On join, the client sends its **skin (face/body id + body color),
tracer color and current gun** to the server; the server creates the
remote player with that appearance and gun, and includes each player's
appearance in the state snapshots so the client can render remote players.

## Architecture

```
client A                          server                         client B
  |  HELLO(name, skin, gun)  --->  (creates player "A",          |
  |<-- WELCOME(name) ----------------  binds net-controller,    |
  |  INPUT(32-tick window) ------->  applies 1 input tick/tick) |
  |  STATE(per client, per frame) <-  broadcasts snapshots  ---> |
```

* Synchronous, non-blocking UDP in the main loop (`sf::UdpSocket::recvFrom`
  with a small timeout each frame) — no extra threads, no locks.
* UDP datagrams carry a 4-byte header `u16 type, u16 payload_size` followed
  by a payload serialized with the existing `ss::serializer` (little-endian).

### Protocol messages (protocol version 5)

| id  | dir        | payload |
|-----|------------|---------|
| 1   | C → S      | HELLO: `u16 version`, player name, pistol section, face_id, body_id, body color, tracer color. A version other than `net::protocol_version` is answered with ERROR |
| 2   | S → C      | WELCOME: player name |
| 3   | C → S      | INPUT: `u32 last_seq`, `u32 client_time_ms`, `u16 rtt_ms`, `u16 interp_ms`, `vector<u16> bits` — the input bits of the last up to 32 ticks, `bits[i]` belongs to tick `last_seq - size + 1 + i` |
| 4   | S → C      | STATE (encoded per client): `u32 tick` (broadcast sequence number), `u32 server_time_ms` (time of the simulated tick), `u32 ack_seq` (last input tick applied to the client's player), `u32 echo_client_time_ms` + `u16 echo_hold_ms` (RTT echo of the newest input); per player: name, pistol, face_id, body_id, body color, tracer color, physic state (pos.xy, vel.xy, jumps_left, lock_y, accel factors), on_left, shooting, group, teleport counter, ids of the bullets that hit the player in the last second; the bullets spawned this frame (owner, id, shot key: pull_seq/shot_index/pellet, muzzle start.xy, spawn pos.xy, vel.xy, hit_mass, tracer color, gravity); the crates (id, pos.xy, vel.xy, landed); the dropped guns (id, section, pos.xy, vel.xy, angle, on_left, long_shot); the ids of all bullets alive on the server |
| 5   | C → S      | LEAVE: player name |
| 6   | S → C      | ERROR: message string (join failed, version mismatch, etc.) |

Input bits (per tick: held states are the full current state, edges are
set on the tick of the key press only):

| bit | meaning |
|-----|---------|
| 0 | move LEFT (held) |
| 1 | move RIGHT (held) |
| 2 | JUMP (edge) |
| 3 | JUMP-DOWN (edge) |
| 4 | SHOT (held) |
| 6 | LONG-SHOT (held) |
| 7 | DROP gun (edge) |

## Netcode

Both sides run the fixed 60 Hz simulation; `physic_simulation` exposes
tick begin/end callbacks that the network layer hooks into.

### Ordered per-tick input

* The client samples its input once per **simulation tick**
  (`net_client::input_source`, i.e. `net_player_controller::take_input_state`),
  applies it to the local player and sends, every frame, a window of the
  last 32 ticks. A lost or reordered datagram costs nothing: every newer
  datagram repeats the recent history.
* The server queues input ticks by seq, ignores ticks it already applied
  (so a late or duplicated datagram can never bring back an old input —
  no more stuck shooting) and applies exactly one input tick per server
  tick. A 3-tick jitter buffer refills after the queue runs dry (a dry
  tick repeats the last held state without edges); above 8 queued ticks
  the oldest are merged keeping their edges; after 0.5 s without input
  the player gets a neutral input.

### Client-side prediction and rollback

* Every predicted tick is kept in an `input_history` (input, recoil
  impulse, predicted physic state) until the server acknowledges it.
* A STATE carries the server physic state of the local player after its
  input tick `ack_seq`. If it differs from the prediction of that tick
  (> 1 px, > 10 px/s, ground/jump state), the client restores the server
  state and re-simulates the unacknowledged ticks for the local player
  only (`physic_simulation::step_isolated`, `player::replaying`). The
  visual jump is smoothed by a decaying render offset; corrections above
  150 px (respawn) snap.
* The local player is `server_driven`: no local bullets, no local hit
  knockback (it arrives through reconciliation), no local death.

### Remote players

* `server_clock` estimates the server time from the least delayed state
  in a 2 s window and derives the interpolation delay from the latency
  jitter (33..300 ms). Remote players are shown at
  `server_now - interp_delay`, interpolated between snapshots
  (`snapshot_buffer`), with a monotonic, slewed render clock.
* States arrive out of order: only the newest one updates configs,
  crates, dropped guns and the alive bullet set.

### Lag compensation

* Clients report their RTT (measured from the echoed client time) and
  interpolation delay with every input. The server sets per network
  player `lag = rtt / 2 + queued input ticks` and
  `rewind = lag + interp_delay` (both capped at 1 s).
* All weapon bullets go through `game_state::spawn_bullet()`. A network
  player's bullet spawns forwarded to `start + vel * lag` (ballistic with
  bullet gravity); the skipped segment is a hit-scan
  (`game_state::hitscan`) against the other players **rewound** to where
  the shooter saw them (`player::position_trace_lookup`). A hit applies
  the bullet hit (`player::on_bullet_hit`) and consumes the bullet.
* The compensation covers the bullet's whole flight, not only the skipped
  segment: the shooter sees its bullet fly against the players drawn in
  the past, so a lag-compensated bullet (`bullet::lag_compensate`) does not
  collide with players physically; every tick the path it flew is swept
  against the players rewound by the same amount
  (`game_state::rewound_bullets_tick_update`). Without this, a falling
  player (fast, accelerating) was already far below the spot the shooter
  aimed at when the bullet arrived.
* The broadcast carries the muzzle position. A client forwards a
  received bullet once more by the age of its state (half the fastest
  recent round trip plus this datagram's extra delay over the fastest
  recent state), so it appears at `start + vel * (shooter lag + own lag)`,
  in present time, and hit-scans
  the skipped segment against its own view for hit feedback (hits are
  decided by the server). Bullets the server already consumed are not
  spawned.

### Shots and recoil

Players (weapons, shots, recoil, death) are updated at the end of every
simulation tick (`game_state::players_tick_update`), not once per frame,
and the weapon fire rate counts simulation time. A network player's
weapon counts its applied input ticks (`weapon_instance::input_tick`):
the server may tick without a new input while its input queue refills,
so this keeps every shot, and its recoil, on the same input tick on the
server and in the client's prediction. The prediction records the recoil
as an after-movement impulse of the tick that fired it and re-simulations
apply it there (the client's tick end callback runs after the game's,
`add_tick_end_callback(..., order)`).

### Own bullet prediction

* The client's local player (`player::predicts_bullets`) spawns its own
  bullets immediately, without a net id.
* Every network shot has a deterministic key: the input tick of the
  trigger pull, the shot index since the pull and the pellet number
  (`game_state::shot_key_t`). On a pull, the weapon is seeded with the
  pull tick (`weapon_instance::seed_shots`) and the dispersion of each
  shot comes from (pull tick, shot index), so the client and the server
  fire exactly the same directions.
* The broadcast carries the key. The client confirms the predicted
  bullet with the same key: it takes the server id and the authoritative
  start/velocity, forwarded by its own simulated flight time (no lag
  estimate needed), or is removed if the server bullet is already dead.
  A server bullet without a prediction is spawned normally; a prediction
  the server never confirms expires after `max(0.5 s, 2 * rtt + 0.25 s)`.

### Teleports

`player::teleport(pos)` (used by `game_state::player_spawn`, i.e. spawn and
respawn) moves the player instantly, stops it, restarts its lag
compensation position trace and bumps its teleport counter, sent in the
state. Clients never interpolate a remote player across a counter change
(it jumps at the first snapshot after the teleport) and drop its pending
predicted knockbacks; the local player snaps to the server state (restore
and re-simulate, no correction smoothing) as soon as a state with a newer
counter arrives. The client no longer spawns its local player on join: the
first server state places it.

### Hit prediction

Bullet hits push the players on the client right away; the server reports
per player the ids of the bullets that hit it in the last second
(`player::recent_hit_ids`, `state_player::hits`), so the predictions are
matched to the authoritative hits by bullet (`bullet_ref` on the bullet's
physic point: the server id, or the shot key of an unconfirmed own
bullet, resolved to the id when the bullet is confirmed).

* **Local player** (any bullet or the cosmetic hit-scan): the knockback
  (`player::apply_hit_physics`) applies at once and is recorded with its
  tick in the prediction history, so re-simulations keep it. Hits the
  server state already contains are stripped from the unacknowledged
  ticks (never applied twice); a hit the server did not register is undone
  by the next reconciliation (smoothed).
* **Remote players** (own bullets, other players' bullets): the hit
  starts a `knockback_offset` on top of the interpolated snapshots,
  simulated with the game physics: two ghosts of the player start from
  its shown state, one with the hit applied and one without, stepped with
  `physic_simulation::step_isolated` + `player::physic_update` on a
  hidden ghost player (outside of the simulation, so the shown player's
  legs and weapon bobbing are never touched), and the offset is their difference, so
  it follows the level geometry (slides off platform edges, falls, lands).
  When the rendered snapshots show the hit, a second pair starts from the
  last snapshot before the hit, brought to the render time, and is
  subtracted: the offset converges while the server's motion takes over.
  A hit the server did not register within
  `max(1 s, 2 * rtt + interp + 0.3 s)` fades out.
* Player groups are synchronized, so client-side collisions respect teams.
* Legs are placed by the physics in absolute coordinates: players moved
  outside of it (snapshots) re-anchor them (`player::place_legs`), and they
  are drawn with the correction render offset like the rest of the body.

### Testing with latency

`udp_channel::set_simulated_latency(min_ms, max_ms)` delays (and so
reorders) every datagram it sends and receives; `DFDH_DEBUG_PING`
(`./build-plz --debug --define=DFDH_DEBUG_PING`) enables 80..170 ms by
default. Both ends delay, so a client and a server built with it see a
one-way trip of 160..340 ms (~500 ms ping).
`tests/net_test.cpp` runs real-time loopback games with simulated latency.

## File layout

| file | purpose |
|------|---------|
| `src/net/net_protocol.hpp` | message ids, structs, (de)serialization, datagram encode/decode |
| `src/net/net_udp.hpp`       | thin `sf::UdpSocket` wrapper: bind/connect, `send_to`, `recv` loop |
| `src/net/net_player_controller.hpp` | client-side controller: key events → `player_input_state` (extends `player_controller` for the keymap) |
| `src/net/net_server.hpp`    | server: join/leave, input application, state broadcast |
| `src/net/net_client.hpp`    | client: join handshake, input sending, state application |
| `src/player.hpp` (extend)   | `player_input_state`, `player::apply_input()` |
| `src/game_state.hpp` (extend) | `controller_attach()` for attaching a ready-made controller |
| `diefastdiehard.cpp` (extend) | `--server --port=` / `--client ip:port` parsing, wiring net layer into the loop |
| `tests/net_test.cpp`        | Catch2 tests |
| `build-plz` (extend)        | `net_test` build target |

## Step-by-step plan

### Step 1 — Network protocol

Add `src/net/net_protocol.hpp`: message id enum, payload structs
(`hello`, `welcome`, `input`, `state`, `leave`, `error`) and
`serialize()/deserialize()` methods; `net_msg` (type + payload span) encode
/ decode helpers.

**Tests** (`tests/net_test.cpp`):
* round-trip encode/decode for every message type;
* header parsing / out-of-range / truncated datagrams;
* input bits ↔ `player_input_state` mapping.

### Step 2 — UDP channel

Add `src/net/net_udp.hpp` wrapping `sf::UdpSocket`:
`bind(port)`, `connect(addr)`, `send_to(addr, bytes)`, `recv(timeout)`
draining loop returning `(addr, datagram)` pairs.

**Tests**: loopback UDP send/receive through the wrapper on a local port,
including multi-packet drain and short-timeout behavior.

### Step 3 — Input abstraction + network player controller

* Add `player_input_state` and `player::apply_input()` in `src/player.hpp`
  (held move/shot + jump/shot/long-shot edges) and reuse the existing
  `move_left/move_right/stop/jump/jump_down/shot/relax/enable_long_shot`
  methods.
* Add `net_player_controller` (derived from `player_controller` so it
  reuses the `player_controller_<id>` keymap settings): tracks key events
  and yields a `player_input_state` per frame.

**Tests**:
* `apply_input` drives the player (velocity direction after a tick, jump
  flag, shot trigger state) — no textures/sound needed for these paths;
* controller event tracking: press/release combinations produce the right
  `player_input_state` and edge flags are consumed once.

### Step 4 — Server

Add `src/net/net_server.hpp`:

* `net_server(port)` — binds a UDP socket;
* `update(game_state&)` called once per frame:
  * drain datagrams;
  * HELLO: validate name (unique among connected clients), create the player
    via `game_state::player_create`, apply the received skin/tracer/gun
    (`set_body/set_face/setup_pistol/tracer_color`), spawn it on the
    current level, answer WELCOME; duplicate name → ERROR;
  * INPUT: store per-client held bits + edge queue;
  * LEAVE: remove client and its player;
  * each tick: apply accumulated `player_input_state` to the client's
    player, then broadcast a STATE snapshot of all players to every client.

**Tests**:
* full loopback handshake: a raw UDP client sends HELLO, server answers
  WELCOME, player appears in `game_state` with the requested skin/gun;
* INPUT packets drive the player through `apply_input` (velocity/position
  change after sim ticks);
* LEAVE removes the player; duplicate HELLO name yields ERROR.

### Step 5 — Client

Add `src/net/net_client.hpp`:

* `net_client(server_address, name, skin, tracer, gun)` — connects and
  sends HELLO immediately;
* `update(game_state&)` called once per frame:
  * drain datagrams;
  * WELCOME: mark joined (like the existing `client_sync_state`), fire
    `sig_joined(name)`;
  * ERROR: fire `sig_error(msg)`;
  * STATE: for every remote player ensure it exists locally (create +
    appearance if new), apply position/velocity/jumps/direction; skip the
    locally controlled player (its own movement is client-predicted);
  * `send_input(player_input_state)` — sends an INPUT datagram.

**Tests**:
* loopback handshake against `net_server`: WELCOME received, `sig_joined`
  fired;
* STATE application: remote player is created locally with the right skin
  and its position/velocity follow the server snapshots;
* local player name is skipped when applying state.

### Step 6 — Game integration

Extend `diefastdiehard.cpp`:

* parse `--server --port=PORT` (default port `10443`) and
  `--client IP:PORT` (with optional `--name`, default `client0`);
* server mode: create `net_server`, call `server->update(gs)` in
  `game_update()`;
* client mode: read local `player_configurator` for the player name to get
  skin/tracer/gun, create `net_client`; on `sig_joined`:
  `player_create(name)`, attach a `net_player_controller` via a new
  `game_state::controller_attach(id, ctrl)` + `rebuild_controlled_players()`
  so the local keyboard works exactly like a regular player controller;
  in `handle_event()` feed events to the net controller and send
  `take_input_state()` each frame; on shutdown send LEAVE.

**Build/test**: add `net_test` target to `build-plz`; build the debug
build with `./build-plz --debug` and run all tests
(`cfg_test`, `tasks_test`, `net_test`).

**Smoke test** (headless, `xvfb-run`): start a server on a free port,
connect one client with `--client 127.0.0.1:PORT --name=mp1`, check that
the server log shows the join and the client log shows WELCOME and the
local player creation.

## Out of scope (future)

* latency compensation / interpolation smoothing, server tick rate,
  kick/death/event broadcasting, matchmaking, reconnection, encryption.

## Status

Implemented and verified (steps 1–6):

* `./build-plz --debug` builds clean; all suites pass:
  `cfg_test` (181 assertions), `net_test` (217 assertions, 8 cases),
  `tasks_test` (7 assertions).
* Smoke test (`xvfb-run`): `dfdh --server --port 10443` +
  `dfdh --client 127.0.0.1:10443 --name smoketest` — client joins,
  server logs the join, keyboard is bound to the network controller
  (`controller_attach(1000)`), input flows to the server player.

### Fixes made along the way

* **`fixed_str_t` off-by-one** (`src/base/fixed_string.hpp`): the
  string-literal constructor copied `_len - 1` chars instead of `_len`,
  silently truncating every name built from a literal ("tester" → "teste").
  Fixed: `memcpy(_data.data(), str, _len); _data[_len] = '\0';`
* **Empty-pistol guard**: `setup_pistol("")` throws `Section [] not found`;
  `apply_player_config` (server and client) now skips empty pistol fields.
* **`--name` arg**: client used key `name` but `args_view` matches the
  dashed form — now `--name`.
* **Keyboard unbind race**: the level lua script creates `controller0`
  *after* the deferred `sig_level_changed`, so deleting it from the signal
  handler failed. Instead, in client mode `game_update` drops
  `controller0` on the first frame it exists (idempotent, re-applies on
  every level reload). Added `game_state::has_controller()`.

### Synced gameplay fixes (phase 2)

* **Server-side bullets for network players**: server players were never
  in `controlled_players`, so `player::game_update` ran with
  `spawn_bullet=false` and client shots produced no server bullets. Fixed
  with a `player::set_net_controlled()` flag: the server marks net players
  as net-controlled and `game_state::game_update` gives them the full
  simulation (local bullet spawning + hit feedback) without attaching a
  controller, so the host keyboard does not drive them.
* **Client position desync**: `net_client::handle_state` used to skip the
  local player, so the locally simulated (predicted) player drifted from
  the server. Superseded by prediction with rollback (see Netcode).
* **Bullet broadcasting**: the server now collects bullets spawned during
  the frame (`game_state::cur_frame_bullets`) and appends them to the
  periodic `state` message (`bullet_info`: owner, pos, vel, mass, tracer
  color, gravity flag — so the client re-spawns each bullet with exactly
  the gravity the server gave it). **All** server-side bullets are broadcast — network players,
  the host's local player and bots. The client spawns them in its own
  `bullet_mgr` and disables local bullet spawning
  (`player::set_server_driven(true)`) for every server-known player, so
  no side ever double-spawns a bullet.
* **Client bullet removal**: the server assigns every spawned bullet a
  unique id (stored on the bullet itself) and appends to the `state`
  broadcast both the ids of the bullets spawned this frame (parallel to
  the bullet list) and the set of all bullet ids currently alive. The
  client tags the bullets it spawns from the broadcast with their ids and,
  on every state, drops any of its tracked bullets that the server no
  longer lists as alive. This removes the artifact where a bullet that hit
  a player on the server kept flying on a client whose local simulation
  diverged slightly and didn't register the hit.
* **Remote pistol animation**: `state_player` carries a `shooting` flag
  (trigger held). The client calls `shot()`/`relax()` on remote players so
  muzzle flash, recoil and sounds play locally; the actual bullets still
  come from the server broadcast.
* **Body facing desync**: the body/face/hat sprites were flipped only in
  `player::draw()` when `_dir` changed, which happens only from local
  input — remote players got `_on_left` synced but `_dir` stayed `dir_none`,
  so the body kept its spawn-facing while the hands (which read `_on_left`
  directly) flipped correctly. `player::on_left(bool)` now flips the
  sprite scales when the value changes.

### Known limitations (this phase)

* Bots are also state-synced (position, bullets, shooting) as long as
  they exist on both sides with the same name and the levels match;
  local bot AI runs between server-state snaps but is overwritten every
  frame. Levels/platforms must match by name.
* Players are in group 0 by default and bullets skip same-group players;
  assign groups (console `group`) for players to hit each other.
* One player per client address; rename on rejoin deletes the old player.
* Remote pistol ammo/reload animation is driven locally on the client and
  can drift from the server's (same weapon, slightly different timing);
  server bullets remain authoritative.
