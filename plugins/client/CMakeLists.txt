# Bundled CLIENT plugin implementations.
#
# The host machinery lives in src/client/plugins (track E5); this subtree is for
# implementations, categorically identical to a third-party client plugin and
# differing only in who publishes it. The external todo plugin is the intended
# first consumer; until it lands, E5's proof is the test fixtures
# (tests/fixtures/client-*), which use minimal_add_client_fixture_plugin.
#
# logo is the first bundled client plugin: the ASCII-banner start view. It is
# deliberately the thinnest possible real plugin -- one StartView registration
# over a pure generator -- so its value is as a reference shape as much as a
# feature.

add_subdirectory(logo)

# cost-view is the second: a `{cost}` status-bar segment consuming the
# bundled server-side cost-tracker plugin's PluginCall RPC, the intended
# first real (non-reference-shape) consumer of that surface.

add_subdirectory(cost-view)

# checklist-view is the third: a `{todo}` status-bar segment consuming the
# bundled server-side checklist plugin's PluginCall RPC, on the cost-view
# shape (background poll, tab-switch refresh, unfilled-segment degradation).

add_subdirectory(checklist-view)

# mcp-view is the client half of the mcp plugin pair: the `/mcp` picker as a
# modal view over the server-side mcp plugin's endpoint. Core has no MCP UI.

add_subdirectory(mcp-view)

# ask-view is the client half of the tool-ask plugin pair: the "ask" prompt
# renderer (the modal ask_user parks on) and the "ask" display renderer (its
# transcript row). Core has no ask UI.

add_subdirectory(ask-view)

# config-view is the TUI for plugin configuration: /config, a modal over core's
# `plugins` endpoint, built on client core's reusable schema form.

add_subdirectory(config-view)

# help-view is the key bindings and /-commands overlays (alt+h, /keymap, a
# bare /help), over the host's own reads; core keeps only the vocabulary.

add_subdirectory(help-view)
