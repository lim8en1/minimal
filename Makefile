# Classic flow: ./configure && make build && sudo make install
#
# ./configure does the real work (vcpkg bootstrap, preset selection, install
# prefix) and writes build/config.mk recording the chosen preset and prefix.
# These targets just replay that state; they auto-configure first if the build
# tree does not exist yet. To change options, re-run ./configure, e.g.
# `./configure --debug`. The per-preset targets below (debug, asan, ...) are
# shorthands for exactly that.
#
# `install` installs the configured preset's build tree into the prefix chosen
# at configure time (override per-invocation with `make install PREFIX=...`).
# DESTDIR is honored by `cmake --install` for staged packaging. Note: a
# non-empty DESTDIR also makes cmake/SeedConfig.cmake SKIP config seeding
# (there is no user whose config dir a staged package should write into) --
# seed by running the server once on the target machine, or by hand.
#
# `clean` keeps .vcpkg/ (expensive to re-clone; deps are binary-cached anyway).
# `distclean` removes it too.

-include build/config.mk
PRESET ?= default
BUILD_DIR ?= build
PREFIX ?= $(CONFIG_PREFIX)
ifeq ($(strip $(PREFIX)),)
PREFIX := /usr/local
endif

.PHONY: configure build install test debug server-only asan tsan \
        debug-test asan-test tsan-test clean distclean vcpkg-clean help

# Run ./configure if the build tree for the recorded preset does not exist yet.
# Tolerates config.mk being stale (preset reconfigured elsewhere) by trusting
# config.mk over the cache -- re-running ./configure fixes any disagreement.
ensure-configured = \
	@if [ ! -f $(BUILD_DIR)/CMakeCache.txt ]; then \
		echo "make: $(BUILD_DIR)/ is not configured; running ./configure first"; \
		./configure || exit 1; \
	fi

configure:
	./configure

build:
	$(ensure-configured)
	cmake --build --preset $(PRESET)

# sudo installs would seed root's home (SeedConfig.cmake resolves $HOME);
# redirect seeding to the invoking user's config dir instead. An explicit
# MINIMAL_CONFIG_DIR in the environment always wins.
install: 
	$(ensure-configured)
	@if [ -n "$${SUDO_USER:-}" ] && [ -z "$${MINIMAL_CONFIG_DIR:-}" ]; then \
		h=$$(eval echo ~$$SUDO_USER 2>/dev/null || true); \
		if [ ! -d "$$h" ] && command -v getent >/dev/null 2>&1; then \
			h=$$(getent passwd $$SUDO_USER 2>/dev/null | cut -d: -f6); \
		fi; \
		if [ -n "$$h" ] && [ -d "$$h" ]; then \
			echo "install: seeding $$SUDO_USER's config ($$h/.config/minimal), not root's"; \
			MINIMAL_CONFIG_DIR="$$h/.config/minimal" cmake --install $(BUILD_DIR) --prefix $(PREFIX); \
		else \
			echo "install: could not resolve $$SUDO_USER's home; seeding as the running user"; \
			cmake --install $(BUILD_DIR) --prefix $(PREFIX); \
		fi; \
	else \
		cmake --install $(BUILD_DIR) --prefix $(PREFIX); \
	fi

test:
	$(ensure-configured)
	ctest --preset $(PRESET)

debug:
	./configure --debug
	cmake --build --preset debug

server-only:
	./configure --server-only
	cmake --build --preset server-only

asan:
	./configure --asan
	cmake --build --preset asan

tsan:
	./configure --tsan
	cmake --build --preset tsan

debug-test:
	./configure --debug
	ctest --preset debug

asan-test:
	./configure --asan
	ctest --preset asan

tsan-test:
	./configure --tsan
	ctest --preset tsan

clean:
	rm -rf build build-debug build-server build-asan build-tsan

distclean: clean
	rm -rf .vcpkg

# Reclaim vcpkg's transient per-port dirs (buildtrees/packages). Newer builds
# clean these automatically (VCPKG_INSTALL_OPTIONS in CMakeLists.txt); this
# clears leftovers from older runs. Keeps the checkout, downloads/ cache, and
# binary cache.
vcpkg-clean:
	rm -rf .vcpkg/buildtrees .vcpkg/packages

help:
	@echo "minimal build targets:"
	@echo "  ./configure          set up vcpkg + cmake (run this first; see --help)"
	@echo "  make build           build the configured preset"
	@echo "  make test            run the test suite"
	@echo "  make install         install to the configured prefix (PREFIX=... to override)"
	@echo "  make debug|asan|tsan configure + build that preset"
	@echo "  make clean           remove build trees (keeps .vcpkg/)"
	@echo "  make vcpkg-clean     remove vcpkg buildtrees/packages (keeps .vcpkg/ checkout)"
	@echo "  make distclean       also remove .vcpkg/"
