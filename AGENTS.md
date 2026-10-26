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
- Linking (see BUILDING.md): **static** project-pinned libs only (SFML 2.5.1
  `libsfml-*-s.a`, nuklear, LuaJIT) + **dynamic** system libs (GL/GLX, GLEW,
  OpenAL, X11, codecs, freetype, udev, glibc). Do **not** try to statically
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
./easybuild-debug/cfg_test    # also: tasks_test, net_test
```

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
- `src/physic/physic_simulation.hpp` — fixed 60 Hz sim; extensible via
  `add_update_callback` / `add_platform_callback` / `add_bounce_callback`.
  Default gravity is **zero**.
- `src/player.hpp`, `src/weapon.hpp`, `src/bullet.hpp` — player, pistol,
  bullets. Bullet gravity is spawned per-bullet; the global
  `diefast::bullet_gravity_enabled` (src/bullet.hpp) is what
  `game_state::gravity_for_bullets` references (toggle from Lua:
  `GS.gravity_for_bullets`).
- `src/net/` — multiplayer: authoritative UDP server. Protocol in
  `net_protocol.hpp` (bump `protocol_version` on wire changes); the server
  sends a per-client state every tick; clients send per-tick input
  windows, predict the local player with rollback and interpolate remote
  players (`net_sync.hpp`); server-side lag compensation in
  `game_state::spawn_bullet`. Details in `docs/multiplayer.md`. Server
  players of clients are `player::net_controlled()`, client mirrors of
  server players are `player::server_driven()`. Simulated latency:
  `udp_channel::set_simulated_latency` / `--define=DFDH_DEBUG_PING`.
- `src/script/` — LuaJIT via luacpp; user types in `src/script/typeinit/`.
  Scripts in `data/scripts/*.lua`; `assist.lua`/`assist_ai.lua` are
  **generated at runtime** — never edit or commit them.
- `src/base/` — utilities: cfg, serialization, signals, tasks, args_view,
  fixed_string, log, types.
- `data/` — runtime assets; `data/settings/` is written by the game at
  runtime. `fs.cfg` sits at the **repo root** (global config).

## API pitfalls (costly to rediscover)

- **SFML is 2.5.1, not 2.6**: no `open()`/`isOpen()`, no `UdpSocket::connect()`,
  no `setReceiveTimeout()`, no `sf::IpAddress::isValid()`. Use `bind()`,
  `setBlocking(false)` + `SocketSelector` for timed receive.
  `UdpSocket::receive` takes `std::size_t&` for the received size.
- `ss::serializer` must be constructed with **rvalue** storage to own its
  buffer: `auto ser = ss::serializer(std::vector<u8>{});`
- `args_view`: `get(name)`, `next(err)`, `try_next()`, `by_key_opt<T>(name)` —
  there is **no** `has()`. Key strings include the leading dash.
- `player_name_t = fixed_str<23>`: 22 chars max + nul, 24-byte aligned buffer.
- `cfg::mutable_global()` loads `fs.cfg`; section hot-reload goes through the
  cfg watcher (`replace_changed_sections` in `game_state::game_update`).

## build-plz pitfalls

- `build_executable` caches do **not** track linked libraries — `rm` the
  binary to force a relink after swapping archives.
- `3rd/lib` and `3rd/lib64` are always on `-L`: a stale `.a` with the same
  name as a system `.so` will **silently shadow** it (this bit us with an
  old `libopenal.a`).
- CMake submodule done-markers don't track install-prefix contents: if you
  delete `3rd/` or a submodule build dir, also delete its `.easybuild_done`.
- The custom GCC 16.2 auto-attaches the LTO plugin; static links use
  `-fno-lto`. Cyclic SFML module archives need `-Wl,--start-group/--end-group`.
