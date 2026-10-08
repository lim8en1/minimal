# Bundled CLIENT plugin implementations.
#
# The host machinery lives in src/client/plugins, the SDK plugins build
# against in src/client/plugins/include/minimal/client/sdk; this subtree is
# implementations, categorically identical to a third-party client plugin and
# differing only in who publishes it. Several fill slots the TUI cannot run
# without (chat-view, prompt-editor, ui-kit, consent-prompts); the rest add
# views, segments and renderers.
#
# logo: the ASCII-banner start view -- deliberately the thinnest real plugin,
# a reference shape as much as a feature.

add_subdirectory(logo)

# cost-view: a `cost` status-bar segment over the server-side cost-tracker
# plugin's PluginCall endpoint.

add_subdirectory(cost-view)

# checklist-view: a `todo` status-bar segment over the server-side
# checklist plugin, on the cost-view shape (background poll, tab-switch
# refresh, unfilled-segment degradation).

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

# theme-view is the theme picker and /theme, over the host's ThemeNames read
# and SetTheme action.

add_subdirectory(theme-view)

# selection-view is the agent, model and subagent-model choosers: the views
# core's /agent, /model and /subagent-model (and the first message, when it
# needs a choice) open by role.

add_subdirectory(selection-view)

# session-view is the session and subagent pickers: the "sessions" and
# "subagents" views core (ctrl+o, a bare /sessions) and the server's view
# hints open.

add_subdirectory(session-view)

# session-status is the status bar's usual segments (version, session,
# tokens, model, agent, permission_mode, paused, activity), each placing
# itself, over the focused tab's state the host hands every segment.

add_subdirectory(session-status)

# ui-kit is the widget kit every view builds on (menus, the schema form, the
# model selector, modal chrome): the required widget-kit slot's default.

add_subdirectory(ui-kit)

# syntax-grammars is the bundled tree-sitter grammar set code blocks are
# highlighted with.

add_subdirectory(syntax-grammars)

# markdown-md4c is the markdown engine (md4c, debounced streaming, fenced code
# through the host's highlighter); without it markdown reads as plain text.

add_subdirectory(markdown-md4c)

# consent-prompts draws core's two prompt kinds, "permission" and
# "confirmation" (both required: the TUI refuses to start without them).

add_subdirectory(consent-prompts)

# prompt-editor: the required prompt editor slot (one editor per tab).
add_subdirectory(prompt-editor)

# tools-panel: the running and recent tool calls across tabs (ctrl+t).
add_subdirectory(tools-panel)

# tab-bar: the row of tabs above the chat (optional: none, no bar).
add_subdirectory(tab-bar)

# chat-renderers: the default row renderers for every message kind and
# display payload; any plugin's claim of a kind replaces its fallback.
add_subdirectory(chat-renderers)

# chat-view: the required chat surface -- one scrolling pane per tab.
add_subdirectory(chat-view)
