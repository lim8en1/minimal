# chat-view

The chat: the TUI's body, one per tab.

## What it adds

The exclusive, required chat-surface slot (`registries.hpp` `ChatSurface`):
the transcript and the turn in flight -- rows through the message and display
renderers (bundled: `chat-renderers`), live markdown streamed as it arrives,
live tool rows with their clocks, queued messages (click one to cancel it),
row folding, timestamps, and line-granular scrolling that holds still while
you read. Its keys are rebindable in `keybindings.json`: `chat.scroll_up`,
`chat.scroll_down`, `chat.scroll_bottom`, `chat.row_up`, `chat.row_down`,
`chat.toggle_collapse`, `chat.toggle_collapse_all`, `chat.toggle_timestamps`,
`chat.toggle_thinking_fold`, `chat.toggle_tool_fold`.

Core keeps the data -- the transcript window, scrollback paging
(`chat.load_older`), the live turn -- and hands each tab's pane a `ChatFrame`
every frame. With no chat surface the TUI refuses to start.

## Configuration

`client-plugins.json`, this plugin's `config`:

- `fold_thinking`, `fold_tools` (`"auto"`, `"expanded"` or `"collapsed"`,
  default `"auto"`): how thinking and tool rows start. The fold toggles
  override them, and are kept across restarts (the plugin's own state).
