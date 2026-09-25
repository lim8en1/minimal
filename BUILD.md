# Building and installing minimal

minimal is a C++20 CMake project with all third-party dependencies resolved
through **vcpkg manifest mode** (`vcpkg.json`) — a fresh checkout plus a vcpkg
checkout is the whole setup. The tree-sitter grammars, which vcpkg does not
package, are vendored via `FetchContent`, pinned, and fetched automatically.

## Quick start

```sh
./configure && make build && sudo make install
```

`./configure` checks prerequisites, resolves vcpkg (see below), configures the
default preset, and records the choice in `build/config.mk`; the `make` targets
replay that state and auto-configure first if the build tree is missing.
Options: `--prefix=DIR`, `--debug`, `--server-only`, `--asan`, `--tsan`,
`--vcpkg-root=DIR`. Re-running `./configure` with different flags reconfigures;
the per-preset `make debug` / `make asan` / ... targets are shorthands for the
same thing.

`sudo make install` seeds the invoking user's config dir (not root's). `make
clean` removes build trees but keeps `.vcpkg/`; `make distclean` removes that
too. The rest of this document describes what runs underneath.

## Prerequisites

| Tool | Version | Notes |
|---|---|---|
| CMake | ≥ 3.28 | needed for `EXCLUDE_FROM_ALL` on FetchContent |
| Ninja | any recent | all presets use the Ninja generator |
| C++ compiler | C++20 | Clang or GCC; macOS and Linux. Native Windows is not supported (use WSL) |
| vcpkg | current | `$VCPKG_ROOT` must point at a vcpkg checkout |

On Linux, vcpkg itself also needs `curl`, `zip`, `unzip`, `tar`, and
`pkg-config` (on Debian/Ubuntu, with `build-essential`, `git`, `ninja-build`, and
`cmake`).

All C/C++ dependencies come from the vcpkg manifest: `grpc`, `protobuf`,
`curl`, `sqlite3`, `nlohmann-json`, `yaml-cpp`, `spdlog`, `tree-sitter`, `re2`
— plus `ftxui` and `md4c` for the client and `catch2` for tests (vcpkg
features, on by default). The per-language tree-sitter grammars (bash, C++,
Python, JS/TS, JSON, YAML, Rust, Go) are vendored via FetchContent as well.

`VCPKG_ROOT` is optional: if it is not set, `./configure` bootstraps a vcpkg
checkout into `./.vcpkg` (pinned to the `builtin-baseline` commit in
`vcpkg.json`) on first run. Exporting it is only needed to use an existing
checkout:

```sh
export VCPKG_ROOT=/path/to/vcpkg   # optional; ./configure bootstraps one otherwise
```

If `vcpkg install` fails with "failed to unpack tree object" / "cloned as a
shallow repository", the vcpkg checkout in use is missing history that a
pinned port version needs. If `$VCPKG_ROOT` is set, check that it points at
a full (non-shallow) clone synced to this repo's `builtin-baseline`; for the
auto-bootstrapped `./.vcpkg`, `./configure` now always ensures full history
(`git -C .vcpkg rev-parse --is-shallow-repository` should print `false`) --
delete `.vcpkg/` and re-run `./configure` if it's stuck shallow from an
older `./configure`.

### Dependency linkage

Dependencies are built as **shared libraries**. When `VCPKG_TARGET_TRIPLET` is
not set, the top-level `CMakeLists.txt` picks `<arch>-<os>-dynamic`
(`arm64-osx-dynamic`, `x64-linux-dynamic`, ...) rather than vcpkg's static
default. This is a correctness requirement, not a preference. A static
protobuf/gRPC/abseil is copied into every shared object that links it: both
cores, every plugin, the tests. On Linux those copies share one descriptor
pool, and the server aborts at startup with `File already exists in database`.
macOS survives only because its two-level namespace keeps the copies apart.

