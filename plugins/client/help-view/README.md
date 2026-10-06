# help-view

The TUI's two reference overlays.

## What it adds

- **View** `help.overlay` (modal, default key `alt+h`): every key binding in
  force -- core's actions and the plugin views' -- grouped by area, the key in
  a column, what it does in words. Core supplies the words and the grouping
  (`HostUi::KeyBindings`); the view only lays them out. Type to filter on the
  key, the action or the description.
- **View** `help.commands` (modal): the /-commands the focused tab offers
  (`HostUi::CommandCatalog` -- without a session only what runs without one,
  on a subagent only what is read-only there). Enter puts `/name ` in the
  prompt, ready for arguments (`HostActions::InsertPrompt`).
- **Commands** `/keymap` (the key bindings) and a bare `/help` (the commands);
  `/help <command>` stays the server's.

Both read the host afresh on every open and draw through the widget kit.

## Configuration

None. The key is rebound like any view's, under its id in
`keybindings.json`.
