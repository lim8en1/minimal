# config-view

Plugin configuration in the TUI: `/config`.

## What it adds

- **View** `config` (modal): the plugins that take configuration, then one
  plugin's form — every field with its value (`*` = set in the file, the rest
  are defaults), whether it applies **live** or on **restart**, and
  **restart pending** where the file differs from what the running plugin
  uses; a field the current mode does not use (memory's http fields under
  `strategy: sidecar`) is dimmed and tagged **not used**, still editable. The
  selected field's type, default, range, doc and, when unused, why show below.
- **Command** `/config [plugin]` — opens the list, or straight onto a
  plugin's form.

Keys in the form: up/down (`k`/`j`) select; enter toggles a boolean, steps
an enum, or edits a value inline (enter saves, esc cancels); `d` resets a
field to its default; left/backspace goes back to the list; esc closes.

Edits go to core's `plugins` endpoint: the plugin's own parser checks the
value (a refusal shows in red, nothing is written), the file
`plugins.d/<name>.json` is written, and the change applies live when every
changed field is live — otherwise the form says to restart the server.

## Configuration

None.

## Built on

`client::MakeSchemaForm` (`minimal/client/view/schema_form.hpp`): the form is
client core's, reusable by any view with a `describe`-shaped description —
`{schema, config, pending}`, the schema being what
`minimal_plugin_config_schema` records.
