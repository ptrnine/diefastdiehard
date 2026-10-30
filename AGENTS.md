# AGENTS.md

diefastdiehard: 2D arena platformer.

## Workflow

When the user asks you to implement something, **commit the result**
(feature, fix, or refactor) once it builds and the tests pass. Follow the
existing commit style: `<component>: <short description>` subject, bullet
body. Do not commit runtime-generated files (`data/scripts/assist*.lua`).
 C++20, header-centric (almost everything
lives in `.hpp`, only a few `.cpp` translation units). Built with `./build-plz`.

## Build & run

```sh
./build-plz --debug      # debug build -> easybuild-debug/ (dfdh + tests)
./build-plz prod         # release build -> easybuild/ + self-contained easybuild/dist/
./build-plz dfdh         # release build, no dist
```

- `easybuild/` = release builddir, `easybuild-debug/` = debug builddir.
- Linking (see BUILDING.md): **static** project-pinned libs only (SFML 3
  `libsfml-*-s.a` + the deps it bundles: freetype, harfbuzz, codecs,
  mbedTLS, libssh2; nuklear, LuaJIT) + **dynamic** system libs (GL/GLX,
  GLEW, X11, udev, glibc). Do **not** try to statically
  link glibc or GLVND — GLVND dlopens the TLS-using Mesa ICD chain at
  runtime and static glibc cannot dlopen it.
- `--dynamic` = all-dynamic link (needs LD_LIBRARY_PATH at runtime);
  `--asan` implies dynamic.
- **The game resolves ALL assets via `fs::current_path()`**: it must run from
  a directory containing `data/` and `fs.cfg` (repo root or `easybuild/dist`).
- Smoke test: `cd easybuild/dist && xvfb-run -a ./dfdh`

## Tests

Tests are Catch2 v3 (`Catch::Approx` in the `Catch` namespace). Texture
loading needs a GL context, so run under xvfb:

```sh
xvfb-run -a env LD_LIBRARY_PATH=easybuild-debug/3rd/lib:easybuild-debug/3rd/lib64:\
easybuild-debug/submodules/luacpp/3rd/lib:easybuild-debug/submodules/luacpp/3rd/lib64 \
./easybuild-debug/cfg_test    # also: tasks_test, net_test, lua_test
```

Under `--asan`, set `LSAN_OPTIONS=suppressions=$PWD/tests/lsan.supp` (Mesa
GLX driver leaks). CI (`.github/workflows/`) runs all three tests on Linux
(ASan) and on Windows (cross-built by `build-windows.sh`, Mesa llvmpipe as
`opengl32.dll`), and uploads the Linux and Windows dists as artifacts.

Game time (sim ticks, net timing, simulated latency) comes from
`game_clock` (src/base/game_clock.hpp), not `std::chrono::steady_clock`:
tests drive it by hand (`game_clock_manual_guard` + `game_clock::advance`)
instead of sleeping. Use it for any new gameplay/network timer.

## Layout

- `diefastdiehard.cpp` — main: CLI args, subsystem composition (console,
  player configurator, multiplayer server/client).
- `src/engine.hpp` — app shell: window, main loop, subsystems, Lua boot.
- `src/game_state.hpp` — the central game object: players, `sim`
  (physic_simulation), `blt_mgr` (bullets), `kick_mgr`, camera, AI manager,
  config watcher. **Entirely public** — no private sections.
- `src/physic/physic_simulation.hpp` — fixed 120 Hz sim (`physic_simulation::tick_rate`, ticks
  exactly 1/120 s apart; a network client slews the real period by ≤ 0.5 %, `tick_period_scale`,
  the timestep stays). Tick counts that mean a time derive from `tick_rate`; extensible via
  `add_update_callback` / `add_platform_callback` / `add_bounce_callback`.
  Default gravity is **zero**.
- `src/player.hpp`, `src/weapon.hpp`, `src/bullet.hpp` — player, pistol,
  bullets. Weapon configs have `slot = primary|secondary`: the player
  configures (and spawns with) a secondary gun only (others are replaced by
  `wpn_glk17`), secondary guns have unlimited ammo and are never dropped,
  dropping a primary gun gives the secondary one back. Bullet gravity is spawned per-bullet; the global
  `diefast::bullet_gravity_enabled` (src/bullet.hpp) is what
  `game_state::gravity_for_bullets` references (toggle from Lua:
  `GS.gravity_for_bullets`).
