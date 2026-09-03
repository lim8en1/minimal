# Building and installing minimal

minimal is a C++20 CMake project with all third-party dependencies resolved
through **vcpkg manifest mode** (`vcpkg.json`) — a fresh checkout plus a vcpkg
checkout is the whole setup. The only dependency not in the vcpkg registry
(`hkr04/cpp-mcp`) is vendored via `FetchContent`, pinned to a commit SHA, and
fetched automatically.

## Prerequisites

| Tool | Version | Notes |
|---|---|---|
| CMake | ≥ 3.28 | needed for `EXCLUDE_FROM_ALL` on FetchContent |
| Ninja | any recent | all presets use the Ninja generator |
| C++ compiler | C++20 | Clang or GCC; native Windows is not supported (use WSL) |
| vcpkg | current | `$VCPKG_ROOT` must point at a vcpkg checkout |

All C/C++ dependencies come from the vcpkg manifest: `grpc`, `protobuf`,
`curl`, `sqlite3`, `nlohmann-json`, `yaml-cpp`, `spdlog`, `tree-sitter`, `re2`
— plus `ftxui` and `md4c` for the client and `catch2` for tests (vcpkg
features, on by default). The per-language tree-sitter grammars (bash, C++,
Python, JS/TS, JSON, YAML, Rust, Go) are vendored via FetchContent as well.

Export `VCPKG_ROOT` before configuring:

```sh
export VCPKG_ROOT=/path/to/vcpkg
```

## Build

```sh
cmake --preset default          # configure (vcpkg toolchain, RelWithDebInfo)
cmake --build --preset default  # build
```

or, equivalently, `make build` (see the root `Makefile`, which only wraps the
presets). First configure takes a while — vcpkg builds the whole dependency
tree. Subsequent builds are incremental.

### Presets

| Preset | Binary dir | Description |
|---|---|---|
| `default` | `build/` | vcpkg toolchain, RelWithDebInfo, client + tests |
| `debug` | `build-debug/` | Debug build |
| `server-only` | `build-server/` | `MINIMAL_BUILD_CLIENT=OFF` — no TUI client |
| `asan` | `build-asan/` | AddressSanitizer, first-party code *and* dependencies instrumented (`arm64-osx-asan` overlay triplet in `cmake/triplets/`) |
| `tsan` | `build-tsan/` | ThreadSanitizer, same approach (`arm64-osx-tsan`) |

Every preset inherits `default`, so all of them require `$VCPKG_ROOT`.

### CMake options

| Option | Default | Description |
|---|---|---|
| `MINIMAL_BUILD_CLIENT` | `ON` | Build the FTXUI TUI client (`minimal`) |
| `MINIMAL_BUILD_TESTS` | `ON` | Build the Catch2 test suite |
| `MINIMAL_PLUGIN_INSTALL_DIR` | `<libdir>/minimal/plugins` | Install location for bundled server plugins |
| `MINIMAL_CLIENT_PLUGIN_INSTALL_DIR` | `<libdir>/minimal/plugins/client` | Install location for bundled client plugins |

## Tests

```sh
ctest --preset default   # or: make test
```

Use `ctest --preset asan` / `ctest --preset tsan` with the sanitizer builds.
The asan test preset sets `ASAN_OPTIONS=detect_leaks=0`. Sanitizer presets
instrument dependencies too, so the first configure is slow; that is the point
— leaks and races inside a dependency are still bugs that bite the harness.

## Install

```sh
make install                       # installs the default (build/) tree
# or directly:
cmake --install build --prefix /usr/local
```

`DESTDIR` is honored for staged packaging. **A non-empty `DESTDIR` skips config
seeding** (below): a staging root has no user whose home it should write into.
Seed on the target machine by running the server once, or by hand.

### What gets installed

```
<prefix>/bin/minimal                              TUI + headless client (one binary)
<prefix>/bin/minimal-server                       the server
<prefix>/lib/libminimal_server_core.0.dylib|.so.0   shared core, plugins link it
<prefix>/lib/libminimal_client_core.0.dylib|.so.0
<prefix>/lib/minimal/plugins/                     bundled server plugins (*.so|*.dylib)
<prefix>/lib/minimal/plugins/client/              bundled client plugins
```

Binaries, cores, and plugins carry RPATHs relative to their own location
(`@loader_path` / `$ORIGIN`), so the installed tree is relocatable — no
`ldconfig`, no install-name fixing.

## Config seeding

At install time, `cmake/SeedConfig.cmake` copies `bundled/` — default
`settings.json`, `permissions.json`, `providers.json` + example, `hooks.json` +
example, `keybindings.json`, `ui.json`, `themes/`, `agents/`, `prompts/`,
`cost-rates.json` — into the user's config dir, resolved in the same order the
server uses at runtime:

1. `$MINIMAL_CONFIG_DIR`
2. `$XDG_CONFIG_HOME/minimal`
3. `$HOME/.config/minimal`

**Existing files are never overwritten** — once seeded, a file is the user's.
A later release's improved defaults do not propagate to anyone who already has
the file. Runtime state (SQLite session store, logs, plugin data) lives in the
data dir: `$XDG_DATA_HOME/minimal` or `$HOME/.local/share/minimal`.

Seeding tells you when `providers.json` is still empty — that is the one thing
it cannot do for you; see `providers.example.json` and
`docs/CONFIGURATION.md` for the format.

## Rebuilding plugins

Plugins are **source-compatible, not binary-compatible**: they are rebuilt
together with the harness, and both host and plugin bake the same ABI tag
(`<compiler-id>-<compiler-version>-cxx20-<system>`). A mismatched tag refuses
to load — after pulling a new revision, rebuild rather than reusing plugin
artifacts from an old build tree. Bundled plugins fail the server's startup
sanity check if they do not load; user plugins only warn.