And they install **outside `<prefix>/lib` itself** -- into
`<libdir>/minimal/runtime` (`MINIMAL_RUNTIME_DEP_INSTALL_DIR`) -- for the same
class of reason. `ldconfig` indexes `<prefix>/lib` (Debian/Ubuntu's `libc.conf`
lists it) but not its subdirectories, and our gathered libraries keep distro
sonames: vcpkg's curl installs as `libcurl.so.4.8.0`, the exact name every
distro binary requests. In `lib/` itself it would win the ld.so cache and be
loaded machine-wide: every distro curl consumer would then print
`/usr/local/lib/libcurl.so.4: no version information available`, and -- the
dangerous part, because the warning is only cosmetic -- silently run against
vcpkg's curl and OpenSSL instead of the OS's. From `minimal/runtime/` no
loader's default search can ever pick them up; the binaries, cores and plugins
reach them through an extra RPATH hop (`minimal/runtime`), and the curl overlay
(above) gives our copy the `CURL_OPENSSL_4` version node so a mistaken foreign
bind fails loudly instead of substituting. Our own cores stay in `lib/`
proper -- no distro ships `libminimal_*`.

- Configure **fails** if protobuf or gRPC resolves to a static library,
  including when you pass `-DVCPKG_TARGET_TRIPLET=<static triplet>`.
- A build tree that cached the old static default is migrated automatically on
  its next configure. vcpkg then rebuilds the dependency tree once under the
  new triplet.