- Score: a hit records its shooter/thrower (`bullet_ref::owner`) as the
  victim's `player::attacker` (the last hit wins), forgotten on the ground
  after `player::hit_cooldown` (time to compensate the knockback with air
  control). `game_state::player_on_dead`: +1 to the attacker, or -1 to the
  player (suicide). The server syncs it in `state_player::score`.
  Game mode `score_system = lives`: the score is the lives left (`start_lives`),
  -1 for any death; at 0 the player is `player::eliminated()` (parked at
  `game_state::eliminated_park_pos`, not updated/drawn/respawned, out of the
  AI's world). Clients derive it from the synced score. A switch to other
  score rules resets the scores (`game_state::players_score_reset`).
- `src/game_mode.hpp` — game mode: game rules (bullet gravity, crate drop
  period, magazines count: primary gun ammo limit, emptied guns are dropped) from the `gm_` sections of `data/game_mode.cfg`, not from the
  level. `gm_default` is applied on start; console `game mode [name|list]`
  (`game_state::game_mode_apply`). The server sends it to clients in STATE.
- `src/ui/main_menu.hpp` — main menu ("Host Lobby", "Connect Lobby",
  "Settings", "Exit"; columns are `menu_column`s; Settings opens a "Player",
  "Network", "Back" submenu right of the dimmed main column):
  shown out of a game with no window open (no server/client, no lobby,
  connect window or player configurator), fades in/out; hovered options grow
  via a scaled copy of `ui_ctx::menu_font()` (nuklear scales baked fonts by
  `nk_user_font::height`; the copy must outlive `ui.render()`). Host starts a
  server on `host_port` (`[game_settings]`, default 10443; also `--server`'s
  default `--port`; edited in Settings -> Network, `src/ui/network_ui.hpp`,
  saved on enter / focus lost / close when valid) and opens its
  lobby with the current player; cancelling (or closing) that lobby stops the
  server and removes the player. Settings -> Player: the player configurator
  with a nickname field.
- Current player: `player_configurator::current_player()` (`[players]
  current_player` in `data/settings/players_settings.cfg`, `Player` on the
  first run): hosts and joins lobbies (the connect window has no nickname),
  `--client`'s default `--name`. `player_configurator_ui` saves on every change
  (no save on close); with `name_edit`, a committed nickname (Enter / focus
  lost) loads that player's settings and becomes the current player. Any
  configurator opens with the bounds the last one was closed with
  (`pconf_window_pos`/`pconf_window_size` in `[game_settings]`).
- Esc (`diefastdiehard::close_top_window`): closes the topmost window — player
  configurator, connect window, network window, lobby (= its close button: cancel lobby /
  leave) — else the settings submenu; windows expose `close()` for it.
- `src/lobby.hpp` + `src/ui/lobby_ui.hpp` — multiplayer lobby (opened on
  `--server`/`--client` launch, console `lobby [start]`): the owner picks the
  level/mode/groups and starts (`game_state::lobby_start`); closing the window
  is cancel lobby (owner) / leave (client); nothing is
  simulated while `game_state::lobby.active`. The UI only calls
  `lobby_ui::actions_t` callbacks (wired in `diefastdiehard.cpp`).
  `src/ui/connect_ui.hpp`: "Connect to lobby" window (console `ui connect`):
  address/port, a loader until the server's WELCOME (timeout
  `connect_ui::timeout_s`), the rejection/timeout error; joined, it closes and
  the client lobby shows. Leaving a lobby joined from it returns to it.
  Its fields are loaded on open and saved on any close (`connect_host`,
  `connect_port` in `[game_settings]`).
- `src/net/` — multiplayer: authoritative UDP server. Protocol in
  `net_protocol.hpp` (bump `protocol_version` on wire changes); the server
  sends a per-client state every tick; clients send per-tick input
  windows, predict the local player with rollback and interpolate remote
  players (`net_sync.hpp`); server-side lag compensation in
  `game_state::spawn_bullet`. Clients simulate server bullets in the local
  player's (future) frame and test hits on it exactly like the server
  (`net_client::tick_hit`). Details in `docs/multiplayer.md`. Server
  players of clients are `player::net_controlled()`, client mirrors of
  server players are `player::server_driven()`. Simulated latency:
  `udp_channel::set_simulated_latency` / `--define=DFDH_DEBUG_PING`.
  Latency settings (input jitter buffer, interpolation delay, input
  margin): console `net` (on the screen: `hud net`), see `docs/multiplayer.md`.
