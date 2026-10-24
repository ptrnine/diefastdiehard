# Build & linking

## Modes

`./build-plz --debug` (default) and `./build-plz --debug --dynamic` build the
same sources with two different linking strategies. `--asan` forces dynamic
linking (ASan is incompatible with our static flags).

### Default: hybrid (static project libs, dynamic system libs)

Only the libraries the project pins are statically linked into `dfdh`:

* **SFML 2.5.1** (`3rd/lib/libsfml-*-s.a`) — the submodule version, which is
  not what most distros ship.
* **Nuklear** (`libnuklear.a`) — vendored single-file UI.
* **LuaJIT 2.1** (`submodules/luacpp/3rd/lib/libluajit-5.1.a`) — pinned by the
  luacpp submodule.

Everything else links against the system shared libraries: GL/GLX/GLdispatch
(GLVND), GLEW, OpenAL, X11 (+Xext/Xcursor/Xrandr/Xfixes/Xrender/Xau/Xdmcp),
freetype, FLAC/ogg/vorbis, udev, and glibc itself. These ABIs are stable and
the packages exist in every Linux distro, so there is no reason to bundle
them.

Rationale for keeping glibc dynamic: GLVND dlopens the GL vendor ICD at
runtime (`libGLX_indirect.so.0` → `libGLX_mesa.so.0` → `libgallium` →
`libLLVM`), and that chain uses thread-local storage. A fully static glibc
binary cannot provide dynamic TLS to dlopened libraries — the
`__tls_get_addr` JUMP_SLOT in a dlopened module resolves to a bogus address
and the process crashes. glibc 2.41 documents this:

> Support for statically linked applications which call dlopen is
> deprecated and will be removed in a future version of glibc.
> Applications which call dlopen need to be linked dynamically instead.

(Static GLVND + dynamic glibc was tried and worked for GLX queries, but
failed at GLEW's `glXGetProcAddress` dispatch; the system dynamic GLVND has
no such issues and is the supported path.)

### `prod` — release build + dist assembly

`./build-plz prod` is `build-plz dfdh` (release mode: `-O2 -march=native`,
`easybuild/` builddir, static project libs) plus an assembly step: it copies
the `dfdh` binary, the whole `data/` tree and the root-level `fs.cfg` into
`easybuild/dist/`. That directory is self-contained — the game resolves all
assets relative to the working directory (`<cwd>/data/...`, `<cwd>/fs.cfg`)
and the default linking has no non-system shared library dependencies, so:

```
cd easybuild/dist && ./dfdh            # singleplayer
./dfdh --server --port 10450          # multiplayer server
./dfdh --client <ip:port> --name X    # multiplayer client
```

`prod` can be combined with `--debug` (then dist is assembled from
`easybuild-debug/dist`, but note the debug build is still the default static
linking); with `--dynamic` the dist binary would additionally need the
SFML/LuaJIT shared libs on `LD_LIBRARY_PATH`.

### Dynamic mode

The previous behavior: SFML built as shared libs (`libsfml-*.so` in
`3rd/lib`), everything else dynamic. `dfdh` then needs
`LD_LIBRARY_PATH` covering `3rd/lib`, `3rd/lib64` and
`submodules/luacpp/3rd/lib` (+`lib64`) to find the SFML/LuaJIT shared libs.

Test binaries (`cfg_test`, `tasks_test`, `net_test`) are always dynamically
linked and need the same `LD_LIBRARY_PATH`; they must be run under a display
(or `xvfb-run -a`).

## Build order

1. SFML, Catch2, luacpp submodules (CMake, via `build_cmake_submodule`;
   SFML with `BUILD_SHARED_LIBS=OFF` in static mode, `ON` in dynamic mode).
2. Nuklear header copied to `3rd/include`.
3. `dfdh` and the tests are compiled (GCH cache in `easybuild-debug/gch`)
   and linked.

Notes:

* The linker is invoked with `-fno-lto` in static mode: this toolchain's GCC
  auto-attaches its LTO plugin at link time, which breaks extraction of
  plain (non-LTO) objects from static archives (LuaJIT).
* The SFML module archives have cyclic internal dependencies, hence
  `-Wl,--start-group … -Wl,--end-group` around them.
* `build_executable` always adds `-L 3rd/lib`, `-L 3rd/lib64`, `-L <builddir>`
  and the luacpp lib dirs; if a stale static archive with the same name as a
  needed shared library sits there (e.g. an old `libopenal.a`), `-lopenal`
  silently picks the archive. Remove stale archives from those dirs when
  switching strategies.
* Cache invalidation: the executable build cache does not track linked
  library changes; delete the binary (or use `--rebuild-submodules`/full
  clean) after swapping archives.

## Verifying

* `ldd easybuild-debug/dfdh` — static mode should list only system `.so`
  files (no SFML/LuaJIT); dynamic mode should also list `libsfml-*.so`.
* `nm easybuild-debug/dfdh | grep -c ' T sf::'` — hundreds in static mode,
  zero in dynamic mode.
* Smoke test: `xvfb-run -a ./easybuild-debug/dfdh` (singleplayer) and
  `--server --port 10450` / `--client 127.0.0.1:10450 --name smoketest`
  (see multiplayer.md).
