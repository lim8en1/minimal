# theme-view

Switching the UI theme.

## What it adds

- **View** `theme` (modal): the themes in the config dir's `themes/`, the
  active one marked. Enter switches.
- **Command** `/theme` -- bare opens the picker, `/theme <name>` switches
  directly. Read-only, so it works on any tab.

The switch is the host's (`HostActions::SetTheme`): it reloads the palette,
re-pushes it to every view, and records it in `ui.json` for the next start.
A theme that fails to load changes nothing and says why.

## Configuration

None.