- `src/efx/` — effects: JSON-described animations applied to SFML drawables
  (adopted from fever_dream). `efx.hpp`: `efx` (drawables + named handlers +
  duration), `efx_instance`/`efx_mgr` (play/update/draw to any `sf::RenderTarget`),
  `efx_handlers` (position/scale/rotation/gravity). `efx_editor.hpp`:
  `efx_builder` parses effect JSON (nlohmann json, `submodules/nlohmann_json`);
  effect files live in `data/efx/<name>.json` (`efx_builder::effect_path`),
  texture paths in them are relative to `data/textures`. `keyframe_animation.hpp`
  (keyframes: hold/linear/bezier), `sfml_types.hpp` (`drawable_t` variant +
  `downcast`).
- `src/script/` — LuaJIT via luacpp; user types in `src/script/typeinit/`.
  Lua `print()` writes to the game log; `cmd("...")` queues console commands.
  External Lua: `--lua-exec-stdin` (once at startup) and
  `--lua-exec-fifo=<path>` (`lua_fifo.hpp`: a named pipe recreated on
  startup, one chunk per writer close, polled in `game_update`; POSIX only).
  Scripts in `data/scripts/*.lua`; `assist.lua`/`assist_ai.lua` are
  **generated at runtime** — never edit or commit them.
- `src/base/` — utilities: cfg, serialization, signals, tasks, args_view,
  fixed_string, log, types.
- `data/` — runtime assets; `data/settings/` is written by the game at
  runtime (`game_settings.cfg`: window, connect window fields, configurator bounds — `engine::game_settings()`). `fs.cfg` sits at the **repo root** (global config).

## API pitfalls (costly to rediscover)

- **SFML is 3.x**: events are a variant (`pollEvent()` returns
  `std::optional<sf::Event>`, `evt.getIf<sf::Event::KeyPressed>()`), enums
  are scoped (`sf::Keyboard::Key::A`), setters take vectors
  (`setScale({x, y})`), rects are `{position, size}` (`findIntersection`),
  rotations are `sf::Angle` (`sf::degrees`). `sf::IpAddress` has no default
  constructor (`sf::IpAddress::Any`), `UdpSocket::receive` takes a
  `std::optional<sf::IpAddress>&`, there is no `UdpSocket::connect()`.
- `sf::Sprite` needs a texture: default constructible sprites are
  `dfdh::sprite_t` (src/sprite.hpp), whose first `setTexture()` resets the
  texture rect (SFML 2 behavior; plain SFML 3 keeps the empty rect).
- `ss::serializer` must be constructed with **rvalue** storage to own its
  buffer: `auto ser = ss::serializer(std::vector<u8>{});`
- `args_view`: `get(name)`, `next(err)`, `try_next()`, `by_key_opt<T>(name)` —
  there is **no** `has()`. Key strings include the leading dash.
- `player_name_t = fixed_str<23>`: 22 chars max + nul, 24-byte aligned buffer.
- cfg comments start with `#`: a `//` line is parsed as a `key = value`
  (silently, if it has a `=`).
- `cfg::mutable_global()` loads `fs.cfg`; section hot-reload goes through the
  cfg watcher (`replace_changed_sections` in `game_state::game_update`).

## build-plz pitfalls

- `build_executable` caches do **not** track linked libraries — `rm` the
  binary to force a relink after swapping archives.
- `3rd/lib` and `3rd/lib64` are always on `-L`: a stale `.a` with the same
  name as a system `.so` will **silently shadow** it (this bit us with an
  old `libopenal.a`).
- CMake submodule done-markers track the submodule commit and flags, not
  install-prefix contents: if you delete `3rd/` or a submodule build dir,
  also delete its `.easybuild_done`. After a submodule update, remove the
  previous version's installed headers/libs from `3rd/` (stale files stay).
- The custom GCC 16.2 auto-attaches the LTO plugin; static links use
  `-fno-lto`. Cyclic SFML module archives need `-Wl,--start-group/--end-group`.
