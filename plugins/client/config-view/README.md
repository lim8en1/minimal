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

Keys in the form: up/down (`k`/`j`) select; enter toggles a boolean, opens a
chooser for an enum (its choices) or a model ref (the server's models, an
"empty" row that unsets it, and any typed `<provider>/<model>` the list does
not have), or edits a value inline (enter saves, esc cancels; a secret is
typed masked); `d` resets a field to its default (a set secret asks twice);
left or esc goes back to the list (Backspace does not: a habitual delete
right after the editor closed used to leave the form); esc on the list
closes -- and in a chooser or the editor, goes back to the form.

Edits go to core's `plugins` endpoint: the plugin's own parser checks the
value (a refusal shows in red, nothing is written), the file
`plugins.d/<name>.json` is written, and the change applies live when every
changed field is live — otherwise the form says to restart the server.

## Configuration

None.

## Built on

The widget kit (`ClientPluginRegistrationContext::widgets`): its schema form
and filterable menu. The form is reusable by any view with a
`describe`-shaped description —
`{schema, config, pending}`, the schema being what
`minimal_plugin_config_schema` records.