- The sanitizer overlay triplets (`cmake/triplets/`) are dynamic too.
- On macOS, `cmake/triplets/{arm64,x64}-osx-dynamic.cmake` shadow vcpkg's
  community triplets of the same name. They use the same settings, plus
  per-port workarounds for SDKs newer than the deployment target (currently
  c-ares's `pipe2` probe).
- `vcpkg.json` requests `protobuf[libprotoc]`. On a dynamic triplet, grpc always
  builds a shared `libgrpc_plugin_support` that needs libprotoc, and macOS's
  linker refuses to leave those symbols unresolved. Nothing here links
  libprotoc, so it is not installed.
- abseil comes from an overlay port, `cmake/ports/abseil/`: the baseline port
  with its static-only restriction (outside Windows) removed, so
  it builds shared like Homebrew's and Debian's. A static abseil gets embedded
  separately in libprotobuf, libgrpc, libgpr, ..., and on macOS a gRPC server
  shutdown then waits forever on a CondVar belonging to one copy while another
  copy signals it. When bumping `builtin-baseline`, refresh the overlay from the
  new baseline's `ports/abseil/` and keep the one-line portfile change.
- curl comes from an overlay port too, `cmake/ports/curl/`: the baseline port
  plus, on Linux only, `-DCURL_LIBCURL_VERSIONED_SYMBOLS=ON`. Upstream's CMake
  build defaults to unversioned symbols, so vcpkg's `libcurl.so.4` would carry
  no `CURL_OPENSSL_4` node -- distro libcurls have one, and every binary they
  link records it. See "Dependency linkage" below. When bumping
  `builtin-baseline`, refresh the overlay from the new baseline's `ports/curl/`
  and keep the guarded `list(APPEND OPTIONS ...)` splice.
- ftxui comes from an overlay port too, `cmake/ports/ftxui/`: the stock 7.0.3
  port plus three patches (applied in this order):
  - `minimal-ftxui-frame-diff.patch` — one requirement pass per frame
    instead of two, identical-frame write skip, dirty-region write;
  - `minimal-ftxui-event-loop.patch` — the event loop blocks in `poll()`
    on terminal input / posted tasks / due delayed tasks instead of
    waking at ~60 Hz, and the unconditional 15 ms animation self-repost
    is gated (removed: a constant ~1.5% CPU burn whenever a session was
    open, idle or not);
  - `minimal-ftxui-dirty-rows.patch` — per-row cell hashing; only rows
    whose content changed are serialized (`Screen::ToStringRow`) and
    written. Net effect measured: streaming CPU ~3% → ~0.2%, silent tool
    run ~1.9% → ~0.3%, idle-with-session ~0.4% → ~0.2%, terminal write
    bandwidth −99%.
  All three only change WHAT is written and how often the loop wakes,
  never WHAT is composed. `MINIMAL_FTXUI_DIFF=0` in the client's
  environment restores upstream write-everything behavior. When bumping
  `builtin-baseline`, refresh the overlay from the new baseline's
  `ports/ftxui/` and rebase the patches (they apply in the order above).
- Disk usage: vcpkg cleans each port's transient `buildtrees/` and
  `packages/` entries right after it installs (`VCPKG_INSTALL_OPTIONS`,
  set in `CMakeLists.txt`), so `.vcpkg/` stays small instead of accumulating
  ~25 GiB. What persists is the binary cache (`~/.cache/vcpkg` — compressed
  per-port packages that make rebuilds free) and the installed tree
  (`build/vcpkg_installed/`). A **failed** port keeps its buildtree for log
  inspection — cleanup only happens on success. The flags are appended to
  `VCPKG_INSTALL_OPTIONS` only when absent, so you can manage them yourself by
  passing `-DVCPKG_INSTALL_OPTIONS=...` explicitly; `make vcpkg-clean` reclaims
  leftovers from older runs (pre-flag checkouts can hold ~20 GiB of them).

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

Every preset inherits `default`, so all of them need a vcpkg checkout — via
`$VCPKG_ROOT` or the one `./configure` bootstraps into `./.vcpkg`.

### CMake options

| Option | Default | Description |
|---|---|---|
| `MINIMAL_BUILD_CLIENT` | `ON` | Build the FTXUI TUI client (`minimal`) |
| `MINIMAL_BUILD_TESTS` | `ON` | Build the Catch2 test suite |
| `MINIMAL_PLUGIN_INSTALL_DIR` | `<libdir>/minimal/plugins/server` | Install location for bundled server plugins |
| `MINIMAL_CLIENT_PLUGIN_INSTALL_DIR` | `<libdir>/minimal/plugins/client` | Install location for bundled client plugins |
| `MINIMAL_RUNTIME_DEP_INSTALL_DIR` | `<libdir>/minimal/runtime` | Install location for the gathered vcpkg runtime libraries (never `<libdir>` itself -- see "Dependency linkage") |

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
<prefix>/lib/libminimal_proto.0.dylib|.so.0         generated protobuf/gRPC bindings
<prefix>/lib/minimal/runtime/                       vcpkg dependencies (shared, see above)
<prefix>/lib/minimal/plugins/server/                bundled server plugins (*.so|*.dylib)
<prefix>/lib/minimal/plugins/client/              bundled client plugins
```

Binaries, cores, plugins, and the vcpkg libraries carry RPATHs relative to
their own location (`@loader_path` / `$ORIGIN`), so the installed tree is
relocatable — no `ldconfig`, no install-name fixing. The dependency libraries
are gathered by the `minimal_runtime_deps` runtime-dependency set. Only files
from the vcpkg installed tree are copied; system libraries are never bundled.

One-time cleanup on a machine that ran an install of a tree older than the
`minimal/runtime` split: `cmake --install` never uninstalls, so the libraries
the old layout dropped into `<prefix>/lib` are still there and still shadow the
system ones once `ldconfig` runs. Remove them by hand, then refresh the cache:

```sh
sudo rm /usr/local/lib/libcurl.so.4*   # and any other vcpkg-era lib* leftovers
sudo ldconfig                          # Linux only
```

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

## User plugins

Plugins developed outside this repo (your own, or cloned third-party ones)
are registered under `user-plugins/{server,client}/<name>` — a gitignored
tree of plugin directories, symlinks into your own plugin repos, or clones.
The harness build compiles them with the same toolchain and ABI tag and
deploys every rebuild into the config dir (`<config>/plugins/`,
`<config>/client-plugins/`); they start disabled and are enabled in
`plugins.json` / `client-plugins.json`.

```sh
scripts/new-plugin.sh my-plugin        # scaffold (add --client for client side)
scripts/new-plugin.sh --link ../my-plugins/server/my-plugin   # register an existing dir
```

See `docs/PLUGINS.md`, section "User plugins", for the full story, including
the `MINIMAL_USER_PLUGIN_DEPLOY_DIR` override for CI.
