# tab-bar

The row of tabs above the chat.

## What it adds

The exclusive, optional tab-bar slot (`sdk/render.hpp` `TabBar`): one cell per
visible tab -- its number, name, a state glyph and color (a spinner while busy,
✓ done, ✗ error), `!` when it waits on you, `↳` for a subagent -- the focused
one highlighted, a window around the focused tab with `«N`/`N»` counts when
they do not all fit, and the tab filter's badge at the end. Hidden while only
one tab is open and no filter is on. Click a cell to focus it.

Core keeps the tabs, their order and the filter; it hands the bar the visible
tabs and the badge each frame. To use a different bar, disable this plugin in
`client-plugins.json` and enable one that registers a `TabBar`; with none
there is no bar. A replacement must show a color-independent mark for a tab
that needs attention.

## Configuration

None.
