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
  |  INPUT(bits)  every tick ------>  applies input to pl "A")  |
  |  STATE(all players, 60Hz) <-----  broadcasts snapshots  ---> |
```

* Synchronous, non-blocking UDP in the main loop (`sf::UdpSocket::recvFrom`
  with a small timeout each frame) — no extra threads, no locks.
* UDP datagrams carry a 4-byte header `u16 type, u16 payload_size` followed
  by a payload serialized with the existing `ss::serializer` (little-endian).

### Protocol messages

| id  | dir        | payload |
|-----|------------|---------|
| 1   | C → S      | HELLO: player name, pistol section, face_id, body_id, body color, tracer color |
| 2   | S → C      | WELCOME: player name |
| 3   | C → S      | INPUT: `u16 seq`, `u16 bits` (see below) |
| 4   | S → C      | STATE: `u16 tick`, per player: name, pistol, face_id, body_id, body color, tracer color, pos.xy, vel.xy, jumps_left, on_left, shooting; then the bullets spawned this frame by any server player (owner, pos.xy, vel.xy, hit_mass, tracer color, gravity) |
| 5   | C → S      | LEAVE: player name |
| 6   | S → C      | ERROR: message string (join failed, etc.) |

Input bits:

| bit | meaning |
|-----|---------|
| 0 | move LEFT (held) |
| 1 | move RIGHT (held) |
| 2 | JUMP (pressed edge) |
| 3 | JUMP-DOWN (pressed edge) |
| 4 | SHOT (held) |
| 5 | SHOT released (edge) |
| 6 | LONG-SHOT toggle (edge) |

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
  the server. The server state now overwrites position/velocity/facing for
  *all* players including the local one. Local simulation is prediction
  only.
* **Bullet broadcasting**: the server now collects bullets spawned during
  the frame (`game_state::cur_frame_bullets`) and appends them to the
  periodic `state` message (`bullet_info`: owner, pos, vel, mass, tracer
  color, gravity flag — so the client re-spawns each bullet with exactly
  the gravity the server gave it). **All** server-side bullets are broadcast — network players,
  the host's local player and bots. The client spawns them in its own
  `bullet_mgr` and disables local bullet spawning
  (`player::set_spawn_bullets(false)`) for every server-known player, so
  no side ever double-spawns a bullet.
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
* No interpolation — client snaps to the latest server state.
* One player per client address; rename on rejoin deletes the old player.
* Remote pistol ammo/reload animation is driven locally on the client and
  can drift from the server's (same weapon, slightly different timing);
  server bullets remain authoritative.
